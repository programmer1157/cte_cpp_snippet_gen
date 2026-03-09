/*
C++17 Keyword-driven snippet generator — persistent custom keywords with parameters,
help shows C++ standard keywords, and lightweight optimizations.

Changes/additions from original provided source:
 1) All (previously unmapped) standard C++17 keywords now have small, meaningful
    handlers so the follow-up prompts provide constructive, illustrative questions.
    The handlers are intentionally lightweight and preserve the original program
    flow and behavior.
 2) During processing of an input line, if a token looks like an undefined custom
    keyword (i.e. it's not a C++17 keyword and not previously stored), the tool
    pauses and asks the user whether to define it now or skip it. If the user
    chooses to define it, a small define flow runs (parameters + snippet) and the
    new custom keyword is saved and used immediately for the continuing processing.

Other behavior/features unchanged: sequence-aware detection of every keyword
occurrence, follow-up prompts reference each occurrence, read_multiline_body(...)
implemented, EOF during follow-ups aborts cleanly, commands :add/:define, :list,
:delete, :help retained and extended.

Compile: g++ -std=c++17 -O2 -Wall -Wextra -o snippet_gen snippet_gen_updated.cpp
*/

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <unordered_set>
#include <vector>
#include <thread>
#include <chrono>
#include <unordered_map>

using std::cin;
using std::cout;
using std::cerr;
using std::endl;
using std::string;
using std::getline;
using std::optional;
using std::nullopt;
using std::ostringstream;
using std::vector;
using std::map;
using std::set;
using std::unordered_set;

// EOF signal during prompts.
struct EOFExit : public std::runtime_error {
    EOFExit() : std::runtime_error("EOF received during prompt") {}
};

// -------------------- Small helpers (required names) --------------------

static optional<string> prompt_opt(const string &prompt) {
    cout << prompt;
    cout.flush();
    string line;
    if (!getline(cin, line)) throw EOFExit();
    if (line.empty()) return nullopt;
    return line;
}

static string ask(const string &prompt, const string &def) {
    ostringstream p;
    p << prompt << " [" << def << "]: ";
    auto res = prompt_opt(p.str());
    if (!res.has_value()) return def;
    return *res;
}

static string trim(const string &s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static string normalize_token(const string &token) {
    size_t i = 0, j = token.size();
    while (i < j && std::ispunct(static_cast<unsigned char>(token[i]))) ++i;
    while (j > i && std::ispunct(static_cast<unsigned char>(token[j - 1]))) --j;
    string t = token.substr(i, j - i);
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return t;
}

static vector<string> split_csv(const string &s) {
    vector<string> out;
    std::istringstream iss(s);
    string item;
    while (std::getline(iss, item, ',')) out.push_back(trim(item));
    return out;
}

// Normalize an include string into a "printable key" that retains angle/quote
// semantics for printing and allows deduplication.
// Input forms we accept here:
//   - "<vector>"  (already bracketed)
//   - "\"my.h\""  (already quoted)
//   - "vector"    (plain token like earlier code used)
// The returned key will be: "<vector>", "\"my.h\"" or "<vector>" for plain "vector".
static string normalize_include_for_key(const string &raw) {
    string s = trim(raw);
    if (s.empty()) return s;
    if (s.front() == '<' && s.back() == '>') return s;            // already <...>
    if (s.front() == '"' && s.back() == '"') return s;            // already "..."
    // otherwise assume a plain header name -> print with angle brackets
    return string("<") + s + string(">");
}

static string make_program_from_body_lines(const vector<string> &body_lines,
                                          const vector<string> &extra_includes = {},
                                          const vector<string> &extra_top = {}) {
    std::ostringstream out;
    // Always print iostream first
    out << "#include <iostream>\n";

    // build set of unique include keys (like "<vector>" or "\"my.h\"")
    std::set<string> uniq;
    for (const auto &h : extra_includes) {
        string key = normalize_include_for_key(h);
        if (key.empty()) continue;
        if (key == "<iostream>") continue; // iostream already emitted
        uniq.insert(key);
    }
    for (const auto &h : uniq) {
        out << "#include " << h << "\n";
    }
    out << "\n";

    for (auto &t : extra_top) out << t << "\n";
    out << "\nusing namespace std;\n\n";
    out << "int main(int argc, char *argv[]) {\n";
    for (auto &line : body_lines) out << "    " << line << "\n";
    out << "    return 0;\n";
    out << "}\n";
    return out.str();
}

// read_multiline_body implementation: reads lines until a single 'QED' line.
// Returns collected lines (excluding the 'QED') and throws EOFExit on EOF.
static std::vector<std::string> read_multiline_body(const std::string &instruction =
    "Enter lines, finish with a single 'QED' on its own line:") {
    std::cout << instruction << std::endl;
    std::vector<std::string> lines;
    lines.reserve(16);
    std::string line;
    while (true) {
        std::cout << "> ";
        std::cout.flush();
        if (!std::getline(std::cin, line)) throw EOFExit();
        if (line == "QED") break;
        lines.push_back(line);
    }
    return lines;
}

// streambuf that forwards to an underlying buffer but adds a tiny delay per character.
// It also implements xsputn by forwarding character-by-character to ensure the delay
// is applied to bulk writes as well.
class SlowBuf : public std::streambuf {
    std::streambuf* orig_;
    unsigned int ms_per_char_;
public:
    SlowBuf(std::streambuf* orig, unsigned int ms_per_char = 6)
        : orig_(orig), ms_per_char_(ms_per_char) {}

protected:
    // replace SlowBuf::overflow with this
    virtual int_type overflow(int_type ch) override {
        if (ch == traits_type::eof()) return traits_type::not_eof(ch);
        char c = static_cast<char>(ch);

        // write the char to the original buffer
        if (orig_->sputc(c) == traits_type::eof()) return traits_type::eof();

        // force the underlying buffer to flush so the terminal displays the char immediately
        orig_->pubsync();

        // avoid long delay on newline to keep interactivity reasonable
        if (c != '\n') std::this_thread::sleep_for(std::chrono::milliseconds(ms_per_char_));
        return ch;
    }

    // replace SlowBuf::xsputn with this
    virtual std::streamsize xsputn(const char* s, std::streamsize n) override {
        // forward one-by-one to overflow so each character is flushed and delayed
        for (std::streamsize i = 0; i < n; ++i) {
            if (overflow(static_cast<unsigned char>(s[i])) == traits_type::eof())
                return i;
        }
        return n;
    }
    // forward sync/flush to underlying buffer
    virtual int sync() override {
        return orig_->pubsync();
    }
};

// global pointers to allow install/uninstall
static SlowBuf *g_slow_cout = nullptr;
static SlowBuf *g_slow_cerr = nullptr;
static std::streambuf* g_old_cout = nullptr;
static std::streambuf* g_old_cerr = nullptr;

// call once at program start to enable slow character-by-character output.
// ms_per_char: milliseconds per non-newline character (6ms ≈ ChatGPT feel; adjust as desired).
static void install_slow_output(unsigned int ms_per_char = 6) {
    if (g_slow_cout) return; // already installed
    g_old_cout = std::cout.rdbuf();
    g_old_cerr = std::cerr.rdbuf();
    g_slow_cout = new SlowBuf(g_old_cout, ms_per_char);
    g_slow_cerr = new SlowBuf(g_old_cerr, ms_per_char);
    std::cout.rdbuf(g_slow_cout);
    std::cerr.rdbuf(g_slow_cerr);
}

// -------------------- C++17 keyword set (function-local static) --------------------

static const unordered_set<string>& cpp17_keywords() {
    static const unordered_set<string> kws = [](){
        const char* arr[] = {
            "alignas","alignof","and","and_eq","asm","auto","bitand","bitor","bool","break",
            "case","catch","char","char16_t","char32_t","class","compl","const","constexpr","const_cast",
            "continue","decltype","default","delete","do","double","dynamic_cast","else","enum","explicit",
            "export","extern","false","float","for","friend","goto","if","inline","int",
            "long","mutable","namespace","new","noexcept","not","not_eq","nullptr","operator","or",
            "or_eq","private","protected","public","register","reinterpret_cast","return","short","signed","sizeof",
            "static","static_assert","static_cast","struct","switch","template","this","thread_local","throw","true",
            "try","typedef","typeid","typename","union","unsigned","using","virtual","void","volatile",
            "wchar_t","while","xor","xor_eq"
        };
        unordered_set<string> s;
        s.reserve(256);
        for (auto p : arr) s.insert(string(p));
        return s;
    }();
    return kws;
}

// -------------------- Persistence for user-defined keywords with parameters ----

// File format:
// ===KEYWORD:<name>===
// ===PARAMS:name=default,other=val===   (optional; if absent there are no params)
// <snippet lines...>
// ===END===

static const char *USER_KW_FILE = "user_keywords.db";

// Represents a user-defined keyword with its snippet and parameters (name, default)
struct UserKeyword {
    string snippet;                         // raw multiline snippet
    vector<std::pair<string,string>> params; // ordered list of (name, default)
};

// Use a hash-map for user keywords for O(1) average lookup
using UserKeywordMap = std::unordered_map<std::string, UserKeyword>;

// Load user keywords into out_map (key -> UserKeyword)
static void load_user_keywords(UserKeywordMap &out_map, const string &path = USER_KW_FILE) {
    out_map.clear();
    std::ifstream ifs(path, std::ios::in);
    if (!ifs) return;
    string line;
    string current_key;
    vector<std::pair<string,string>> current_params;
    std::ostringstream buffer;
    bool in_entry = false;
    while (std::getline(ifs, line)) {
        if (!in_entry) {
            if (line.rfind("===KEYWORD:", 0) == 0) {
                // parse key
                size_t colon = line.find(':');
                size_t last = line.rfind("===");
                if (colon != string::npos && last != string::npos && last > colon+1) {
                    current_key = trim(line.substr(colon + 1, last - (colon + 1)));
                    current_params.clear();
                    buffer.str("");
                    buffer.clear();
                    in_entry = true;
                    // peek next line to see if it's PARAMS, but we continue loop
                }
            }
        } else {
            if (line.rfind("===PARAMS:", 0) == 0) {
                // parse params line
                size_t colon = line.find(':');
                size_t last = line.rfind("===");
                if (colon != string::npos && last != string::npos && last > colon+1) {
                    string paramstr = trim(line.substr(colon + 1, last - (colon + 1)));
                    if (!paramstr.empty()) {
                        auto parts = split_csv(paramstr);
                        for (auto &p : parts) {
                            size_t eq = p.find('=');
                            string name = trim((eq==string::npos)?p:p.substr(0,eq));
                            string def = trim((eq==string::npos)?"":p.substr(eq+1));
                            if (!name.empty()) current_params.emplace_back(name, def);
                        }
                    }
                }
            } else if (line == "===END===") {
                UserKeyword uk;
                uk.snippet = buffer.str();
                uk.params = current_params;
                out_map[trim(current_key)] = std::move(uk);
                in_entry = false;
                current_key.clear();
                current_params.clear();
                buffer.str("");
                buffer.clear();
            } else {
                buffer << line << "\n";
            }
        }
    }
    // commit if file ended mid-entry
    if (in_entry && !current_key.empty()) {
        UserKeyword uk;
        uk.snippet = buffer.str();
        uk.params = current_params;
        out_map[trim(current_key)] = std::move(uk);
    }
}

// Save user keywords map to disk
static bool save_user_keywords(const UserKeywordMap &m, const string &path = USER_KW_FILE) {
    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    if (!ofs) return false;
    for (const auto &kv : m) {
        ofs << "===KEYWORD:" << kv.first << "===\n";
        // write params
        if (!kv.second.params.empty()) {
            ofs << "===PARAMS:";
            bool first = true;
            for (const auto &pp : kv.second.params) {
                if (!first) ofs << ",";
                ofs << pp.first << "=" << pp.second;
                first = false;
            }
            ofs << "===\n";
        }
        // snippet
        ofs << kv.second.snippet;
        if (!kv.second.snippet.empty() && kv.second.snippet.back() != '\n') ofs << "\n";
        ofs << "===END===\n";
    }
    return true;
}

// -------------------- Parts & Context (unchanged) --------------------

struct Parts {
    vector<string> includes;
    vector<string> top;
    vector<string> body;
};

struct Frame {
    Parts parts;       // header + inner lines (header stored in parts.body[0] if present)
    size_t insert_pos; // index in aggregated.body where inner lines should be inserted
};

struct Context {
    map<string,string> vars;
    std::set<string> types;
    string last_var;
    string last_type;
    map<string,string> meta;
    std::vector<Frame> control_stack;
};

// trim a string (preserve original indentation elsewhere)
static inline std::string trim_copy(const std::string &s) {
    size_t l = 0;
    while (l < s.size() && isspace((unsigned char)s[l])) ++l;
    size_t r = s.size();
    while (r > l && isspace((unsigned char)s[r-1])) --r;
    return s.substr(l, r - l);
}

static bool parts_is_opening_block(const Parts &p) {
    // Look for a control-header line anywhere in the Parts body (conservative window).
    // We consider the common headers: for(), while(), if(), switch(), do {, case <val>:
    for (const auto &ln : p.body) {
        std::string t;
        // trim leading whitespace for pattern checks
        size_t pos = 0;
        while (pos < ln.size() && isspace((unsigned char)ln[pos])) ++pos;
        if (pos >= ln.size()) continue;
        t = ln.substr(pos);
        if (t.rfind("for (", 0) == 0 || t.rfind("while (", 0) == 0 ||
            t.rfind("if (", 0) == 0 || t.rfind("switch (", 0) == 0 ||
            t.rfind("do {", 0) == 0 || t.rfind("case ", 0) == 0 ||
            t.rfind("try", 0) == 0 || t.rfind("catch(", 0) == 0 ||
            t.rfind("catch (", 0) == 0 || t.rfind("else", 0) == 0) {
                return true;
        }
        // generic: any non-empty line that ends with '{' is a candidate
        if (!t.empty() && t.back() == '{') return true;
    }
    return false;
}

static void extract_block_header_and_inner(const Parts &p,
                                           std::vector<std::string> &out_preceding,
                                           std::string &out_header,
                                           std::vector<std::string> &out_inner,
                                           bool &had_closing) {
    out_preceding.clear();
    out_header.clear();
    out_inner.clear();
    had_closing = false;

    // find index of the first header-like line
    int header_idx = -1;
    for (size_t i = 0; i < p.body.size(); ++i) {
        const string &ln = p.body[i];
        size_t pos = 0;
        while (pos < ln.size() && isspace((unsigned char)ln[pos])) ++pos;
        if (pos >= ln.size()) {
            // empty line -> treat as preceding
            out_preceding.push_back(ln);
            continue;
        }
        string t = ln.substr(pos);
        if (t.rfind("for (", 0) == 0 || t.rfind("while (", 0) == 0 ||
            t.rfind("if (", 0) == 0 || t.rfind("switch (", 0) == 0 ||
            t.rfind("do {", 0) == 0 || t.rfind("case ", 0) == 0 || (!t.empty() && t.back() == '{')) {
            header_idx = static_cast<int>(i);
            break;
        } else {
            out_preceding.push_back(ln);
        }
    }

    if (header_idx == -1) {
        // no header found: nothing special — treat entire body as "preceding"
        return;
    }

    // header is at header_idx
    out_header = p.body[header_idx];

    // collect inner lines after header up to possibly a trailing single '}'
    size_t j = header_idx + 1;
    for (; j < p.body.size(); ++j) {
        string t = p.body[j];
        // check if final single '}' (trimmed) and it's the last line
        size_t pos = 0;
        while (pos < t.size() && isspace((unsigned char)t[pos])) ++pos;
        size_t endpos = t.size();
        while (endpos > pos && isspace((unsigned char)t[endpos-1])) --endpos;
        string trimmed = (pos < endpos) ? t.substr(pos, endpos - pos) : string();
        if (trimmed == "}" && j == p.body.size() - 1) {
            had_closing = true;
            break;
        }
        out_inner.push_back(p.body[j]);
    }
}

// ----------------- Identifier helpers & improved declare_variable -----------------
static std::string sanitize_identifier(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            (c == '_')) {
            out.push_back(c);
        } else if (c == ' ' || c == '-' || c == '.') {
            out.push_back('_');
        } else {
            // drop other characters
        }
    }
    if (out.empty()) out = "v";
    if ((out[0] >= '0' && out[0] <= '9')) out = std::string("_") + out;
    return out;
}

