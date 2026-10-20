// The markdown subset, without any UI. Exit code 0: all passed.
#include <iostream>
#include <string>

#include "core/markdown.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what, const std::string& detail = {}) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << (detail.empty() ? "" : "\n  " + detail) << "\n";
    }
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (const wchar_t c : text) {
        out += c < 128 ? static_cast<char>(c) : '?';
    }
    return out;
}

// The text a span covers.
std::wstring Cover(const aic::MdText& md, const aic::MdSpan& span) {
    return md.text.substr(span.start, span.length);
}

const aic::MdSpan* Find(const aic::MdText& md, unsigned flag) {
    for (const aic::MdSpan& span : md.spans) {
        if ((span.flags & flag) != 0) {
            return &span;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    using aic::ParseMarkdown;
    {
        const auto md = ParseMarkdown(L"plain **bold** and *it* and `code` end");
        Expect(md.text == L"plain bold and it and code end", "markup is removed", Narrow(md.text));
        const auto* bold = Find(md, aic::kMdBold);
        const auto* italic = Find(md, aic::kMdItalic);
        const auto* code = Find(md, aic::kMdCode);
        Expect(bold && Cover(md, *bold) == L"bold", "bold span");
        Expect(italic && Cover(md, *italic) == L"it", "italic span");
        Expect(code && Cover(md, *code) == L"code" && !code->block, "inline code span");
    }
    {
        const auto md = ParseMarkdown(L"**bold *and italic***");
        Expect(md.text == L"bold and italic", "nesting", Narrow(md.text));
        const auto* italic = Find(md, aic::kMdItalic);
        Expect(italic && Cover(md, *italic) == L"and italic", "italic inside bold");
    }
    {
        const auto md = ParseMarkdown(L"snake_case_name and 2*3*4 and a_b");
        Expect(Find(md, aic::kMdItalic) == nullptr || Cover(md, *Find(md, aic::kMdItalic)) != L"case",
               "underscores inside words are not italics", Narrow(md.text));
        Expect(md.text.find(L"snake_case_name") != std::wstring::npos, "snake_case stays");
    }
    {
        const auto md = ParseMarkdown(L"**not closed and `neither");
        Expect(md.text == L"**not closed and `neither" && md.spans.empty(), "unclosed markup stays as typed", Narrow(md.text));
    }
    {
        const auto md = ParseMarkdown(L"# Title\ntext\n## Sub\n");
        Expect(md.text == L"Title\ntext\n\nSub", "headings", Narrow(md.text));
        const auto* heading = Find(md, aic::kMdHeading);
        Expect(heading && Cover(md, *heading) == L"Title" && heading->scale > 1.3f, "heading span and size");
    }
    {
        const auto md = ParseMarkdown(L"- one\n- two **b**\n  - nested\n1. first");
        Expect(md.text == L"• one\n• two b\n  • nested\n1. first", "lists", Narrow(md.text));
    }
    {
        const auto md = ParseMarkdown(L"before\n```cpp\nint a = 1;\n  return *p;\n```\nafter");
        Expect(md.text == L"before\n\nint a = 1;\n  return *p;\n\nafter", "fenced block, markup inside kept", Narrow(md.text));
        const auto* code = Find(md, aic::kMdCode);
        Expect(code && code->block && Cover(md, *code) == L"int a = 1;\n  return *p;", "block span", Narrow(md.text));
    }
    {
        const auto md = ParseMarkdown(L"```\nstreaming, never closed\nmore");
        const auto* code = Find(md, aic::kMdCode);
        Expect(code && code->block && Cover(md, *code) == L"streaming, never closed\nmore", "an open fence is a block");
    }
    {
        const auto md = ParseMarkdown(L"> quoted *text*\n\nsee [the docs](http://x.y) now");
        Expect(md.text == L"quoted text\n\nsee the docs now", "quote and link", Narrow(md.text));
        const auto* link = Find(md, aic::kMdLink);
        Expect(link && Cover(md, *link) == L"the docs", "link label");
        Expect(Find(md, aic::kMdQuote) != nullptr, "quote span");
    }
    {
        const auto md = ParseMarkdown(L"a\n\n\n\nb\n\n");
        Expect(md.text == L"a\n\nb", "blank lines collapse, the end is trimmed", Narrow(md.text));
    }
    {
        const auto md = ParseMarkdown(L"\\*not italic\\* and \\`tick");
        Expect(md.text == L"*not italic* and `tick" && md.spans.empty(), "escapes", Narrow(md.text));
    }
    {
        const auto md = ParseMarkdown(L"win\r\nlines\r\n");
        Expect(md.text == L"win\nlines", "CRLF", Narrow(md.text));
        Expect(ParseMarkdown(L"").text.empty(), "empty");
    }
    std::cout << (failures == 0 ? "all passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
