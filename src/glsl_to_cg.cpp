#include "glsl_to_cg.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>

namespace glsl {
namespace {

// --- tokens -------------------------------------------------------------------------
struct Tok {
    std::string s;
    bool nl = false;  // a line break precedes this token
};
using Toks = std::vector<Tok>;

bool ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool ident_char(char c) { return std::isalnum((unsigned char)c) || c == '_'; }
bool is_ident(const std::string& s) { return !s.empty() && ident_start(s[0]); }
bool is_number(const std::string& s) {
    return !s.empty() && (std::isdigit((unsigned char)s[0]) || (s[0] == '.' && s.size() > 1));
}

Toks lex(const std::string& src) {
    static const char* const multi[] = {"<<=", ">>=", "++", "--", "+=", "-=", "*=", "/=", "%=", "==", "!=",
                                        "<=", ">=", "&&", "||", "^^", "<<", ">>", "##"};
    Toks out;
    bool nl = false;
    for (size_t i = 0; i < src.size();) {
        char c = src[i];
        if (c == '\n') { nl = true; i++; continue; }
        if (std::isspace((unsigned char)c)) { i++; continue; }
        size_t j = i;
        if (ident_start(c)) {
            while (j < src.size() && ident_char(src[j])) j++;
        } else if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < src.size() && std::isdigit((unsigned char)src[i + 1]))) {
            while (j < src.size() && (std::isalnum((unsigned char)src[j]) || src[j] == '.' ||
                                      ((src[j] == '+' || src[j] == '-') && (src[j - 1] == 'e' || src[j - 1] == 'E'))))
                j++;
        } else {
            j = i + 1;
            for (const char* m : multi)
                if (src.compare(i, strlen(m), m) == 0) { j = i + strlen(m); break; }
        }
        out.push_back({src.substr(i, j - i), nl});
        nl = false;
        i = j;
    }
    return out;
}

std::string strip_comments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 2, "//") == 0) {
            while (i < s.size() && s[i] != '\n') i++;
        } else if (s.compare(i, 2, "/*") == 0) {
            i += 2;
            while (i < s.size() && s.compare(i, 2, "*/") != 0) out += s[i++] == '\n' ? "\n" : "";
            i += 2;
            out += ' ';
        } else {
            out += s[i++];
        }
    }
    return out;
}

// --- preprocessor -------------------------------------------------------------------
struct Macro {
    bool func = false;
    std::vector<std::string> params, body;
};
using Macros = std::map<std::string, Macro>;
using Strs = std::vector<std::string>;

Strs words(const Toks& t, size_t from = 0) {
    Strs w;
    for (size_t i = from; i < t.size(); i++) w.push_back(t[i].s);
    return w;
}

Strs expand(const Strs& toks, const Macros& macros, std::set<std::string>& busy) {
    Strs out;
    for (size_t i = 0; i < toks.size(); i++) {
        const std::string& t = toks[i];
        auto it = macros.find(t);
        if (it == macros.end() || busy.count(t)) {
            out.push_back(t);
            continue;
        }
        const Macro& m = it->second;
        Strs body;
        if (!m.func) {
            body = m.body;
        } else {
            if (i + 1 >= toks.size() || toks[i + 1] != "(") {
                out.push_back(t);
                continue;
            }
            std::vector<Strs> args(1);
            int depth = 0;
            size_t j = i + 1;
            for (; j < toks.size(); j++) {
                const std::string& u = toks[j];
                if (u == "(") {
                    if (depth++ > 0) args.back().push_back(u);
                } else if (u == ")") {
                    if (--depth == 0) break;
                    args.back().push_back(u);
                } else if (u == "," && depth == 1) {
                    args.emplace_back();
                } else {
                    args.back().push_back(u);
                }
            }
            if (j >= toks.size()) {
                out.push_back(t);
                continue;
            }
            for (auto& a : args) a = expand(a, macros, busy);
            for (const std::string& b : m.body) {
                auto p = std::find(m.params.begin(), m.params.end(), b);
                size_t k = (size_t)(p - m.params.begin());
                if (p != m.params.end() && k < args.size()) body.insert(body.end(), args[k].begin(), args[k].end());
                else body.push_back(b);
            }
            i = j;
        }
        busy.insert(t);
        Strs e = expand(body, macros, busy);
        busy.erase(t);
        out.insert(out.end(), e.begin(), e.end());
    }
    return out;
}