/*
 Improved declare_variable:
  - Sanitizes the requested base name.
  - If the sanitized name already exists in ctx.vars, repeatedly asks the user
    for another name (showing a suggested alternative) until a unique name is provided.
  - Registers the chosen name in ctx.vars and sets ctx.last_var.
  - Returns the textual declaration (e.g. "int foo = 0;").
*/
static std::string declare_variable(Context &ctx,
                                    const std::string &type,
                                    const std::string &base_name,
                                    const std::string &init = "")
{
    std::string base = sanitize_identifier(base_name);
    std::string candidate = base;

    if (ctx.vars.find(candidate) != ctx.vars.end()) {
        // Build an initial suggestion
        std::string suggestion;
        int suffix = 1;
        do {
            suggestion = base + std::to_string(suffix++);
        } while (ctx.vars.find(suggestion) != ctx.vars.end());

        // Prompt user until they provide a unique identifier
        while (true) {
            std::string prompt = "Variable name '" + candidate + "' is already used. "
                                 "Choose another variable name (suggestion: " + suggestion + "):";
            std::string reply = ask(prompt, suggestion);
            std::string sanitized = sanitize_identifier(reply);
            if (sanitized.empty()) sanitized = suggestion;

            if (ctx.vars.find(sanitized) == ctx.vars.end()) {
                candidate = sanitized;
                break;
            }

            // prepare a different suggestion if needed
            // simple incremental global suggestion to avoid stuck collisions
            static int __global_sugg = 1000;
            while (ctx.vars.find(suggestion) != ctx.vars.end()) {
                suggestion = base + std::to_string(__global_sugg++);
            }
            // loop will re-prompt
            candidate = sanitized;
        }
    }

    // Register and return declaration
    ctx.vars[candidate] = type;
    ctx.last_var = candidate;

    std::string decl;
    if (init.empty()) decl = type + " " + candidate + ";";
    else decl = type + " " + candidate + " = " + init + ";";

    return decl;
}

static void append_parts(Parts &acc, const Parts &p) {
    for (auto &inc : p.includes) acc.includes.push_back(inc);
    for (auto &t : p.top) acc.top.push_back(t);
    for (auto &b : p.body) acc.body.push_back(b);
}

static inline std::string trim_leading(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}

/// Helper: trim trailing whitespace (CR/LF/space/tab)
static std::string trim_trailing(const std::string &s) {
    size_t end = s.size();
    while (end > 0 && (s[end-1] == '\r' || s[end-1] == '\n' || s[end-1] == ' ' || s[end-1] == '\t')) --end;
    return s.substr(0, end);
}

// Helper: get leading whitespace substring of a line
static std::string leading_ws_of(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return line.substr(0, i);
}

// Helper: find the index of the nearest *unclosed* header line (a line whose trimmed trailing char is '{')
// that corresponds to the position `search_from`. We scan backward and account for braces so that we find
// the most-recent header that is still open at search_from. Returns string::npos if not found.
static size_t find_unclosed_header_index(const Parts &acc, size_t search_from) {
    if (acc.body.empty()) return std::string::npos;
    size_t i = (search_from == 0 ? 0 : (search_from > acc.body.size() ? acc.body.size() : search_from));
    int depth = 0;
    while (i > 0) {
        --i;
        std::string line = trim_trailing(acc.body[i]);
        // check if line ends with '}' or '{'
        if (!line.empty()) {
            char last = line.back();
            if (last == '}') {
                ++depth;
                continue;
            } else if (last == '{') {
                if (depth == 0) {
                    return i;
                } else {
                    --depth;
                    continue;
                }
            }
        }
    }
    return std::string::npos;
}

// Helper: produce a preview for a stored open frame: prefer the header line (if found in acc) else first non-empty stored inner line.
static std::string preview_for_frame(const Parts &acc, const Frame &f) {
    size_t hi = find_unclosed_header_index(acc, f.insert_pos);
    if (hi != std::string::npos) return acc.body[hi];
    for (const auto &ln : f.parts.body) {
        std::string t = trim_leading(ln);
        if (!t.empty()) return ln;
    }
    return std::string("(open block)");
}

// Replaces previous append_parts_with_nesting with corrected header-matching, previewing, indentation,
// insert_pos bookkeeping and "try next older" flow.
static void append_parts_with_nesting(Parts &acc, const Parts &p, Context &ctx, const std::string &kw) {
    const std::string INDENT = std::string(4, ' ');

    // 1) If there are open frames, ask from most-recent to older whether to insert there.
    if (!ctx.control_stack.empty()) {
        for (int fi = static_cast<int>(ctx.control_stack.size()) - 1; fi >= 0; --fi) {
            const Frame &frame = ctx.control_stack[fi];
            std::string preview = preview_for_frame(acc, frame);
            std::string q = "Insert snippet for '" + kw + "' inside open block: " + preview + " ? (y/n)";
            std::string resp = ask(q, "y");
            if (!resp.empty() && (resp[0] == 'y' || resp[0] == 'Y')) {
                // chosen to insert into this frame
                for (const auto &inc : p.includes) acc.includes.push_back(inc);
                for (const auto &t : p.top) acc.top.push_back(t);

                // insertion position
                size_t pos = frame.insert_pos;
                if (pos > acc.body.size()) pos = acc.body.size();

                // find the header corresponding to this frame
                size_t header_idx = find_unclosed_header_index(acc, pos);
                std::string header_ws = (header_idx == std::string::npos) ? std::string() : leading_ws_of(acc.body[header_idx]);

                // If incoming snippet is an opening control block, treat header & inner specially
                if (parts_is_opening_block(p)) {
                    std::vector<std::string> preceding;
                    std::string header;
                    std::vector<std::string> inner;
                    bool had_closing = false;
                    extract_block_header_and_inner(p, preceding, header, inner, had_closing);

                    // Build insertion lines: preceding (at header level), header (one indent deeper than parent), initial inner (one more)
                    std::vector<std::string> to_insert;
                    std::string header_indent = header_ws + INDENT;
                    std::string nested_inner_indent = header_indent + INDENT;

                    for (const auto &pr : preceding) {
                        std::string t = trim_leading(pr);
                        if (t.empty()) to_insert.push_back(std::string());
                        else to_insert.push_back(header_indent + t);
                    }

                    std::string header_trim = trim_leading(header);
                    to_insert.push_back(header_indent + header_trim);

                    for (const auto &ln : inner) {
                        std::string t = trim_leading(ln);
                        if (t.empty()) to_insert.push_back(std::string());
                        else to_insert.push_back(nested_inner_indent + t);
                    }

                    // insert at pos
                    acc.body.insert(acc.body.begin() + pos, to_insert.begin(), to_insert.end());

                    // update insert_pos for all frames whose insert_pos >= pos
                    size_t inserted_count = to_insert.size();
                    for (size_t fj = 0; fj < ctx.control_stack.size(); ++fj) {
                        if (ctx.control_stack[fj].insert_pos >= pos) ctx.control_stack[fj].insert_pos += inserted_count;
                    }

                    // ask whether to keep the newly-inserted block open
                    std::string keep = ask("Detected control block header for '" + kw + "'.\nKeep this block open for nested inserts? (y/n)", "y");
                    if (!keep.empty() && (keep[0] == 'y' || keep[0] == 'Y')) {
                        // push new frame: insert_pos immediately after the header + any initial inner lines
                        Frame nf;
                        // The new insert position should be at the end of the inserted chunk (i.e. pos + inserted_count)
                        nf.parts.body.clear();
                        nf.insert_pos = pos + inserted_count;
                        ctx.control_stack.push_back(std::move(nf));
                        return;
                    } else {
                        // not keeping open -> ensure closing brace present aligned with header_indent
                        size_t close_pos = pos + inserted_count; // after inserted lines
                        if (!had_closing) {
                            if (close_pos > acc.body.size()) close_pos = acc.body.size();
                            bool has_closing = false;
                            if (close_pos < acc.body.size()) {
                                std::string t = trim_leading(acc.body[close_pos]);
                                if (!t.empty() && t == "}") has_closing = true;
                            }
                            if (!has_closing) {
                                acc.body.insert(acc.body.begin() + close_pos, header_ws + "}");
                                // bump insert_pos of earlier frames (older) so their positions remain valid
                                for (size_t oj = 0; oj < ctx.control_stack.size(); ++oj) {
                                    if (ctx.control_stack[oj].insert_pos >= close_pos) ctx.control_stack[oj].insert_pos += 1;
                                }
                            }
                        }
                        return;
                    }
                } // end opening-block handling

                // Non-opening snippet: indent relative to the header_ws
                std::string insert_indent = header_ws + INDENT;
                std::vector<std::string> to_insert;
                for (const auto &ln : p.body) {
                    std::string t = trim_leading(ln);
                    if (t.empty()) to_insert.push_back(std::string());
                    else to_insert.push_back(insert_indent + t);
                }

                // Insert and update bookkeeping
                acc.body.insert(acc.body.begin() + pos, to_insert.begin(), to_insert.end());
                size_t inserted_count = to_insert.size();

                // update this frame's insert_pos
                ctx.control_stack[fi].insert_pos += inserted_count;
                // bump other frames' insert_pos if necessary
                for (size_t fj = 0; fj < ctx.control_stack.size(); ++fj) {
                    if (fj == static_cast<size_t>(fi)) continue;
                    if (ctx.control_stack[fj].insert_pos >= pos) ctx.control_stack[fj].insert_pos += inserted_count;
                }
                return;
            } // end if user accepted this frame

            // user declined this frame -> ask whether to try the next older open frame
            if (fi > 0) {
                std::string try_next = ask("Try the next older open block? (y/n)", "y");
                if (try_next.empty() || (try_next[0] != 'y' && try_next[0] != 'Y')) {
                    break; // stop trying older frames and fall through to top-level handling
                } else {
                    continue; // try next older
                }
            }
            // no older frames: fall through
        } // end for frames
        // fell out: no open frame chosen.
    } // end if control_stack not empty

    // Continue with top-level handling.
    // 2) No frame chosen (or none existed). Handle opening-block at top-level.
    if (parts_is_opening_block(p)) {
        std::vector<std::string> preceding;
        std::string header;
        std::vector<std::string> inner;
        bool had_closing = false;
        extract_block_header_and_inner(p, preceding, header, inner, had_closing);

        // emit includes/top and preceding lines
        for (const auto &inc : p.includes) acc.includes.push_back(inc);
        for (const auto &t : p.top) acc.top.push_back(t);
        for (const auto &ln : preceding) acc.body.push_back(trim_leading(ln));

        // ask whether to keep open
        std::string keep = ask("Detected control block header for '" + kw + "'.\nKeep this block open for nested inserts? (y/n)", "y");
        if (!keep.empty() && (keep[0] == 'y' || keep[0] == 'Y')) {
            // write header (top-level)
            std::string header_line = trim_leading(header);
            acc.body.push_back(header_line);
            // initial inner lines indented one level
            for (const auto &ln : inner) {
                std::string t = trim_leading(ln);
                if (t.empty()) acc.body.push_back(std::string());
                else acc.body.push_back(INDENT + t);
            }
            // push frame with insert_pos after header + any initial inner
            Frame f;
            f.parts.body.clear();
            f.insert_pos = acc.body.size();
            ctx.control_stack.push_back(std::move(f));
            return;
        }

        // not keeping open: emit header+inner+closing immediately
        if (!header.empty()) acc.body.push_back(trim_leading(header));
        for (const auto &ln : inner) {
            std::string t = trim_leading(ln);
            if (t.empty()) acc.body.push_back(std::string());
            else acc.body.push_back(INDENT + t);
        }
        if (!had_closing) acc.body.push_back(std::string("}"));
        return;
    }

    // 3) Default: append normally to top-level
    append_parts(acc, p);
}

// Replaces previous flush_control_stack: closes frames in LIFO order, aligns braces to the matching header,
// and updates other frames' insert_pos accordingly.
static void flush_control_stack(Parts &acc, Context &ctx) {
    if (ctx.control_stack.empty()) return;

    // close frames in LIFO order
    for (int fi = static_cast<int>(ctx.control_stack.size()) - 1; fi >= 0; --fi) {
        Frame &frame = ctx.control_stack[fi];
        size_t close_pos = frame.insert_pos;
        if (close_pos > acc.body.size()) close_pos = acc.body.size();

        // skip if there's already a closing brace at close_pos
        bool already = false;
        if (close_pos < acc.body.size()) {
            std::string t = trim_leading(acc.body[close_pos]);
            if (!t.empty() && t == "}") already = true;
        }

        if (!already) {
            // find corresponding header and its leading whitespace
            size_t header_idx = find_unclosed_header_index(acc, close_pos);
            std::string header_ws;
            if (header_idx != std::string::npos) header_ws = leading_ws_of(acc.body[header_idx]);
            else header_ws = std::string();

            // insert aligned closing brace
            acc.body.insert(acc.body.begin() + close_pos, header_ws + "}");

            // bump insert_pos of earlier frames (older) so their positions remain valid
            for (int oj = 0; oj < fi; ++oj) {
                if (ctx.control_stack[oj].insert_pos >= close_pos) ctx.control_stack[oj].insert_pos += 1;
            }
        }
    }

    ctx.control_stack.clear();
}

// Replace all occurrences of token in s with repl.
static void replace_all(string &s, const string &token, const string &repl) {
    if (token.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(token, pos)) != string::npos) {
        s.replace(pos, token.size(), repl);
        pos += repl.size();
    }
}

