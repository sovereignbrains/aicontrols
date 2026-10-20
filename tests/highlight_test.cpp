// The code colors, without any UI. Exit code 0: all passed.
#include <iostream>
#include <string>

#include "core/highlight.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

// Whether `kind` colors exactly `text` somewhere in `code`.
bool Has(std::wstring_view code, std::wstring_view language, aic::TokenKind kind, std::wstring_view text) {
    for (const aic::Token& token : aic::Highlight(code, language)) {
        if (token.kind == kind && code.substr(token.start, token.length) == text) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    using aic::TokenKind;
    Expect(Has(L"int x = 42; // note", L"cpp", TokenKind::Keyword, L"int"), "cpp keyword");
    Expect(Has(L"int x = 42; // note", L"cpp", TokenKind::Number, L"42"), "cpp number");
    Expect(Has(L"int x = 42; // note", L"cpp", TokenKind::Comment, L"// note"), "cpp line comment");
    Expect(!Has(L"print = 1;", L"cpp", TokenKind::Keyword, L"int"), "a keyword inside a word is not one");
    Expect(Has(L"a /* one\ntwo */ b", L"c", TokenKind::Comment, L"/* one\ntwo */"), "block comment across lines");
    Expect(Has(L"s = \"a // not a comment\"", L"js", TokenKind::String, L"\"a // not a comment\""), "string hides comment");
    Expect(Has(L"s = \"say \\\"hi\\\"\";", L"js", TokenKind::String, L"\"say \\\"hi\\\"\""), "escaped quote");
    Expect(Has(L"char c = 'x';", L"cpp", TokenKind::String, L"'x'"), "char literal");
    Expect(!Has(L"fn f<'a>(x: &'a str) {}", L"rust", TokenKind::String, L"'a>(x: &'"), "a lifetime is no string");
    Expect(Has(L"let s = 'long string here';", L"js", TokenKind::String, L"'long string here'"), "js single quotes");
    Expect(Has(L"\"unclosed\nnext", L"js", TokenKind::String, L"\"unclosed"), "unclosed string stops at the line end");

    Expect(Has(L"def f(): # hi\n  return None", L"python", TokenKind::Comment, L"# hi"), "python comment");
    Expect(Has(L"def f(): # hi\n  return None", L"python", TokenKind::Keyword, L"None"), "python None");
    Expect(Has(L"x = \"\"\"a\nb\"\"\"", L"py", TokenKind::String, L"\"\"\"a\nb\"\"\""), "triple quotes");
    Expect(!Has(L"curl http://x/#frag", L"bash", TokenKind::Comment, L"#frag"), "# glued to a word is no comment");
    Expect(Has(L"echo $HOME ${x}", L"bash", TokenKind::Variable, L"$HOME"), "shell variable");
    Expect(Has(L"echo $HOME ${x}", L"bash", TokenKind::Variable, L"${x}"), "braced variable");
    Expect(Has(L"Get-Item $env:Path\nForEach ($i in 1)", L"powershell", TokenKind::Variable, L"$env:Path"), "ps variable");
    Expect(Has(L"ForEach ($i in 1)", L"ps1", TokenKind::Keyword, L"ForEach"), "ps keywords ignore case");
    Expect(Has(L"<# doc #> x", L"pwsh", TokenKind::Comment, L"<# doc #>"), "ps block comment");
    Expect(Has(L"SELECT a FROM t -- all", L"sql", TokenKind::Keyword, L"SELECT"), "sql keyword, any case");
    Expect(Has(L"SELECT a FROM t -- all", L"sql", TokenKind::Comment, L"-- all"), "sql comment");

    Expect(Has(L"@@ -1 +1 @@\n-old\n+new\n ctx", L"diff", TokenKind::Removed, L"-old"), "diff removed");
    Expect(Has(L"@@ -1 +1 @@\n-old\n+new\n ctx", L"diff", TokenKind::Added, L"+new"), "diff added");
    Expect(Has(L"--- a\n+++ b", L"diff", TokenKind::Keyword, L"+++ b"), "diff header");

    Expect(aic::Highlight(L"int x = 42;", L"").empty(), "no language, no tokens");
    Expect(aic::Highlight(L"int x = 42;", L"brainfuck").empty(), "unknown language, no tokens");
    Expect(Has(L"int x = 1;", L"CPP title=\"a\"", TokenKind::Keyword, L"int"), "language is the first word, any case");

    size_t end = 0;  // tokens are ordered and apart
    bool ordered = true;
    const std::wstring_view mixed = L"for (int i = 0; i < 10; ++i) { s += \"a\"; } // done";
    for (const aic::Token& token : aic::Highlight(mixed, L"cpp")) {
        ordered = ordered && token.start >= end && token.length > 0 && token.start + token.length <= mixed.size();
        end = token.start + token.length;
    }
    Expect(ordered, "tokens are ordered, apart and inside the code");

    std::cout << (failures == 0 ? "all passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