// #if expressions
struct Eval {
    const Strs& t;
    size_t i = 0;
    std::string peek() const { return i < t.size() ? t[i] : ""; }
    long primary() {
        std::string s = peek();
        i++;
        if (s == "(") {
            long v = binary(0);
            if (peek() == ")") i++;
            return v;
        }
        if (s == "!") return !primary();
        if (s == "-") return -primary();
        if (s == "+") return primary();
        if (is_number(s)) return std::strtol(s.c_str(), nullptr, 0);
        return 0;  // unknown identifier
    }
    static int prec(const std::string& op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "==" || op == "!=") return 3;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 4;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        return 0;
    }
    long binary(int min) {
        long l = primary();
        for (;;) {
            std::string op = peek();
            int p = prec(op);
            if (!p || p <= min) return l;
            i++;
            long r = binary(p);
            if (op == "||") l = l || r;
            else if (op == "&&") l = l && r;
            else if (op == "==") l = l == r;
            else if (op == "!=") l = l != r;
            else if (op == "<") l = l < r;
            else if (op == ">") l = l > r;
            else if (op == "<=") l = l <= r;
            else if (op == ">=") l = l >= r;
            else if (op == "+") l += r;
            else if (op == "-") l -= r;
            else if (op == "*") l *= r;
            else if (op == "/") l = r ? l / r : 0;
            else if (op == "%") l = r ? l % r : 0;
        }
    }
};

long eval_if(const Strs& expr, const Macros& macros) {
    Strs t;
    for (size_t i = 0; i < expr.size(); i++) {
        if (expr[i] == "defined") {
            std::string name;
            if (i + 1 < expr.size() && expr[i + 1] == "(") {
                name = i + 2 < expr.size() ? expr[i + 2] : "";
                i += 3;
            } else {
                name = i + 1 < expr.size() ? expr[i + 1] : "";
                i += 1;
            }
            t.push_back(macros.count(name) ? "1" : "0");
        } else {
            t.push_back(expr[i]);
        }
    }
    std::set<std::string> busy;
    Strs e = expand(t, macros, busy);
    Eval ev{e};
    return ev.binary(0);
}

std::string preprocess(const std::string& source, bool vertex) {
    (void)vertex;
    Macros macros;
    macros["GL_ES"].body = {"1"};
    macros["__VERSION__"].body = {"100"};
    struct Cond { bool parent, taken, active; };
    std::vector<Cond> conds;
    auto active = [&] { return conds.empty() || conds.back().active; };

    std::string text = strip_comments(source), out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        int extra_lines = 0;
        while (!line.empty() && line.back() == '\\' && end < text.size()) {  // continuation
            size_t e2 = text.find('\n', end + 1);
            if (e2 == std::string::npos) e2 = text.size();
            line = line.substr(0, line.size() - 1) + text.substr(end + 1, e2 - end - 1);
            end = e2;
            extra_lines++;
        }
        pos = end + 1;

        size_t first = line.find_first_not_of(" \t\r");
        if (first != std::string::npos && line[first] == '#') {
            Toks t = lex(line.substr(first + 1));
            std::string d = t.empty() ? "" : t[0].s;
            Strs rest = words(t, 1);
            if (d == "ifdef" || d == "ifndef" || d == "if") {
                bool v = d == "if" ? eval_if(rest, macros) != 0 : (macros.count(rest.empty() ? "" : rest[0]) != 0) == (d == "ifdef");
                bool parent = active();
                conds.push_back({parent, parent && v, parent && v});
            } else if (d == "elif" && !conds.empty()) {
                Cond& c = conds.back();
                if (c.taken) c.active = false;
                else c.taken = c.active = c.parent && eval_if(rest, macros) != 0;
            } else if (d == "else" && !conds.empty()) {
                Cond& c = conds.back();
                c.active = c.parent && !c.taken;
                c.taken = true;
            } else if (d == "endif" && !conds.empty()) {
                conds.pop_back();
            } else if (d == "define" && active() && !rest.empty()) {
                Macro m;
                // function-like only if '(' directly follows the name
                size_t name_at = line.find(rest[0], first);
                size_t after = name_at + rest[0].size();
                size_t body_from = 1;
                if (after < line.size() && line[after] == '(') {
                    m.func = true;
                    size_t k = 2;
                    for (; k < rest.size() && rest[k] != ")"; k++)
                        if (rest[k] != ",") m.params.push_back(rest[k]);
                    body_from = k + 1;
                }
                for (size_t k = body_from; k < rest.size(); k++) m.body.push_back(rest[k]);
                macros[rest[0]] = m;
            } else if (d == "undef" && active() && !rest.empty()) {
                macros.erase(rest[0]);
            }
            out += "\n";
        } else if (active()) {
            std::set<std::string> busy;
            Strs e = expand(words(lex(line)), macros, busy);
            for (const std::string& w : e) out += w + " ";
            out += "\n";
        } else {
            out += "\n";
        }
        for (int k = 0; k < extra_lines; k++) out += "\n";
        if (end >= text.size()) break;
    }
    return out;
}