// Build parts from a user snippet, with parameter substitution applied.
// NOTE: custom snippets must NOT contain an int main(. We also extract #include
// lines from the snippet and place them into Parts.includes so they will be
// emitted before main. The snippet body lines (without includes) are returned
// in Parts.body.
static Parts parts_from_user_snippet_with_params(const UserKeyword &uk, const map<string,string> &values, const string &tag) {
    // copy snippet and replace placeholders {name} with provided values
    string transformed = uk.snippet;
    for (const auto &pp : uk.params) {
        const string &pname = pp.first;
        auto it = values.find(pname);
        string val = (it != values.end()) ? it->second : pp.second;
        // replace occurrences of "{" + pname + "}" with val
        replace_all(transformed, "{" + pname + "}", val);
    }
    // Enforce: custom snippets must not contain main()
    if (transformed.find("int main(") != string::npos) {
        // This should not happen because we prevent storing snippets with main,
        // but be defensive: return a generic informative body emphasizing the issue.
        Parts p;
        p.body.push_back("// (" + tag + ") ERROR: user snippet contains 'int main('. This is disallowed for custom keywords.");
        p.body.push_back("// Please redefine this custom keyword without a main() function.");
        return p;
    }

    Parts p;
    // scan lines: extract #include lines (variants like "#include <...>" or "# include \"...\"" )
    std::istringstream iss(transformed);
    string line;
    while (std::getline(iss, line)) {
        string tline = trim(line);
        // accept both "#include" and "# include"
        if (tline.rfind("#include", 0) == 0) {
            // remainder after "#include"
            string rem = trim(tline.substr(8));
            if (!rem.empty()) {
                // store the remainder as the include token (like "<vector>" or "\"my.h\"" or "vector")
                p.includes.push_back(rem);
            }
            continue; // do not add include lines to body
        } else if (tline.rfind("# include", 0) == 0) {
            string rem = trim(tline.substr(8));
            if (!rem.empty()) p.includes.push_back(rem);
            continue;
        } else {
            p.body.push_back(line);
        }
    }

    // Add a small comment header to body to note parameter substitution
    p.body.insert(p.body.begin(), "// (" + tag + ") User-defined snippet (with parameter substitution):");
    return p;
}

// -------------------- Built-in handlers (tag-aware) --------------------
// For brevity and to preserve original behavior these are similar to previous implementations.
// Each accepts a 'tag' string to reference the occurrence.

static Parts handle_type_like(Context &ctx, const string &kw, const string &tag) {
    Parts p;
    string def_name = "x";
    string def_value = (kw == "char") ? "'a'" : (kw == "bool" ? "true" : "0");
    if (kw == "double") def_value = "3.14";
    if (kw == "float") def_value = "2.5f";
    if (kw == "long") def_value = "123456789L";
    if (kw == "short") def_value = "42";
    if (kw == "wchar_t") def_value = "L'a'";
    if (kw == "char16_t") def_value = "u'a'";
    if (kw == "char32_t") def_value = "U'a'";

    string name = ask("[" + tag + "] Variable name for type '" + kw + "'", def_name);
    string init = ask("[" + tag + "] Initial value for " + name, def_value);
    string decl = declare_variable(ctx, kw, name, init);
    p.body.push_back("// (" + tag + ") Demonstrate type: " + kw);
    p.body.push_back(decl);
    p.body.push_back("cout << \"" + ctx.last_var + " = \" << " + ctx.last_var + " << endl;");
    return p;
}

static Parts handle_auto(Context &ctx, const string &tag) {
    Parts p;
    string init_default = ctx.last_var.empty() ? "42" : ctx.last_var;
    string init = ask("[" + tag + "] Initializer expression for auto variable", init_default);
    string name = ask("[" + tag + "] Variable name", "v");
    string unique = name;
    int suffix = 1;
    while (ctx.vars.find(unique) != ctx.vars.end()) unique = name + std::to_string(suffix++);
    p.body.push_back("// (" + tag + ") Demonstrate auto (type deduction)");
    p.body.push_back("auto " + unique + " = " + init + ";");
    ctx.vars[unique] = "auto";
    ctx.last_var = unique;
    p.body.push_back("cout << \"" + unique + " (deduced) = \" << " + unique + " << endl;");
    return p;
}

static Parts handle_if_else(Context &ctx, const string &tag) {
    Parts p;
    string cond_default = ctx.last_var.empty() ? "x > 0" : (ctx.last_var + " > 0");
    string cond = ask("[" + tag + "] Condition expression for if", cond_default);
    string then_stmt = ask("[" + tag + "] Then-branch ", "cout << \"then\" << endl;");
    string else_stmt = ask("[" + tag + "] Else-branch ", "cout << \"else\" << endl;");
    p.body.push_back("// (" + tag + ") Demonstrate if/else");
    p.body.push_back("if (" + cond + ") {");
    p.body.push_back("    " + then_stmt);
    p.body.push_back("} else {");
    p.body.push_back("    " + else_stmt);
    p.body.push_back("}");
    return p;
}

static Parts handle_for(Context &ctx, const string &tag) {
    Parts p;
    string init = ask("[" + tag + "] Initializer for for-loop", "int i = 0");
    string cond = ask("[" + tag + "] Condition for for-loop", "i < 5");
    string incr = ask("[" + tag + "] Increment expression", "++i");
    string body_stmt = ask("[" + tag + "] Body statement", "cout << i << endl;");
    p.body.push_back("// (" + tag + ") Demonstrate for loop");
    {
        std::istringstream iss(init);
        string t, n;
        if (iss >> t >> n) {
            size_t eq = n.find('=');
            string varname = (eq==string::npos) ? n : n.substr(0, eq);
            varname = normalize_token(varname);
            if (!varname.empty()) { ctx.vars[varname] = t; ctx.last_var = varname; }
        }
    }
    p.body.push_back(init + ";");
    p.body.push_back("for (" + init + "; " + cond + "; " + incr + ") {");
    p.body.push_back("    " + body_stmt);
    p.body.push_back("}");
    return p;
}

static Parts handle_while(Context &ctx, const string &tag) {
    Parts p;
    string init = ask("[" + tag + "] Initializer (e.g., int n = 3)", "int n = 3");
    string cond = ask("[" + tag + "] Condition", "n-- > 0");
    string body_stmt = ask("[" + tag + "] Loop body", "cout << n << endl;");
    {
        std::istringstream iss(init);
        string t, n;
        if (iss >> t >> n) {
            size_t eqpos = n.find('=');
            string varname = (eqpos==string::npos) ? n : n.substr(0, eqpos);
            varname = normalize_token(varname);
            if (!varname.empty()) { ctx.vars[varname] = t; ctx.last_var = varname; }
        }
    }
    p.body.push_back("// (" + tag + ") Demonstrate while");
    p.body.push_back(init + ";");
    p.body.push_back("while (" + cond + ") {");
    p.body.push_back("    " + body_stmt);
    p.body.push_back("}");
    return p;
}

static Parts handle_do(Context &ctx, const string &tag) {
    Parts p;
    string init = ask("[" + tag + "] Initializer (e.g., int n = 3)", "int n = 3");
    string cond = ask("[" + tag + "] Condition (after body)", "n-- > 0");
    string body_stmt = ask("[" + tag + "] Loop body", "cout << n << endl;");
    {
        std::istringstream iss(init);
        string t, n;
        if (iss >> t >> n) {
            size_t eqpos = n.find('=');
            string varname = (eqpos==string::npos) ? n : n.substr(0, eqpos);
            varname = normalize_token(varname);
            if (!varname.empty()) { ctx.vars[varname] = t; ctx.last_var = varname; }
        }
    }
    p.body.push_back("// (" + tag + ") Demonstrate do/while");
    p.body.push_back(init + ";");
    p.body.push_back("do {");
    p.body.push_back("    " + body_stmt);
    p.body.push_back("} while (" + cond + ");");
    return p;
}

static Parts handle_switch(Context &ctx, const string &tag) {
    Parts p;
    string init = ask("[" + tag + "] Initializer (e.g., int n = 2)", "int n = 2");
    // try to register variable from initializer (like other handlers do)
    {
        std::istringstream iss(init);
        string t, n;
        if (iss >> t >> n) {
            size_t eqpos = n.find('=');
            string varname = (eqpos==string::npos) ? n : n.substr(0, eqpos);
            varname = normalize_token(varname);
            if (!varname.empty()) { ctx.vars[varname] = t; ctx.last_var = varname; }
        }
    }

    string expr = ask("[" + tag + "] Expression to switch on", ctx.last_var.empty() ? "n" : ctx.last_var);
    string cases = ask("[" + tag + "] Comma-separated case values", "1,2,3");
    vector<string> case_list = split_csv(cases);

    p.body.push_back("// (" + tag + ") Demonstrate switch");
    p.body.push_back(init + ";");
    p.body.push_back("switch (" + expr + ") {");

    for (auto &c : case_list) {
        // prompt for either a single-line or multiline body for this case
        string single = ask("[" + tag + "] Single-line for case " + c + " (enter 'm' for multiline)", "cout << \"case " + c + "\" << endl; break;");
        if (single == "m" || single == "M") {
            // multiline mode
            p.body.push_back("    case " + c + ":");
            vector<string> lines = read_multiline_body("Enter lines for case " + c + " (finish with a single '.' on its own line):");
            bool has_break = false;
            for (auto &ln : lines) {
                string tln = trim(ln);
                p.body.push_back("        " + ln);
                // simple detection of break; or return; to avoid appending an extra break
                if (tln == "break;" || tln.rfind("break", 0) == 0 || tln.find("return ") == 0 || tln == "return;") has_break = true;
            }
            if (!has_break) p.body.push_back("        break;");
        } else {
            // single-line case: place the user's line directly (assume they include terminating ';' or break)
            p.body.push_back("    case " + c + ": " + single);
        }
    }

    p.body.push_back("    default: cout << \"default\" << endl; break;");
    p.body.push_back("}");
    return p;
}

static Parts handle_return(Context &ctx, const string &tag) {
    Parts p;

    // Disallow at top-level (no open control frames).
    if (ctx.control_stack.empty()) {
        p.body.push_back("// (" + tag + ") ERROR: 'return' is disallowed at top-level.");
        p.body.push_back("// Allowed only inside an if/else, a loop (for/while/do), switch/case, try or catch block.");
        return p;
    }

    // Walk innermost -> outer control frames and examine their header lines.
    bool allowed = true;
    std::string header;
    for (int i = static_cast<int>(ctx.control_stack.size()) - 1; i >= 0; --i) {
        const Parts &fp = ctx.control_stack[i].parts;

        // Reuse repo helper to extract frame header.
        std::vector<std::string> preceding;
        std::vector<std::string> inner;
        bool had_closing = false;
        extract_block_header_and_inner(fp, preceding, header, inner, had_closing);

        std::string h = trim(header);
        if (h.empty()) continue;

        // Normalize leading non-letters (handles " } else {" previews).
        size_t pos = 0;
        while (pos < h.size() && !std::isalpha(static_cast<unsigned char>(h[pos]))) ++pos;
        std::string h2 = (pos < h.size()) ? h.substr(pos) : h;

        // Allow only these constructs (checking start).
        if (h2.rfind("if", 0) == 0 ||
            h2.rfind("else", 0) == 0 ||
            h2.rfind("for", 0) == 0 ||
            h2.rfind("while", 0) == 0 ||
            h2.rfind("do", 0) == 0 ||
            h2.rfind("switch", 0) == 0 ||
            h2.rfind("case ", 0) == 0 ||
            h2.rfind("try", 0) == 0 ||
            h2.rfind("catch", 0) == 0) {
            allowed = false;
            break;
        }
    }

    if (!allowed) {
        p.body.push_back("// (" + tag + ") ERROR: 'return' not inside an allowed construct.");
        p.body.push_back("// Allowed only inside an if/else, a loop (for/while/do), switch/case, try or catch block.");
    } else {
        // Allowed — prompt ONCE for optional return expression.
        std::string expr = ask("[" + tag + "] Return expression (leave empty for a bare 'return;')", "");

        // Normalize: trim and drop a trailing semicolon if present.
        if (!expr.empty()) {
            expr = trim(expr);
            if (!expr.empty() && expr.back() == ';') expr.pop_back();
            expr = trim(expr);
        }

        p.body.push_back("// (" + tag + ") Inserted return");
        if (expr.empty()) p.body.push_back("return;");
        else p.body.push_back("return " + expr + ";");
    }

    return p;
}

static Parts handle_class_struct_union(Context &ctx, const string &kw, const string &tag) {
    Parts p;
    string name = ask("[" + tag + "] Name for " + kw, (kw == "union") ? "MyUnion" : "MyType");
    string members = ask("[" + tag + "] Comma-separated members (name:type)", "value:int");
    vector<string> mems = split_csv(members);
    ctx.types.insert(name);
    ctx.last_type = name;
    if (kw == "union") {
        std::ostringstream def;
        def << "union " << name << " {";
        for (auto &m : mems) {
            auto pos = m.find(':');
            string n = (pos == string::npos) ? m : m.substr(0,pos);
            string t = (pos == string::npos) ? "int" : m.substr(pos+1);
            def << "\n    " << t << " " << n << ";";
        }
        def << "\n};";
        p.top.push_back(def.str());
        string var = name + "_u";
        string decl = declare_variable(ctx, name, var, "{}");
        p.body.push_back("// (" + tag + ") Demonstrate union");
        p.body.push_back(decl);
        if (!mems.empty()) {
            auto pos = mems[0].find(':');
            string n = (pos == string::npos) ? mems[0] : mems[0].substr(0,pos);
            p.body.push_back(ctx.last_var + "." + n + " = 123;");
            p.body.push_back("cout << \"" + ctx.last_var + "." + n + " = \" << " + ctx.last_var + "." + n + " << endl;");
        }
        return p;
    } else {
        std::ostringstream def;
        def << kw << " " << name << " {\npublic:\n";
        for (auto &m : mems) {
            auto pos = m.find(':');
            string n = (pos == string::npos) ? m : m.substr(0,pos);
            string t = (pos == string::npos) ? "int" : m.substr(pos+1);
            def << "    " << t << " " << n << ";\n";
        }
        def << "    " << name << "(";
        bool first = true;
        for (auto &m : mems) {
            auto pos = m.find(':');
            string n = (pos == string::npos) ? m : m.substr(0,pos);
            string t = (pos == string::npos) ? "int" : m.substr(pos+1);
            if (!first) def << ", ";
            def << t << " " << n << "_";
            first = false;
        }
        def << ") : ";
        first = true;
        for (auto &m : mems) {
            auto pos = m.find(':');
            string n = (pos == string::npos) ? m : m.substr(0,pos);
            if (!first) def << ", ";
            def << n << "(" << n << "_)";
            first = false;
        }
        def << " {}\n};";
        p.top.push_back(def.str());
        std::ostringstream usage;
        usage << name << " obj(";
        bool first2 = true;
        for (auto &m : mems) {
            if (!first2) usage << ", ";
            auto pos = m.find(':');
            string t = (pos == string::npos) ? "int" : m.substr(pos+1);
            if (t == "string") usage << "\"hi\"";
            else if (t == "double") usage << "3.14";
            else usage << "0";
            first2 = false;
        }
        usage << ");";
        p.body.push_back("// (" + tag + ") Demonstrate " + kw);
        p.body.push_back(usage.str());
        if (!mems.empty()) {
            auto pos = mems[0].find(':');
            string n = (pos == string::npos) ? mems[0] : mems[0].substr(0,pos);
            p.body.push_back("cout << \"obj." + n + " = \" << obj." + n + " << endl;");
        }
        for (auto &m : mems) if (m.find("string") != string::npos) p.includes.push_back("string");
        return p;
    }
}

static Parts handle_enum(Context &ctx, const string &tag) {
    Parts p;
    string name = ask("[" + tag + "] Enum name", "Color");
    string items = ask("[" + tag + "] Comma-separated enumerators", "Red,Green,Blue");
    vector<string> enumerators = split_csv(items);
    ctx.types.insert(name);
    ctx.last_type = name;
    std::ostringstream def;
    def << "enum class " << name << " { ";
    for (size_t i = 0; i < enumerators.size(); ++i) {
        if (i) def << ", ";
        def << enumerators[i];
    }
    def << " };";
    p.top.push_back(def.str());
    p.body.push_back("// (" + tag + ") Demonstrate enum");
    p.body.push_back(name + " c = " + name + "::" + enumerators.front() + ";");
    p.body.push_back("cout << static_cast<int>(c) << endl;");
    return p;
}

