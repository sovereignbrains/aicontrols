#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aic {

inline constexpr uint32_t kVtDefault = 0xff000000;  // "the theme's color": a real color is 0xRRGGBB

enum VtAttr : uint8_t {
    kVtBold = 1,
    kVtDim = 2,
    kVtItalic = 4,
    kVtUnderline = 8,
    kVtInverse = 16,
    kVtStrike = 32,
};

struct VtCell {
    char32_t ch = U' ';  // 0: the right half of a wide character
    uint32_t fg = kVtDefault;
    uint32_t bg = kVtDefault;
    uint8_t attrs = 0;
    bool operator==(const VtCell&) const = default;
};
using VtLine = std::vector<VtCell>;

// Columns a character takes: 0 for combining marks, 2 for East Asian wide characters and emoji, else 1.
int VtCharWidth(char32_t c);

// A terminal screen fed with the bytes a console program writes (VT100/xterm subset, enough for
// PowerShell under ConPTY, less, vim-like programs): text, colors, cursor movement, erasing, scroll
// regions, the alternate screen, and a scrollback. Not thread-safe: one thread feeds and reads.
class VtScreen {
public:
    using Reply = std::function<void(std::string)>;  // what the terminal answers to the program (cursor report ...)

    VtScreen(int cols, int rows, size_t scrollbackLimit = 5000);

    void SetReply(Reply reply) { reply_ = std::move(reply); }
    void Feed(std::string_view bytes);
    void Resize(int cols, int rows);

    int Cols() const { return cols_; }
    int Rows() const { return rows_; }
    size_t ScrollbackSize() const { return alt_ ? 0 : scrollback_.size(); }
    size_t LineCount() const { return ScrollbackSize() + static_cast<size_t>(rows_); }
    const VtLine& Line(size_t index) const;  // 0 is the oldest line in the scrollback
    int CursorRow() const { return curRow_; }  // on the screen, not counting the scrollback
    int CursorCol() const { return curCol_; }
    bool CursorVisible() const { return cursorVisible_; }
    bool AppCursorKeys() const { return appCursor_; }
    bool BracketedPaste() const { return bracketedPaste_; }
    bool AltScreen() const { return alt_; }
    const std::wstring& Title() const { return title_; }
    void ClearScrollback() { scrollback_.clear(); }  // the lines above the screen; the screen stays

    // The text between two points (line index, column), the end exclusive; rows joined by \n, trailing blanks cut.
    std::wstring TextBetween(size_t fromLine, int fromCol, size_t toLine, int toCol) const;

private:
    enum class Mode { Ground, Escape, EscapeSkip, Csi, Osc, OscEscape, Skip, SkipEscape };

    void Control(unsigned char c);
    void Print(char32_t c);
    void EscapeFinal(unsigned char c);
    void CsiFinal(unsigned char c);
    void Sgr();
    void SetMode(bool on);
    void OscEnd();

    int Param(size_t index, int fallback) const;
    VtLine Blank() const;
    void LineFeed();
    void ReverseIndex();
    void ScrollUp(int count);
    void ScrollDown(int count);
    void EraseLine(int from, int to);  // columns [from, to) of the cursor's row
    void SaveCursor();
    void RestoreCursor();
    void EnterAlt(bool on);
    void Reset();
    void Answer(std::string text);

    int cols_;
    int rows_;
    size_t scrollbackLimit_;
    std::vector<VtLine> grid_;
    std::vector<VtLine> saved_;  // the main screen while the alternate one is shown
    std::deque<VtLine> scrollback_;
    int curRow_ = 0;
    int curCol_ = 0;
    bool wrapPending_ = false;
    int top_ = 0;
    int bottom_;
    VtCell pen_;
    int savedRow_ = 0;
    int savedCol_ = 0;
    VtCell savedPen_;
    bool cursorVisible_ = true;
    bool appCursor_ = false;
    bool bracketedPaste_ = false;
    bool autowrap_ = true;
    bool alt_ = false;
    std::wstring title_;

    Mode mode_ = Mode::Ground;
    std::vector<int> params_;
    int current_ = -1;
    char prefix_ = 0;
    std::string osc_;
    std::string utf8_;
    size_t utf8Need_ = 0;
    Reply reply_;
};

}  // namespace aic