// --- types --------------------------------------------------------------------------
const std::set<std::string> GLSL_TYPES = {"void", "bool", "int", "float", "vec2", "vec3", "vec4", "bvec2",
                                          "bvec3", "bvec4", "ivec2", "ivec3", "ivec4", "mat2", "mat3", "mat4",
                                          "sampler2D", "samplerCube"};
const std::map<std::string, std::string> CG_TYPES = {
    {"vec2", "float2"}, {"vec3", "float3"}, {"vec4", "float4"}, {"ivec2", "int2"}, {"ivec3", "int3"},
    {"ivec4", "int4"}, {"bvec2", "bool2"}, {"bvec3", "bool3"}, {"bvec4", "bool4"}, {"mat2", "float2x2"},
    {"mat3", "float3x3"}, {"mat4", "float4x4"}, {"samplerCube", "samplerCUBE"}};
const std::set<std::string> CG_RESERVED = {
    "texture", "sampler", "half", "fixed", "string", "pass", "technique", "input", "output", "packed",
    "interface", "register", "inline", "asm", "typedef", "compile", "emit", "pixelshader", "vertexshader",
    "double", "long", "short", "unsigned", "signed", "union", "template", "this", "namespace", "using",
    "public", "private", "protected", "virtual", "friend", "operator", "new", "delete", "sizeof", "switch",
    "case", "default", "goto", "enum", "class", "extern", "volatile", "auto", "catch", "throw", "try",
    "typename", "mutable", "explicit", "char", "matrix", "vector", "sampler_state", "shared", "snorm",
    "unorm", "lerp", "frac", "mul", "tex2D", "texCUBE", "saturate", "rsqrt", "atan2", "fmod", "ddx", "ddy"};

bool is_mat(const std::string& t) { return t == "mat2" || t == "mat3" || t == "mat4"; }
bool is_scalar(const std::string& t) { return t == "float" || t == "int" || t == "bool"; }
int vec_size(const std::string& t) {
    if (t.size() >= 4 && t.compare(t.size() - 4, 3, "vec") == 0) return t.back() - '0';
    return t == "float" || t == "int" || t == "bool" ? 1 : 0;
}
std::string vec_of(const std::string& base, int n) {
    if (n == 1) return base == "ivec" ? "int" : base == "bvec" ? "bool" : "float";
    return base + std::to_string(n);
}
std::string vec_base(const std::string& t) {
    if (t[0] == 'i') return "ivec";
    if (t[0] == 'b') return "bvec";
    return "vec";
}

struct Ctx {
    std::map<std::string, std::string> var_type;
    std::set<std::string> arrays;
    std::map<std::string, std::string> func_ret;
    bool swap_mul = true;
};

size_t match_close(const Toks& t, size_t i) {  // t[i] is ( or [
    const std::string open = t[i].s, close = open == "(" ? ")" : "]";
    int d = 0;
    for (size_t j = i; j < t.size(); j++) {
        if (t[j].s == open) d++;
        else if (t[j].s == close && --d == 0) return j;
    }
    return t.size();
}
size_t match_open(const Toks& t, size_t i) {  // t[i] is ) or ]
    const std::string close = t[i].s, open = close == ")" ? "(" : "[";
    int d = 0;
    for (size_t j = i + 1; j-- > 0;) {
        if (t[j].s == close) d++;
        else if (t[j].s == open && --d == 0) return j;
    }
    return 0;
}

