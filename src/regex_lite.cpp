#include "regex_lite.h"
#include <memory>

namespace ag {

namespace {
struct Node {
    enum Kind { Lit, Any, Cls, Bol, Eol, WordB, NWordB, Cat, Alt, Star, Plus, Quest, Empty } k;
    unsigned char c = 0;
    int cls = 0;
    std::vector<std::unique_ptr<Node>> kids;
    explicit Node(Kind kk) : k(kk) {}
};
inline unsigned char lc(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c; }
inline bool is_word(int c) { return c >= 0 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'); }
}  // namespace

struct RxCompiler {
    const std::string& p;
    size_t i = 0;
    std::string err;
    Regex& rx;
    RxCompiler(const std::string& pat, Regex& r) : p(pat), rx(r) {}

    void add_class_escape(Regex::CharClass& cc, char e) {
        for (int ch = 0; ch < 256; ch++) {
            bool in = false;
            switch (e) {
            case 's': case 'S': in = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; break;
            case 'd': case 'D': in = ch >= '0' && ch <= '9'; break;
            case 'w': case 'W': in = is_word(ch); break;
            }
            if (e == 'S' || e == 'D' || e == 'W') in = !in;
            if (in) cc.set[ch] = true;
        }
    }
    int new_class() { rx.classes_.emplace_back(); return (int)rx.classes_.size() - 1; }