static Parts handle_template(Context &ctx, const string &tag) {
    Parts p;
    string kind = ask("[" + tag + "] Template kind ('function' or 'class')", "function");
    if (kind == "class") {
        string name = ask("[" + tag + "] Template class name", "Box");
        string tparam = ask("[" + tag + "] Type parameter name", "T");
        std::ostringstream def;
        def << "template <typename " << tparam << ">\n";
        def << "struct " << name << " { " << tparam << " value; " << name << "(" << tparam << " v) : value(v) {} };";
        p.top.push_back(def.str());
        p.body.push_back("// (" + tag + ") Demonstrate class template");
        p.body.push_back(name + "<int> b(5);");
        p.body.push_back("cout << b.value << endl;");
        ctx.types.insert(name);
        ctx.last_type = name;
        return p;
    } else {
        string name = ask("[" + tag + "] Template function name", "add");
        string tparam = ask("[" + tag + "] Type parameter name", "T");
        std::ostringstream def;
        def << "template <typename " << tparam << ">\n" << tparam << " " << name << "(" << tparam << " a, " << tparam << " b) { return a + b; }";
        p.top.push_back(def.str());
        p.body.push_back("// (" + tag + ") Demonstrate function template");
        p.body.push_back("cout << " + name + "(2, 3) << endl;");
        return p;
    }
}

static Parts handle_cast(Context &ctx, const string &castkw, const string &tag) {
    Parts p;
    if (castkw == "static_cast") {
        string from = ask("[" + tag + "] Source expression (e.g., 3.14)", "3.14");
        string to = ask("[" + tag + "] Target type (e.g., int)", "int");
        p.body.push_back("// (" + tag + ") Demonstrate static_cast");
        p.body.push_back(to + " v = static_cast<" + to + ">(" + from + ");");
        p.body.push_back("cout << v << endl;");
        return p;
    } else if (castkw == "dynamic_cast") {
        p.top.push_back("struct Base { virtual ~Base() = default; }; ");
        p.top.push_back("struct Derived : Base { int x = 42; }; ");
        p.body.push_back("// (" + tag + ") Demonstrate dynamic_cast");
        p.body.push_back("Base* b = new Derived();");
        p.body.push_back("if (Derived* d = dynamic_cast<Derived*>(b)) {");
        p.body.push_back("    cout << \"dynamic_cast succeeded: \" << d->x << endl;");
        p.body.push_back("} else {");
        p.body.push_back("    cout << \"dynamic_cast failed\" << endl;");
        p.body.push_back("}");
        p.body.push_back("delete b;");
        return p;
    } else if (castkw == "const_cast") {
        p.body.push_back("// (" + tag + ") Demonstrate const_cast (illustrative)");
        p.body.push_back("const int ci = 10;");
        p.body.push_back("int &r = const_cast<int&>(ci);");
        p.body.push_back("r = 20; // undefined behavior but illustrative");
        p.body.push_back("cout << \"ci (after const_cast attempt) = \" << ci << endl;");
        return p;
    } else {
        p.body.push_back("// (" + tag + ") Demonstrate reinterpret_cast");
        p.body.push_back("int x = 0x12345678;");
        p.body.push_back("char* p = reinterpret_cast<char*>(&x);");
        p.body.push_back("cout << \"First byte (interpretation): \" << static_cast<int>(p[0]) << endl;");
        return p;
    }
}

static Parts handle_new_delete(Context &ctx, const string &tag) {
    Parts p;
    string typeName = ask("[" + tag + "] Type to allocate", "int");
    string init = ask("[" + tag + "] Initial value", "42");
    p.body.push_back("// (" + tag + ") Demonstrate new/delete");
    p.body.push_back(typeName + "* p = new " + typeName + "(" + init + ");");
    p.body.push_back("cout << \"*p = \" << *p << endl;");
    p.body.push_back("delete p;");
    return p;
}

static Parts handle_operator_keyword(Context &ctx, const string &tag) {
    Parts p;
    string op = ask("[" + tag + "] Operator to demonstrate/overload (e.g. +, <<)", "+");
    p.top.push_back("struct Point { int x, y; Point(int x_, int y_):x(x_),y(y_){} };");
    if (op == "+") {
        p.top.push_back("Point operator+(const Point& a, const Point& b) { return Point(a.x + b.x, a.y + b.y); }");
        p.body.push_back("// (" + tag + ") Demonstrate operator+");
        p.body.push_back("Point a(1,2), b(3,4);");
        p.body.push_back("Point c = a + b;");
        p.body.push_back("cout << \"c = (\" << c.x << \",\" << c.y << \")\" << endl;");
    } else if (op == "<<") {
        p.top.push_back("std::ostream& operator<<(std::ostream& os, const Point& p) { return os << '(' << p.x << ',' << p.y << ')'; }");
        p.body.push_back("// (" + tag + ") Demonstrate operator<<");
        p.body.push_back("Point a(1,2), b(3,4);");
        p.body.push_back("cout << a << \" \" << b << endl;");
    } else {
        p.top.push_back("// (" + tag + ") Operator not specially implemented; showing operator+ instead");
        p.top.push_back("Point operator+(const Point& a, const Point& b) { return Point(a.x + b.x, a.y + b.y); }");
        p.body.push_back("Point a(1,2), b(3,4);");
        p.body.push_back("Point c = a + b;");
        p.body.push_back("cout << \"c = (\" << c.x << \",\" << c.y << \")\" << endl;");
    }
    return p;
}

static Parts handle_try_catch_throw(Context &ctx, const string &tag) {
    Parts p;
    string msg = ask("[" + tag + "] Exception message to throw", "Something went wrong");
    p.body.push_back("// (" + tag + ") Demonstrate try/catch/throw");
    p.body.push_back("try {");
    p.body.push_back("    throw std::runtime_error(\"" + msg + "\");");
    p.body.push_back("} catch (const std::exception& e) {");
    p.body.push_back("    cout << \"Caught: \" << e.what() << endl;");
    p.body.push_back("}");
    p.includes.push_back("stdexcept");
    return p;
}

static Parts handle_constexpr(Context &ctx, const string &tag) {
    Parts p;
    string expr = ask("[" + tag + "] Provide either a constexpr function or a constant expression", "int square(int x){return x*x;}");
    if (expr.find('{') != string::npos) {
        p.top.push_back("constexpr " + expr);
        p.body.push_back("// (" + tag + ") Demonstrate constexpr function");
        p.body.push_back("cout << square(5) << endl;");
    } else {
        p.body.push_back("// (" + tag + ") Demonstrate constexpr value");
        p.body.push_back("constexpr auto v = " + expr + ";");
        p.body.push_back("cout << v << endl;");
    }
    return p;
}

static Parts handle_static_assert(Context &ctx, const string &tag) {
    Parts p;
    string cond = ask("[" + tag + "] Condition to assert at compile time", "sizeof(int) >= 4");
    string msg = ask("[" + tag + "] Message for static_assert", "int_size_ok");
    p.top.push_back("static_assert(" + cond + ", \"" + msg + "\");");
    p.body.push_back("// (" + tag + ") static_assert present above; runtime note:");
    p.body.push_back("cout << \"static_assert present; program compiled successfully\" << endl;");
    return p;
}

static Parts handle_alignas_alignof(Context &ctx, const string &tag) {
    Parts p;

    // Questions
    string struct_name = ask("[" + tag + "] Struct name", "Demo");
    string struct_align_s = ask("[" + tag + "] Struct alignment in bytes (positive integer)", "16");
    string fields_s = ask("[" + tag + "] Number of fields in struct", "5");

    // parse numeric answers safely
    int struct_align = 16;
    int nfields = 5;
    try { struct_align = std::stoi(struct_align_s); if (struct_align <= 0) struct_align = 16; } catch(...) {}
    try { nfields = std::stoi(fields_s); if (nfields < 0) nfields = 0; } catch(...) {}

    // typical sizes (x86_64 conventions)
    auto typical_size = [](const string &t)->int {
        if (t == "char") return 1;
        if (t == "short") return 2;
        if (t == "int") return 4;
        if (t == "long") return 8;
        if (t == "long long") return 8;
        if (t == "float") return 4;
        if (t == "double") return 8;
        if (t == "bool") return 1;
        // fallback: unknown types — use 0 to indicate unknown
        return 0;
    };

    // collect fields
    struct Field { string type; string name; int length; string align; };
    vector<Field> fields;
    for (int i = 0; i < nfields; ++i) {
        string idx = std::to_string(i+1);
        string typ = ask("[" + tag + "] Field #" + idx + " type", (i==0 ? "int" : (i==1 ? "int" : (i==2 ? "short" : (i==3 ? "char" : "char")))));
        string name = ask("[" + tag + "] Field #" + idx + " name", "var" + idx);
        string len_s = ask("[" + tag + "] Field #" + idx + " array length (0 = not an array)", "0");
        int len = 0;
        try { len = std::stoi(len_s); if (len < 0) len = 0; } catch(...) {}
        string falign = ask("[" + tag + "] Field #" + idx + " alignment in bytes (empty = none)", "");
        // normalize common synonyms
        if (typ == "signed char") typ = "char";
        if (typ == "unsigned char") typ = "char";
        fields.push_back({typ, name, len, falign});
    }

    // register last variable name in context for consistency with other handlers
    if (!fields.empty()) { ctx.vars[fields[0].name] = struct_name; ctx.last_var = fields[0].name; }

    // Build struct declaration lines (multi-line, commented)
    p.top.push_back(std::string("struct alignas(") + std::to_string(struct_align) + ") " + struct_name);
    p.top.push_back("{");

    for (auto &f : fields) {
        // declaration
        std::string decl = "    ";
        if (!f.align.empty()) decl += "alignas(" + f.align + ") ";
        decl += f.type + " " + f.name;
        if (f.length > 0) decl += "[" + std::to_string(f.length) + "]";
        decl += ";";

        // comment with typical size and alignment note (if provided)
        int tsize = typical_size(f.type);
        std::string comment;
        if (tsize > 0) {
            comment = " // " + std::to_string(tsize) + " bytes";
            if (f.length > 0) comment += " x " + std::to_string(f.length) + " elements";
        } else {
            comment = " // size: platform-dependent";
            if (f.length > 0) comment += " x " + std::to_string(f.length) + " elements";
        }
        if (!f.align.empty()) comment += "; aligned to " + f.align + " bytes";

        p.top.push_back(decl + comment);
    }

    p.top.push_back("");
    p.top.push_back("    // example: an aligned sub-object (member) with explicit alignment");
    // Optionally (nothing here) — we already allowed per-field alignas above.
    p.top.push_back("};");

    // Body: demonstration code
    // instance name
    string inst_name = ask("[" + tag + "] Instance name to create", "d");
    ctx.vars[inst_name] = struct_name;
    ctx.last_var = inst_name;

    p.body.push_back("// (" + tag + ") Demonstrate alignas/alignof for " + struct_name);
    p.body.push_back(struct_name + " " + inst_name + ";");
    p.body.push_back(std::string("cout << \"alignof(") + struct_name + ") = \" << alignof(" + struct_name + ") << endl;");
    p.body.push_back(std::string("cout << \"sizeof(") + struct_name + ") = \" << sizeof(" + struct_name + ") << endl;");
    p.body.push_back(std::string("cout << \"address of ") + inst_name + " = \" << (void*)&" + inst_name + " << endl;");
    p.body.push_back(std::string("cout << \"address mod ") + std::to_string(struct_align) + " = \" << (reinterpret_cast<uintptr_t>(&" + inst_name + ") % " + std::to_string(struct_align) + ") << endl;");

    // If there are array members we can also instantiate an array of structs and print element addresses
    string arr_count_s = ask("[" + tag + "] Create an array of instances? (enter count or 0 for single instance)", "3");
    int arr_count = 0;
    try { arr_count = std::stoi(arr_count_s); } catch(...) { arr_count = 0; }
    if (arr_count > 0) {
        p.body.push_back(struct_name + " arr_" + inst_name + "[" + std::to_string(arr_count) + "];");
        p.body.push_back(std::string("cout << \"alignof(") + struct_name + ") = \" << alignof(" + struct_name + ") << endl;");
        p.body.push_back(std::string("cout << \"sizeof(") + struct_name + ") = \" << sizeof(" + struct_name + ") << \", elements = " + std::to_string(arr_count) + "\" << endl;");
        for (int i = 0; i < arr_count; ++i) {
            p.body.push_back(std::string("cout << \"&arr_") + inst_name + "[" + std::to_string(i) + "] = \" << (void*)&arr_" + inst_name + "[" + std::to_string(i) + "]"
                                  " << \", addr mod " + std::to_string(struct_align) + " = \" << (reinterpret_cast<uintptr_t>(&arr_" + inst_name + "[" + std::to_string(i) + "]) % " + std::to_string(struct_align) + ") << endl;");
        }
        if (arr_count > 1) {
            p.body.push_back(std::string("cout << \"distance between element 0 and 1 = \" << (reinterpret_cast<uintptr_t>(&arr_") + inst_name + "[1]) - reinterpret_cast<uintptr_t>(&arr_" + inst_name + "[0]) << endl;");
        }
    }

    // final note comment in generated code (informational)
    p.body.push_back("// Note: sizes shown in comments are typical for x86_64 and may vary by platform/ABI.");

    return p;
}

static Parts handle_thread_local(Context &ctx, const string &tag) {
    Parts p;
    string name = ask("[" + tag + "] Thread-local variable name", "counter");
    string init = ask("[" + tag + "] Initial value", "0");
    p.top.push_back("thread_local int " + name + " = " + init + ";");
    p.body.push_back("// (" + tag + ") Demonstrate thread_local");
    p.body.push_back("cout << \"" + name + " = \" << " + name + " << endl;");
    return p;
}

static Parts handle_mutable(Context &ctx, const string &tag) {
    Parts p;
    string member = ask("[" + tag + "] Mutable member name", "cached");
    p.top.push_back("struct S { mutable int " + member + " = 0; int value = 0; int get() const { return " + member + " = value; } }; ");
    p.body.push_back("// (" + tag + ") Demonstrate mutable");
    p.body.push_back("S s{0, 7};");
    p.body.push_back("cout << \"get() = \" << s.get() << endl;");
    return p;
}

static Parts handle_sizeof_typeid(Context &ctx, const string &tag) {
    Parts p;
    string expr = ask("[" + tag + "] Expression or type to inspect", "int");
    p.body.push_back("// (" + tag + ") Demonstrate sizeof and typeid");
    p.body.push_back("cout << \"sizeof(" + expr + ") = \" << sizeof(" + expr + ") << endl;");
    p.body.push_back("cout << \"typeid(" + expr + ").name() = \" << typeid(" + expr + ").name() << endl;");
    p.includes.push_back("typeinfo");
    return p;
}