// top-level comma-separated ranges of the call whose '(' is at open
std::vector<std::pair<size_t, size_t>> call_args(const Toks& t, size_t open) {
    std::vector<std::pair<size_t, size_t>> args;
    size_t close = match_close(t, open), start = open + 1;
    int d = 0;
    for (size_t j = open + 1; j < close; j++) {
        const std::string& s = t[j].s;
        if (s == "(" || s == "[") d++;
        else if (s == ")" || s == "]") d--;
        else if (s == "," && d == 0) {
            args.push_back({start, j});
            start = j + 1;
        }
    }
    if (close > open + 1) args.push_back({start, close});
    return args;
}

bool operand_end(const std::string& s) { return is_ident(s) || is_number(s) || s == ")" || s == "]"; }
const std::set<std::string> NOT_CALLS = {"return", "if", "for", "while", "else", "do"};

std::string type_of(const Ctx& c, const Toks& t, size_t a, size_t b);

std::string call_type(const Ctx& c, const Toks& t, const std::string& name, size_t open) {
    if (GLSL_TYPES.count(name)) return name;
    static const std::set<std::string> to_float = {"dot", "length", "distance"};
    static const std::set<std::string> to_vec4 = {"texture2D", "texture2DProj", "texture2DLod", "textureCube",
                                                  "texture2DRect", "tex2D", "texCUBE", "tex2Dbias"};
    if (to_float.count(name)) return "float";
    if (to_vec4.count(name)) return "vec4";
    if (name == "cross") return "vec3";
    auto args = call_args(t, open);
    if (name == "mul" && args.size() == 2) {
        std::string x = type_of(c, t, args[0].first, args[0].second);
        std::string y = type_of(c, t, args[1].first, args[1].second);
        if (is_mat(x) && is_mat(y)) return x;
        return is_mat(x) ? y : x;
    }
    auto f = c.func_ret.find(name);
    if (f != c.func_ret.end()) return f->second;
    // generic builtins (normalize, clamp, mix, pow, max, ...) follow their first argument
    return args.empty() ? "" : type_of(c, t, args[0].first, args[0].second);
}

std::string type_of(const Ctx& c, const Toks& t, size_t a, size_t b) {
    if (a >= b) return "";
    if (t[a].s == "(" && match_close(t, a) == b - 1) return type_of(c, t, a + 1, b - 1);
    // find the lowest-precedence top-level binary operator
    int d = 0;
    size_t q = 0, add = 0, mulop = 0, cmp = 0;
    for (size_t j = a; j < b; j++) {
        const std::string& s = t[j].s;
        if (s == "(" || s == "[") d++;
        else if (s == ")" || s == "]") d--;
        else if (d == 0) {
            bool binary = j > a && operand_end(t[j - 1].s);
            if (s == "?" && !q) q = j;
            else if ((s == "&&" || s == "||" || s == "==" || s == "!=" || s == "<" || s == ">" || s == "<=" ||
                      s == ">=") && binary)
                cmp = j;
            else if ((s == "+" || s == "-") && binary) add = j;
            else if ((s == "*" || s == "/") && binary) mulop = j;
        }
    }
    if (q) {
        int nest = 0;
        for (size_t j = q + 1; j < b; j++) {
            if (t[j].s == "?") nest++;
            else if (t[j].s == ":" && nest-- == 0) return type_of(c, t, q + 1, j);
        }
        return "";
    }
    if (cmp) return "bool";
    if (add) {
        std::string l = type_of(c, t, a, add), r = type_of(c, t, add + 1, b);
        return is_scalar(l) || l.empty() ? (r.empty() ? l : r) : l;
    }
    if (mulop) {
        std::string l = type_of(c, t, a, mulop), r = type_of(c, t, mulop + 1, b);
        if (is_mat(l) && is_mat(r)) return l;
        if (is_mat(l)) return r;
        if (is_mat(r)) return l;
        return is_scalar(l) || l.empty() ? (r.empty() ? l : r) : l;
    }
    const std::string& s = t[a].s;
    if (s == "-" || s == "+" || s == "!" || s == "++" || s == "--") return type_of(c, t, a + 1, b);
    // postfix expression
    std::string type;
    bool array = false;
    size_t p;
    if (s == "(") {
        size_t e = match_close(t, a);
        type = type_of(c, t, a + 1, e);
        p = e + 1;
    } else if (is_ident(s) && a + 1 < b && t[a + 1].s == "(") {
        type = call_type(c, t, s, a + 1);
        p = match_close(t, a + 1) + 1;
    } else if (is_ident(s)) {
        auto v = c.var_type.find(s);
        type = v != c.var_type.end() ? v->second : "";
        array = c.arrays.count(s) != 0;
        p = a + 1;
    } else if (is_number(s)) {
        type = s.find_first_of(".eE") != std::string::npos && s.find("0x") != 0 ? "float" : "int";
        p = a + 1;
    } else {
        return "";
    }
    while (p < b) {
        if (t[p].s == "[") {
            if (array) array = false;
            else if (is_mat(type)) type = "vec" + std::string(1, type.back());
            else if (vec_size(type) > 1) type = vec_of(vec_base(type), 1);
            p = match_close(t, p) + 1;
        } else if (t[p].s == "." && p + 1 < b) {
            int n = (int)t[p + 1].s.size();
            type = vec_size(type) ? vec_of(vec_base(type.empty() ? "vec" : type), n) : (n == 1 ? "float" : "vec" + std::to_string(n));
            p += 2;
        } else if (t[p].s == "++" || t[p].s == "--") {
            p++;
        } else {
            break;
        }
    }
    return type;
}

