#include "core/highlight.h"

#include <algorithm>
#include <cwctype>
#include <set>
#include <string>

namespace aic {
namespace {

using Words = std::set<std::wstring_view>;

const Words& CWords() {
    static const Words words{
        L"abstract", L"alignas", L"as", L"async", L"auto", L"await", L"bool", L"boolean", L"break", L"byte",
        L"case", L"catch", L"chan", L"char", L"class", L"const", L"constexpr", L"continue", L"crate", L"data",
        L"debugger", L"default", L"defer", L"delete", L"do", L"double", L"dyn", L"else", L"enum", L"explicit",
        L"export", L"extends", L"extern", L"false", L"final", L"finally", L"float", L"fn", L"for", L"foreach",
        L"fun", L"func", L"function", L"go", L"goto", L"if", L"impl", L"implements", L"import", L"in",
        L"inline", L"instanceof", L"int", L"interface", L"is", L"let", L"long", L"loop", L"map", L"match",
        L"mod", L"module", L"move", L"mut", L"mutable", L"namespace", L"new", L"nil", L"noexcept", L"null",
        L"nullptr", L"object", L"of", L"operator", L"override", L"package", L"private", L"protected",
        L"protocol", L"pub", L"public", L"range", L"readonly", L"record", L"ref", L"return", L"sealed",
        L"select", L"self", L"short", L"signed", L"sizeof", L"static", L"string", L"struct", L"super",
        L"switch", L"template", L"this", L"throw", L"throws", L"trait", L"true", L"try", L"type", L"typedef",
        L"typeof", L"undefined", L"union", L"unsafe", L"unsigned", L"use", L"using", L"val", L"var",
        L"virtual", L"void", L"volatile", L"when", L"where", L"while", L"with", L"yield"};
    return words;
}

const Words& HashWords() {  // Python, Ruby, YAML and friends
    static const Words words{
        L"False", L"None", L"True", L"and", L"as", L"assert", L"async", L"await", L"begin", L"break",
        L"class", L"continue", L"def", L"del", L"do", L"elif", L"elsif", L"else", L"end", L"ensure",
        L"except", L"false", L"finally", L"for", L"from", L"global", L"if", L"import", L"in", L"is",
        L"lambda", L"module", L"nil", L"nonlocal", L"not", L"null", L"or", L"pass", L"puts", L"raise",
        L"require", L"rescue", L"return", L"self", L"then", L"true", L"try", L"unless", L"until", L"while",
        L"with", L"yield"};
    return words;
}

const Words& ShellWords() {
    static const Words words{
        L"alias", L"break", L"case", L"cd", L"continue", L"declare", L"do", L"done", L"echo", L"elif",
        L"else", L"esac", L"eval", L"exec", L"exit", L"export", L"false", L"fi", L"for", L"function", L"if",
        L"in", L"local", L"readonly", L"return", L"select", L"set", L"shift", L"source", L"then", L"trap",
        L"true", L"unset", L"until", L"while"};
    return words;
}

const Words& PowerShellWords() {  // lower case: matched without regard to case
    static const Words words{
        L"begin", L"break", L"catch", L"class", L"continue", L"data", L"do", L"dynamicparam", L"else",
        L"elseif", L"end", L"enum", L"exit", L"filter", L"finally", L"for", L"foreach", L"from", L"function",
        L"if", L"in", L"param", L"process", L"return", L"switch", L"throw", L"trap", L"try", L"until",
        L"using", L"while", L"workflow"};
    return words;
}

const Words& SqlWords() {  // lower case, as above
    static const Words words{
        L"all", L"alter", L"and", L"as", L"asc", L"begin", L"between", L"by", L"case", L"check", L"commit",
        L"constraint", L"create", L"cross", L"database", L"default", L"delete", L"desc", L"distinct", L"drop",
        L"else", L"end", L"exists", L"false", L"foreign", L"from", L"full", L"group", L"having", L"in",
        L"index", L"inner", L"insert", L"into", L"is", L"join", L"key", L"left", L"like", L"limit", L"not",
        L"null", L"offset", L"on", L"or", L"order", L"outer", L"primary", L"references", L"right",
        L"rollback", L"select", L"set", L"table", L"then", L"true", L"truncate", L"union", L"unique",
        L"update", L"values", L"view", L"when", L"where", L"with"};
    return words;
}

struct Syntax {
    const Words* words = nullptr;
    std::wstring_view lineComment;
    bool blockC = false;       // /* ... */
    bool blockPs = false;      // <# ... #>
    bool ignoreCase = false;
    bool dollarVars = false;   // $name, ${name}
    bool backtickStrings = false;
    bool tripleQuotes = false;
    bool shortChars = false;   // 'x' is a character, not a string: it closes within a few units or it is no token
    bool commentAfterSpace = false;  // # starts a comment only at a line's start or after a space
    wchar_t escape = L'\\';    // 0: none
    bool diff = false;
};

const Syntax kCLike{&CWords(), L"//", true, false, false, false, true, false, false, false, L'\\', false};
const Syntax kCShort{&CWords(), L"//", true, false, false, false, false, false, true, false, L'\\', false};
const Syntax kHash{&HashWords(), L"#", false, false, false, false, false, true, false, true, L'\\', false};
const Syntax kShell{&ShellWords(), L"#", false, false, false, true, false, false, false, true, L'\\', false};
const Syntax kPowerShell{&PowerShellWords(), L"#", false, true, true, true, false, false, false, true, L'`', false};
const Syntax kSql{&SqlWords(), L"--", true, false, true, false, false, false, false, false, 0, false};
const Syntax kDiff{nullptr, {}, false, false, false, false, false, false, false, false, 0, true};

const Syntax* Lookup(std::wstring_view language) {
    size_t end = 0;
    while (end < language.size() && !std::iswspace(language[end]) && language[end] != L'{' && language[end] != L',') {
        ++end;
    }
    std::wstring name(language.substr(0, end));
    for (wchar_t& c : name) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    static const std::pair<std::wstring_view, const Syntax*> table[] = {
        {L"js", &kCLike},        {L"javascript", &kCLike}, {L"jsx", &kCLike},   {L"mjs", &kCLike},
        {L"ts", &kCLike},        {L"typescript", &kCLike}, {L"tsx", &kCLike},   {L"php", &kCLike},
        {L"json", &kCLike},      {L"jsonc", &kCLike},      {L"c", &kCShort},    {L"cpp", &kCShort},
        {L"c++", &kCShort},      {L"cc", &kCShort},        {L"cxx", &kCShort},  {L"h", &kCShort},
        {L"hpp", &kCShort},      {L"cs", &kCShort},        {L"csharp", &kCShort}, {L"java", &kCShort},
        {L"go", &kCShort},       {L"golang", &kCShort},    {L"rust", &kCShort}, {L"rs", &kCShort},
        {L"kotlin", &kCShort},   {L"kt", &kCShort},        {L"swift", &kCShort}, {L"scala", &kCShort},
        {L"dart", &kCShort},     {L"py", &kHash},          {L"python", &kHash}, {L"python3", &kHash},
        {L"yaml", &kHash},       {L"yml", &kHash},         {L"toml", &kHash},   {L"rb", &kHash},
        {L"ruby", &kHash},       {L"dockerfile", &kHash},  {L"ini", &kHash},    {L"sh", &kShell},
        {L"bash", &kShell},      {L"shell", &kShell},      {L"zsh", &kShell},   {L"powershell", &kPowerShell},
        {L"ps1", &kPowerShell},  {L"pwsh", &kPowerShell},  {L"ps", &kPowerShell}, {L"sql", &kSql},
        {L"diff", &kDiff},       {L"patch", &kDiff}};
    for (const auto& [key, syntax] : table) {
        if (key == name) {
            return syntax;
        }
    }
    return nullptr;
}

bool IsIdentStart(wchar_t c) {
    return std::iswalpha(c) != 0 || c == L'_';
}

bool IsIdent(wchar_t c) {
    return std::iswalnum(c) != 0 || c == L'_';
}

void Diff(std::wstring_view code, std::vector<Token>& out) {
    size_t pos = 0;
    while (pos < code.size()) {
        size_t eol = code.find(L'\n', pos);
        if (eol == std::wstring_view::npos) {
            eol = code.size();
        }
        const std::wstring_view line = code.substr(pos, eol - pos);
        TokenKind kind = TokenKind::None;
        if (line.starts_with(L"+++") || line.starts_with(L"---") || line.starts_with(L"@@") ||
            line.starts_with(L"diff ") || line.starts_with(L"index ")) {
            kind = TokenKind::Keyword;
        } else if (line.starts_with(L'+')) {
            kind = TokenKind::Added;
        } else if (line.starts_with(L'-')) {
            kind = TokenKind::Removed;
        }
        if (kind != TokenKind::None && !line.empty()) {
            out.push_back(Token{pos, line.size(), kind});
        }
        pos = eol + 1;
    }
}

}  // namespace

std::vector<Token> Highlight(std::wstring_view code, std::wstring_view language) {
    std::vector<Token> out;
    const Syntax* syntax = Lookup(language);
    if (syntax == nullptr) {
        return out;
    }
    if (syntax->diff) {
        Diff(code, out);
        return out;
    }
    const Syntax& s = *syntax;
    const size_t n = code.size();
    const auto add = [&](size_t from, size_t to, TokenKind kind) { out.push_back(Token{from, to - from, kind}); };
    size_t i = 0;
    while (i < n) {
        const wchar_t c = code[i];
        const std::wstring_view tail = code.substr(i);

        if (s.dollarVars && c == L'$' && i + 1 < n) {
            size_t j = i + 1;
            if (code[j] == L'{') {
                const size_t close = code.find(L'}', j);
                j = close == std::wstring_view::npos ? n : close + 1;
            } else if (IsIdent(code[j])) {
                while (j < n && (IsIdent(code[j]) || (code[j] == L':' && j + 1 < n && IsIdent(code[j + 1])))) {
                    ++j;
                }
            } else if (std::wstring_view(L"?#@!$*").find(code[j]) != std::wstring_view::npos) {
                ++j;
            }
            if (j > i + 1) {
                add(i, j, TokenKind::Variable);
                i = j;
                continue;
            }
        }

        if (s.blockC && tail.starts_with(L"/*")) {
            const size_t close = code.find(L"*/", i + 2);
            const size_t stop = close == std::wstring_view::npos ? n : close + 2;
            add(i, stop, TokenKind::Comment);
            i = stop;
            continue;
        }
        if (s.blockPs && tail.starts_with(L"<#")) {
            const size_t close = code.find(L"#>", i + 2);
            const size_t stop = close == std::wstring_view::npos ? n : close + 2;
            add(i, stop, TokenKind::Comment);
            i = stop;
            continue;
        }
        if (!s.lineComment.empty() && tail.starts_with(s.lineComment) &&
            (!s.commentAfterSpace || i == 0 || std::iswspace(code[i - 1]))) {
            const size_t eol = code.find(L'\n', i);
            const size_t stop = eol == std::wstring_view::npos ? n : eol;
            add(i, stop, TokenKind::Comment);
            i = stop;
            continue;
        }

        if (c == L'"' || c == L'\'' || (c == L'`' && s.backtickStrings)) {
            if (s.tripleQuotes && tail.size() >= 3 && tail[1] == c && tail[2] == c) {
                const std::wstring marker(3, c);
                const size_t close = code.find(marker, i + 3);
                const size_t stop = close == std::wstring_view::npos ? n : close + 3;
                add(i, stop, TokenKind::String);
                i = stop;
                continue;
            }
            size_t j = i + 1;
            bool closed = false;
            while (j < n) {
                if (s.escape != 0 && code[j] == s.escape && j + 1 < n) {
                    j += 2;
                } else if (code[j] == c) {
                    ++j;
                    closed = true;
                    break;
                } else if (code[j] == L'\n' && c != L'`') {
                    break;
                } else {
                    ++j;
                }
            }
            j = std::min(j, n);
            if (c == L'\'' && s.shortChars && (!closed || j - i > 4)) {  // a lifetime or a stray quote, not a character
                ++i;
                continue;
            }
            add(i, j, TokenKind::String);
            i = j;
            continue;
        }

        if (std::iswdigit(c) != 0 && (i == 0 || !IsIdent(code[i - 1]))) {
            size_t j = i;
            while (j < n && (std::iswalnum(code[j]) != 0 || code[j] == L'_' ||
                             (code[j] == L'.' && j + 1 < n && std::iswdigit(code[j + 1]) != 0))) {
                ++j;
            }
            add(i, j, TokenKind::Number);
            i = j;
            continue;
        }

        if (IsIdentStart(c)) {
            size_t j = i;
            while (j < n && IsIdent(code[j])) {
                ++j;
            }
            if (s.words != nullptr) {
                std::wstring word(code.substr(i, j - i));
                if (s.ignoreCase) {
                    for (wchar_t& w : word) {
                        w = static_cast<wchar_t>(std::towlower(w));
                    }
                }
                if (s.words->contains(std::wstring_view(word))) {
                    add(i, j, TokenKind::Keyword);
                }
            }
            i = j;
            continue;
        }
        ++i;
    }
    return out;
}

}  // namespace aic
