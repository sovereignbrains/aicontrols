#include "app/popup_menu.h"

#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace aic {
namespace {

// In DIPs.
constexpr float kPad = 5.f;         // inside the box, around the rows
constexpr float kRow = 28.f;
constexpr float kSeparator = 9.f;
constexpr float kRadius = 8.f;
constexpr float kTextLeft = 12.f;
constexpr float kTextRight = 12.f;
constexpr float kShortcutGap = 28.f;  // between an item and its shortcut
constexpr float kMark = 18.f;         // a check or a submenu's arrow, at the right
constexpr float kMinWidth = 150.f;
constexpr float kMaxWidth = 440.f;

constexpr uint32_t kBack = 0x1f1f22;
constexpr uint32_t kBorder = 0x34343a;
constexpr uint32_t kHot = 0x2d2d33;
constexpr uint32_t kText = 0xf2f4f8;
constexpr uint32_t kDim = 0x9c9ea4;
constexpr uint32_t kDisabled = 0x65666b;
constexpr uint32_t kAccent = 0x4c92ff;

D2D1_COLOR_F Color(uint32_t rgb) {
    return D2D1::ColorF(rgb, 1.f);
}

struct Item {
    UINT id = 0;
    std::wstring text;
    std::wstring shortcut;
    bool separator = false;
    bool disabled = false;
    bool checked = false;
    HMENU submenu = nullptr;
    float top = 0;  // in the box, DIPs
    float height = 0;
};

struct Level {
    std::vector<Item> items;
    HWND window = nullptr;
    RECT screen{};  // where it is, px
    float width = 0;
    float height = 0;
    int hot = -1;
    int openFor = -1;  // the item whose submenu is open beside it
};

// The capture went elsewhere (another app, a system dialog): the menu goes.
bool g_lost = false;

LRESULT CALLBACK MenuProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CAPTURECHANGED) {
        g_lost = true;
    }
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

std::vector<Item> ReadItems(HMENU menu) {
    std::vector<Item> items;
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        wchar_t buffer[512]{};
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        info.dwTypeData = buffer;
        info.cch = static_cast<UINT>(std::size(buffer) - 1);
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) {
            continue;
        }
        Item item;
        item.id = info.wID;
        item.separator = (info.fType & MFT_SEPARATOR) != 0;
        item.disabled = (info.fState & MFS_DISABLED) != 0;
        item.checked = (info.fState & MFS_CHECKED) != 0;
        item.submenu = info.hSubMenu;
        std::wstring text(buffer);
        std::wstring plain;  // no mnemonics: "&&" is an ampersand
        for (size_t k = 0; k < text.size(); ++k) {
            if (text[k] == L'&' && k + 1 < text.size()) {
                ++k;
            }
            plain += text[k];
        }
        const size_t tab = plain.find(L'\t');
        item.text = plain.substr(0, tab);
        if (tab != std::wstring::npos) {
            item.shortcut = plain.substr(tab + 1);
        }
        items.push_back(std::move(item));
    }
    return items;
}

float Measure(const MenuLook& look, const std::wstring& text) {
    wil::com_ptr<IDWriteTextLayout> layout;
    if (text.empty() || FAILED(look.dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), look.text,
                                                             10000.f, 100.f, layout.put()))) {
        return 0;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return std::ceil(metrics.widthIncludingTrailingWhitespace) + 1.f;
}

void LayOut(Level& level, const MenuLook& look) {
    float text = 0;
    float shortcut = 0;
    bool marks = false;
    float y = kPad;
    for (Item& item : level.items) {
        item.top = y;
        item.height = item.separator ? kSeparator : kRow;
        y += item.height;
        if (!item.separator) {
            text = std::max(text, Measure(look, item.text));
            shortcut = std::max(shortcut, Measure(look, item.shortcut));
            marks = marks || item.checked || item.submenu != nullptr;
        }
    }
    const float width = kTextLeft + text + (shortcut > 0 ? kShortcutGap + shortcut : 0.f) + (marks ? kMark + 6.f : 0.f) +
                        kTextRight + 2 * kPad;
    level.width = std::clamp(width, kMinWidth, kMaxWidth);
    level.height = y + kPad;
}