static Parts handle_alternative_tokens(Context &ctx, const string &kw, const string &tag) {
    Parts p;

    // helper: check if type is integral
    auto is_integral = [&](const string &t)->bool {
        static const vector<string> ints = {
            "char","signed char","unsigned char",
            "short","unsigned short",
            "int","unsigned int",
            "long","unsigned long",
            "long long","unsigned long long"
        };
        for (auto &s : ints) if (t == s) return true;
        return false;
    };

    // helper: ask for name + type but enforce "integral only"
    auto ask_integral = [&](const string &prefix, const string &def_name, const string &def_type){
        string name = ask("[" + tag + "] " + prefix + " name", def_name);
        string type;

        while (true) {
            type = ask("[" + tag + "] " + prefix + " type (integral only)", def_type);
            if (is_integral(type)) break;
            cout << "Type '" << type << "' is not integral. Allowed: int, long, short, char, unsigned..., etc.\n";
        }
        return std::pair<string,string>(type,name);
    };

    // utility: make a safe result variable name from components
    auto make_res_name = [&](const string &a, const string &b, const string &suffix) {
        return a + "_" + b + "_" + suffix;
    };
    auto make_res_name_unary = [&](const string &a, const string &suffix) {
        return a + "_" + suffix;
    };

    // boolean alternative tokens: and / or / not
    if (kw == "and" || kw == "or" || kw == "not") {
        // For logical demonstrations we use bool variables for operands.
        if (kw == "not") {
            string name = ask("[" + tag + "] Variable name", "x");
            string val  = ask("[" + tag + "] Initial boolean value (true/false)", "true");

            // define operand
            p.body.push_back("// (" + tag + ") Demonstrate 'not' (logical negation) with a variable");
            p.body.push_back("bool " + name + " = " + val + ";");
            // compute result into a variable
            string res = make_res_name_unary(name, "not_res");
            p.body.push_back("bool " + res + " = (not " + name + ");");
            p.body.push_back("cout << \"" + res + " = \" << (" + res + " ? \"true\" : \"false\") << endl;");
            // update context
            ctx.vars[name] = "bool";
            ctx.vars[res] = "bool";
            ctx.last_var = res;
            return p;
        } else {
            // and / or: two boolean operands
            string a_name = ask("[" + tag + "] Left operand name", "x");
            string a_val  = ask("[" + tag + "] Left operand initial boolean (true/false)", "true");
            string b_name = ask("[" + tag + "] Right operand name", "y");
            string b_val  = ask("[" + tag + "] Right operand initial boolean (true/false)", "false");

            p.body.push_back("// (" + tag + ") Demonstrate '" + kw + "' (logical) using two bool variables");
            p.body.push_back("bool " + a_name + " = " + a_val + ";");
            p.body.push_back("bool " + b_name + " = " + b_val + ";");

            // compute into a result variable
            string res = make_res_name(a_name, b_name, "logic_res");
            string op = (kw == "and") ? "and" : "or";
            p.body.push_back("bool " + res + " = (" + a_name + " " + op + " " + b_name + ");");
            p.body.push_back("cout << \"" + res + " = \" << (" + res + " ? \"true\" : \"false\") << endl;");
            // update context
            ctx.vars[a_name] = "bool";
            ctx.vars[b_name] = "bool";
            ctx.vars[res] = "bool";
            ctx.last_var = res;
            return p;
        }
    }

    // bitwise binaries: xor, bitand, bitor
    if (kw == "xor" || kw == "bitand" || kw == "bitor") {

        auto L = ask_integral("Left operand", "a", "int");
        auto R = ask_integral("Right operand", "b", "int");

        string a_type = L.first, a_name = L.second;
        string b_type = R.first, b_name = R.second;

        string a_val = ask("[" + tag + "] Left operand initial value", "5");
        string b_val = ask("[" + tag + "] Right operand initial value", "3");

        // ensure variables exist
        p.body.push_back("// (" + tag + ") Demonstrate alternative token '" + kw + "'");
        p.body.push_back(a_type + " " + a_name + " = " + a_val + ";");
        p.body.push_back(b_type + " " + b_name + " = " + b_val + ";");

        // compute result into a named variable and print both the alternative-token expression and the symbolic expression
        string res = make_res_name(a_name, b_name, "res");
        // result type is same as operands
        p.body.push_back(a_type + " " + res + " = (" + a_name + " " + (kw == "xor" ? "^" : (kw=="bitand" ? "&" : "|")) + " " + b_name + ");");

        // print: first the alternative token form, then symbolic form (both refer to the computed result)
        p.body.push_back("cout << \"" + a_name + " " + kw + " " + b_name + " = \" << " + res + " << endl;");
        p.body.push_back("cout << \"" + a_name + " " + (kw=="xor" ? "^" : (kw=="bitand" ? "&" : "|")) + " " + b_name +
                         " (symbol) = \" << " + res + " << endl;");

        ctx.vars[a_name] = a_type;
        ctx.vars[b_name] = b_type;
        ctx.vars[res] = a_type;
        ctx.last_var = res;
        return p;
    }

    // unary: compl
    if (kw == "compl") {
        auto V = ask_integral("Variable", "x", "int");
        string t = V.first, v = V.second;
        string val = ask("[" + tag + "] Initial value", "42");

        p.body.push_back("// (" + tag + ") Demonstrate 'compl' and '~' with a variable");
        p.body.push_back(t + " " + v + " = " + val + ";");

        // compute complemented result into named variable
        string inv = make_res_name_unary(v, "compl_res");
        p.body.push_back(t + " " + inv + " = (compl " + v + ");");
        p.body.push_back("cout << \"" + inv + " (compl " + v + ") = \" << " + inv + " << endl;");
        // show symbolic ~ form too but using same computed value
        p.body.push_back("cout << \"~" + v + " = \" << " + inv + " << endl;");

        ctx.vars[v] = t;
        ctx.vars[inv] = t;
        ctx.last_var = inv;
        return p;
    }

    // not_eq: inequality (alternative token for '!=')
    if (kw == "not_eq") {
        auto L = ask_integral("Left operand", "x", "int");
        string t = L.first, left = L.second;

        string right_val = ask("[" + tag + "] Right operand/value", "0");
        // ensure left variable exists
        p.body.push_back("// (" + tag + ") Demonstrate 'not_eq' (inequality) with a computed bool result");
        p.body.push_back(t + " " + left + " = 1; // example");
        // compute comparison result into a bool variable
        string cmp = make_res_name_unary(left, "not_eq_res");
        p.body.push_back("bool " + cmp + " = (" + left + " not_eq " + right_val + ");");
        p.body.push_back("cout << \"" + cmp + " ( " + left + " not_eq " + right_val + ") = \" << (" + cmp + " ? \"true\" : \"false\") << endl;");

        ctx.vars[left] = t;
        ctx.vars[cmp] = "bool";
        ctx.last_var = cmp;
        return p;
    }

    // compound: and_eq, or_eq, xor_eq
    if (kw == "and_eq" || kw == "or_eq" || kw == "xor_eq") {
        auto V = ask_integral("Variable to modify", "v", "int");
        string t = V.first, v = V.second;

        string val = ask("[" + tag + "] Initial value", "15");
        string rhs = ask("[" + tag + "] RHS value", "6");

        p.body.push_back("// (" + tag + ") Demonstrate '" + kw + "' with before/after variables");
        // create explicit before variable so every value has a variable
        string before = make_res_name_unary(v, "before");
        p.body.push_back(t + " " + before + " = " + val + ";");
        p.body.push_back(t + " " + v + " = " + before + ";");
        // compute RHS into its own variable
        string rhs_name = make_res_name_unary(v, "rhs");
        p.body.push_back(t + " " + rhs_name + " = " + rhs + ";");

        // print before
        p.body.push_back("cout << \"before: " + before + " = \" << " + before + " << endl;");

        // perform the compound operation on v (which was initialized from before)
        string sym = (kw=="and_eq") ? "&=" : (kw=="or_eq" ? "|=" : "^=");
        p.body.push_back(v + " " + kw + " " + rhs_name + ";");

        // assign after to an explicit after variable
        string after = make_res_name_unary(v, "after");
        p.body.push_back(t + " " + after + " = " + v + ";");

        // print after using the after variable
        p.body.push_back(std::string("cout << \"after (" + v + " " + sym + " " + rhs_name + "): \" << ") + after + " << endl;");

        // update context
        ctx.vars[before] = t;
        ctx.vars[v] = t;
        ctx.vars[rhs_name] = t;
        ctx.vars[after] = t;
        ctx.last_var = after;
        return p;
    }

    // fallback
    p.body.push_back("// (" + tag + ") Unknown alternative token");
    p.body.push_back(std::string("cout << \"Alternative token: ") + kw + "\" << endl;");
    return p;
}

static Parts handle_generic_with_body(Context &ctx, const string &kw, const string &tag) {
    Parts p;
    cout << "[" << tag << "] No tailored snippet for '" << kw << "'. Please paste a small code fragment." << endl;
    vector<string> lines = read_multiline_body("Finish the fragment with a single '.' on its own line:");
    bool has_main = false;
    for (auto &l : lines) if (l.find("int main(") != string::npos) { has_main = true; break; }
    if (has_main) {
        std::ostringstream ss;
        for (auto &l : lines) ss << l << "\n";
        p.top.push_back(ss.str());
        p.body.push_back("// (" + tag + ") User provided a full program above; no extra main content added.");
    } else {
        for (auto &l : lines) p.body.push_back(l);
    }
    return p;
}

// -------------------- Additional handlers for previously unmapped keywords --------------------

static Parts handle_extern(Context &ctx, const string &tag) {
    Parts p;
    string decl = ask("[" + tag + "] Declaration to treat as 'extern' (e.g. int x)", "int external_value");
    p.top.push_back("extern " + decl + ";");
    p.body.push_back("// (" + tag + ") Demonstrate extern declaration above; at runtime we just note it.");
    p.body.push_back("cout << \"extern declaration inserted: \" << \"" + decl + "\" << endl;");
    return p;
}

static Parts handle_inline(Context &ctx, const string &tag) {
    Parts p;
    string sig = ask("[" + tag + "] Inline function signature (without body)", "int foo()");
    string body = ask("[" + tag + "] Inline function body single statement", "return 42;");
    p.top.push_back("inline " + sig + " { " + body + " }");
    p.body.push_back("// (" + tag + ") Demonstrate inline function above and call it:");
    // attempt to derive a function name
    string fname = sig;
    size_t pos = fname.find('(');
    if (pos != string::npos) fname = fname.substr(0, pos);
    size_t sp = fname.find_last_of(' ');
    if (sp != string::npos) fname = fname.substr(sp+1);
    p.body.push_back("cout << " + fname + "() << endl;");
    return p;
}

static Parts handle_register(Context &ctx, const string &tag) {
    Parts p;
    string decl = ask("[" + tag + "] Variable declaration using 'register' (e.g. int i = 0)", "int i = 0");
    p.body.push_back("// (" + tag + ") Demonstrate register (historical, may be ignored by modern compilers)");
    p.body.push_back("register " + decl + ";");
    // record variable if possible
    std::istringstream iss(decl);
    string t, n;
    if (iss >> t >> n) {
        size_t eq = n.find('=');
        string varname = (eq==string::npos) ? n : n.substr(0, eq);
        varname = normalize_token(varname);
        if (!varname.empty()) { ctx.vars[varname] = t; ctx.last_var = varname; }
    }
    p.body.push_back("cout << \"register var processed.\" << endl;");
    return p;
}

static Parts handle_asm(Context &ctx, const string &tag) {
    Parts p;
    string code = ask("[" + tag + "] Inline assembly snippet (single string)", "\"nop\"");
    p.body.push_back("// (" + tag + ") Demonstrate asm (platform dependent; illustrative)");
    p.body.push_back("asm(" + code + ");");
    p.body.push_back("cout << \"Inserted asm snippet.\" << endl;");
    return p;
}

static Parts handle_goto(Context &ctx, const string &tag) {
    Parts p;
    string label = ask("[" + tag + "] Label name to create/jump to", "L1");
    p.body.push_back("// (" + tag + ") Demonstrate goto (use sparingly)");
    p.body.push_back(label + ": ;");
    p.body.push_back("goto " + label + ";");
    p.body.push_back("cout << \"Performed goto to label " + label + "\" << endl;");
    return p;
}

static Parts handle_break_continue(Context &ctx, const string &kw, const string &tag) {
    Parts p;

    // Basic loop parameters
    string loop_type = ask("[" + tag + "] Loop type to demonstrate (for / while / do-while)", "for");
    string start = ask("[" + tag + "] Start index (integer)", "0");
    string step  = ask("[" + tag + "] Step (increment, integer)", "1");
    string iterations = ask("[" + tag + "] Number of iterations to demonstrate", "5");
    string trigger = ask("[" + tag + "] Iteration index that triggers '" + kw + "' (integer)", "2");
    string print_before = ask("[" + tag + "] Execute user body before the trigger check? (y/n)", "y");
    string custom_msg = ask("[" + tag + "] Message to print when '" + kw + "' occurs (empty = default)", "");
    if (custom_msg.empty()) custom_msg = (kw == "break") ? ("Breaking at i=" + trigger) : ("Continuing at i=" + trigger);

    // Read user-supplied loop content (multiline). Signature: vector<string> read_multiline_body(const string &)
    vector<string> user_lines = read_multiline_body("[" + tag + "] Enter loop body lines (use {i} for index); finish with a single '.' line");

    // If user provided no lines, supply a sensible default
    if (user_lines.empty()) user_lines.push_back("cout << i << endl;");

    // Detect if user body already contains 'break' or 'continue' to avoid duplicate automatic insertion
    bool user_has_control = false;
    for (const auto &ln : user_lines) {
        if (ln.find("break") != string::npos || ln.find("continue") != string::npos) {
            user_has_control = true;
            break;
        }
    }

    // Helpers
    auto replace_all = [](string s, const string &from, const string &to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != string::npos) {
            s.replace(pos, from.length(), to);
            pos += to.length();
        }
        return s;
    };

    auto emit_user_body = [&](Parts &out) {
        for (const auto &ln : user_lines) {
            string replaced = replace_all(ln, "{i}", "i");
            out.body.push_back("    " + replaced);
        }
    };

    // Header
    p.body.push_back("// (" + tag + ") Demonstrate '" + kw + "' inside a " + loop_type + " loop");

    string end_expr = "(" + start + " + " + iterations + ")";
    bool check_before = (!print_before.empty() && (print_before[0] == 'y' || print_before[0] == 'Y'));

    // Emit loops; if user already included control, do not auto-insert trigger check
    auto emit_trigger_stmt = [&](Parts &out) {
        if (user_has_control) return; // user supplied their own break/continue
        if (check_before) {
            if (kw == "break") out.body.push_back("    if (i == " + trigger + ") { cout << \"" + custom_msg + "\" << endl; break; }");
            else               out.body.push_back("    if (i == " + trigger + ") { cout << \"" + custom_msg + "\" << endl; continue; }");
        } else {
            // when printing after, trigger is emitted after user body — handled by callers
        }
    };

    if (loop_type == "for" || loop_type == "For" || loop_type == "FOR") {
        p.body.push_back("for (int i = " + start + "; i < " + end_expr + "; i += " + step + ") {");
        if (check_before) {
            emit_trigger_stmt(p);
            emit_user_body(p);
        } else {
            emit_user_body(p);
            if (!user_has_control) {
                if (kw == "break") p.body.push_back("    if (i == " + trigger + ") { cout << \"" + custom_msg + "\" << endl; break; }");
                else               p.body.push_back("    if (i == " + trigger + ") { cout << \"" + custom_msg + "\" << endl; continue; }");
            }
        }
        p.body.push_back("}");
    }
    else if (loop_type == "while" || loop_type == "While" || loop_type == "WHILE") {
        p.body.push_back("int i = " + start + ";");
        p.body.push_back("while (i < " + end_expr + ") {");
        if (check_before) {
            emit_trigger_stmt(p);
            emit_user_body(p);
        } else {
            emit_user_body(p);
            if (!user_has_control) {
                if (kw == "break") p.body.push_back("    if (i == " + trigger + ") { cout << \"" + custom_msg + "\" << endl; break; }");
                else               p.body.push_back("    if (i == " + trigger + ") { cout << \"" + custom_msg + "\" << endl; i += " + step + "; continue; }");
            }
        }
        p.body.push_back("    i += " + step + ";");
        p.body.push_back("}");
    }
    else if (loop_type == "do-while" || loop_type == "Do-while" || loop_type == "DO-WHILE") {
        p.body.push_back("int i = " + start + ";");
        p.body.push_back("do {");
        if (check_before) {
            emit_trigger_stmt(p);
            emit_user_body(p);
        } else {
            emit_user_body(p);
            if (!user_has_control) {
                if (kw == "break") p.body.push_back("    if (i == " + trigger + ") { cout << \"" + custom_msg + "\" << endl; break; }");
                else               p.body.push_back("    if (i == " + trigger + ") { cout << \"" + custom_msg + "\" << endl; i += " + step + "; continue; }");
            }
        }
        p.body.push_back("    i += " + step + ";");
        p.body.push_back("} while (i < " + end_expr + ");");
    }
    else {
        // fallback to simple for-loop
        p.body.push_back("// Unrecognized loop type; falling back to for-loop demonstration");
        p.body.push_back("for (int i = " + start + "; i < " + end_expr + "; ++i) {");
        if (check_before) {
            emit_trigger_stmt(p);
            emit_user_body(p);
        } else {
            emit_user_body(p);
            if (!user_has_control) {
                if (kw == "break") p.body.push_back("    if (i == " + trigger + ") { cout << \"" + custom_msg + "\" << endl; break; }");
                else               p.body.push_back("    if (i == " + trigger + ") { cout << \"" + custom_msg + "\" << endl; continue; }");
            }
        }
        p.body.push_back("}");
    }

    return p;
}

