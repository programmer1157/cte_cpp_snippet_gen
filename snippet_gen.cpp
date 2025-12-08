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
:remove, :help retained and extended.

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

static string tail_name(const string &qualified) {
    auto pos = qualified.rfind("::");
    if (pos == string::npos) return qualified;
    return qualified.substr(pos + 2);
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

// read_multiline_body implementation: reads lines until a single '.' line.
// Returns collected lines (excluding the '.') and throws EOFExit on EOF.
static vector<string> read_multiline_body(const string &instruction = "Enter lines, finish with a single '.' on its own line:") {
    cout << instruction << endl;
    vector<string> lines;
    lines.reserve(16);
    string line;
    while (true) {
        cout << "> ";
        cout.flush();
        if (!getline(cin, line)) throw EOFExit();
        if (line == ".") break;
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

// optional: restore original buffers (call at exit if you want to be tidy)
static void restore_output() {
    if (g_old_cout) std::cout.rdbuf(g_old_cout);
    if (g_old_cerr) std::cerr.rdbuf(g_old_cerr);
    delete g_slow_cout;
    delete g_slow_cerr;
    g_slow_cout = g_slow_cerr = nullptr;
    g_old_cout = g_old_cerr = nullptr;
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

// Load user keywords into out_map (key -> UserKeyword)
static void load_user_keywords(map<string,UserKeyword> &out_map, const string &path = USER_KW_FILE) {
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
static bool save_user_keywords(const map<string,UserKeyword> &m, const string &path = USER_KW_FILE) {
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

struct Context {
    map<string,string> vars;
    std::set<string> types;
    string last_var;
    string last_type;
    map<string,string> meta;
};

static string declare_variable(Context &ctx, const string &type, const string &base_name, const string &init) {
    string name = base_name;
    int suffix = 1;
    while (ctx.vars.find(name) != ctx.vars.end()) name = base_name + std::to_string(suffix++);
    ctx.vars[name] = type;
    ctx.last_var = name;
    return type + " " + name + " = " + init + ";";
}

static void append_parts(Parts &acc, const Parts &p) {
    for (auto &inc : p.includes) acc.includes.push_back(inc);
    for (auto &t : p.top) acc.top.push_back(t);
    for (auto &b : p.body) acc.body.push_back(b);
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
    string then_stmt = ask("[" + tag + "] Then-branch (single statement)", "cout << \"then\" << endl;");
    string else_stmt = ask("[" + tag + "] Else-branch (single statement)", "cout << \"else\" << endl;");
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
    string expr = ask("[" + tag + "] Expression to return from main", "0");
    p.body.push_back("// (" + tag + ") Demonstrate return");
    p.body.push_back("cout << \"About to return: \" << (" + expr + ") << endl;");
    p.body.push_back("return " + expr + ";");
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

    // boolean alternative tokens (leave unchanged)
    if (kw == "and" || kw == "or" || kw == "not") {
        string expr = ask("[" + tag + "] Boolean expression", "x > 0 and y > 0");
        p.body.push_back("// (" + tag + ") Demonstrate 'and'/'or'/'not'");
        p.body.push_back("int x = 1, y = 2;");
        p.body.push_back(std::string("if (") + expr + ") cout << \"true\" << endl; else cout << \"false\" << endl;");
        return p;
    }

    // bitwise binaries: xor, bitand, bitor
    if (kw == "xor" || kw == "bitand" || kw == "bitor") {

        auto L = ask_integral("Left operand", "a", "int");
        auto R = ask_integral("Right operand", "b", "int");

        string a_type = L.first, a_name = L.second;
        string b_type = R.first, b_name = R.second;

        string a_val = ask("[" + tag + "] Left operand initial value", "5");
        string b_val = ask("[" + tag + "] Right operand initial value", "3");

        ctx.vars[a_name] = a_type;
        ctx.last_var = a_name;

        string sym = (kw=="xor" ? "^" : (kw=="bitand" ? "&" : "|"));

        p.body.push_back("// (" + tag + ") Demonstrate alternative token '" + kw + "'");
        p.body.push_back(a_type + " " + a_name + " = " + a_val + ";");
        p.body.push_back(b_type + " " + b_name + " = " + b_val + ";");
        p.body.push_back(std::string("cout << \"") + a_name + " " + kw + " " + b_name +
                         " = \" << (" + a_name + " " + kw + " " + b_name + ") << endl;");
        p.body.push_back(std::string("cout << \"") + a_name + " " + sym + " " + b_name +
                         " (symbol) = \" << (" + a_name + " " + sym + " " + b_name + ") << endl;");
        return p;
    }

    // unary: compl
    if (kw == "compl") {
        auto V = ask_integral("Variable", "x", "int");
        string t = V.first, v = V.second;
        string val = ask("[" + tag + "] Initial value", "42");

        ctx.vars[v] = t;
        ctx.last_var = v;

        p.body.push_back("// (" + tag + ") Demonstrate 'compl'");
        p.body.push_back(t + " " + v + " = " + val + ";");
        p.body.push_back(std::string("cout << \"compl " + v + " = \" << (compl " + v + ") << endl;"));
        p.body.push_back(std::string("cout << \"~" + v + " = \" << (~" + v + ") << endl;"));
        return p;
    }

    // not_eq: inequality
    if (kw == "not_eq") {
        auto L = ask_integral("Left operand", "x", "int");
        string t = L.first, left = L.second;

        string right = ask("[" + tag + "] Right operand/value", "0");

        p.body.push_back(t + " " + left + " = 1; // example");
        ctx.vars[left] = t;
        ctx.last_var = left;

        p.body.push_back("// (" + tag + ") Demonstrate 'not_eq'");
        p.body.push_back(std::string("cout << \"") + left + " not_eq " + right +
                         " => \" << ((" + left + " not_eq " + right +
                         ") ? \"true\" : \"false\") << endl;");
        return p;
    }

    // compound: and_eq, or_eq, xor_eq
    if (kw == "and_eq" || kw == "or_eq" || kw == "xor_eq") {
        auto V = ask_integral("Variable to modify", "v", "int");
        string t = V.first, v = V.second;

        string val = ask("[" + tag + "] Initial value", "15");
        string rhs = ask("[" + tag + "] RHS value", "6");

        ctx.vars[v] = t;
        ctx.last_var = v;

        string sym = (kw=="and_eq") ? "&=" : (kw=="or_eq" ? "|=" : "^=");

        p.body.push_back("// (" + tag + ") Demonstrate '" + kw + "'");
        p.body.push_back(t + " " + v + " = " + val + ";");
        p.body.push_back(std::string("cout << \"before: " + v + " = \" << ") + v + " << endl;");
        p.body.push_back(v + " " + kw + " " + rhs + ";");
        p.body.push_back(std::string("cout << \"after (" + v + " " + sym + " " + rhs +
                         "): \" << ") + v + " << endl;");
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

// -------------------- Dispatcher per occurrence, updated to support user keywords with params ------

static Parts generate_parts_for_keyword_occurrence(const string &kw,
                                                   Context &ctx,
                                                   int occurrence_index,
                                                   int token_pos_in_input,
                                                   const map<string,UserKeyword> &user_keywords) {
    ostringstream t;
    t << "occurrence " << occurrence_index << " (token " << token_pos_in_input << ")";
    string tag = t.str();

    // If it's a user-defined keyword, prompt for parameter values and substitute.
    auto uit = user_keywords.find(kw);
    if (uit != user_keywords.end()) {
        const UserKeyword &uk = uit->second;
        // collect parameter values
        map<string,string> values;
        for (const auto &pp : uk.params) {
            const string &pname = pp.first;
            const string &pdef = pp.second;
            string val = ask("[" + tag + "] Value for parameter '" + pname + "'", pdef);
            values[pname] = val;
        }
        return parts_from_user_snippet_with_params(uk, values, tag);
    }

    // built-in keyword routing (unchanged + additional handlers)
    if (kw == "int" || kw == "double" || kw == "float" || kw == "char" ||
        kw == "long" || kw == "short" || kw == "signed" || kw == "unsigned" ||
        kw == "bool" || kw == "wchar_t" || kw == "char16_t" || kw == "char32_t")
        return handle_type_like(ctx, kw, tag);
    if (kw == "auto") return handle_auto(ctx, tag);
    if (kw == "if" || kw == "else") return handle_if_else(ctx, tag);
    if (kw == "for") return handle_for(ctx, tag);
    if (kw == "while") return handle_while(ctx, tag);
    if (kw == "do") return handle_do(ctx, tag);
    if (kw == "switch" || kw == "case") return handle_switch(ctx, tag);
    if (kw == "return") return handle_return(ctx, tag);
    if (kw == "class" || kw == "struct" || kw == "union") return handle_class_struct_union(ctx, kw, tag);
    if (kw == "enum") return handle_enum(ctx, tag);
    if (kw == "template") return handle_template(ctx, tag);
    if (kw == "static_cast" || kw == "dynamic_cast" || kw == "const_cast" || kw == "reinterpret_cast")
        return handle_cast(ctx, kw, tag);
    if (kw == "new" || kw == "delete") return handle_new_delete(ctx, tag);
    if (kw == "operator") return handle_operator_keyword(ctx, tag);
    if (kw == "try" || kw == "catch" || kw == "throw") return handle_try_catch_throw(ctx, tag);
    if (kw == "constexpr") return handle_constexpr(ctx, tag);
    if (kw == "static_assert") return handle_static_assert(ctx, tag);
    if (kw == "alignas" || kw == "alignof") return handle_alignas_alignof(ctx, tag);
    if (kw == "thread_local") return handle_thread_local(ctx, tag);
    if (kw == "mutable") return handle_mutable(ctx, tag);
    if (kw == "sizeof" || kw == "typeid") return handle_sizeof_typeid(ctx, tag);
    if (kw == "and" || kw == "or" || kw == "not" || kw == "xor" || kw == "bitand" || kw == "bitor" || kw == "compl" || kw == "not_eq" || kw == "and_eq" || kw == "or_eq" || kw == "xor_eq")
        return handle_alternative_tokens(ctx, kw, tag);

    // additional standard keyword handlers
    if (kw == "extern") return handle_extern(ctx, tag);
    if (kw == "inline") return handle_inline(ctx, tag);
    if (kw == "register") return handle_register(ctx, tag);
    if (kw == "asm") return handle_asm(ctx, tag);
    if (kw == "goto") return handle_goto(ctx, tag);
    if (kw == "break" || kw == "continue") return handle_break_continue(ctx, kw, tag);
    if (kw == "export") return handle_export(ctx, tag);

    // newly added keyword handlers
    if (kw == "const") return handle_const(ctx, tag);
    if (kw == "decltype") return handle_decltype(ctx, tag);
    if (kw == "explicit") return handle_explicit(ctx, tag);
    if (kw == "true" || kw == "false") return handle_bool_literal(ctx, kw, tag);
    if (kw == "friend") return handle_friend(ctx, tag);
    if (kw == "namespace") return handle_namespace(ctx, tag);
    if (kw == "noexcept") return handle_noexcept(ctx, tag);
    if (kw == "nullptr") return handle_nullptr(ctx, tag);
    if (kw == "private" || kw == "protected" || kw == "public") return handle_access_specifiers(ctx, kw, tag);
    if (kw == "static") return handle_static(ctx, tag);
    if (kw == "this") return handle_this(ctx, tag);
    if (kw == "typedef" || kw == "typename") return handle_typedef_typename(ctx, kw, tag);
    if (kw == "using") return handle_using(ctx, tag);
    if (kw == "virtual") return handle_virtual(ctx, tag);
    if (kw == "void") return handle_void(ctx, tag);
    if (kw == "volatile") return handle_volatile(ctx, tag);

    // fallback
    return handle_generic_with_body(ctx, kw, tag);
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
    cout << "  :edit <keyword>        - interactively edit a stored custom keyword (params & snippet)\n";
    cout << "  :remove <keyword>      - remove a stored custom keyword\n";
    cout << "  :help                  - show help (includes C++ standard keywords)\n";
    cout << "Type 'exit' or send EOF to quit.\n\n";

    // load persisted user keywords
    map<string,UserKeyword> user_keywords;
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
                    vector<string> snippet_lines = read_multiline_body("End with a single '.' line:");
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
            } else if (cmd == ":edit") {
                // :edit <keyword> — interactive edit for params and snippet, preserve current format
                string key; iss >> key;
                if (key.empty()) {
                    key = ask(":edit which custom keyword? (name)", "");
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
                UserKeyword uk = it->second; // copy for editing
                cout << "Editing custom keyword '" << key << "'. Current parameters:";
                if (uk.params.empty()) cout << " (none)";
                cout << "\n";
                // show current params and allow edit
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
            } else if (cmd == ":remove") {
                string key; iss >> key;
                if (key.empty()) { cout << "Usage: :remove <keyword>\n"; continue; }
                key = normalize_token(key);
                if (user_keywords.erase(key)) {
                    if (save_user_keywords(user_keywords)) cout << "Removed '" << key << "' and saved changes.\n";
                    else cout << "Removed '" << key << "' but failed to save to disk.\n";
                } else {
                    cout << "No such custom keyword: '" << key << "'.\n";
                }
                continue;
            } else if (cmd == ":help") {
                cout << "Commands:\n"
                     << "  :add / :define     - define a new custom keyword with parameters\n"
                     << "  :list              - list stored custom keywords\n"
                     << "  :search <term>     - search stored custom keywords (name or snippet text)\n"
                     << "  :edit <keyword>    - interactively edit a stored custom keyword (params & snippet)\n"
                     << "  :remove <keyword>  - remove a stored custom keyword\n"
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
                            vector<string> snippet_lines = read_multiline_body("End with a single '.' line:");
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
                append_parts(aggregated, p);
                cout << "\n";
            }
        } catch (const EOFExit&) {
            cout << "\nEOF received during follow-up prompts. Cancelling and exiting.\n";
            return 0;
        } catch (const std::exception &ex) {
            cerr << "Error during prompts: " << ex.what() << "\n";
            return 1;
        }

        // assemble final program
        string final_program = make_program_from_body_lines(aggregated.body, aggregated.includes, aggregated.top);
        cout << "\n--- Generated C++17 program (single integrated example) ---\n";
        cout << final_program << "\n";
        cout << "Copy the program into a .cpp file and compile: g++ -std=c++17 yourfile.cpp\n\n";
    }

    return 0;
}