int ToPx(float dip, const MenuLook& look) {
    return static_cast<int>(std::lround(dip * look.dpi / 96.f));
}

float ToDip(int px, const MenuLook& look) {
    return static_cast<float>(px) * 96.f / look.dpi;
}

void Line(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, D2D1_POINT_2F a, D2D1_POINT_2F b,
          ID2D1StrokeStyle* round) {
    target->DrawLine(a, b, brush, 1.4f, round);
}

// The level into its layered window: exactly the rounded box, the rest see-through.
void Paint(const Level& level, const MenuLook& look, ID2D1StrokeStyle* round) {
    const int width = level.screen.right - level.screen.left;
    const int height = level.screen.bottom - level.screen.top;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const wil::unique_hdc memory(CreateCompatibleDC(nullptr));
    const wil::unique_hbitmap bitmap(CreateDIBSection(memory.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!memory || !bitmap) {
        return;
    }
    const auto selected = wil::SelectObject(memory.get(), bitmap.get());
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        look.dpi, look.dpi);
    wil::com_ptr<ID2D1DCRenderTarget> target;
    const RECT whole{0, 0, width, height};
    if (FAILED(look.d2d->CreateDCRenderTarget(&properties, target.put())) || FAILED(target->BindDC(memory.get(), &whole))) {
        return;
    }
    wil::com_ptr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(Color(kBack), brush.put());
    if (!brush) {
        return;
    }
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.f));
    const D2D1_RECT_F box = D2D1::RectF(0.5f, 0.5f, level.width - 0.5f, level.height - 0.5f);
    target->FillRoundedRectangle(D2D1::RoundedRect(box, kRadius, kRadius), brush.get());
    brush->SetColor(Color(kBorder));
    target->DrawRoundedRectangle(D2D1::RoundedRect(box, kRadius, kRadius), brush.get(), 1.f);
    for (size_t i = 0; i < level.items.size(); ++i) {
        const Item& item = level.items[i];
        if (item.separator) {
            brush->SetColor(Color(kBorder));
            const float y = item.top + item.height / 2;
            target->DrawLine({kPad + 6.f, y}, {level.width - kPad - 6.f, y}, brush.get(), 1.f);
            continue;
        }
        const D2D1_RECT_F row = D2D1::RectF(kPad, item.top, level.width - kPad, item.top + item.height);
        if (static_cast<int>(i) == level.hot || static_cast<int>(i) == level.openFor) {
            brush->SetColor(Color(kHot));
            target->FillRoundedRectangle(D2D1::RoundedRect(row, 5.f, 5.f), brush.get());
        }
        const float markLeft = row.right - kTextRight - kMark;
        const bool marked = item.checked || item.submenu != nullptr;
        float right = row.right - kTextRight - (marked ? kMark + 6.f : 0.f);
        if (!item.shortcut.empty()) {
            const float shortcut = Measure(look, item.shortcut);
            brush->SetColor(Color(item.disabled ? kDisabled : kDim));
            target->DrawText(item.shortcut.c_str(), static_cast<UINT32>(item.shortcut.size()), look.text,
                             D2D1::RectF(right - shortcut, row.top, right, row.bottom), brush.get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            right -= shortcut + kShortcutGap;
        }
        brush->SetColor(Color(item.disabled ? kDisabled : kText));
        target->DrawText(item.text.c_str(), static_cast<UINT32>(item.text.size()), look.text,
                         D2D1::RectF(row.left + kTextLeft - kPad, row.top, std::max(row.left + 20.f, right), row.bottom),
                         brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        const D2D1_POINT_2F c{markLeft + kMark / 2, (row.top + row.bottom) / 2};
        if (item.submenu != nullptr) {  // ">"
            brush->SetColor(Color(item.disabled ? kDisabled : kDim));
            Line(target.get(), brush.get(), {c.x - 2.f, c.y - 4.f}, {c.x + 2.f, c.y}, round);
            Line(target.get(), brush.get(), {c.x + 2.f, c.y}, {c.x - 2.f, c.y + 4.f}, round);
        } else if (item.checked) {
            brush->SetColor(Color(kAccent));
            Line(target.get(), brush.get(), {c.x - 4.5f, c.y}, {c.x - 1.5f, c.y + 3.f}, round);
            Line(target.get(), brush.get(), {c.x - 1.5f, c.y + 3.f}, {c.x + 4.5f, c.y - 3.5f}, round);
        }
    }
    if (FAILED(target->EndDraw())) {
        return;
    }
    POINT at{level.screen.left, level.screen.top};
    POINT origin{};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(level.window, nullptr, &at, &size, memory.get(), &origin, 0, &blend, ULW_ALPHA);
}

// The level's window, placed within the monitor: from `at` down (or up from it when `above`), flipped where
// the screen ends.
bool Open(Level& level, HWND owner, POINT at, bool above, int flipLeft, const MenuLook& look) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW menuClass{sizeof(menuClass)};
        menuClass.lpfnWndProc = &MenuProc;
        menuClass.hInstance = GetModuleHandleW(nullptr);
        menuClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        menuClass.lpszClassName = L"AIControlsMenu";
        RegisterClassExW(&menuClass);
        registered = true;
    }
    LayOut(level, look);
    const int width = ToPx(level.width, look);
    const int height = ToPx(level.height, look);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& work = monitor.rcWork;
    int x = at.x;
    if (x + width > work.right) {
        x = flipLeft >= 0 ? flipLeft - width : work.right - width;
    }
    x = std::max<int>(x, work.left);
    int y = above ? at.y - height : at.y;
    if (y + height > work.bottom) {
        y = std::max<int>(work.top, work.bottom - height);
    }
    y = std::max<int>(y, work.top);
    level.screen = RECT{x, y, x + width, y + height};
    level.window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"AIControlsMenu",
                                   L"", WS_POPUP, x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    return level.window != nullptr;
}