static Parts handle_export(Context &ctx, const string &tag) {
    Parts p;
    p.body.push_back("// (" + tag + ") 'export' keyword is largely historical in header/module contexts; illustrative only");
    p.body.push_back("cout << \"export (illustrative)\" << endl;");
    return p;
}

// -------------------- New handlers for requested standard keywords --------------------

static Parts handle_const(Context &ctx, const string &tag) {
    Parts p;
    string type = ask("[" + tag + "] Type for const variable", "int");
    string name = ask("[" + tag + "] Name for const variable", "kValue");
    string val = ask("[" + tag + "] Initial value for " + name, "100");
    p.body.push_back("// (" + tag + ") Declare and use a meaningful const variable");
    p.body.push_back("const " + type + " " + name + " = " + val + ";");
    p.body.push_back("cout << \"" + name + " = \" << " + name + " << endl;");
    return p;
}

static Parts handle_decltype(Context &ctx, const string &tag) {
    Parts p;
    string expr = ask("[" + tag + "] An expression to inspect with decltype", "42");
    string name = ask("[" + tag + "] Variable name to declare with decltype", "y");
    p.body.push_back("// (" + tag + ") Use decltype to deduce the type of an expression and declare a variable");
    p.body.push_back("decltype(" + expr + ") " + name + " = " + expr + ";");
    p.body.push_back("cout << \"declared var '" + name + "' = \" << " + name + " << endl;");
    return p;
}

static Parts handle_explicit(Context &ctx, const string &tag) {
    Parts p;
    string cls = ask("[" + tag + "] Class name to create with explicit constructor", "Number");
    p.top.push_back("struct " + cls + " { int v; explicit " + cls + "(int x):v(x){} int get() const { return v; } }; ");
    p.body.push_back("// (" + tag + ") Use explicit constructor to avoid implicit conversions; construct explicitly");
    p.body.push_back(cls + " n(" + ask("[" + tag + "] Constructor argument for " + cls, "7") + ");");
    p.body.push_back("cout << \"" + cls + "::get() = \" << n.get() << endl;");
    return p;
}

static Parts handle_bool_literal(Context &ctx, const string &kw, const string &tag) {
    Parts p;
    string name = ask("[" + tag + "] Name for bool variable", "flag");
    string val = (kw == "true") ? "true" : "false";
    p.body.push_back("// (" + tag + ") Demonstrate boolean literal '" + val + "' stored and checked meaningfully");
    p.body.push_back("bool " + name + " = " + val + ";");
    p.body.push_back("if (" + name + ") cout << \"" + name + " is true\" << endl; else cout << \"" + name + " is false\" << endl;");
    return p;
}

static Parts handle_friend(Context &ctx, const string &tag) {
    Parts p;
    string cls = ask("[" + tag + "] Class name to create with a friend accessor", "Box");
    p.top.push_back("struct " + cls + " { private: int secret = 99; public: friend int reveal(const " + cls + "& b); };");
    p.top.push_back("int reveal(const " + cls + "& b) { return b.secret; }");
    p.body.push_back("// (" + tag + ") Use friend function to access private member meaningfully");
    p.body.push_back(cls + " b;" );
    p.body.push_back("cout << \"friend reveal = \" << reveal(b) << endl;");
    return p;
}

static Parts handle_namespace(Context &ctx, const string &tag) {
    Parts p;
    string ns = ask("[" + tag + "] Namespace name to create", "myns");
    string fname = ask("[" + tag + "] Function name inside namespace", "answer");
    string ret = ask("[" + tag + "] Integer result the function should return", "123");
    std::ostringstream ss;
    ss << "namespace " << ns << " { int " << fname << "() { return " << ret << "; } }";
    p.top.push_back(ss.str());
    p.body.push_back("// (" + tag + ") Call a namespaced function and use its result meaningfully");
    p.body.push_back("cout << \"namespace::function() = \" << " + ns + "::" + fname + "() << endl;");
    return p;
}

static Parts handle_noexcept(Context &ctx, const string &tag) {
    Parts p;
    string fname = ask("[" + tag + "] Name for noexcept function", "safe_func");
    string ret = ask("[" + tag + "] Integer value to return from function", "7");
    p.top.push_back("int " + fname + "() noexcept { return " + ret + "; }");
    p.body.push_back("// (" + tag + ") Call noexcept function and use result");
    p.body.push_back("cout << \"noexcept result = \" << " + fname + "() << endl;");
    return p;
}

static Parts handle_nullptr(Context &ctx, const string &tag) {
    Parts p;
    string type = ask("[" + tag + "] Pointer type to demonstrate (e.g. int)", "int");
    p.body.push_back("// (" + tag + ") Demonstrate nullptr usage and safe check before dereference");
    p.body.push_back(type + "* p = nullptr;");
    p.body.push_back("if (p == nullptr) { cout << \"pointer is nullptr, allocating and assigning\" << endl; p = new " + type + "(42); cout << *p << endl; delete p; } else cout << *p << endl;");
    return p;
}

static Parts handle_access_specifiers(Context &ctx, const string &kw, const string &tag) {
    Parts p;
    string cls = ask("[" + tag + "] Class name to create", "C");
    std::ostringstream def;
    def << "struct " << cls << " {\n"
        << "private:\n"
        << "    int priv = 1;\n"
        << "protected:\n"
        << "    int prot = 2;\n"
        << "public:\n"
        << "    int pub = 3;\n"
        << "    int get_priv() const { return priv; }\n"
        << "    int get_prot() const { return prot; }\n"
        << "};";
    p.top.push_back(def.str());
    p.body.push_back("// (" + tag + ") Use accessors to read private/protected/public members meaningfully");
    p.body.push_back(cls + " o;");
    // Single valid C++ statement string
    p.body.push_back("cout << \"pub=\" << o.pub << \", priv=\" << o.get_priv() << \", prot=\" << o.get_prot() << endl;");
    return p;
}

static Parts handle_static(Context &ctx, const string &tag) {
    Parts p;
    string fname = ask("[" + tag + "] Function name to hold a static counter", "counter_func");
    p.top.push_back("int " + fname + "() { static int cnt = 0; return ++cnt; }");
    p.body.push_back("// (" + tag + ") Demonstrate static local lifetime across calls");
    p.body.push_back("cout << \"call1=\" << " + fname + "() << \", call2=\" << " + fname + "() << endl;");
    return p;
}

static Parts handle_this(Context &ctx, const string &tag) {
    Parts p;
    string cls = ask("[" + tag + "] Class name to create that uses this", "Thing");
    p.top.push_back("struct " + cls + " { int v = 0; void set(int x) { this->v = x; } int get() const { return v; } }; ");
    p.body.push_back("// (" + tag + ") Use this-> to refer to members inside methods and show effect");
    p.body.push_back(cls + " t; t.set(" + ask("[" + tag + "] Value to set via this->", "9") + ");");
    p.body.push_back("cout << \"this-> set value = \" << t.get() << endl;");
    return p;
}

static Parts handle_typedef_typename(Context &ctx, const string &kw, const string &tag) {
    Parts p;
    if (kw == "typedef") {
        string orig = ask("[" + tag + "] Original type to alias", "long");
        string alias = ask("[" + tag + "] Alias name", "LInt");
        p.top.push_back("typedef " + orig + " " + alias + ";");
        p.body.push_back("// (" + tag + ") Use typedef alias to declare a variable meaningfully");
        p.body.push_back(alias + " v = 123456789L; cout << v << endl;");
    } else {
        string tparam = ask("[" + tag + "] Template parameter type to use with typename (e.g. T)", "T");
        std::ostringstream def;
        def << "template <typename " << tparam << ">\nstruct Holder { " << tparam << " value; Holder(" << tparam << " v):value(v){} };";
        p.top.push_back(def.str());
        p.body.push_back("// (" + tag + ") Use typename in a template context: instantiate Holder<int>");
        p.body.push_back("Holder<int> h(5); cout << h.value << endl;");
    }
    return p;
}

static Parts handle_using(Context &ctx, const string &tag) {
    Parts p;
    string kind = ask("[" + tag + "] 'alias' or 'directive'?", "alias");
    if (kind == "directive") {
        string ns = ask("[" + tag + "] Namespace to bring in (e.g. std)", "std");
        p.body.push_back("// (" + tag + ") Demonstrate using-directive (note: program already uses namespace std globally)");
        p.body.push_back("cout << \"using directive for namespace " + ns + " noted.\" << endl;");
    } else {
        string orig = ask("[" + tag + "] Original type to alias (e.g. std::string)", "std::string");
        string alias = ask("[" + tag + "] Alias name", "Str");
        p.top.push_back("using " + alias + " = " + orig + ";");
        p.body.push_back("// (" + tag + ") Use alias in main meaningfully");
        p.body.push_back(alias + " s = \"hi\"; cout << s << endl;");
    }
    return p;
}

static Parts handle_virtual(Context &ctx, const string &tag) {
    Parts p;
    p.top.push_back("struct BaseV { virtual ~BaseV() = default; virtual int id() const { return 1; } }; ");
    p.top.push_back("struct DerivedV : BaseV { int id() const override { return 2; } }; ");
    p.body.push_back("// (" + tag + ") Demonstrate virtual dispatch via base pointer to derived instance");
    p.body.push_back("BaseV* b = new DerivedV(); cout << \"virtual id=\" << b->id() << endl; delete b;");
    return p;
}

static Parts handle_void(Context &ctx, const string &tag) {
    Parts p;
    string fname = ask("[" + tag + "] Function name that returns void", "doit");
    string stmt = ask("[" + tag + "] Statement inside the void function (single)", "cout << \"did it\" << endl;");
    p.top.push_back("void " + fname + "() { " + stmt + " }");
    p.body.push_back("// (" + tag + ") Call void function for its side-effect");
    p.body.push_back(fname + "();");
    return p;
}

static Parts handle_volatile(Context &ctx, const string &tag) {
    Parts p;
    string type = ask("[" + tag + "] Type to declare volatile variable (e.g. int)", "int");
    p.body.push_back("// (" + tag + ") Demonstrate volatile qualification for a variable that may change externally");
    p.body.push_back("volatile " + type + " v = 0; cout << \"volatile v initial=\" << v << endl; v = 1; cout << \"volatile v after change=\" << v << endl;");
    return p;
}

// --- Helper: parse param list entered as "name=default, other=val" into vector<pair>
static std::vector<std::pair<std::string,std::string>> parse_param_list(const std::string &in) {
    std::vector<std::pair<std::string,std::string>> out;
    std::string cur;
    size_t i = 0, n = in.size();
    while (i < n) {
        // collect until comma
        cur.clear();
        while (i < n && in[i] != ',') {
            cur.push_back(in[i]);
            i++;
        }
        // skip comma
        if (i < n && in[i] == ',') i++;
        // trim spaces
        auto trim = [](std::string &st) {
            size_t b = 0; while (b < st.size() && isspace((unsigned char)st[b])) b++;
            size_t e = st.size();
            while (e>b && isspace((unsigned char)st[e-1])) e--;
            st = st.substr(b, e-b);
        };
        trim(cur);
        if (cur.empty()) continue;
        size_t eq = cur.find('=');
        if (eq == std::string::npos) {
            out.emplace_back(cur, std::string{});
        } else {
            std::string name = cur.substr(0, eq);
            std::string def  = cur.substr(eq+1);
            trim(name); trim(def);
            out.emplace_back(name, def);
        }
    }
    return out;
}

// -------------------- Dispatcher per occurrence, updated to support user keywords with params ------

