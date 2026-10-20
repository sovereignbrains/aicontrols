#include "core/vt_screen.h"

#include <algorithm>
#include <array>

#include "core/claude_process.h"

namespace aic {
namespace {

// The Campbell palette (Windows Terminal's default).
constexpr std::array<uint32_t, 16> kAnsi = {0x0c0c0c, 0xc50f1f, 0x13a10e, 0xc19c00, 0x0037da, 0x881798,
                                            0x3a96dd, 0xcccccc, 0x767676, 0xe74856, 0x16c60c, 0xf9f1a5,
                                            0x3b78ff, 0xb4009e, 0x61d6d6, 0xf2f2f2};

uint32_t Palette(int index) {
    if (index < 0) {
        return kVtDefault;
    }
    if (index < 16) {
        return kAnsi[static_cast<size_t>(index)];
    }
    if (index < 232) {
        constexpr uint32_t levels[] = {0, 95, 135, 175, 215, 255};
        const auto i = static_cast<uint32_t>(index - 16);
        return levels[i / 36] << 16 | levels[(i / 6) % 6] << 8 | levels[i % 6];
    }
    if (index < 256) {
        const auto v = static_cast<uint32_t>(8 + (index - 232) * 10);
        return v << 16 | v << 8 | v;
    }
    return kVtDefault;
}

// Columns a character takes: 0 for combining marks, 2 for East Asian wide and emoji, else 1.
int Width(char32_t c) {
    if ((c >= 0x0300 && c <= 0x036f) || (c >= 0x200b && c <= 0x200f) || (c >= 0xfe00 && c <= 0xfe0f) || c == 0x20e3) {
        return 0;
    }
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3) ||
        (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe6f) || (c >= 0xff00 && c <= 0xff60) ||
        (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1f64f) || (c >= 0x1f900 && c <= 0x1f9ff) ||
        (c >= 0x20000 && c <= 0x3fffd)) {
        return 2;
    }
    return 1;
}

void AppendUtf16(std::wstring& out, char32_t c) {
    if (c > 0xffff) {
        const char32_t v = c - 0x10000;
        out += static_cast<wchar_t>(0xd800 + (v >> 10));
        out += static_cast<wchar_t>(0xdc00 + (v & 0x3ff));
    } else {
        out += static_cast<wchar_t>(c);
    }
}

}  // namespace

int VtCharWidth(char32_t c) {
    return Width(c);
}

VtScreen::VtScreen(int cols, int rows, size_t scrollbackLimit)
    : cols_(std::max(1, cols)), rows_(std::max(1, rows)), scrollbackLimit_(scrollbackLimit), bottom_(rows_ - 1) {
    grid_.assign(static_cast<size_t>(rows_), Blank());
}

const VtLine& VtScreen::Line(size_t index) const {
    static const VtLine empty;
    const size_t back = ScrollbackSize();
    if (index < back) {
        return scrollback_[index];
    }
    const size_t row = index - back;
    return row < grid_.size() ? grid_[row] : empty;
}

VtLine VtScreen::Blank() const {
    return VtLine(static_cast<size_t>(cols_), VtCell{U' ', kVtDefault, pen_.bg, 0});
}

int VtScreen::Param(size_t index, int fallback) const {
    return index < params_.size() && params_[index] >= 0 ? params_[index] : fallback;
}

void VtScreen::Answer(std::string text) {
    if (reply_) {
        reply_(std::move(text));
    }
}

