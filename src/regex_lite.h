// Tiny case-insensitive regex (Thompson NFA / Pike VM: linear time, no deep recursion on input).
// Syntax: literals, . [abc] [^a-z] \s \S \d \D \w \W \b \B ^ $ ( ) | * + ? and \x to escape x.
// ^ and $ also match at line breaks. Search is unanchored (matches anywhere).
#pragma once
#include <string>
#include <vector>

namespace ag {

class Regex {
public:
    bool compile(const std::string& pattern, std::string* err);
    bool search(const std::string& text) const;
    const std::string& pattern() const { return pattern_; }

    struct Inst {
        enum Op { Char, Any, Class, Split, Jmp, Match, Bol, Eol, WordB, NWordB } op;
        unsigned char c = 0;
        int x = 0, y = 0;  // jump targets / class index
    };
    struct CharClass { bool neg = false; bool set[256] = {}; };

private:
    std::string pattern_;
    std::vector<Inst> prog_;
    std::vector<CharClass> classes_;
    friend struct RxCompiler;
};

}  // namespace ag