int ItemAt(const Level& level, POINT screen, const MenuLook& look) {
    if (!PtInRect(&level.screen, screen)) {
        return -1;
    }
    const float y = ToDip(screen.y - level.screen.top, look);
    for (size_t i = 0; i < level.items.size(); ++i) {
        const Item& item = level.items[i];
        if (y >= item.top && y < item.top + item.height) {
            return item.separator || item.disabled ? -1 : static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

int ShowMenu(HWND owner, HMENU menu, POINT screen, bool above, const MenuLook& look) {
    if (look.d2d == nullptr || look.dwrite == nullptr || look.text == nullptr) {
        return 0;
    }
    D2D1_STROKE_STYLE_PROPERTIES stroke = D2D1::StrokeStyleProperties();
    stroke.startCap = stroke.endCap = D2D1_CAP_STYLE_ROUND;
    stroke.lineJoin = D2D1_LINE_JOIN_ROUND;
    wil::com_ptr<ID2D1StrokeStyle> round;
    look.d2d->CreateStrokeStyle(stroke, nullptr, 0, round.put());

    std::vector<Level> levels(1);
    levels[0].items = ReadItems(menu);
    if (levels[0].items.empty() || !Open(levels[0], owner, screen, above, -1, look)) {
        return 0;
    }
    const auto show = [&](Level& level) {
        Paint(level, look, round.get());
        ShowWindow(level.window, SW_SHOWNOACTIVATE);
    };
    show(levels[0]);
    const auto closeFrom = [&](size_t keep) {  // levels past `keep` go
        while (levels.size() > keep) {
            DestroyWindow(levels.back().window);
            levels.pop_back();
        }
        if (!levels.empty()) {
            levels.back().openFor = -1;
        }
    };
    const auto openSub = [&](size_t at, int index) {
        Level& parent = levels[at];
        if (index < 0 || parent.items[static_cast<size_t>(index)].submenu == nullptr ||
            parent.items[static_cast<size_t>(index)].disabled) {
            return;
        }
        if (parent.openFor == index) {
            return;
        }
        closeFrom(at + 1);
        Level child;
        child.items = ReadItems(parent.items[static_cast<size_t>(index)].submenu);
        const POINT corner{parent.screen.right - ToPx(4.f, look),
                           parent.screen.top + ToPx(parent.items[static_cast<size_t>(index)].top - kPad, look)};
        if (child.items.empty() || !Open(child, owner, corner, false, parent.screen.left + ToPx(4.f, look), look)) {
            return;
        }
        levels[at].openFor = index;
        levels.push_back(std::move(child));
        show(levels.back());
        Paint(levels[at], look, round.get());
    };
    const auto levelAt = [&](POINT point) -> int {
        for (size_t i = levels.size(); i-- > 0;) {
            if (PtInRect(&levels[i].screen, point)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    const auto move = [&](Level& level, int step) {  // the keys: the next item that can be chosen
        const int count = static_cast<int>(level.items.size());
        int index = level.hot;
        for (int tries = 0; tries < count; ++tries) {
            index = (index + step + count) % count;
            const Item& item = level.items[static_cast<size_t>(index)];
            if (!item.separator && !item.disabled) {
                level.hot = index;
                return;
            }
        }
    };

    g_lost = false;
    SetCapture(levels[0].window);
    int chosen = 0;
    bool done = false;
    while (!done) {
        MSG message{};
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            break;
        }
        switch (message.message) {
        case WM_MOUSEMOVE: {
            const int at = levelAt(message.pt);
            if (at < 0) {
                break;
            }
            Level& level = levels[static_cast<size_t>(at)];
            const int index = ItemAt(level, message.pt, look);
            if (index != level.hot) {
                level.hot = index;
                if (static_cast<size_t>(at) + 1 < levels.size() && index != level.openFor && index >= 0) {
                    closeFrom(static_cast<size_t>(at) + 1);
                }
                Paint(level, look, round.get());
                openSub(static_cast<size_t>(at), index);
            }
            break;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
        case WM_NCRBUTTONDOWN:
            if (levelAt(message.pt) < 0) {  // a click outside closes it, and is spent on that
                done = true;
            }
            break;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            const int at = levelAt(message.pt);
            if (at < 0) {
                break;
            }
            Level& level = levels[static_cast<size_t>(at)];
            const int index = ItemAt(level, message.pt, look);
            if (index >= 0) {
                const Item& item = level.items[static_cast<size_t>(index)];
                if (item.submenu != nullptr) {
                    openSub(static_cast<size_t>(at), index);
                } else {
                    chosen = static_cast<int>(item.id);
                    done = true;
                }
            }
            break;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            Level& level = levels.back();
            switch (message.wParam) {
            case VK_ESCAPE:
            case VK_LEFT:
                if (levels.size() > 1) {
                    closeFrom(levels.size() - 1);
                    Paint(levels.back(), look, round.get());
                } else if (message.wParam == VK_ESCAPE) {
                    done = true;
                }
                break;
            case VK_UP:
            case VK_DOWN:
                move(level, message.wParam == VK_DOWN ? 1 : -1);
                Paint(level, look, round.get());
                break;
            case VK_RIGHT:
                openSub(levels.size() - 1, level.hot);
                if (levels.size() > 1 && levels.back().hot < 0) {
                    move(levels.back(), 1);
                    Paint(levels.back(), look, round.get());
                }
                break;
            case VK_RETURN:
            case VK_SPACE:
                if (level.hot >= 0) {
                    const Item& item = level.items[static_cast<size_t>(level.hot)];
                    if (item.submenu != nullptr) {
                        openSub(levels.size() - 1, level.hot);
                    } else {
                        chosen = static_cast<int>(item.id);
                        done = true;
                    }
                }
                break;
            case VK_MENU:
                done = true;
                break;
            default:
                break;
            }
            break;
        }
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_KEYUP:
        case WM_SYSKEYUP:
        case WM_MOUSEWHEEL:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
            break;  // the menu's while it is open
        default:
            TranslateMessage(&message);
            DispatchMessageW(&message);
            break;
        }
        if (g_lost || GetCapture() != levels[0].window) {
            done = true;
        }
    }
    if (GetCapture() == levels[0].window) {
        ReleaseCapture();
    }
    closeFrom(0);
    return chosen;
}

}  // namespace aic