void VtScreen::Feed(std::string_view bytes) {
    for (const char raw : bytes) {
        const auto b = static_cast<unsigned char>(raw);
        switch (mode_) {
        case Mode::Ground:
            if (utf8Need_ > 0) {
                if ((b & 0xc0) == 0x80) {
                    utf8_ += raw;
                    if (--utf8Need_ == 0) {
                        const auto lead = static_cast<unsigned char>(utf8_[0]);
                        auto cp = static_cast<char32_t>(utf8_.size() == 2 ? lead & 0x1f : (utf8_.size() == 3 ? lead & 0x0f : lead & 0x07));
                        for (size_t i = 1; i < utf8_.size(); ++i) {
                            cp = cp << 6 | (static_cast<unsigned char>(utf8_[i]) & 0x3f);
                        }
                        utf8_.clear();
                        Print(cp);
                    }
                    continue;
                }
                utf8_.clear();  // a broken sequence: dropped, this byte is read afresh
                utf8Need_ = 0;
            }
            if (b == 0x1b) {
                mode_ = Mode::Escape;
            } else if (b < 0x20) {
                Control(b);
            } else if (b < 0x7f) {
                Print(b);
            } else if (b >= 0xc2 && b <= 0xf4) {
                utf8_.assign(1, raw);
                utf8Need_ = b >= 0xf0 ? 3 : (b >= 0xe0 ? 2 : 1);
            }
            break;
        case Mode::Escape:
            if (b == '[') {
                mode_ = Mode::Csi;
                params_.clear();
                current_ = -1;
                prefix_ = 0;
            } else if (b == ']') {
                mode_ = Mode::Osc;
                osc_.clear();
            } else if (b == 'P' || b == '_' || b == '^' || b == 'X') {
                mode_ = Mode::Skip;
            } else if (b == '(' || b == ')' || b == '*' || b == '+' || b == '#' || b == '%' || b == ' ') {
                mode_ = Mode::EscapeSkip;
            } else if (b == 0x1b) {
                // stays
            } else if (b < 0x20) {
                Control(b);
            } else {
                mode_ = Mode::Ground;
                EscapeFinal(b);
            }
            break;
        case Mode::EscapeSkip:
            mode_ = Mode::Ground;  // the byte after "ESC (" and the like: a character set, ignored
            break;
        case Mode::Csi:
            if (b == 0x1b) {
                mode_ = Mode::Escape;
            } else if (b >= '0' && b <= '9') {
                current_ = std::min(1'000'000, (current_ < 0 ? 0 : current_) * 10 + (b - '0'));
            } else if (b == ';' || b == ':') {
                params_.push_back(current_);
                current_ = -1;
            } else if (b >= '<' && b <= '?') {
                prefix_ = raw;
            } else if (b >= 0x20 && b <= 0x2f) {
                // intermediates (as in "CSI 2 SP q") change nothing we implement
            } else if (b >= 0x40 && b <= 0x7e) {
                params_.push_back(current_);
                mode_ = Mode::Ground;
                CsiFinal(b);
            } else if (b < 0x20) {
                Control(b);
            }
            break;
        case Mode::Osc:
            if (b == 0x07) {
                OscEnd();
                mode_ = Mode::Ground;
            } else if (b == 0x1b) {
                mode_ = Mode::OscEscape;
            } else if (osc_.size() < 4096) {
                osc_ += raw;
            }
            break;
        case Mode::OscEscape:
            OscEnd();  // "ESC \" ends it; anything else too, the sequence was cut short
            mode_ = Mode::Ground;
            break;
        case Mode::Skip:
            if (b == 0x07) {
                mode_ = Mode::Ground;
            } else if (b == 0x1b) {
                mode_ = Mode::SkipEscape;
            }
            break;
        case Mode::SkipEscape:
            mode_ = Mode::Ground;
            break;
        }
    }
}

void VtScreen::Control(unsigned char c) {
    switch (c) {
    case '\r':
        curCol_ = 0;
        wrapPending_ = false;
        break;
    case '\n':
    case 0x0b:
    case 0x0c:
        LineFeed();
        wrapPending_ = false;
        break;
    case '\b':
        if (curCol_ > 0) {
            --curCol_;
        }
        wrapPending_ = false;
        break;
    case '\t':
        curCol_ = std::min(cols_ - 1, (curCol_ / 8 + 1) * 8);
        wrapPending_ = false;
        break;
    default:  // BEL and the rest: nothing to do
        break;
    }
}