// start of the postfix expression that ends just before i
size_t left_operand(const Toks& t, size_t i) {
    size_t j = i;
    for (;;) {
        if (j == 0) return j;
        const std::string& p = t[j - 1].s;
        if (p == "]") {
            j = match_open(t, j - 1);
            continue;
        }
        if (is_ident(p) || is_number(p)) {
            if (j >= 2 && t[j - 2].s == ".") {
                j -= 2;
                continue;
            }
            return j - 1;
        }
        if (p == ")") {
            size_t o = match_open(t, j - 1);
            if (o > 0 && is_ident(t[o - 1].s) && !NOT_CALLS.count(t[o - 1].s)) return o - 1;
            return o;
        }
        return j;
    }
}

// end (exclusive) of the unary/postfix expression starting after i
size_t right_operand(const Toks& t, size_t i) {
    size_t j = i + 1;
    while (j < t.size() && (t[j].s == "-" || t[j].s == "+" || t[j].s == "!")) j++;
    if (j >= t.size()) return j;
    if (t[j].s == "(") {
        j = match_close(t, j) + 1;
    } else if (is_ident(t[j].s)) {
        j++;
        if (j < t.size() && t[j].s == "(") j = match_close(t, j) + 1;
    } else {
        j++;
    }
    while (j < t.size()) {
        if (t[j].s == "[") j = match_close(t, j) + 1;
        else if (t[j].s == "." && j + 1 < t.size()) j += 2;
        else break;
    }
    return j;
}

// matrix products -> mul()
void rewrite_matrix_products(const Ctx& c, Toks& t) {
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 1; i < t.size(); i++) {
            if (t[i].s != "*" || !operand_end(t[i - 1].s)) continue;
            size_t l0 = left_operand(t, i), r1 = right_operand(t, i);
            std::string lt = type_of(c, t, l0, i), rt = type_of(c, t, i + 1, r1);
            if (!(is_mat(lt) || is_mat(rt)) || is_scalar(lt) || is_scalar(rt)) continue;
            Toks lhs(t.begin() + l0, t.begin() + i), rhs(t.begin() + i + 1, t.begin() + r1);
            Toks rep;
            rep.push_back({"mul", t[l0].nl});
            rep.push_back({"("});
            const Toks& first = c.swap_mul ? rhs : lhs;
            const Toks& second = c.swap_mul ? lhs : rhs;
            rep.insert(rep.end(), first.begin(), first.end());
            rep.push_back({","});
            rep.insert(rep.end(), second.begin(), second.end());
            rep.push_back({")"});
            rep[2].nl = false;
            t.erase(t.begin() + l0, t.begin() + r1);
            t.insert(t.begin() + l0, rep.begin(), rep.end());
            changed = true;
            break;
        }
    }
}

