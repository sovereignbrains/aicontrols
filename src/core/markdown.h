#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/highlight.h"

namespace aic {

// What a run of text looks like. Spans may overlap (bold inside a link); each flag is its own property.
enum MdFlag : unsigned {
    kMdBold = 1,
    kMdItalic = 2,
    kMdCode = 4,
    kMdLink = 8,
    kMdQuote = 16,   // quotes and rules: toned down
    kMdHeading = 32,
    kMdToken = 64,   // a colored part of a fenced block (see MdSpan::token)
    kMdTable = 128,  // with kMdCode and block: a table, aligned in the code font
};

struct MdSpan {
    size_t start = 0;  // in UTF-16 units of MdText::text
    size_t length = 0;
    unsigned flags = 0;
    float scale = 1.f;   // headings: the size relative to the body
    bool block = false;  // kMdCode only: a fenced block (the lines get a panel), not inline code
    TokenKind token = TokenKind::None;  // kMdToken only
    std::wstring url;                   // kMdLink only: where it goes (also for bare http(s):// addresses)
};

// A path on this computer: C:\..., C:/... or \\server\share. Such `code` and bare ones become links to the file.
bool IsLocalPath(std::wstring_view text);

// The text to show, without the markup, and how its runs are styled.
struct MdText {
    std::wstring text;
    std::vector<MdSpan> spans;
};

// A forgiving subset: # headings, **bold**, *italic*, `code`, ``` fences (colored by their language), - lists,
// > quotes, [links](url) and bare http(s):// addresses, | tables |, --- rules. Markup that is not closed (a message still streaming) stays as typed.
// `tableWidth`: how many characters of the code font a line holds; wider tables wrap inside their cells (0: no limit).
MdText ParseMarkdown(std::wstring_view source, size_t tableWidth = 0);

}  // namespace aic