void VtScreen::Print(char32_t c) {
    const int width = Width(c);
    if (width == 0) {
        return;
    }
    if (wrapPending_ && autowrap_) {
        curCol_ = 0;
        LineFeed();
    }
    wrapPending_ = false;
    if (width == 2 && curCol_ == cols_ - 1) {
        if (!autowrap_) {
            return;
        }
        grid_[static_cast<size_t>(curRow_)][static_cast<size_t>(curCol_)] = VtCell{U' ', kVtDefault, pen_.bg, 0};
        curCol_ = 0;
        LineFeed();
    }
    VtLine& line = grid_[static_cast<size_t>(curRow_)];
    line[static_cast<size_t>(curCol_)] = VtCell{c, pen_.fg, pen_.bg, pen_.attrs};
    if (width == 2 && curCol_ + 1 < cols_) {
        line[static_cast<size_t>(curCol_) + 1] = VtCell{0, pen_.fg, pen_.bg, pen_.attrs};
    }
    curCol_ += width;
    if (curCol_ >= cols_) {
        curCol_ = cols_ - 1;
        wrapPending_ = true;
    }
}

void VtScreen::LineFeed() {
    if (curRow_ == bottom_) {
        ScrollUp(1);
    } else if (curRow_ < rows_ - 1) {
        ++curRow_;
    }
}

void VtScreen::ReverseIndex() {
    if (curRow_ == top_) {
        ScrollDown(1);
    } else if (curRow_ > 0) {
        --curRow_;
    }
}

void VtScreen::ScrollUp(int count) {
    count = std::min(count, bottom_ - top_ + 1);
    for (int i = 0; i < count; ++i) {
        if (!alt_ && top_ == 0) {
            scrollback_.push_back(std::move(grid_[0]));
            while (scrollback_.size() > scrollbackLimit_) {
                scrollback_.pop_front();
            }
        }
        grid_.erase(grid_.begin() + top_);
        grid_.insert(grid_.begin() + bottom_, Blank());
    }
}

void VtScreen::ScrollDown(int count) {
    count = std::min(count, bottom_ - top_ + 1);
    for (int i = 0; i < count; ++i) {
        grid_.erase(grid_.begin() + bottom_);
        grid_.insert(grid_.begin() + top_, Blank());
    }
}

void VtScreen::EraseLine(int from, int to) {
    VtLine& line = grid_[static_cast<size_t>(curRow_)];
    for (int col = std::max(0, from); col < std::min(to, cols_); ++col) {
        line[static_cast<size_t>(col)] = VtCell{U' ', kVtDefault, pen_.bg, 0};
    }
}

void VtScreen::SaveCursor() {
    savedRow_ = curRow_;
    savedCol_ = curCol_;
    savedPen_ = pen_;
}

void VtScreen::RestoreCursor() {
    curRow_ = std::clamp(savedRow_, 0, rows_ - 1);
    curCol_ = std::clamp(savedCol_, 0, cols_ - 1);
    pen_ = savedPen_;
    wrapPending_ = false;
}

void VtScreen::EnterAlt(bool on) {
    if (on == alt_) {
        return;
    }
    if (on) {
        SaveCursor();
        saved_ = std::move(grid_);
        grid_.assign(static_cast<size_t>(rows_), Blank());
        curRow_ = curCol_ = 0;
    } else {
        grid_ = std::move(saved_);
        saved_.clear();
        RestoreCursor();
    }
    alt_ = on;
    top_ = 0;
    bottom_ = rows_ - 1;
    wrapPending_ = false;
}

void VtScreen::Reset() {
    pen_ = VtCell{};
    grid_.assign(static_cast<size_t>(rows_), Blank());
    saved_.clear();
    alt_ = false;
    curRow_ = curCol_ = 0;
    top_ = 0;
    bottom_ = rows_ - 1;
    wrapPending_ = false;
    cursorVisible_ = true;
    appCursor_ = false;
    bracketedPaste_ = false;
    autowrap_ = true;
}