    std::unique_ptr<Node> parse_alt() {
        auto left = parse_cat();
        if (!left) return nullptr;
        while (i < p.size() && p[i] == '|') {
            i++;
            auto right = parse_cat();
            if (!right) return nullptr;
            auto n = std::unique_ptr<Node>(new Node(Node::Alt));
            n->kids.push_back(std::move(left));
            n->kids.push_back(std::move(right));
            left = std::move(n);
        }
        return left;
    }
    std::unique_ptr<Node> parse_cat() {
        auto cat = std::unique_ptr<Node>(new Node(Node::Cat));
        while (i < p.size() && p[i] != '|' && p[i] != ')') {
            auto a = parse_atom();
            if (!a) return nullptr;
            while (i < p.size() && (p[i] == '*' || p[i] == '+' || p[i] == '?')) {
                Node::Kind k = p[i] == '*' ? Node::Star : p[i] == '+' ? Node::Plus : Node::Quest;
                i++;
                auto r = std::unique_ptr<Node>(new Node(k));
                r->kids.push_back(std::move(a));
                a = std::move(r);
            }
            cat->kids.push_back(std::move(a));
        }
        return cat;
    }
    std::unique_ptr<Node> parse_atom() {
        char c = p[i++];
        switch (c) {
        case '(': {
            if (i + 1 < p.size() && p[i] == '?' && p[i + 1] == ':') i += 2;  // allow (?:...)
            auto n = parse_alt();
            if (!n) return nullptr;
            if (i >= p.size() || p[i] != ')') { err = "missing )"; return nullptr; }
            i++;
            return n;
        }
        case '.': return std::unique_ptr<Node>(new Node(Node::Any));
        case '^': return std::unique_ptr<Node>(new Node(Node::Bol));
        case '$': return std::unique_ptr<Node>(new Node(Node::Eol));
        case '*': case '+': case '?': err = "quantifier without operand"; return nullptr;
        case '[': {
            int ci = new_class();
            Regex::CharClass& cc = rx.classes_[ci];
            if (i < p.size() && p[i] == '^') { cc.neg = true; i++; }
            bool first = true;
            while (i < p.size() && (p[i] != ']' || first)) {
                first = false;
                unsigned char a = (unsigned char)p[i++];
                if (a == '\\' && i < p.size()) {
                    char e = p[i++];
                    if (e == 's' || e == 'S' || e == 'd' || e == 'D' || e == 'w' || e == 'W') { add_class_escape(cc, e); continue; }
                    a = (unsigned char)(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
                }
                unsigned char b = a;
                if (i + 1 < p.size() && p[i] == '-' && p[i + 1] != ']') {
                    i++;
                    b = (unsigned char)p[i++];
                    if (b == '\\' && i < p.size()) b = (unsigned char)p[i++];
                }
                for (int ch = a; ch <= b; ch++) { cc.set[ch] = true; cc.set[lc((unsigned char)ch)] = true; }
            }
            if (i >= p.size()) { err = "missing ]"; return nullptr; }
            i++;
            auto n = std::unique_ptr<Node>(new Node(Node::Cls));
            n->cls = ci;
            return n;
        }
        case '\\': {
            if (i >= p.size()) { err = "trailing backslash"; return nullptr; }
            char e = p[i++];
            if (e == 'b') return std::unique_ptr<Node>(new Node(Node::WordB));
            if (e == 'B') return std::unique_ptr<Node>(new Node(Node::NWordB));
            if (e == 's' || e == 'S' || e == 'd' || e == 'D' || e == 'w' || e == 'W') {
                int ci = new_class();
                add_class_escape(rx.classes_[ci], e);
                auto n = std::unique_ptr<Node>(new Node(Node::Cls));
                n->cls = ci;
                return n;
            }
            auto n = std::unique_ptr<Node>(new Node(Node::Lit));
            n->c = lc((unsigned char)(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e));
            return n;
        }
        default: {
            auto n = std::unique_ptr<Node>(new Node(Node::Lit));
            n->c = lc((unsigned char)c);
            return n;
        }
        }
    }

    int emit(Regex::Inst::Op op, int x = 0, int y = 0, unsigned char c = 0) {
        Regex::Inst in;
        in.op = op; in.x = x; in.y = y; in.c = c;
        rx.prog_.push_back(in);
        return (int)rx.prog_.size() - 1;
    }
    void gen(const Node* n) {
        typedef Regex::Inst I;
        switch (n->k) {
        case Node::Lit: emit(I::Char, 0, 0, n->c); break;
        case Node::Any: emit(I::Any); break;
        case Node::Cls: emit(I::Class, n->cls); break;
        case Node::Bol: emit(I::Bol); break;
        case Node::Eol: emit(I::Eol); break;
        case Node::WordB: emit(I::WordB); break;
        case Node::NWordB: emit(I::NWordB); break;
        case Node::Empty: break;
        case Node::Cat: for (auto& k : n->kids) gen(k.get()); break;
        case Node::Alt: {
            int s = emit(I::Split);
            rx.prog_[s].x = s + 1;
            gen(n->kids[0].get());
            int j = emit(I::Jmp);
            rx.prog_[s].y = (int)rx.prog_.size();
            gen(n->kids[1].get());
            rx.prog_[j].x = (int)rx.prog_.size();
            break;
        }
        case Node::Star: {
            int s = emit(I::Split);
            rx.prog_[s].x = s + 1;
            gen(n->kids[0].get());
            emit(I::Jmp, s);
            rx.prog_[s].y = (int)rx.prog_.size();
            break;
        }
        case Node::Plus: {
            int start = (int)rx.prog_.size();
            gen(n->kids[0].get());
            int s = emit(I::Split, start);
            rx.prog_[s].y = s + 1;
            break;
        }
        case Node::Quest: {
            int s = emit(I::Split);
            rx.prog_[s].x = s + 1;
            gen(n->kids[0].get());
            rx.prog_[s].y = (int)rx.prog_.size();
            break;
        }
        }
    }
};

bool Regex::compile(const std::string& pattern, std::string* err) {
    pattern_ = pattern;
    prog_.clear();
    classes_.clear();
    RxCompiler c(pattern, *this);
    auto root = c.parse_alt();
    if (root && c.i < pattern.size()) c.err = "unbalanced )";
    if (!root || !c.err.empty()) {
        if (err) *err = c.err.empty() ? "parse error" : c.err;
        prog_.clear();
        return false;
    }
    c.gen(root.get());
    c.emit(Inst::Match);
    return true;
}

namespace {
struct ThreadList {
    std::vector<int> pcs;
    std::vector<unsigned> mark;
};
}  // namespace

bool Regex::search(const std::string& text) const {
    if (prog_.empty()) return false;
    const size_t n = prog_.size();
    ThreadList a, b;
    a.mark.assign(n, 0);
    b.mark.assign(n, 0);
    unsigned gen = 0;
    ThreadList* cur = &a;
    ThreadList* nxt = &b;
    // explicit stack instead of recursion for epsilon closure
    std::vector<int> stack;

    auto add = [&](ThreadList* l, int pc0, size_t pos, unsigned g) -> bool {
        int prev = pos == 0 ? -1 : (unsigned char)text[pos - 1];
        int curc = pos < text.size() ? (unsigned char)text[pos] : -1;
        stack.clear();
        stack.push_back(pc0);
        while (!stack.empty()) {
            int pc = stack.back();
            stack.pop_back();
            if (l->mark[pc] == g) continue;
            l->mark[pc] = g;
            const Inst& in = prog_[pc];
            switch (in.op) {
            case Inst::Jmp: stack.push_back(in.x); break;
            case Inst::Split: stack.push_back(in.y); stack.push_back(in.x); break;
            case Inst::Bol: if (prev == -1 || prev == '\n') stack.push_back(pc + 1); break;
            case Inst::Eol: if (curc == -1 || curc == '\n' || curc == '\r') stack.push_back(pc + 1); break;
            case Inst::WordB: if (is_word(prev) != is_word(curc)) stack.push_back(pc + 1); break;
            case Inst::NWordB: if (is_word(prev) == is_word(curc)) stack.push_back(pc + 1); break;
            case Inst::Match: return true;
            default: l->pcs.push_back(pc); break;
            }
        }
        return false;
    };

    gen++;
    cur->pcs.clear();
    if (add(cur, 0, 0, gen)) return true;
    for (size_t pos = 0; pos < text.size(); pos++) {
        unsigned char ch = lc((unsigned char)text[pos]);
        gen++;
        nxt->pcs.clear();
        for (size_t k = 0; k < cur->pcs.size(); k++) {
            const Inst& in = prog_[cur->pcs[k]];
            bool ok = false;
            switch (in.op) {
            case Inst::Char: ok = in.c == ch; break;
            case Inst::Any: ok = ch != '\n'; break;
            case Inst::Class: { const CharClass& cc = classes_[in.x]; ok = cc.set[ch] != cc.neg; break; }
            default: break;
            }
            if (ok && add(nxt, cur->pcs[k] + 1, pos + 1, gen)) return true;
        }
        // unanchored: start a new thread at the next position
        if (add(nxt, 0, pos + 1, gen)) return true;
        ThreadList* t = cur; cur = nxt; nxt = t;
    }
    return false;
}

}  // namespace ag
