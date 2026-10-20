// The terminal emulator without any UI. Exit code 0: all passed.
#include <iostream>
#include <string>

#include "core/vt_screen.h"

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

// A screen row as text, trailing blanks cut.
std::string Row(const aic::VtScreen& screen, int row) {
    const size_t index = screen.ScrollbackSize() + static_cast<size_t>(row);
    std::wstring text = screen.TextBetween(index, 0, index, screen.Cols());
    const size_t last = text.find_last_not_of(L' ');
    text.resize(last == std::wstring::npos ? 0 : last + 1);
    return Narrow(text);
}

}  // namespace

int main() {
    {
        aic::VtScreen s(20, 5);
        s.Feed("hello\r\nworld");
        Expect(Row(s, 0) == "hello" && Row(s, 1) == "world", "text and newlines", Row(s, 0) + "|" + Row(s, 1));
        Expect(s.CursorRow() == 1 && s.CursorCol() == 5, "cursor after text");
    }
    {
        aic::VtScreen s(10, 3);
        s.Feed("0123456789AB");  // wraps after the tenth column
        Expect(Row(s, 0) == "0123456789" && Row(s, 1) == "AB", "autowrap", Row(s, 0) + "|" + Row(s, 1));
    }
    {
        aic::VtScreen s(10, 3);
        s.Feed("one\r\ntwo\r\nthree\r\nfour");  // the first line scrolls into the scrollback
        Expect(s.ScrollbackSize() == 1 && Row(s, 0) == "two" && Row(s, 2) == "four", "scrolling");
        Expect(Narrow(s.TextBetween(0, 0, 0, 10)).substr(0, 3) == "one", "the scrollback keeps the line");
    }
    {
        aic::VtScreen s(20, 5);
        s.Feed("\x1b[2J\x1b[3;4Hx\x1b[Hy");
        Expect(Row(s, 2) == "   x" && Row(s, 0) == "y", "cursor addressing", Row(s, 2));
    }
    {
        aic::VtScreen s(20, 3);
        s.Feed("abcdef\x1b[3D\x1b[K");  // back three, erase to the end of the line
        Expect(Row(s, 0) == "abc", "erase in line", Row(s, 0));
        s.Feed("\r\x1b[2K");
        Expect(Row(s, 0).empty(), "erase the whole line");
    }
    {
        aic::VtScreen s(20, 3);
        s.Feed("abcdef\r\x1b[2P");
        Expect(Row(s, 0) == "cdef", "delete characters", Row(s, 0));
        s.Feed("\r\x1b[2@");
        Expect(Row(s, 0) == "  cdef", "insert blanks", Row(s, 0));
        s.Feed("\r\x1b[3X");
        Expect(Row(s, 0) == "   def", "erase characters", Row(s, 0));
    }
    {
        aic::VtScreen s(10, 4);
        s.Feed("1\r\n2\r\n3\r\n4\x1b[2;3r");  // the region is rows 2..3, the cursor goes home
        s.Feed("\x1b[3;1H\r\nX");              // a line feed at the region's bottom scrolls only the region
        Expect(Row(s, 0) == "1" && Row(s, 3) == "4", "rows outside the region stay", Row(s, 0) + "|" + Row(s, 3));
        Expect(Row(s, 1) == "3" && Row(s, 2) == "X", "the region scrolls", Row(s, 1) + "|" + Row(s, 2));
        Expect(s.ScrollbackSize() == 0, "a region below the top does not feed the scrollback");
    }
    {
        aic::VtScreen s(20, 3);
        s.Feed("\x1b[31mred\x1b[0m \x1b[1;38;2;1;2;3mrgb\x1b[48;5;196mx");
        const aic::VtLine& line = s.Line(s.ScrollbackSize());
        Expect(line[0].fg == 0xc50f1f, "ANSI color");
        Expect(line[3].fg == aic::kVtDefault, "reset");
        Expect(line[4].fg == 0x010203 && (line[4].attrs & aic::kVtBold) != 0, "true color and bold");
        Expect(line[7].bg == 0xff0000, "256-color background");
    }
    {
        aic::VtScreen s(20, 3);
        s.Feed("привет \xF0\x9F\x98\x80z");  // Cyrillic, then an emoji (two columns), then z
        const aic::VtLine& line = s.Line(s.ScrollbackSize());
        Expect(line[0].ch == U'п' && line[5].ch == U'т', "UTF-8 text");
        Expect(line[7].ch == 0x1F600 && line[8].ch == 0 && line[9].ch == U'z', "a wide character takes two cells");
    }
    {
        aic::VtScreen s(20, 3);
        s.Feed("ab\xd0");  // a character cut between two reads
        s.Feed("\xbf" "c");
        Expect(Row(s, 0) == "ab?c", "UTF-8 split across Feed calls", Row(s, 0));
    }
    {
        aic::VtScreen s(20, 3);
        std::string reply;
        s.SetReply([&](std::string text) { reply += text; });
        s.Feed("ab\x1b[6n");
        Expect(reply == "\x1b[1;3R", "cursor position report", reply);
        reply.clear();
        s.Feed("\x1b[c");
        Expect(reply.starts_with("\x1b[?"), "device attributes");
    }
    {
        aic::VtScreen s(20, 3);
        s.Feed("main\x1b[?1049h");
        Expect(s.AltScreen() && Row(s, 0).empty(), "the alternate screen is empty");
        s.Feed("alt");
        Expect(Row(s, 0) == "alt", "text on the alternate screen");
        s.Feed("\x1b[?1049l");
        Expect(!s.AltScreen() && Row(s, 0) == "main" && s.CursorCol() == 4, "the main screen comes back", Row(s, 0));
    }
    {
        aic::VtScreen s(20, 3);
        s.Feed("\x1b[?25l\x1b[?2004h\x1b[?1h");
        Expect(!s.CursorVisible() && s.BracketedPaste() && s.AppCursorKeys(), "private modes");
        s.Feed("\x1b[?25h");
        Expect(s.CursorVisible(), "cursor shown again");
    }
    {
        aic::VtScreen s(20, 3);
        s.Feed("\x1b]0;My title\x07" "text\x1b]2;Other\x1b\\");
        Expect(Row(s, 0) == "text" && s.Title() == L"Other", "OSC titles are consumed", Row(s, 0));
    }
    {
        aic::VtScreen s(20, 4);
        s.Feed("a\r\nb\r\nc\r\nd");
        s.Resize(10, 2);  // the lines above the cursor go to the scrollback
        Expect(s.Rows() == 2 && s.Cols() == 10 && Row(s, 1) == "d" && s.ScrollbackSize() == 2, "shrinking", Row(s, 1));
        s.Resize(30, 6);
        Expect(s.Rows() == 6 && s.Cols() == 30, "growing");
    }
    {
        aic::VtScreen s(20, 3);
        s.Feed("abc\x1b[3;1H\n");
        Expect(s.ScrollbackSize() == 1, "a line feed on the last row scrolls");
        s.Feed("\x1b[1;1H\x1bM");  // reverse index at the top scrolls down
        Expect(Row(s, 0).empty(), "reverse index");
    }
    std::cout << (failures == 0 ? "all passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
