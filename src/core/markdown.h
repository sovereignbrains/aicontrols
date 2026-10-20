#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aic {

// What a run of text looks like. Spans may overlap (bold inside a link); each flag is its own property.
enum MdFlag : unsigned {
    kMdBold = 1,
    kMdItalic = 2,
    kMdCode = 4,
    kMdLink = 8,
    kMdQuote = 16,   // quotes and rules: toned down
    kMdHeading = 32,
};

struct MdSpan {
    size_t start = 0;  // in UTF-16 units of MdText::text
    size_t length = 0;
    unsigned flags = 0;
    float scale = 1.f;   // headings: the size relative to the body
    bool block = false;  // kMdCode only: a fenced block (the lines get a panel), not inline code
};

// The text to show, without the markup, and how its runs are styled.
struct MdText {
    std::wstring text;
    std::vector<MdSpan> spans;
};

// A forgiving subset: # headings, **bold**, *italic*, `code`, ``` fences, - lists, > quotes, [links](url),
// --- rules. Markup that is not closed (a message still streaming) stays as typed.
MdText ParseMarkdown(std::wstring_view source);

}  // namespace aic