std::string zero_of(const std::string& cg_type) {
    if (cg_type == "float" || cg_type == "int") return "0";
    if (cg_type == "bool") return "false";
    int n = cg_type.back() - '0';
    std::string z = cg_type + "(";
    for (int i = 0; i < n; i++) z += i ? ",0" : "0";
    return z + ")";
}
std::string cg_type(const std::string& t) {
    auto it = CG_TYPES.find(t);
    return it != CG_TYPES.end() ? it->second : t;
}

struct Decl {
    std::string type, name;
};

struct Parsed {
    Toks body;                 // everything except attribute/varying declarations and main()
    Toks main_body;            // inside main's braces
    std::vector<Decl> attributes, varyings;
    bool has_main = false;
};

// Splits the global declarations out of the token stream.
Parsed parse_globals(const Toks& in) {
    Parsed p;
    int depth = 0;
    bool stmt_start = true;
    for (size_t i = 0; i < in.size(); i++) {
        const std::string& s = in[i].s;
        if (depth == 0 && stmt_start) {
            if (s == "attribute" || s == "varying") {
                size_t e = i;
                while (e < in.size() && in[e].s != ";") e++;
                // qualifier type name
                if (i + 2 < e) (s == "attribute" ? p.attributes : p.varyings).push_back({in[i + 1].s, in[i + 2].s});
                i = e;
                continue;
            }
            if (s == "void" && i + 1 < in.size() && in[i + 1].s == "main") {
                size_t open = i + 2;  // '('
                size_t close = match_close(in, open);
                size_t brace = close + 1;
                if (brace < in.size() && in[brace].s == "{") {
                    int d = 0;
                    size_t e = brace;
                    for (; e < in.size(); e++) {
                        if (in[e].s == "{") d++;
                        else if (in[e].s == "}" && --d == 0) break;
                    }
                    p.main_body.assign(in.begin() + brace + 1, in.begin() + e);
                    p.has_main = true;
                    i = e;
                    continue;
                }
            }
            if (s == "const") {
                p.body.push_back({"static", in[i].nl});
                p.body.push_back({"const"});
                stmt_start = false;
                continue;
            }
            // other global variables are not uniforms in Cg unless 'static'
            if (GLSL_TYPES.count(s) && i + 2 < in.size() && is_ident(in[i + 1].s) && in[i + 2].s != "(")
                p.body.push_back({"static", in[i].nl});
        }
        if (s == "{") depth++;
        if (s == "}") depth--;
        stmt_start = depth == 0 && (s == ";" || s == "}");
        p.body.push_back(in[i]);
    }
    return p;
}

void collect_types(Ctx& c, const Toks& t) {
    int depth = 0;
    for (size_t i = 0; i + 1 < t.size(); i++) {
        if (t[i].s == "{") depth++;
        if (t[i].s == "}") depth--;
        if (!GLSL_TYPES.count(t[i].s) || !is_ident(t[i + 1].s) || GLSL_TYPES.count(t[i + 1].s)) continue;
        const std::string& type = t[i].s;
        if (i + 2 < t.size() && t[i + 2].s == "(" && depth == 0) {
            c.func_ret[t[i + 1].s] = type;
            continue;
        }
        c.var_type[t[i + 1].s] = type;
        if (i + 2 < t.size() && t[i + 2].s == "[") c.arrays.insert(t[i + 1].s);
        // further names in "vec3 a, b;"
        int d = 0;
        for (size_t j = i + 2; j < t.size() && t[j].s != ";"; j++) {
            if (t[j].s == "(" || t[j].s == "[") d++;
            else if (t[j].s == ")" || t[j].s == "]") { if (--d < 0) break; }
            else if (d == 0 && t[j].s == "," && j + 1 < t.size() && is_ident(t[j + 1].s)) {
                c.var_type[t[j + 1].s] = type;
                if (j + 2 < t.size() && t[j + 2].s == "[") c.arrays.insert(t[j + 1].s);
            }
        }
    }
}