void VtScreen::EscapeFinal(unsigned char c) {
    switch (c) {
    case '7':
        SaveCursor();
        break;
    case '8':
        RestoreCursor();
        break;
    case 'M':
        ReverseIndex();
        break;
    case 'D':
        LineFeed();
        break;
    case 'E':
        curCol_ = 0;
        LineFeed();
        break;
    case 'c':
        Reset();
        break;
    default:  // '=' '>' '\\' and the rest
        break;
    }
}

void VtScreen::OscEnd() {
    const size_t semicolon = osc_.find(';');
    if (semicolon == std::string::npos) {
        return;
    }
    const std::string code = osc_.substr(0, semicolon);
    if (code == "0" || code == "2") {
        title_ = ToWide(osc_.substr(semicolon + 1));
    }
}

void VtScreen::SetMode(bool on) {
    for (const int mode : params_) {
        switch (mode) {
        case 1:
            appCursor_ = on;
            break;
        case 7:
            autowrap_ = on;
            break;
        case 25:
            cursorVisible_ = on;
            break;
        case 47:
        case 1047:
        case 1049:
            EnterAlt(on);
            break;
        case 2004:
            bracketedPaste_ = on;
            break;
        default:  // mouse reporting, focus events, win32-input-mode ...: not offered
            break;
        }
    }
}

