/*
C++17 Keyword-driven snippet generator — persistent custom keywords with parameters,
help shows C++ standard keywords, and lightweight optimizations.

Changes/additions (preserve existing behavior):
 1) Custom keywords now support meaningful parameters. When defining a custom
    keyword you may specify parameters in the form: name=default,another=42
    The stored snippet may use placeholders {name} which will be replaced
    with the prompted values when generating the program.
 2) The :help command now shows the list of C++17 standard keywords.
 3) Minor performance-conscious changes: passing large containers by const-ref,
    avoiding unnecessary copies, and small allocations reserved where appropriate.

Other behavior/features unchanged:
 - Sequence-aware detection of every keyword occurrence.
 - Follow-up prompts reference each occurrence by index and token position.
 - read_multiline_body(...) implemented (reads until a single '.' line).
 - EOF during any follow-up prompts aborts and exits cleanly.
 - User commands :add/:define, :list, :remove, :help retained and extended.

Compile: g++ -std=c++17 -O2 -Wall -Wextra -o snippet_gen snippet_gen.cpp
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

static string make_program_from_body_lines(const vector<string> &body_lines,
                                          const vector<string> &extra_includes = {},
                                          const vector<string> &extra_top = {}) {
    std::ostringstream out;
    out << "#include <iostream>\n";
    // dedupe includes
    std::set<string> uniq;
    for (auto &h : extra_includes) if (!h.empty()) uniq.insert(h);
    for (auto &h : uniq) out << "#include <" << h << ">\n";
    out << "\n";
    for (auto &t : extra_top) out << t << "\n";
    out << "\nusing namespace std;\n\n";
    out << "int main() {\n";
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
        s.reserve(128);
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
static Parts parts_from_user_snippet_with_params(const UserKeyword &uk, const map<string,string> &values, const string &tag) {
    // copy snippet and replace placeholders {name} with provided values
    string transformed = uk.snippet;
    for (const auto &pp : uk.params) {
        const string &pname = pp.first;
        auto it = values.find(pname);
        string val = (it != values.end()) ? it->second : pp.second;
        // replace occurrences of "{pname}" with val
        replace_all(transformed, "{" + pname + "}", val);
    }
    // Now place transformed into parts
    Parts p;
    if (transformed.find("int main(") != string::npos) {
        p.top.push_back("// (" + tag + ") User-defined full program:");
        p.top.push_back(transformed);
        p.body.push_back("// (" + tag + ") User program above; no extra main content inserted here.");
    } else {
        p.body.push_back("// (" + tag + ") User-defined snippet (with parameter substitution):");
        std::istringstream iss(transformed);
        string line;
        while (std::getline(iss, line)) p.body.push_back(line);
    }
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
    string expr = ask("[" + tag + "] Expression to switch on", "n");
    string cases = ask("[" + tag + "] Comma-separated case values", "1,2,3");
    vector<string> case_list = split_csv(cases);
    p.body.push_back("// (" + tag + ") Demonstrate switch");
    p.body.push_back(init + ";");
    p.body.push_back("switch (" + expr + ") {");
    for (auto &c : case_list) p.body.push_back("    case " + c + ": cout << \"case " + c + "\" << endl; break;");
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
        p.top.push_back("struct Base { virtual ~Base() = default; };");
        p.top.push_back("struct Derived : Base { int x = 42; };");
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
    p.top.push_back("struct alignas(32) Aligned { char data[64]; };");
    p.body.push_back("// (" + tag + ") Demonstrate alignas/alignof");
    p.body.push_back("Aligned a;");
    p.body.push_back("cout << \"alignof(Aligned) = \" << alignof(Aligned) << endl;");
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
    p.top.push_back("struct S { mutable int " + member + " = 0; int value = 0; int get() const { return " + member + " = value; } };");
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
    if (kw == "and" || kw == "or" || kw == "not") {
        string expr_default = ctx.last_var.empty() ? "x > 0 and y > 0" : ctx.last_var + " > 0 and true";
        string expr = ask("[" + tag + "] A simple Boolean expression (you may use alternative tokens)", expr_default);
        p.body.push_back("// (" + tag + ") Demonstrate alternative tokens like 'and'/'or'/'not'");
        p.body.push_back("int x = 1, y = 2;");
        p.body.push_back("if (" + expr + ") cout << \"expression true\" << endl; else cout << \"expression false\" << endl;");
    } else {
        p.body.push_back("// (" + tag + ") Demonstrate alternative token: " + kw);
        p.body.push_back("cout << \"Alternative token: " + kw + "\" << endl;");
    }
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

    // built-in keyword routing (unchanged)
    if (kw == "int" || kw == "double" || kw == "float" || kw == "char" ||
        kw == "long" || kw == "short" || kw == "signed" || kw == "unsigned" ||
        kw == "bool" || kw == "wchar_t" || kw == "char16_t" || kw == "char32_t")
        return handle_type_like(ctx, kw, tag);
    if (kw == "auto") return handle_auto(ctx, tag);
    if (kw == "if" || kw == "else") return handle_if_else(ctx, tag);
    if (kw == "for") return handle_for(ctx, tag);
    if (kw == "while") return handle_while(ctx, tag);
    if (kw == "do") return handle_do(ctx, tag);
    if (kw == "switch") return handle_switch(ctx, tag);
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

    cout << "C++17 Keyword-driven snippet generator — sequence-aware with parameterized custom keywords\n";
    cout << "Enter a line containing C++17 keywords (duplicates allowed). The tool\n";
    cout << "will ask follow-up questions for every occurrence in order and then\n";
    cout << "produce a single integrated C++17 program.\n\n";
    cout << "Commands:\n";
    cout << "  :add / :define         - define a new custom keyword with parameters\n";
    cout << "  :list                  - list stored custom keywords\n";
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

        // tokenize input and capture every occurrence in order (including custom keywords)
        vector<string> tokens = tokenize(trimmed);
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