// builtin renames, casts and type names (after the matrix pass)
bool finish(const Ctx& c, Toks& t, std::string& error) {
    static const std::map<std::string, std::string> FUNCS = {
        {"texture2D", "tex2D"}, {"texture2DProj", "tex2Dproj"}, {"textureCube", "texCUBE"},
        {"texture2DRect", "tex2D"}, {"mix", "lerp"}, {"fract", "frac"}, {"mod", "fmod"},
        {"inversesqrt", "rsqrt"}, {"dFdx", "ddx"}, {"dFdy", "ddy"}};
    for (size_t i = 0; i < t.size(); i++) {
        const std::string s = t[i].s;  // a copy: the inserts below move the tokens
        bool call = i + 1 < t.size() && t[i + 1].s == "(" && (i == 0 || t[i - 1].s != ".");
        if (call && s == "texture2D" && call_args(t, i + 1).size() == 3) {
            // bias: tex2Dbias(s, float4(coord, 0, bias))
            auto a = call_args(t, i + 1);
            Toks coord(t.begin() + a[1].first, t.begin() + a[1].second);
            Toks bias(t.begin() + a[2].first, t.begin() + a[2].second);
            Toks rep{{"float4"}, {"("}};
            rep.insert(rep.end(), coord.begin(), coord.end());
            rep.push_back({","});
            rep.push_back({"0"});
            rep.push_back({","});
            rep.insert(rep.end(), bias.begin(), bias.end());
            rep.push_back({")"});
            t.erase(t.begin() + a[1].first, t.begin() + a[2].second);
            t.insert(t.begin() + a[1].first, rep.begin(), rep.end());
            t[i].s = "tex2Dbias";
            continue;
        }
        if (call && s == "atan" && call_args(t, i + 1).size() == 2) { t[i].s = "atan2"; continue; }
        if (call && FUNCS.count(s)) { t[i].s = FUNCS.at(s); continue; }
        if (call && (s == "float" || s == "int" || s == "bool")) {
            // scalar constructor -> cast: ((T)(x))
            size_t close = match_close(t, i + 1);
            t.insert(t.begin() + close + 1, Tok{")"});
            t[i].s = "(";
            t.insert(t.begin() + i + 1, {Tok{"("}, Tok{s}, Tok{")"}});
            i += 3;
            continue;
        }
        if (call && vec_size(s) > 1 && call_args(t, i + 1).size() == 1) {
            // vecN(scalar) -> floatN(x, x, ...); vecN(vecN) is fine as it is
            auto a = call_args(t, i + 1)[0];
            if (is_scalar(type_of(c, t, a.first, a.second)) || (a.second == a.first + 1 && is_number(t[a.first].s))) {
                Toks x(t.begin() + a.first, t.begin() + a.second), rep;
                for (int k = 0; k < vec_size(s); k++) {
                    if (k) rep.push_back({","});
                    rep.push_back({"("});
                    rep.insert(rep.end(), x.begin(), x.end());
                    rep.push_back({")"});
                }
                t.erase(t.begin() + a.first, t.begin() + a.second);
                t.insert(t.begin() + a.first, rep.begin(), rep.end());
            }
        }
        if (CG_TYPES.count(s)) t[i].s = CG_TYPES.at(s);
        if (s.compare(0, 3, "gl_") == 0) {
            error = "unsupported built-in " + s;
            return false;
        }
    }
    return true;
}

std::string join(const Toks& t, const std::string& indent = "") {
    std::string out;
    bool line_start = true;
    for (const Tok& k : t) {
        if (k.nl) {
            out += "\n" + indent;
            line_start = true;
        }
        if (!line_start) out += ' ';
        out += k.s;
        line_start = false;
    }
    return out;
}