// NOTE: this variant:
//  - preserves exact token order across lines,
//  - ignores tokens inside quotes/comments,
//  - substitutes nested bodies inline at the token position,
//  - prompts to define previously-undefined unquoted tokens (one prompt per unique token per top-level expansion),
//  - detects recursion and avoids cycles.
// Uses UserKeywordMap (alias to unordered_map) for user_keywords.
static Parts generate_parts_for_keyword_occurrence(const std::string &kw,
                                                   Context &ctx,
                                                   int occurrence_index,
                                                   int token_pos_in_input,
                                                   UserKeywordMap &user_keywords,
                                                   std::unordered_set<std::string> *active = nullptr) {
    std::ostringstream t;
    t << "occurrence " << occurrence_index << " (token " << token_pos_in_input << ")";
    std::string tag = t.str();

    // Prepare active recursion tracking set
    std::unordered_set<std::string> local_active;
    std::unordered_set<std::string> *active_ptr = active ? active : &local_active;

    // Cycle detection
    if (active_ptr->count(kw)) {
        std::cout << "[" << tag << "] Detected recursive keyword reference for '" << kw
                  << "'. Skipping nested expansion to avoid infinite recursion.\n";
        return handle_generic_with_body(ctx, kw, tag);
    }
    active_ptr->insert(kw);

    // 1) If user-defined: ask for its parameters and generate its raw parts (placeholders substituted)
    auto uit = user_keywords.find(kw);
    Parts p;
    if (uit != user_keywords.end()) {
        const UserKeyword &uk = uit->second;
        std::map<std::string,std::string> values;
        for (const auto &pp : uk.params) {
            const std::string &pname = pp.first;
            const std::string &pdef  = pp.second;
            std::string val = ask("[" + tag + "] Value for parameter '" + pname + "'", pdef);
            values[pname] = val;
        }
        p = parts_from_user_snippet_with_params(uk, values, tag);
    }

    // If not user-defined, handle builtins
    if (uit == user_keywords.end()) {
        // handle built-in keywords (same as previous function) — keep exhaustive list
        const std::string k = kw;
        if (k == "int" || k == "double" || k == "float" || k == "char" ||
            k == "long" || k == "short" || k == "signed" || k == "unsigned" ||
            k == "bool" || k == "wchar_t" || k == "char16_t" || k == "char32_t")
            { active_ptr->erase(kw); return handle_type_like(ctx, k, tag); }
        if (k == "auto") { active_ptr->erase(kw); return handle_auto(ctx, tag); }
        if (k == "if" || k == "else") { active_ptr->erase(kw); return handle_if_else(ctx, tag); }
        if (k == "for") { active_ptr->erase(kw); return handle_for(ctx, tag); }
        if (k == "while") { active_ptr->erase(kw); return handle_while(ctx, tag); }
        if (k == "do") { active_ptr->erase(kw); return handle_do(ctx, tag); }
        if (k == "switch" || k == "case") { active_ptr->erase(kw); return handle_switch(ctx, tag); }
        if (k == "return") { active_ptr->erase(kw); return handle_return(ctx, tag); }
        if (k == "class" || k == "struct" || k == "union") { active_ptr->erase(kw); return handle_class_struct_union(ctx, k, tag); }
        if (k == "enum") { active_ptr->erase(kw); return handle_enum(ctx, tag); }
        if (k == "template") { active_ptr->erase(kw); return handle_template(ctx, tag); }
        if (k == "static_cast" || k == "dynamic_cast" || k == "const_cast" || k == "reinterpret_cast") { active_ptr->erase(kw); return handle_cast(ctx, k, tag); }
        if (k == "new" || k == "delete") { active_ptr->erase(kw); return handle_new_delete(ctx, tag); }
        if (k == "operator") { active_ptr->erase(kw); return handle_operator_keyword(ctx, tag); }
        if (k == "try" || k == "catch" || k == "throw") { active_ptr->erase(kw); return handle_try_catch_throw(ctx, tag); }
        if (k == "constexpr") { active_ptr->erase(kw); return handle_constexpr(ctx, tag); }
        if (k == "static_assert") { active_ptr->erase(kw); return handle_static_assert(ctx, tag); }
        if (k == "alignas" || k == "alignof") { active_ptr->erase(kw); return handle_alignas_alignof(ctx, tag); }
        if (k == "thread_local") { active_ptr->erase(kw); return handle_thread_local(ctx, tag); }
        if (k == "mutable") { active_ptr->erase(kw); return handle_mutable(ctx, tag); }
        if (k == "sizeof" || k == "typeid") { active_ptr->erase(kw); return handle_sizeof_typeid(ctx, tag); }
        if (k == "and" || k == "or" || k == "not" || k == "xor" ||
            k == "bitand" || k == "bitor" || k == "compl" || k == "not_eq" || k == "and_eq" || k == "or_eq" || k == "xor_eq")
            { active_ptr->erase(kw); return handle_alternative_tokens(ctx, k, tag); }
        if (k == "extern") { active_ptr->erase(kw); return handle_extern(ctx, tag); }
        if (k == "inline") { active_ptr->erase(kw); return handle_inline(ctx, tag); }
        if (k == "register") { active_ptr->erase(kw); return handle_register(ctx, tag); }
        if (k == "asm") { active_ptr->erase(kw); return handle_asm(ctx, tag); }
        if (k == "goto") { active_ptr->erase(kw); return handle_goto(ctx, tag); }
        if (k == "export") { active_ptr->erase(kw); return handle_export(ctx, tag); }
        if (k == "const") { active_ptr->erase(kw); return handle_const(ctx, tag); }
        if (k == "decltype") { active_ptr->erase(kw); return handle_decltype(ctx, tag); }
        if (k == "explicit") { active_ptr->erase(kw); return handle_explicit(ctx, tag); }
        if (k == "true" || kw == "false") { active_ptr->erase(kw); return handle_bool_literal(ctx, kw, tag); }
        if (k == "friend") { active_ptr->erase(kw); return handle_friend(ctx, tag); }
        if (k == "break" || k == "continue") { active_ptr->erase(kw); return handle_break_continue(ctx, k, tag); }
        if (k == "namespace") { active_ptr->erase(kw); return handle_namespace(ctx, tag); }
        if (k == "noexcept") { active_ptr->erase(kw); return handle_noexcept(ctx, tag); }
        if (k == "nullptr") { active_ptr->erase(kw); return handle_nullptr(ctx, tag); }
        if (k == "private" || k == "protected" || k == "public") { active_ptr->erase(kw); return handle_access_specifiers(ctx, k, tag); }
        if (k == "static") { active_ptr->erase(kw); return handle_static(ctx, tag); }
        if (k == "this") { active_ptr->erase(kw); return handle_this(ctx, tag); }
        if (k == "typedef" || k == "typename") { active_ptr->erase(kw); return handle_typedef_typename(ctx, k, tag); }
        if (k == "using") { active_ptr->erase(kw); return handle_using(ctx, tag); }
        if (k == "virtual") { active_ptr->erase(kw); return handle_virtual(ctx, tag); }
        if (k == "void") { active_ptr->erase(kw); return handle_void(ctx, tag); }
        if (k == "volatile") { active_ptr->erase(kw); return handle_volatile(ctx, tag); }

        // fallback for unknown builtins
        active_ptr->erase(kw);
        return handle_generic_with_body(ctx, kw, tag);
    }

    // At this point: p contains the top-level user-defined snippet (placeholder substituted).
    // We'll perform inline expansion: iterate original p.body lines left-to-right and replace tokens in-place
    // with their expansions. We will also prompt to define unknown unquoted tokens.

    // Keep a set of tokens we already processed (so we only prompt/expand a unique token once per top-level expansion).
    std::unordered_set<std::string> processed_tokens;
    // For merging includes while preserving order of discovery:
    auto append_include_if_new = [&](const std::string &inc) {
        if (std::find(p.includes.begin(), p.includes.end(), inc) == p.includes.end())
            p.includes.push_back(inc);
    };

    // Gather C++ keywords set
    const auto &kwset = cpp17_keywords();

    // We'll build the new body lines as we go.
    std::vector<std::string> new_body_lines;

    for (const std::string &orig_line : p.body) {
        // Start current_lines with one empty working line
        std::vector<std::string> current_lines(1, std::string{});

        // Tokenization that preserves text segments: we need to iterate the line and split into
        // "non-token text" and "token" segments while ignoring quoted/comment tokens.
        size_t i = 0, n = orig_line.size();
        bool in_single = false, in_double = false, in_block = false;
        std::string seg; // segment builder for non-token text
        while (i < n) {
            // detect block comments (won't span multiple original lines here because orig_line is one line)
            if (!in_single && !in_double) {
                if (!in_block && i+1 < n && orig_line[i] == '/' && orig_line[i+1] == '*') {
                    in_block = true; seg.push_back(orig_line[i]); i++; seg.push_back(orig_line[i]); i++; continue;
                } else if (in_block && i+1 < n && orig_line[i]=='*' && orig_line[i+1]=='/') {
                    in_block = false; seg.push_back(orig_line[i]); i++; seg.push_back(orig_line[i]); i++; continue;
                }
            }
            if (in_block) { seg.push_back(orig_line[i]); i++; continue; }

            // line comment
            if (!in_single && !in_double && i + 1 < n && orig_line[i] == '/' && orig_line[i+1] == '/') {
                // rest of line is comment: append remainder as text segment and break
                seg.append(orig_line.substr(i));
                i = n;
                break;
            }

            // quotes
            if (!in_single && orig_line[i] == '"') { in_double = !in_double; seg.push_back(orig_line[i]); i++; continue; }
            if (!in_double && orig_line[i] == '\'') { in_single = !in_single; seg.push_back(orig_line[i]); i++; continue; }

            // When inside quotes, treat everything as text (don't detect tokens)
            if (in_single || in_double) {
                // handle escape
                if (orig_line[i] == '\\' && i + 1 < n) {
                    seg.push_back(orig_line[i]); i++; seg.push_back(orig_line[i]); i++; continue;
                } else {
                    seg.push_back(orig_line[i]); i++; continue;
                }
            }

            // Outside quotes/comments: detect token start
            char c = orig_line[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || (c >= '0' && c <= '9')) {
                // flush current text segment to current_lines
                if (!seg.empty()) {
                    for (auto &ln : current_lines) ln += seg;
                    seg.clear();
                }
                // collect token
                std::string token;
                while (i < n) {
                    char d = orig_line[i];
                    if ((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') || d == '_' || (d >= '0' && d <= '9')) {
                        token.push_back(d); i++;
                    } else break;
                }
                std::string norm = normalize_token(token);
                if (norm.empty()) continue;

                // If we've already processed this token earlier in this top-level call, reuse its expansion (no new prompt)
                if (processed_tokens.count(norm)) {
                    // If it's known user keyword or known built-in, we need to get its expanded parts:
                    Parts nested;
                    if (user_keywords.find(norm) != user_keywords.end()) {
                        nested = generate_parts_for_keyword_occurrence(norm, ctx, 0, 0, user_keywords, active_ptr);
                    } else if (kwset.find(norm) != kwset.end()) {
                        // built-in; call handler once
                        nested = generate_parts_for_keyword_occurrence(norm, ctx, 0, 0, user_keywords, active_ptr);
                    } else {
                        // unknown but previously declined or otherwise skipped -> append raw token
                        for (auto &ln : current_lines) ln += token;
                        continue;
                    }
                    // Merge includes (in discovery order)
                    for (const auto &inc : nested.includes) append_include_if_new(inc);
                    // Inline-append nested.body into current_lines
                    if (!nested.body.empty()) {
                        // append first nested line to every current working line, push remainder as extra lines
                        for (auto &ln : current_lines) ln += nested.body[0];
                        for (size_t bi = 1; bi < nested.body.size(); ++bi) new_body_lines.push_back(nested.body[bi]);
                        // if there were multiple current_lines, coalesce them before continuing
                        if (!new_body_lines.empty()) {
                            // prepend existing current_lines content into new_body_lines front if needed
                            // flush current_lines aggregated into new_body_lines
                            for (auto &cl : current_lines) {
                                new_body_lines.insert(new_body_lines.begin(), cl);
                            }
                            // reset current_lines to a single empty line to continue
                            current_lines.clear();
                            current_lines.emplace_back("");
                        }
                    } else {
                        // nothing to insert, keep token text
                        for (auto &ln : current_lines) ln += token;
                    }
                    continue;
                }

                // If token is a known user keyword or a C++ keyword -> expand (this may prompt)
                if (user_keywords.find(norm) != user_keywords.end() || kwset.find(norm) != kwset.end()) {
                    std::cout << "[" << tag << "] Nested token detected in snippet: '" << norm << "'.\n";
                    Parts nested = generate_parts_for_keyword_occurrence(norm, ctx, 0, 0, user_keywords, active_ptr);
                    // Merge includes
                    for (const auto &inc : nested.includes) append_include_if_new(inc);
                    // Inline-append nested.body into current_lines
                    if (!nested.body.empty()) {
                        for (auto &ln : current_lines) ln += nested.body[0];
                        for (size_t bi = 1; bi < nested.body.size(); ++bi) {
                            new_body_lines.push_back(nested.body[bi]);
                        }
                        if (!new_body_lines.empty()) {
                            for (auto &cl : current_lines) {
                                new_body_lines.insert(new_body_lines.begin(), cl);
                            }
                            current_lines.clear();
                            current_lines.emplace_back("");
                        }
                    }
                    processed_tokens.insert(norm);
                    continue;
                }

                // Unknown unquoted token: prompt user whether to create a definition now
                {
                    std::string q = "[" + tag + "] Token '" + token + "' is used in snippet but not defined. Define it now? (y/N)";
                    std::string resp = ask(q, "n");
                    if (!resp.empty() && (resp == "y" || resp == "Y" || resp == "yes" || resp == "Yes")) {
                        // Ask for param list (comma-separated "name=default" pairs)
                        std::string params_raw = ask("Enter parameters (format: name=default,other=val) or leave blank for none", "");
                        auto parsed = parse_param_list(params_raw);

                        // Ask for a multi-line snippet: user finishes by typing 'QED' on its own line.
                        std::vector<std::string> lines;
                        try {
                            lines = read_multiline_body("Enter the snippet for '" + token + "'. Finish with a single 'QED' on its own line:");
                        } catch (const EOFExit &) {
                            std::cout << "[" << tag << "] EOF while reading snippet — aborting new-definition flow for '" << token << "'.\n";
                            // leave token verbatim and mark processed to avoid repeated asking
                            for (auto &ln : current_lines) ln += token;
                            processed_tokens.insert(norm);
                            continue;
                        }

                        // Join lines into multi-line snippet representation expected by your UserKeyword structure.
                        // If your UserKeyword stores snippet as a single string with embedded newlines:
                        std::string snippet_joined;
                        for (size_t li = 0; li < lines.size(); ++li) {
                            snippet_joined += lines[li];
                            if (li + 1 < lines.size()) snippet_joined += "\n";
                        }

                        // Build UserKeyword entry and insert into user_keywords
                        UserKeyword newuk;
                        newuk.snippet = snippet_joined;
                        newuk.params = parsed;
                        user_keywords[norm] = newuk;

                        // Persist immediately so future top-level expansions (or program runs) will not prompt again
                        if (!save_user_keywords(user_keywords)) {
                            std::cout << "[" << tag << "] Warning: failed to save new user keyword '" << token << "' to disk.\n";
                        }

                        // Now expand it (this will prompt for its params)
                        Parts nested = generate_parts_for_keyword_occurrence(norm, ctx, 0, 0, user_keywords, active_ptr);

                        // Merge includes
                        for (const auto &inc : nested.includes) append_include_if_new(inc);
                        // Inline-append nested.body into current_lines
                        if (!nested.body.empty()) {
                            for (auto &ln : current_lines) ln += nested.body[0];
                            for (size_t bi = 1; bi < nested.body.size(); ++bi) {
                                new_body_lines.push_back(nested.body[bi]);
                            }
                            if (!new_body_lines.empty()) {
                                for (auto &cl : current_lines) {
                                    new_body_lines.insert(new_body_lines.begin(), cl);
                                }
                                current_lines.clear();
                                current_lines.emplace_back("");
                            }
                        }
                        processed_tokens.insert(norm);
                        continue;
                    } else {
                        // user declined: leave token verbatim
                        for (auto &ln : current_lines) ln += token;
                        processed_tokens.insert(norm); // avoid asking again
                        continue;
                    }
                }
            } else {
                // normal non-token char: collect to seg
                seg.push_back(orig_line[i]);
                i++;
            }
        } // end while for characters in line

        // flush any remaining seg text into current_lines
        if (!seg.empty()) {
            for (auto &ln : current_lines) ln += seg;
            seg.clear();
        }

        // finished processing this original line; commit current_lines and any new_body_lines into new_body_lines
        // If there is content in current_lines, append them to new_body_lines in order
        for (auto &ln : current_lines) new_body_lines.push_back(ln);
        // Note: new_body_lines already accumulated any extra nested lines inserted mid-line.

    } // end for each original p.body line

    // Final merged includes already inserted into p.includes; replace p.body with new_body_lines
    p.body = std::move(new_body_lines);

    // Unmark current keyword as active
    active_ptr->erase(kw);

    return p;
}

// -------------------- Tokenization --------------------

static vector<string> tokenize(const string &line) {
    std::istringstream iss(line);
    vector<string> out;
    out.reserve(16);
    string t;
    while (iss >> t) out.push_back(t);
    return out;
}

// -------------------- Main interactive loop (commands and extended help) --------------------

int main() {
    std::ios::sync_with_stdio(false);
    cin.tie(nullptr);

    install_slow_output(10); // <-- enable character-by-character printing (10 ms per char)
    cout << "C++17 Keyword-driven snippet generator. Sequence-aware with parameterized custom keywords.\n";
    cout << "Enter a line containing C++17 keywords (duplicates allowed). The tool\n";
    cout << "will ask follow-up questions for every keyword occurrence in order and then\n";
    cout << "produce a single integrated C++17 program.\n\n";
    cout << "Commands:\n";
    cout << "  :add / :define         - define a new custom keyword with parameters\n";
    cout << "  :list                  - list stored custom keywords\n";
    cout << "  :search <term>         - search stored custom keywords (name or snippet text)\n";
    cout << "  :update <keyword>      - interactively update a stored custom keyword (params & snippet)\n";
    cout << "  :delete <keyword>      - delete a stored custom keyword\n";
    cout << "  :help                  - show help (includes C++ standard keywords)\n";
    cout << "Type 'exit' or send EOF to quit.\n\n";

    // load persisted user keywords
    UserKeywordMap user_keywords;
    load_user_keywords(user_keywords);

    const auto &kwset = cpp17_keywords();
    string line;

    while (true) {
        cout << "Enter keyword(s)> ";
        cout.flush();
        if (!getline(cin, line)) {
            cout << "\nEOF received at top-level. Exiting cleanly.\n";
            return 0;
        }
        string trimmed = trim(line);
        if (trimmed.empty()) continue;

        if (!trimmed.empty() && trimmed[0] == ':') {
            std::istringstream iss(trimmed);
            string cmd;
            iss >> cmd;
            if (cmd == ":add" || cmd == ":define") {
                try {
                    string name = ask("Keyword name to define (single word, no punctuation)", "mykw");
                    name = normalize_token(name);
                    if (name.empty()) { cout << "Empty keyword name; aborting.\n"; continue; }
                    if (kwset.find(name) != kwset.end()) {
                        cout << "That name conflicts with a built-in C++17 keyword. Choose another name.\n";
                        continue;
                    }
                    if (user_keywords.find(name) != user_keywords.end()) {
                        string over = ask("Keyword already exists. Overwrite? (y/n)", "n");
                        if (!(over == "y" || over == "Y")) { cout << "Aborted.\n"; continue; }
                    }
                    // parameters
                    string params_line = ask("Provide parameters (format: name=default,other=val) or leave blank", "");
                    vector<std::pair<string,string>> params;
                    if (!params_line.empty()) {
                        auto parts = split_csv(params_line);
                        for (auto &p : parts) {
                            size_t eq = p.find('=');
                            string pname = trim((eq==string::npos)?p:p.substr(0,eq));
                            string pdef = trim((eq==string::npos)?"":p.substr(eq+1));
                            if (!pname.empty()) params.emplace_back(pname, pdef);
                        }
                    }
                    cout << "Paste the snippet that demonstrates this custom keyword. You may use placeholders {name}.\n";
                    vector<string> snippet_lines = read_multiline_body("End with a single 'QED' on new line:");
                    std::ostringstream ss;
                    for (auto &l : snippet_lines) ss << l << "\n";
                    UserKeyword uk;
                    uk.snippet = ss.str();
                    uk.params = params;
                    user_keywords[name] = std::move(uk);
                    if (save_user_keywords(user_keywords)) {
                        cout << "Custom keyword '" << name << "' saved to disk with " << user_keywords[name].params.size() << " parameter(s).\n";
                    } else {
                        cout << "Failed to save custom keywords to disk.\n";
                    }
                } catch (const EOFExit&) {
                    cout << "\nEOF during custom keyword definition. Cancelling and exiting.\n";
                    return 0;
                }
                continue;
            } else if (cmd == ":list") {
                if (user_keywords.empty()) cout << "No custom keywords stored.\n";
                else {
                    cout << "Stored custom keywords and parameters:\n";
                    for (const auto &kv : user_keywords) {
                        cout << "  - " << kv.first;
                        if (!kv.second.params.empty()) {
                            cout << " (params: ";
                            bool first = true;
                            for (const auto &pp : kv.second.params) {
                                if (!first) cout << ", ";
                                cout << pp.first << "=" << pp.second;
                                first = false;
                            }
                            cout << ")";
                        }
                        cout << "\n";
                    }
                }
                continue;
                        } else if (cmd == ":search") {
                // :search <term> — search by name or by substring in snippet text
                string term;
                // allow direct argument or prompt if missing
                if (!(iss >> term)) {
                    term = ask(":search term (substring search over name and snippet)", "");
                }
                if (term.empty()) {
                    cout << "Empty search term; aborting search.\n";
                } else {
                    size_t found = 0;
                    for (const auto &kv : user_keywords) {
                        const string &name = kv.first;
                        const UserKeyword &uk = kv.second;
                        // build a small searchable string: name + params + snippet
                        std::ostringstream probe;
                        probe << name << " ";
                        for (const auto &pp : uk.params) probe << pp.first << "=" << pp.second << " ";
                        probe << " " << uk.snippet;
                        string hay = probe.str();
                        if (hay.find(term) != string::npos) {
                            cout << "  - " << name;
                            if (!uk.params.empty()) {
                                cout << " (params: ";
                                bool first = true;
                                for (const auto &pp : uk.params) {
                                    if (!first) cout << ", ";
                                    cout << pp.first << "=" << pp.second;
                                    first = false;
                                }
                                cout << ")";
                            }
                            cout << "\n";
                            // show a short preview of the snippet (first non-empty line)
                            {
                                std::istringstream s(uk.snippet);
                                string line;
                                while (std::getline(s, line)) {
                                    if (!line.empty()) { cout << "      snippet preview: " << line << "\n"; break; }
                                }
                            }
                            ++found;
                        }
                    }
                    if (found == 0) cout << "No custom keywords matched '" << term << "'.\n";
                }
                continue;
            } else if (cmd == ":update") {
                // :update <keyword> — interactive update for params and snippet, preserve current format
                string key; iss >> key;
                if (key.empty()) {
                    key = ask(":update which custom keyword? (name)", "");
                }
                if (key.empty()) {
                    cout << "No keyword supplied; aborting.\n";
                    continue;
                }
                auto it = user_keywords.find(key);
                if (it == user_keywords.end()) {
                    cout << "No such custom keyword '" << key << "'.\n";
                    continue;
                }
                UserKeyword uk = it->second; // copy for updateing
                cout << "updateing custom keyword '" << key << "'. Current parameters:";
                if (uk.params.empty()) cout << " (none)";
                cout << "\n";
                // show current params and allow update
                for (size_t i = 0; i < uk.params.size(); ++i) {
                    cout << "  " << (i+1) << ") " << uk.params[i].first << " = " << uk.params[i].second << "\n";
                    string newval = ask("    New default for parameter '" + uk.params[i].first + "' (empty = keep)", "");
                    if (!newval.empty()) uk.params[i].second = newval;
                }
                // ask to add new parameter
                string addp = ask("Add a new parameter? (enter name or leave empty to skip)", "");
                while (!addp.empty()) {
                    string defv = ask("  Default value for '" + addp + "'", "");
                    uk.params.emplace_back(addp, defv);
                    addp = ask("Add another parameter? (enter name or leave empty to finish)", "");
                }
                // show current snippet and allow full replacement
                cout << "Current snippet (lines):\n";
                {
                    std::istringstream s(uk.snippet);
                    string line; int idx = 1;
                    while (std::getline(s, line)) {
                        cout << "  " << idx << ": " << line << "\n";
                        ++idx;
                    }
                }
                string replace_snip = ask("Replace snippet entirely? (y to replace / n to keep)", "n");
                if (replace_snip == "y" || replace_snip == "Y") {
                    cout << "Enter new snippet lines. Finish with a single '.' on its own line.\n";
                    vector<string> new_lines = read_multiline_body("Enter new snippet lines, finish with '.'");
                    std::ostringstream ss;
                    for (const auto &ln : new_lines) ss << ln << "\n";
                    uk.snippet = ss.str();
                }
                // write back and persist
                user_keywords[key] = std::move(uk);
                if (save_user_keywords(user_keywords)) {
                    cout << "Custom keyword '" << key << "' updated and saved (" << user_keywords[key].params.size() << " parameter(s)).\n";
                } else {
                    cout << "Failed to save custom keywords to disk.\n";
                }
                continue;
            } else if (cmd == ":delete") {
                string key; iss >> key;
                if (key.empty()) { cout << "Usage: :delete <keyword>\n"; continue; }
                key = normalize_token(key);
                if (user_keywords.erase(key)) {
                    if (save_user_keywords(user_keywords)) cout << "deleted '" << key << "' and saved changes.\n";
                    else cout << "deleted '" << key << "' but failed to save to disk.\n";
                } else {
                    cout << "No such custom keyword: '" << key << "'.\n";
                }
                continue;
            } else if (cmd == ":help") {
                cout << "Commands:\n"
                     << "  :add / :define     - define a new custom keyword with parameters\n"
                     << "  :list              - list stored custom keywords\n"
                     << "  :search <term>     - search stored custom keywords (name or snippet text)\n"
                     << "  :update <keyword>  - interactively update a stored custom keyword (params & snippet)\n"
                     << "  :delete <keyword>  - delete a stored custom keyword\n"
                     << "  :help              - show this help (includes C++ standard keywords)\n\n";
                // Show C++17 keywords (sorted)
                vector<string> ks;
                ks.reserve(cpp17_keywords().size());
                for (const auto &k : cpp17_keywords()) ks.push_back(k);
                std::sort(ks.begin(), ks.end());
                cout << "C++17 standard keywords:\n";
                for (size_t i = 0; i < ks.size(); ++i) {
                    cout << ks[i];
                    if (i + 1 < ks.size()) cout << ", ";
                    if ((i+1) % 8 == 0) cout << "\n";
                }
                cout << "\n\n";
                continue;
            } else {
                cout << "Unknown command '" << cmd << "'. Type :help for commands.\n";
                continue;
            }
        }

        if (trimmed == "exit") {
            cout << "Exit requested. Goodbye.\n";
            return 0;
        }

        // tokenize input and offer to define any unknown tokens that look like custom keywords
        vector<string> tokens = tokenize(trimmed);
        try {
            for (size_t i = 0; i < tokens.size(); ++i) {
                string raw = tokens[i];
                string norm = normalize_token(raw);
                if (norm.empty()) continue;
                // if it's not a standard keyword and not already a stored user keyword,
                // and it looks like an identifier (starts with alpha or '_'), offer to define or skip
                if (cpp17_keywords().find(norm) == cpp17_keywords().end() && user_keywords.find(norm) == user_keywords.end()) {
                    // check identifier-like
                    if ((std::isalpha(static_cast<unsigned char>(norm[0])) || norm[0] == '_')) {
                        string choice = ask("Token '" + norm + "' is not a C++17 or stored custom keyword. Define it now? (y to define / s to skip)", "s");
                        if (choice == "y" || choice == "Y") {
                            // run a tiny define flow that mirrors :add for this single keyword
                            string name = norm; // use the normalized token as the name
                            if (kwset.find(name) != kwset.end()) {
                                cout << "That name conflicts with a built-in C++17 keyword. Skipping.\n";
                                continue;
                            }
                            // parameters
                            string params_line = ask("Provide parameters (format: name=default,other=val) or leave blank", "");
                            vector<std::pair<string,string>> params;
                            if (!params_line.empty()) {
                                auto parts = split_csv(params_line);
                                for (auto &p : parts) {
                                    size_t eq = p.find('=');
                                    string pname = trim((eq==string::npos)?p:p.substr(0,eq));
                                    string pdef = trim((eq==string::npos)?"":p.substr(eq+1));
                                    if (!pname.empty()) params.emplace_back(pname, pdef);
                                }
                            }
                            cout << "Paste the snippet that demonstrates this custom keyword. You may use placeholders {name}.\n";
                            vector<string> snippet_lines = read_multiline_body("End with a single 'QED' on new line:");
                            std::ostringstream ss;
                            for (auto &l : snippet_lines) ss << l << "\n";
                            UserKeyword uk;
                            uk.snippet = ss.str();
                            uk.params = params;
                            user_keywords[name] = std::move(uk);
                            if (save_user_keywords(user_keywords)) {
                                cout << "Custom keyword '" << name << "' saved to disk with " << user_keywords[name].params.size() << " parameter(s).\n";
                            } else {
                                cout << "Failed to save custom keywords to disk.\n";
                            }
                        } else {
                            // skip this token silently
                            cout << "Skipping token '" << norm << "'.\n";
                        }
                    }
                }
            }
        } catch (const EOFExit&) {
            cout << "\nEOF received during custom-keyword definition prompt. Cancelling and exiting.\n";
            return 0;
        }

        // now build occurrences (includes newly-defined user keywords)
        vector<std::pair<string,int>> occurrences;
        occurrences.reserve(tokens.size());
        for (size_t i = 0; i < tokens.size(); ++i) {
            string norm = normalize_token(tokens[i]);
            if (norm.empty()) continue;
            if (kwset.find(norm) != kwset.end() || user_keywords.find(norm) != user_keywords.end()) {
                occurrences.emplace_back(norm, static_cast<int>(i + 1));
            }
        }

        if (occurrences.empty()) {
            cout << "No recognized C++17 or user-defined keyword found in the input. Try again.\n";
            continue;
        }

        cout << "\nDetected occurrences in order:";
        for (size_t i = 0; i < occurrences.size(); ++i) {
            cout << " [" << (i+1) << "] '" << occurrences[i].first << "'(token " << occurrences[i].second << ")";
        }
        cout << "\n\n";

        // collect parts for each occurrence
        Context ctx;
        Parts aggregated;
        try {
            for (size_t i = 0; i < occurrences.size(); ++i) {
                const string &kw = occurrences[i].first;
                int token_pos = occurrences[i].second;
                int occ_index = static_cast<int>(i + 1);
                cout << "--- Asking about keyword occurrence " << occ_index << ": '" << kw << "' (token " << token_pos << ") ---\n";
                Parts p = generate_parts_for_keyword_occurrence(kw, ctx, occ_index, token_pos, user_keywords);
                append_parts_with_nesting(aggregated, p, ctx, kw);
                cout << "\n";
            }
        } catch (const EOFExit&) {
            cout << "\nEOF received during follow-up prompts. Cancelling and exiting.\n";
            return 0;
        } catch (const std::exception &ex) {
            cerr << "Error during prompts: " << ex.what() << "\n";
            return 1;
        }

         // --- NEW: flush any remaining open control blocks so they appear in the final output ---
        if (!ctx.control_stack.empty()) {
            cout << "Flushing " << ctx.control_stack.size() << " open control block(s) to output.\n";
            flush_control_stack(aggregated, ctx);
        }

        // assemble final program
        string final_program = make_program_from_body_lines(aggregated.body, aggregated.includes, aggregated.top);
        cout << "\n--- Generated C++17 program (single integrated example) ---\n";
        cout << final_program << "\n";
        cout << "Copy the program into a .cpp file and compile: g++ -std=c++17 yourfile.cpp\n\n";
    }

    return 0;
}