void VtScreen::Sgr() {
    const auto color = [&](size_t& i) {
        const int kind = i + 1 < params_.size() ? params_[i + 1] : -1;
        if (kind == 5 && i + 2 < params_.size()) {
            const uint32_t value = Palette(params_[i + 2]);
            i += 2;
            return value;
        }
        if (kind == 2) {
            size_t at = i + 2;
            if (at < params_.size() && params_[at] < 0 && at + 3 < params_.size()) {
                ++at;  // the colon form leaves an empty colour-space slot: 38:2::r:g:b
            }
            if (at + 2 < params_.size()) {
                const auto channel = [&](size_t k) { return static_cast<uint32_t>(std::clamp(params_[k], 0, 255)); };
                const uint32_t value = channel(at) << 16 | channel(at + 1) << 8 | channel(at + 2);
                i = at + 2;
                return value;
            }
        }
        i = params_.size();
        return kVtDefault;
    };
    for (size_t i = 0; i < params_.size(); ++i) {
        const int p = std::max(0, params_[i]);
        if (p == 0) {
            pen_ = VtCell{};
        } else if (p == 1) {
            pen_.attrs |= kVtBold;
        } else if (p == 2) {
            pen_.attrs |= kVtDim;
        } else if (p == 3) {
            pen_.attrs |= kVtItalic;
        } else if (p == 4) {
            pen_.attrs |= kVtUnderline;
        } else if (p == 7) {
            pen_.attrs |= kVtInverse;
        } else if (p == 9) {
            pen_.attrs |= kVtStrike;
        } else if (p == 22) {
            pen_.attrs &= static_cast<uint8_t>(~(kVtBold | kVtDim));
        } else if (p == 23) {
            pen_.attrs &= static_cast<uint8_t>(~kVtItalic);
        } else if (p == 24) {
            pen_.attrs &= static_cast<uint8_t>(~kVtUnderline);
        } else if (p == 27) {
            pen_.attrs &= static_cast<uint8_t>(~kVtInverse);
        } else if (p == 29) {
            pen_.attrs &= static_cast<uint8_t>(~kVtStrike);
        } else if (p >= 30 && p <= 37) {
            pen_.fg = Palette(p - 30);
        } else if (p == 38) {
            pen_.fg = color(i);
        } else if (p == 39) {
            pen_.fg = kVtDefault;
        } else if (p >= 40 && p <= 47) {
            pen_.bg = Palette(p - 40);
        } else if (p == 48) {
            pen_.bg = color(i);
        } else if (p == 49) {
            pen_.bg = kVtDefault;
        } else if (p >= 90 && p <= 97) {
            pen_.fg = Palette(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            pen_.bg = Palette(p - 100 + 8);
        }
    }
}

void VtScreen::CsiFinal(unsigned char c) {
    if (prefix_ == '?') {
        if (c == 'h' || c == 'l') {
            SetMode(c == 'h');
        }
        return;
    }
    if (prefix_ != 0) {  // "CSI > c" and the like
        return;
    }
    const int n = std::max(1, Param(0, 1));
    switch (c) {
    case 'A':
        curRow_ = std::max(curRow_ >= top_ ? top_ : 0, curRow_ - n);
        wrapPending_ = false;
        break;
    case 'B':
        curRow_ = std::min(curRow_ <= bottom_ ? bottom_ : rows_ - 1, curRow_ + n);
        wrapPending_ = false;
        break;
    case 'C':
        curCol_ = std::min(cols_ - 1, curCol_ + n);
        wrapPending_ = false;
        break;
    case 'D':
        curCol_ = std::max(0, curCol_ - n);
        wrapPending_ = false;
        break;
    case 'E':
        curRow_ = std::min(rows_ - 1, curRow_ + n);
        curCol_ = 0;
        wrapPending_ = false;
        break;
    case 'F':
        curRow_ = std::max(0, curRow_ - n);
        curCol_ = 0;
        wrapPending_ = false;
        break;
    case 'G':
        curCol_ = std::clamp(n - 1, 0, cols_ - 1);
        wrapPending_ = false;
        break;
    case 'd':
        curRow_ = std::clamp(n - 1, 0, rows_ - 1);
        wrapPending_ = false;
        break;
    case 'H':
    case 'f':
        curRow_ = std::clamp(std::max(1, Param(0, 1)) - 1, 0, rows_ - 1);
        curCol_ = std::clamp(std::max(1, Param(1, 1)) - 1, 0, cols_ - 1);
        wrapPending_ = false;
        break;
    case 'J': {
        const int mode = Param(0, 0);
        const int row = curRow_;
        if (mode == 0) {
            EraseLine(curCol_, cols_);
            for (int r = row + 1; r < rows_; ++r) {
                curRow_ = r;
                EraseLine(0, cols_);
            }
        } else if (mode == 1) {
            for (int r = 0; r < row; ++r) {
                curRow_ = r;
                EraseLine(0, cols_);
            }
            curRow_ = row;
            EraseLine(0, curCol_ + 1);
        } else {
            for (int r = 0; r < rows_; ++r) {
                curRow_ = r;
                EraseLine(0, cols_);
            }
            if (mode == 3 && !alt_) {
                scrollback_.clear();
            }
        }
        curRow_ = row;
        break;
    }
    case 'K': {
        const int mode = Param(0, 0);
        if (mode == 0) {
            EraseLine(curCol_, cols_);
        } else if (mode == 1) {
            EraseLine(0, curCol_ + 1);
        } else {
            EraseLine(0, cols_);
        }
        break;
    }
    case 'L':
        if (curRow_ >= top_ && curRow_ <= bottom_) {
            const int count = std::min(n, bottom_ - curRow_ + 1);
            for (int i = 0; i < count; ++i) {
                grid_.erase(grid_.begin() + bottom_);
                grid_.insert(grid_.begin() + curRow_, Blank());
            }
            curCol_ = 0;
            wrapPending_ = false;
        }
        break;
    case 'M':
        if (curRow_ >= top_ && curRow_ <= bottom_) {
            const int count = std::min(n, bottom_ - curRow_ + 1);
            for (int i = 0; i < count; ++i) {
                grid_.erase(grid_.begin() + curRow_);
                grid_.insert(grid_.begin() + bottom_, Blank());
            }
            curCol_ = 0;
            wrapPending_ = false;
        }
        break;
    case '@': {
        VtLine& line = grid_[static_cast<size_t>(curRow_)];
        const int count = std::min(n, cols_ - curCol_);
        line.insert(line.begin() + curCol_, static_cast<size_t>(count), VtCell{U' ', kVtDefault, pen_.bg, 0});
        line.resize(static_cast<size_t>(cols_));
        break;
    }
    case 'P': {
        VtLine& line = grid_[static_cast<size_t>(curRow_)];
        const int count = std::min(n, cols_ - curCol_);
        line.erase(line.begin() + curCol_, line.begin() + curCol_ + count);
        line.resize(static_cast<size_t>(cols_), VtCell{U' ', kVtDefault, pen_.bg, 0});
        break;
    }
    case 'X':
        EraseLine(curCol_, curCol_ + n);
        break;
    case 'S':
        ScrollUp(n);
        break;
    case 'T':
        ScrollDown(n);
        break;
    case 'm':
        Sgr();
        break;
    case 'r': {
        const int top = std::max(1, Param(0, 1)) - 1;
        const int bottom = Param(1, rows_) - 1;
        if (top < bottom && bottom < rows_) {
            top_ = top;
            bottom_ = bottom;
        } else {
            top_ = 0;
            bottom_ = rows_ - 1;
        }
        curRow_ = curCol_ = 0;
        wrapPending_ = false;
        break;
    }
    case 's':
        SaveCursor();
        break;
    case 'u':
        RestoreCursor();
        break;
    case 'n':
        if (Param(0, 0) == 6) {
            Answer("\x1b[" + std::to_string(curRow_ + 1) + ";" + std::to_string(curCol_ + 1) + "R");
        } else if (Param(0, 0) == 5) {
            Answer("\x1b[0n");
        }
        break;
    case 'c':
        Answer("\x1b[?1;0c");
        break;
    default:  // window operations, cursor shape ...
        break;
    }
}

void VtScreen::Resize(int cols, int rows) {
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    if (cols == cols_ && rows == rows_) {
        return;
    }
    const auto fit = [&](std::vector<VtLine>& lines, bool main) {
        if (main) {  // a shorter screen pushes the lines above the cursor into the scrollback
            while (static_cast<int>(lines.size()) > rows && curRow_ > 0 && !alt_) {
                scrollback_.push_back(std::move(lines.front()));
                lines.erase(lines.begin());
                --curRow_;
            }
        }
        lines.resize(static_cast<size_t>(rows));
        for (VtLine& line : lines) {
            line.resize(static_cast<size_t>(cols), VtCell{});
        }
    };
    cols_ = cols;
    rows_ = rows;
    fit(grid_, true);
    if (alt_) {
        fit(saved_, false);
    }
    for (VtLine& line : scrollback_) {
        line.resize(static_cast<size_t>(cols_), VtCell{});
    }
    while (scrollback_.size() > scrollbackLimit_) {
        scrollback_.pop_front();
    }
    top_ = 0;
    bottom_ = rows_ - 1;
    curRow_ = std::clamp(curRow_, 0, rows_ - 1);
    curCol_ = std::clamp(curCol_, 0, cols_ - 1);
    wrapPending_ = false;
}

std::wstring VtScreen::TextBetween(size_t fromLine, int fromCol, size_t toLine, int toCol) const {
    std::wstring out;
    const size_t total = LineCount();
    for (size_t index = fromLine; index <= toLine && index < total; ++index) {
        const VtLine& line = Line(index);
        const int start = index == fromLine ? std::clamp(fromCol, 0, cols_) : 0;
        const int end = index == toLine ? std::clamp(toCol, 0, cols_) : cols_;
        std::wstring text;
        for (int col = start; col < end && static_cast<size_t>(col) < line.size(); ++col) {
            if (line[static_cast<size_t>(col)].ch != 0) {
                AppendUtf16(text, line[static_cast<size_t>(col)].ch);
            }
        }
        if (index != toLine) {
            const size_t last = text.find_last_not_of(L' ');
            text.resize(last == std::wstring::npos ? 0 : last + 1);
            text += L'\n';
        }
        out += text;
    }
    return out;
}

}  // namespace aic