Toks prepare(const std::string& source, bool vertex) {
    Toks in = lex(preprocess(source, vertex)), out;
    for (size_t i = 0; i < in.size(); i++) {
        const std::string& s = in[i].s;
        if (s == "lowp" || s == "mediump" || s == "highp" || s == "invariant") continue;
        if (s == "precision") {
            while (i < in.size() && in[i].s != ";") i++;
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

}  // namespace

std::vector<std::string> varyings(const std::string& source, bool vertex) {
    std::vector<std::string> v;
    for (const Decl& d : parse_globals(prepare(source, vertex)).varyings) v.push_back(d.name);
    return v;
}

std::vector<std::pair<std::string, std::string>> attributes(const std::string& source) {
    std::vector<std::pair<std::string, std::string>> v;
    for (const Decl& d : parse_globals(prepare(source, true)).attributes) v.push_back({d.type, d.name});
    return v;
}

std::string renamed_uniform(const std::string& name) { return CG_RESERVED.count(name) ? name + "_" : name; }

bool to_cg(const std::string& source, bool vertex, const std::map<std::string, int>& slots, const Options& opt,
           std::string& out, std::string& error) {
    Parsed p = parse_globals(prepare(source, vertex));
    if (!p.has_main) {
        error = "no main()";
        return false;
    }
    for (const Tok& k : p.main_body)
        if (k.s == "return") {
            error = "return inside main()";
            return false;
        }

    // renames: attributes and varyings become statics; Cg keywords get a '_'
    std::map<std::string, std::string> rename = {{"gl_Position", "vgl_Position"}, {"gl_FragColor", "vgl_FragColor"}};
    for (const Decl& d : p.attributes) rename[d.name] = "vgl_a_" + d.name;
    for (const Decl& d : p.varyings) rename[d.name] = "vgl_v_" + d.name;
    for (Toks* t : {&p.body, &p.main_body})
        for (size_t i = 0; i < t->size(); i++) {
            Tok& k = (*t)[i];
            if (!is_ident(k.s) || (i > 0 && (*t)[i - 1].s == ".")) continue;
            auto r = rename.find(k.s);
            if (r != rename.end()) k.s = r->second;
            else if (CG_RESERVED.count(k.s)) k.s += "_";
        }

    Ctx c;
    c.swap_mul = opt.swap_mul;
    collect_types(c, p.body);
    collect_types(c, p.main_body);
    for (const Decl& d : p.attributes) c.var_type["vgl_a_" + d.name] = d.type;
    for (const Decl& d : p.varyings) c.var_type["vgl_v_" + d.name] = d.type;
    c.var_type["vgl_Position"] = c.var_type["vgl_FragColor"] = "vec4";

    rewrite_matrix_products(c, p.body);
    rewrite_matrix_products(c, p.main_body);
    if (!finish(c, p.body, error) || !finish(c, p.main_body, error)) return false;

    std::string s = "// translated from GLSL by r6sv\n";
    if (vertex) s += "static float4 vgl_Position = float4(0,0,0,0);\n";
    else s += "static float4 vgl_FragColor = float4(0,0,0,0);\n";
    for (const Decl& d : p.attributes) s += "static " + cg_type(d.type) + " vgl_a_" + d.name + ";\n";
    for (const Decl& d : p.varyings) s += "static " + cg_type(d.type) + " vgl_v_" + d.name + " = " + zero_of(cg_type(d.type)) + ";\n";
    s += join(p.body) + "\n\n";

    std::vector<std::string> params, copy_in, copy_out;
    for (const Decl& d : p.attributes) {
        params.push_back(cg_type(d.type) + " " + d.name);
        copy_in.push_back("vgl_a_" + d.name + " = " + d.name + ";");
    }
    for (const Decl& d : p.varyings) {
        auto slot = slots.find(d.name);
        if (slot == slots.end() || slot->second > 9) {
            error = "no TEXCOORD slot for varying " + d.name;
            return false;
        }
        std::string sem = " : TEXCOORD" + std::to_string(slot->second);
        if (vertex) {
            params.push_back("out " + cg_type(d.type) + " vgl_out_" + d.name + sem);
            copy_out.push_back("vgl_out_" + d.name + " = vgl_v_" + d.name + ";");
        } else {
            params.push_back(cg_type(d.type) + " vgl_in_" + d.name + sem);
            copy_in.push_back("vgl_v_" + d.name + " = vgl_in_" + d.name + ";");
        }
    }
    if (vertex) {
        params.push_back("out float4 vgl_out_Position : POSITION");
        copy_out.push_back("vgl_out_Position = vgl_Position;");
    }
    s += vertex ? "void main(" : "float4 main(";
    for (size_t i = 0; i < params.size(); i++) s += (i ? ",\n    " : "\n    ") + params[i];
    s += vertex ? ")\n{\n" : ") : COLOR\n{\n";
    for (const std::string& l : copy_in) s += "    " + l + "\n";
    s += "    " + join(p.main_body, "    ") + "\n";
    for (const std::string& l : copy_out) s += "    " + l + "\n";
    if (!vertex) s += "    return vgl_FragColor;\n";
    s += "}\n";
    out = s;
    return true;
}

}  // namespace glsl
