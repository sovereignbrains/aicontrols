#include "app/chat_window.h"

#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <richedit.h>

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/claude_process.h"
#include "core/store.h"

namespace aic {
namespace {

// Posted from the reader thread; wParam is the chat's generation, lParam owns the payload.
constexpr UINT kClaudeMessage = WM_APP + 1;  // Json*
constexpr UINT kClaudeExit = WM_APP + 2;     // ExitInfo*
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kSubclassId = 1;
constexpr size_t kTranscriptLimit = 200;

// Layout, in DIPs.
constexpr float kHeader = 44.f;
constexpr float kColumnMax = 720.f;
constexpr float kSide = 24.f;
constexpr float kGap = 14.f;
constexpr float kBubblePad = 10.f;
constexpr float kBoxBottom = 16.f;
constexpr float kBoxPad = 12.f;
constexpr float kToolbar = 38.f;
constexpr float kTextMin = 22.f;
constexpr float kTextMax = 220.f;
constexpr float kButton = 32.f;
constexpr float kPopoverWidth = 380.f;
constexpr float kRow = 36.f;
constexpr size_t kRowsShown = 12;

// Sovereign's palette (src/tray/ui_style.h): dark only, one blue accent.
constexpr uint32_t kWindow = 0x0e0e0f;
constexpr uint32_t kPanel = 0x121213;
constexpr uint32_t kCard = 0x18181a;
constexpr uint32_t kRaised = 0x232327;
constexpr uint32_t kUserBubble = 0x1d2738;
constexpr uint32_t kLine = 0x2c2c31;
constexpr uint32_t kPrimary = 0xf2f4f8;
constexpr uint32_t kSecondary = 0x9c9ea4;
constexpr uint32_t kAccent = 0x4c92ff;
constexpr uint32_t kDanger = 0xf06060;

constexpr COLORREF ToColorRef(uint32_t rgb) {
    return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

D2D1_COLOR_F Color(uint32_t rgb, float alpha = 1.f) {
    return D2D1::ColorF(rgb, alpha);
}

bool Inside(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

struct ExitInfo {
    DWORD code;
    std::string stderrTail;
};

enum class Kind { User, Assistant, Notice, Error };

struct FeedItem {
    Kind kind = Kind::Notice;
    std::wstring text;
    std::string messageId;  // assistant text: the API message it streams into
    int block = -1;         // ... and its content block index
    wil::com_ptr<IDWriteTextLayout> layout;
    float layoutWidth = 0;
    float textWidth = 0;
    float height = 0;
    float top = 0;  // where it was last painted, for hit tests
};

enum class Target { None, History, NewChat, Model, Send, Row, Popover };

struct Hit {
    Target target = Target::None;
    size_t row = 0;  // Target::Row: an index into the filtered list
    bool operator==(const Hit&) const = default;
};

// Every rectangle of the window for one client size, in DIPs.
struct Geometry {
    float width = 0;
    float height = 0;
    float column = 0;
    D2D1_RECT_F history{};
    D2D1_RECT_F newChat{};
    D2D1_RECT_F feed{};
    D2D1_RECT_F box{};
    D2D1_RECT_F text{};
    D2D1_RECT_F model{};
    D2D1_RECT_F send{};
    D2D1_RECT_F popover{};
    D2D1_RECT_F search{};
    float listTop = 0;
};

std::wstring LastLines(const std::string& text, int count) {
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r')) {
        --end;
    }
    size_t start = end;
    for (int found = 0; start > 0; --start) {
        if (text[start - 1] == '\n' && ++found == count) {
            break;
        }
    }
    return ToWide(std::string_view(text).substr(start, end - start));
}

// RichEdit's text: paragraphs end in \r (GetWindowText may give \r\n); we send \n.
std::wstring NormalizeNewlines(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') {
                continue;
            }
            out += L'\n';
        } else {
            out += text[i];
        }
    }
    return out;
}

std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length), L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    return text;
}

std::wstring Lowercase(std::wstring text) {
    if (!text.empty()) {
        CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    }
    return text;
}

std::wstring Age(const FILETIME& time) {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& t) {
        return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    const uint64_t minutes = ticks(now) > ticks(time) ? (ticks(now) - ticks(time)) / 600'000'000ULL : 0;
    if (minutes < 1) {
        return L"сейчас";
    }
    if (minutes < 60) {
        return std::format(L"{} мин", minutes);
    }
    const uint64_t hours = minutes / 60;
    if (hours < 24) {
        return std::format(L"{} ч", hours);
    }
    const uint64_t days = hours / 24;
    if (days == 1) {
        return L"вчера";
    }
    return days < 30 ? std::format(L"{} дн", days) : std::format(L"{} мес", days / 30);
}

// Dark native menus (the model picker): uxtheme's SetPreferredAppMode(ForceDark), as in Sovereign.
void UseDarkMenus() {
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (uxtheme == nullptr) {
        return;
    }
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    const auto setMode = reinterpret_cast<SetPreferredAppModeFn>(  // NOLINT(performance-no-int-to-ptr)
        reinterpret_cast<std::uintptr_t>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135))));
    const auto flush = reinterpret_cast<FlushMenuThemesFn>(  // NOLINT(performance-no-int-to-ptr)
        reinterpret_cast<std::uintptr_t>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136))));
    if (setMode != nullptr) {
        setMode(2);
    }
    if (flush != nullptr) {
        flush();
    }
}

class ChatWindow {
public:
    explicit ChatWindow(HINSTANCE instance) : instance_(instance) {}
    int Run(int show);

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK InputProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                      DWORD_PTR self);
    static LRESULT CALLBACK SearchProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                       DWORD_PTR self);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT OnInputFilter(const MSGFILTER& filter);
    void OnCommand(WPARAM wParam, LPARAM lParam);

    float Dpi() const { return static_cast<float>(GetDpiForWindow(hwnd_)); }
    float ToDip(int px) const { return static_cast<float>(px) * 96.f / Dpi(); }
    int ToPx(float dip) const { return static_cast<int>(std::lround(dip * Dpi() / 96.f)); }
    D2D1_POINT_2F CursorDip(LPARAM lParam) const {
        return {ToDip(GET_X_LPARAM(lParam)), ToDip(GET_Y_LPARAM(lParam))};
    }
    Geometry Layout() const;
    Hit HitTest(float x, float y) const;
    void MoveChild(HWND child, const D2D1_RECT_F& rect) const;

    wil::com_ptr<IDWriteTextFormat> MakeFormat(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                                               bool singleLine) const;
    float TextWidth(const std::wstring& text, IDWriteTextFormat* format) const;
    void CreateFonts();
    void CreateInput();
    void StyleInput();
    void PlaceChildren();
    void EnsureTarget();

    void Paint();
    void PaintFeed(const Geometry& g);
    void PaintHeader(const Geometry& g);
    void PaintInput(const Geometry& g);
    void PaintPopover(const Geometry& g);
    void Text(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, uint32_t color);
    void Fill(const D2D1_RECT_F& rect, float radius, uint32_t color, float alpha = 1.f);
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, uint32_t color, float width = 1.6f);
    void MeasureFeed(float column);
    float FeedHeight() const;
    void ScrollBy(float delta);
    void CopyItemAt(float y);
    void Flash(std::wstring status);

    void OpenPopover();
    void ClosePopover();
    void FilterSessions();
    void MoveSelection(int delta);
    void OpenSession(size_t filteredIndex);
    void ShowModelMenu(const Geometry& g);
    std::wstring ModelLabel() const;

    bool StartClaude();
    void Restart();
    void ResetChat();
    void NewChat();
    void Submit();
    void OnClaude(const Json& message);
    void OnClaudeExit(const ExitInfo& exit);
    void Add(Kind kind, std::wstring text);
    void Refresh() { InvalidateRect(hwnd_, nullptr, FALSE); }
    bool HasConversation() const;

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    HWND input_ = nullptr;
    HWND search_ = nullptr;
    wil::unique_hmodule richEdit_;
    wil::unique_hfont uiFont_;
    wil::unique_hbrush raisedBrush_;
    wil::unique_hbrush cardBrush_;

    wil::com_ptr<ID2D1Factory> d2d_;
    wil::com_ptr<IDWriteFactory> dwrite_;
    wil::com_ptr<ID2D1HwndRenderTarget> target_;
    wil::com_ptr<ID2D1SolidColorBrush> brush_;
    wil::com_ptr<ID2D1StrokeStyle> round_;
    wil::com_ptr<IDWriteTextFormat> body_;
    wil::com_ptr<IDWriteTextFormat> notice_;
    wil::com_ptr<IDWriteTextFormat> header_;
    wil::com_ptr<IDWriteTextFormat> ui_;
    wil::com_ptr<IDWriteTextFormat> uiRight_;
    wil::com_ptr<IDWriteTextFormat> greeting_;

    std::vector<FeedItem> feed_;
    float scroll_ = 0;
    float maxScroll_ = 0;
    bool stick_ = true;  // follow the end of the feed as it grows

    float textHeight_ = kTextMin;  // the input's text area, grows with what is typed
    bool inputFocused_ = false;
    bool hasText_ = false;
    Hit hover_;
    bool trackingMouse_ = false;

    bool popoverOpen_ = false;
    std::vector<SessionInfo> sessions_;
    std::vector<size_t> filtered_;
    size_t firstRow_ = 0;
    size_t selected_ = 0;

    std::wstring workspace_;
    Settings settings_;
    Json models_ = Json::array();
    std::string userName_;

    ClaudeProcess claude_;
    WPARAM generation_ = 0;  // bumped per process; stale posts are dropped
    bool running_ = false;
    bool busy_ = false;
    std::string sessionId_;
    std::wstring title_;
    std::string currentMessageId_;
    std::set<std::string> streamed_;  // messages whose text came as deltas
    std::wstring limits_;
    std::wstring route_;
    std::wstring status_;
};

int ChatWindow::Run(int show) {
    workspace_ = DefaultWorkspace();
    settings_ = LoadSettings();
    UseDarkMenus();
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put()));
    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite_.put())));
    D2D1_STROKE_STYLE_PROPERTIES stroke = D2D1::StrokeStyleProperties();
    stroke.startCap = stroke.endCap = D2D1_CAP_STYLE_ROUND;
    stroke.lineJoin = D2D1_LINE_JOIN_ROUND;
    THROW_IF_FAILED(d2d_->CreateStrokeStyle(stroke, nullptr, 0, round_.put()));
    body_ = MakeFormat(15.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    notice_ = MakeFormat(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    header_ = MakeFormat(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    ui_ = MakeFormat(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    uiRight_ = MakeFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    greeting_ = MakeFormat(24.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    raisedBrush_.reset(CreateSolidBrush(ToColorRef(kRaised)));
    cardBrush_.reset(CreateSolidBrush(ToColorRef(kCard)));

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &ChatWindow::Proc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.lpszClassName = L"AIControlsWindow";
    RegisterClassExW(&windowClass);

    CreateWindowExW(0, windowClass.lpszClassName, L"AIControls", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance_, this);
    THROW_LAST_ERROR_IF_NULL(hwnd_);
    SetWindowPos(hwnd_, nullptr, 0, 0, ToPx(1000.f), ToPx(840.f), SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    StartClaude();  // warm up: the first answer does not wait for the CLI to start

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

wil::com_ptr<IDWriteTextFormat> ChatWindow::MakeFormat(float size, DWRITE_FONT_WEIGHT weight,
                                                       DWRITE_TEXT_ALIGNMENT align, bool singleLine) const {
    wil::com_ptr<IDWriteTextFormat> format;
    THROW_IF_FAILED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"ru-ru", format.put()));
    format->SetTextAlignment(align);
    if (singleLine) {
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        wil::com_ptr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(format.get(), ellipsis.put()))) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            format->SetTrimming(&trimming, ellipsis.get());
        }
    }
    return format;
}

float ChatWindow::TextWidth(const std::wstring& text, IDWriteTextFormat* format) const {
    wil::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 10000.f, 100.f,
                                         layout.put()))) {
        return 0;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

LRESULT CALLBACK ChatWindow::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ChatWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<ChatWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ChatWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self != nullptr ? self->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

// The input: a placeholder while it is empty.
LRESULT CALLBACK ChatWindow::InputProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR,
                                       DWORD_PTR self) {
    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    if (message == WM_PAINT && GetWindowTextLengthW(hwnd) == 0) {
        const auto* window = reinterpret_cast<const ChatWindow*>(self);
        HDC dc = GetDC(hwnd);
        RECT area{};
        SendMessageW(hwnd, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&area));
        const HGDIOBJ old = SelectObject(dc, window->uiFont_.get());
        SetTextColor(dc, ToColorRef(kSecondary));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"Опиши задачу или задай вопрос", -1, &area, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
    }
    return result;
}

// The history search: arrows pick a chat, Enter opens it, Esc closes the list.
LRESULT CALLBACK ChatWindow::SearchProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR,
                                        DWORD_PTR self) {
    auto* window = reinterpret_cast<ChatWindow*>(self);
    if (message == WM_KEYDOWN) {
        switch (wParam) {
        case VK_ESCAPE:
            window->ClosePopover();
            return 0;
        case VK_RETURN:
            if (window->selected_ < window->filtered_.size()) {
                window->OpenSession(window->selected_);
            }
            return 0;
        case VK_DOWN:
            window->MoveSelection(1);
            return 0;
        case VK_UP:
            window->MoveSelection(-1);
            return 0;
        default:
            break;
        }
    }
    if (message == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE)) {
        return 0;  // no beep
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT ChatWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        constexpr DWORD kImmersiveDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, kImmersiveDarkMode, &dark, sizeof(dark));
        const COLORREF caption = ToColorRef(kPanel);
        DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        CreateFonts();
        CreateInput();
        return 0;
    }
    case WM_SIZE:
        if (target_) {
            target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        PlaceChildren();
        Refresh();
        return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        target_.reset();
        brush_.reset();
        CreateFonts();
        StyleInput();
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        PlaceChildren();
        return 0;
    }
    case WM_GETMINMAXINFO:
        if (hwnd_ != nullptr) {
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {ToPx(520.f), ToPx(460.f)};
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        Paint();
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lParam) == search_) {
            const auto dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(kPrimary));
            SetBkColor(dc, ToColorRef(kRaised));
            return reinterpret_cast<LRESULT>(raisedBrush_.get());
        }
        break;
    case WM_MOUSEMOVE: {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd_, 0};
            trackingMouse_ = TrackMouseEvent(&track) != FALSE;
        }
        const D2D1_POINT_2F at = CursorDip(lParam);
        if (const Hit hit = HitTest(at.x, at.y); !(hit == hover_)) {
            hover_ = hit;
            if (hit.target == Target::Row) {
                selected_ = hit.row;
            }
            Refresh();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        hover_ = {};
        Refresh();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && reinterpret_cast<HWND>(wParam) == hwnd_) {
            const bool clickable = hover_.target != Target::None && hover_.target != Target::Popover;
            SetCursor(LoadCursorW(nullptr, clickable ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP: {
        const D2D1_POINT_2F at = CursorDip(lParam);
        const Hit hit = HitTest(at.x, at.y);
        if (popoverOpen_ && hit.target != Target::Row && hit.target != Target::Popover &&
            hit.target != Target::History) {
            ClosePopover();
            return 0;
        }
        switch (hit.target) {
        case Target::History:
            popoverOpen_ ? ClosePopover() : OpenPopover();
            break;
        case Target::NewChat:
            NewChat();
            break;
        case Target::Model:
            ShowModelMenu(Layout());
            break;
        case Target::Send:
            if (busy_) {
                claude_.Interrupt();
                Flash(L"останавливаю…");
            } else {
                Submit();
            }
            break;
        case Target::Row:
            OpenSession(hit.row);
            break;
        default:
            break;
        }
        if (!popoverOpen_) {
            SetFocus(input_);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &at);
        const float steps = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        const Geometry g = Layout();
        if (popoverOpen_ && Inside(g.popover, ToDip(at.x), ToDip(at.y))) {
            const size_t maxFirst = filtered_.size() > kRowsShown ? filtered_.size() - kRowsShown : 0;
            const auto moved = static_cast<long long>(firstRow_) + static_cast<long long>(std::lround(steps * 3));
            firstRow_ = static_cast<size_t>(std::clamp<long long>(moved, 0, static_cast<long long>(maxFirst)));
            Refresh();
        } else {
            ScrollBy(steps * 64.f);
        }
        return 0;
    }
    case WM_RBUTTONUP:
        CopyItemAt(CursorDip(lParam).y);
        return 0;
    case WM_SETFOCUS:
        SetFocus(popoverOpen_ ? search_ : input_);
        return 0;
    case WM_TIMER:
        if (wParam == kStatusTimer) {
            KillTimer(hwnd_, kStatusTimer);
            status_.clear();
            Refresh();
        }
        return 0;
    case WM_COMMAND:
        OnCommand(wParam, lParam);
        return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->hwndFrom == input_ && header->code == EN_MSGFILTER) {
            return OnInputFilter(*reinterpret_cast<const MSGFILTER*>(lParam));
        }
        if (header->hwndFrom == input_ && header->code == EN_REQUESTRESIZE) {
            const RECT& wanted = reinterpret_cast<const REQRESIZE*>(lParam)->rc;
            const float height = std::clamp(ToDip(wanted.bottom - wanted.top), kTextMin, kTextMax);
            if (std::abs(height - textHeight_) > 0.5f) {
                textHeight_ = height;
                PlaceChildren();
                Refresh();
            }
            return 0;
        }
        break;
    }
    case kClaudeMessage: {
        const std::unique_ptr<Json> payload(reinterpret_cast<Json*>(lParam));
        if (wParam == generation_) {
            try {
                OnClaude(*payload);
            } catch (const std::exception& e) {
                Add(Kind::Error, L"Не разобрал сообщение CLI: " + ToWide(e.what()));
            }
        }
        return 0;
    }
    case kClaudeExit: {
        const std::unique_ptr<ExitInfo> payload(reinterpret_cast<ExitInfo*>(lParam));
        if (wParam == generation_) {
            OnClaudeExit(*payload);
        }
        return 0;
    }
    case WM_DESTROY:
        ++generation_;
        claude_.Stop();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void ChatWindow::OnCommand(WPARAM wParam, LPARAM lParam) {
    const auto from = reinterpret_cast<HWND>(lParam);
    const WORD code = HIWORD(wParam);
    if (from == input_) {
        if (code == EN_CHANGE) {
            hasText_ = GetWindowTextLengthW(input_) > 0;
            InvalidateRect(input_, nullptr, TRUE);  // the placeholder comes and goes
            Refresh();
        } else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            inputFocused_ = code == EN_SETFOCUS;
            Refresh();
        }
    } else if (from == search_ && code == EN_CHANGE) {
        FilterSessions();
    }
}

// Keys typed into the input: Enter sends, Esc stops the answer, Ctrl+N starts over, Ctrl+H: history.
LRESULT ChatWindow::OnInputFilter(const MSGFILTER& filter) {
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (filter.msg == WM_KEYDOWN) {
        if (filter.wParam == VK_RETURN && !shift) {
            Submit();
            return 1;
        }
        if (filter.wParam == VK_ESCAPE && busy_) {
            claude_.Interrupt();
            Flash(L"останавливаю…");
            return 1;
        }
        if (filter.wParam == 'N' && control) {
            NewChat();
            return 1;
        }
        if (filter.wParam == 'H' && control) {
            OpenPopover();
            return 1;
        }
    }
    if (filter.msg == WM_CHAR && filter.wParam == L'\r' && !shift) {
        return 1;  // the Enter that sent the message
    }
    return 0;
}

Geometry ChatWindow::Layout() const {
    Geometry g;
    RECT client{};
    GetClientRect(hwnd_, &client);
    g.width = ToDip(client.right);
    g.height = ToDip(client.bottom);
    g.column = std::max(0.f, std::min(g.width - 2 * kSide, kColumnMax));
    const float left = (g.width - g.column) / 2;
    const float right = left + g.column;

    const float buttonTop = (kHeader - kButton) / 2;
    g.newChat = D2D1::RectF(g.width - 10 - kButton, buttonTop, g.width - 10, buttonTop + kButton);
    g.history = D2D1::RectF(g.newChat.left - 4 - kButton, buttonTop, g.newChat.left - 4, buttonTop + kButton);

    const float boxBottom = g.height - kBoxBottom;
    const float boxTop = boxBottom - (kBoxPad + textHeight_ + 4 + kToolbar);
    g.box = D2D1::RectF(left, boxTop, right, boxBottom);
    g.text = D2D1::RectF(left + 14, boxTop + kBoxPad, right - 14, boxTop + kBoxPad + textHeight_);
    const float toolTop = boxBottom - kToolbar + 4;
    const float toolBottom = boxBottom - 6;
    const float modelWidth = TextWidth(ModelLabel() + L"  ▾", ui_.get()) + 20;
    g.model = D2D1::RectF(left + 8, toolTop, left + 8 + modelWidth, toolBottom);
    g.send = D2D1::RectF(right - 8 - (toolBottom - toolTop), toolTop, right - 8, toolBottom);
    g.feed = D2D1::RectF(0, kHeader, g.width, boxTop - 8);

    const float popoverWidth = std::min(kPopoverWidth, g.width - 20);
    const float popoverLeft = g.width - 10 - popoverWidth;
    const float popoverTop = kHeader + 4;
    g.search = D2D1::RectF(popoverLeft + 10, popoverTop + 10, g.width - 20, popoverTop + 42);
    g.listTop = g.search.bottom + 8;
    const size_t rows = std::max<size_t>(1, std::min(kRowsShown, filtered_.size()));
    g.popover = D2D1::RectF(popoverLeft, popoverTop, g.width - 10, g.listTop + static_cast<float>(rows) * kRow + 8);
    return g;
}

Hit ChatWindow::HitTest(float x, float y) const {
    const Geometry g = Layout();
    if (popoverOpen_ && Inside(g.popover, x, y)) {
        if (y >= g.listTop) {
            const auto row = firstRow_ + static_cast<size_t>((y - g.listTop) / kRow);
            if (row < filtered_.size() && row < firstRow_ + kRowsShown) {
                return {Target::Row, row};
            }
        }
        return {Target::Popover};
    }
    if (Inside(g.history, x, y)) {
        return {Target::History};
    }
    if (Inside(g.newChat, x, y)) {
        return {Target::NewChat};
    }
    if (Inside(g.model, x, y)) {
        return {Target::Model};
    }
    if (Inside(g.send, x, y)) {
        return {Target::Send};
    }
    return {};
}

void ChatWindow::MoveChild(HWND child, const D2D1_RECT_F& rect) const {
    MoveWindow(child, ToPx(rect.left), ToPx(rect.top), std::max(0, ToPx(rect.right) - ToPx(rect.left)),
               std::max(0, ToPx(rect.bottom) - ToPx(rect.top)), TRUE);
}

void ChatWindow::CreateFonts() {
    LOGFONTW font{};
    font.lfHeight = -MulDiv(11, static_cast<int>(GetDpiForWindow(hwnd_)), 72);
    font.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    uiFont_.reset(CreateFontIndirectW(&font));
    if (search_ != nullptr) {
        SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_.get()), TRUE);
    }
}

void ChatWindow::CreateInput() {
    richEdit_.reset(LoadLibraryW(L"Msftedit.dll"));
    THROW_LAST_ERROR_IF_NULL(richEdit_.get());
    input_ = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL, 0, 0,
                             0, 0, hwnd_, nullptr, instance_, nullptr);
    THROW_LAST_ERROR_IF_NULL(input_);
    SendMessageW(input_, EM_SETTEXTMODE, TM_PLAINTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE, 0);
    SendMessageW(input_, EM_SETEVENTMASK, 0, ENM_KEYEVENTS | ENM_CHANGE | ENM_REQUESTRESIZE);
    SetWindowSubclass(input_, &ChatWindow::InputProc, kSubclassId, reinterpret_cast<DWORD_PTR>(this));
    StyleInput();
    PlaceChildren();
    SetFocus(input_);
}

void ChatWindow::StyleInput() {
    SendMessageW(input_, EM_SETBKGNDCOLOR, 0, ToColorRef(kCard));
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
    format.crTextColor = ToColorRef(kPrimary);
    format.yHeight = 11 * 20;  // twips
    wcscpy_s(format.szFaceName, L"Segoe UI");
    SendMessageW(input_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    SendMessageW(input_, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));
}

void ChatWindow::PlaceChildren() {
    if (input_ == nullptr) {
        return;
    }
    const Geometry g = Layout();
    MoveChild(input_, g.text);
    SendMessageW(input_, EM_REQUESTRESIZE, 0, 0);
    if (search_ != nullptr) {
        MoveChild(search_, D2D1::RectF(g.search.left + 10, g.search.top + 7, g.search.right - 10, g.search.bottom - 6));
    }
}

void ChatWindow::EnsureTarget() {
    if (target_) {
        return;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties();
    properties.dpiX = properties.dpiY = Dpi();
    const D2D1_SIZE_U pixels = D2D1::SizeU(static_cast<UINT32>(client.right), static_cast<UINT32>(client.bottom));
    if (FAILED(d2d_->CreateHwndRenderTarget(properties, D2D1::HwndRenderTargetProperties(hwnd_, pixels),
                                            target_.put()))) {
        return;
    }
    target_->CreateSolidColorBrush(Color(kPrimary), brush_.put());
}

void ChatWindow::Text(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, uint32_t color) {
    brush_->SetColor(Color(color));
    target_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush_.get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void ChatWindow::Fill(const D2D1_RECT_F& rect, float radius, uint32_t color, float alpha) {
    brush_->SetColor(Color(color, alpha));
    target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.get());
}

void ChatWindow::Line(D2D1_POINT_2F a, D2D1_POINT_2F b, uint32_t color, float width) {
    brush_->SetColor(Color(color));
    target_->DrawLine(a, b, brush_.get(), width, round_.get());
}

void ChatWindow::MeasureFeed(float column) {
    for (FeedItem& item : feed_) {
        const float width = item.kind == Kind::User ? column * 0.85f - 2 * kBubblePad : column;
        if (item.layout && item.layoutWidth == width) {
            continue;
        }
        IDWriteTextFormat* format = item.kind == Kind::Notice ? notice_.get() : body_.get();
        item.layout.reset();
        if (FAILED(dwrite_->CreateTextLayout(item.text.c_str(), static_cast<UINT32>(item.text.size()), format,
                                             std::max(width, 1.f), 1e6f, item.layout.put()))) {
            item.height = 0;
            continue;
        }
        item.layoutWidth = width;
        DWRITE_TEXT_METRICS metrics{};
        item.layout->GetMetrics(&metrics);
        item.textWidth = metrics.widthIncludingTrailingWhitespace;
        item.height = metrics.height + (item.kind == Kind::User ? 2 * kBubblePad : 0.f);
    }
}

float ChatWindow::FeedHeight() const {
    float total = 2 * kGap;
    for (const FeedItem& item : feed_) {
        total += item.height + kGap;
    }
    return total;
}

bool ChatWindow::HasConversation() const {
    return std::ranges::any_of(feed_, [](const FeedItem& item) { return item.kind != Kind::Notice; });
}

void ChatWindow::Paint() {
    EnsureTarget();
    if (!target_) {
        return;
    }
    const Geometry g = Layout();
    target_->BeginDraw();
    target_->Clear(Color(kWindow));
    PaintFeed(g);
    PaintHeader(g);
    PaintInput(g);
    if (popoverOpen_) {
        PaintPopover(g);
    }
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        target_.reset();
        brush_.reset();
    }
}

void ChatWindow::PaintFeed(const Geometry& g) {
    const float left = (g.width - g.column) / 2;
    if (!HasConversation()) {
        const std::wstring hello = userName_.empty() ? L"Что дальше?" : L"Что дальше, " + ToWide(userName_) + L"?";
        const float middle = (g.feed.top + g.feed.bottom) / 2;
        Text(hello, greeting_.get(), D2D1::RectF(left, middle - 40, left + g.column, middle), kPrimary);
        std::wstring hint = L"Enter — отправить · Shift+Enter — новая строка · Esc — стоп\n"
                            L"Ctrl+N — новый чат · Ctrl+H — история · правый клик по сообщению — копировать";
        if (!route_.empty()) {
            hint += L"\nсеть: " + route_;
        }
        Text(hint, notice_.get(), D2D1::RectF(left, middle + 4, left + g.column, middle + 70), kSecondary);
    }

    MeasureFeed(g.column);
    maxScroll_ = std::max(0.f, FeedHeight() - (g.feed.bottom - g.feed.top));
    if (stick_) {
        scroll_ = maxScroll_;
    }
    scroll_ = std::clamp(scroll_, 0.f, maxScroll_);

    target_->PushAxisAlignedClip(g.feed, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = g.feed.top + kGap - scroll_;
    for (FeedItem& item : feed_) {
        item.top = y;
        const bool visible = item.layout && y + item.height >= g.feed.top && y <= g.feed.bottom;
        if (visible && !(item.kind == Kind::Notice && !HasConversation())) {
            uint32_t color = kPrimary;
            float x = left;
            if (item.kind == Kind::User) {
                const float width = item.textWidth + 2 * kBubblePad;
                x = left + g.column - width;
                Fill(D2D1::RectF(x, y, x + width, y + item.height), 10.f, kUserBubble);
                x += kBubblePad;
            } else if (item.kind == Kind::Notice) {
                color = kSecondary;
            } else if (item.kind == Kind::Error) {
                color = kDanger;
            }
            brush_->SetColor(Color(color));
            target_->DrawTextLayout({x, y + (item.kind == Kind::User ? kBubblePad : 0.f)}, item.layout.get(),
                                    brush_.get());
        }
        y += item.height + kGap;
    }
    if (maxScroll_ > 0) {  // a thin scroll thumb on the right edge
        const float track = g.feed.bottom - g.feed.top;
        const float thumb = std::max(24.f, track * track / (track + maxScroll_));
        const float top = g.feed.top + (track - thumb) * (scroll_ / maxScroll_);
        Fill(D2D1::RectF(g.width - 7.f, top, g.width - 3.f, top + thumb), 2.f, kLine);
    }
    target_->PopAxisAlignedClip();
}

void ChatWindow::PaintHeader(const Geometry& g) {
    brush_->SetColor(Color(kPanel));
    target_->FillRectangle(D2D1::RectF(0, 0, g.width, kHeader), brush_.get());
    brush_->SetColor(Color(kLine));
    target_->FillRectangle(D2D1::RectF(0, kHeader - 1, g.width, kHeader), brush_.get());

    std::wstring title = title_.empty() ? L"Новый чат" : title_;
    const float titleRight = g.history.left - 12;
    Text(title, header_.get(), D2D1::RectF(16, 0, titleRight, kHeader), kPrimary);
    std::wstring state = busy_ ? L"отвечает…" : L"";
    if (!status_.empty()) {
        state = status_;
    }
    if (!state.empty()) {
        const float titleWidth = std::min(TextWidth(title, header_.get()), titleRight - 16);
        Text(state, ui_.get(), D2D1::RectF(16 + titleWidth + 12, 0, titleRight, kHeader), kSecondary);
    }

    for (const Target button : {Target::History, Target::NewChat}) {
        const D2D1_RECT_F& r = button == Target::History ? g.history : g.newChat;
        const bool active = hover_.target == button || (button == Target::History && popoverOpen_);
        if (active) {
            Fill(r, 6.f, kRaised);
        }
        const D2D1_POINT_2F c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        const uint32_t color = active ? kPrimary : kSecondary;
        if (button == Target::History) {  // a clock
            brush_->SetColor(Color(color));
            target_->DrawEllipse(D2D1::Ellipse(c, 7.5f, 7.5f), brush_.get(), 1.5f);
            Line(c, {c.x, c.y - 4.5f}, color, 1.5f);
            Line(c, {c.x + 3.5f, c.y + 1.5f}, color, 1.5f);
        } else {  // a plus
            Line({c.x - 6.5f, c.y}, {c.x + 6.5f, c.y}, color, 1.6f);
            Line({c.x, c.y - 6.5f}, {c.x, c.y + 6.5f}, color, 1.6f);
        }
    }
}

void ChatWindow::PaintInput(const Geometry& g) {
    Fill(g.box, 12.f, kCard);
    brush_->SetColor(inputFocused_ ? Color(kAccent, 0.75f) : Color(kLine));
    target_->DrawRoundedRectangle(D2D1::RoundedRect(g.box, 12.f, 12.f), brush_.get(), 1.f);

    if (hover_.target == Target::Model) {
        Fill(g.model, 6.f, kRaised);
    }
    Text(ModelLabel() + L"  ▾", ui_.get(), D2D1::RectF(g.model.left + 10, g.model.top, g.model.right, g.model.bottom),
         hover_.target == Target::Model ? kPrimary : kSecondary);

    if (!limits_.empty()) {
        Text(limits_, uiRight_.get(), D2D1::RectF(g.model.right + 8, g.send.top, g.send.left - 10, g.send.bottom),
             kSecondary);
    }

    const bool live = hasText_ || busy_;
    const D2D1_POINT_2F c{(g.send.left + g.send.right) / 2, (g.send.top + g.send.bottom) / 2};
    const float radius = (g.send.right - g.send.left) / 2;
    brush_->SetColor(Color(live ? kAccent : kRaised, live && hover_.target == Target::Send ? 0.85f : 1.f));
    target_->FillEllipse(D2D1::Ellipse(c, radius, radius), brush_.get());
    const uint32_t mark = live ? kPrimary : kSecondary;
    if (busy_) {  // stop
        Fill(D2D1::RectF(c.x - 4.5f, c.y - 4.5f, c.x + 4.5f, c.y + 4.5f), 1.5f, mark);
    } else {  // an arrow up
        Line({c.x, c.y + 5.5f}, {c.x, c.y - 5.5f}, mark, 1.8f);
        Line({c.x - 4.5f, c.y - 1.f}, {c.x, c.y - 5.5f}, mark, 1.8f);
        Line({c.x + 4.5f, c.y - 1.f}, {c.x, c.y - 5.5f}, mark, 1.8f);
    }
}

void ChatWindow::PaintPopover(const Geometry& g) {
    Fill(D2D1::RectF(g.popover.left + 2, g.popover.top + 4, g.popover.right + 2, g.popover.bottom + 6), 10.f, 0x000000,
         0.35f);  // a soft shadow
    Fill(g.popover, 10.f, kCard);
    brush_->SetColor(Color(kLine));
    target_->DrawRoundedRectangle(D2D1::RoundedRect(g.popover, 10.f, 10.f), brush_.get(), 1.f);
    Fill(g.search, 6.f, kRaised);

    if (filtered_.empty()) {
        const std::wstring empty = sessions_.empty() ? L"Чатов пока нет" : L"Ничего не нашлось";
        Text(empty, ui_.get(), D2D1::RectF(g.search.left + 10, g.listTop, g.search.right, g.listTop + kRow),
             kSecondary);
        return;
    }
    const size_t last = std::min(filtered_.size(), firstRow_ + kRowsShown);
    for (size_t row = firstRow_; row < last; ++row) {
        const SessionInfo& session = sessions_[filtered_[row]];
        const float top = g.listTop + static_cast<float>(row - firstRow_) * kRow;
        const D2D1_RECT_F r = D2D1::RectF(g.popover.left + 6, top, g.popover.right - 6, top + kRow);
        if (row == selected_) {
            Fill(r, 6.f, kRaised);
        }
        const std::wstring age = Age(session.modified);
        const float ageWidth = TextWidth(age, uiRight_.get());
        const bool current = session.id == sessionId_;
        Text(session.title.empty() ? L"Без названия" : session.title, ui_.get(),
             D2D1::RectF(r.left + 10, r.top, r.right - ageWidth - 22, r.bottom), current ? kAccent : kPrimary);
        Text(age, uiRight_.get(), D2D1::RectF(r.right - ageWidth - 12, r.top, r.right - 10, r.bottom), kSecondary);
    }
}

void ChatWindow::ScrollBy(float delta) {
    scroll_ = std::clamp(scroll_ + delta, 0.f, maxScroll_);
    stick_ = scroll_ >= maxScroll_ - 1.f;
    Refresh();
}

void ChatWindow::CopyItemAt(float y) {
    for (const FeedItem& item : feed_) {
        if (y < item.top || y >= item.top + item.height || item.text.empty()) {
            continue;
        }
        if (!OpenClipboard(hwnd_)) {
            return;
        }
        EmptyClipboard();
        const size_t bytes = (item.text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes); memory != nullptr) {
            void* data = GlobalLock(memory);
            if (data != nullptr) {
                std::memcpy(data, item.text.c_str(), bytes);
                GlobalUnlock(memory);
            }
            if (data == nullptr || SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
        Flash(L"скопировано");
        return;
    }
}

void ChatWindow::Flash(std::wstring status) {
    status_ = std::move(status);
    SetTimer(hwnd_, kStatusTimer, 1500, nullptr);
    Refresh();
}

void ChatWindow::OpenPopover() {
    sessions_ = ListSessions(workspace_);
    popoverOpen_ = true;
    if (search_ == nullptr) {
        search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_, nullptr,
                                  instance_, nullptr);
        SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_.get()), TRUE);
        SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Поиск по чатам"));
        SetWindowSubclass(search_, &ChatWindow::SearchProc, kSubclassId, reinterpret_cast<DWORD_PTR>(this));
    }
    SetWindowTextW(search_, L"");
    FilterSessions();
    PlaceChildren();
    SetFocus(search_);
    Refresh();
}

void ChatWindow::ClosePopover() {
    if (!popoverOpen_) {
        return;
    }
    popoverOpen_ = false;
    if (search_ != nullptr) {
        RemoveWindowSubclass(search_, &ChatWindow::SearchProc, kSubclassId);
        DestroyWindow(search_);
        search_ = nullptr;
    }
    SetFocus(input_);
    Refresh();
}

void ChatWindow::FilterSessions() {
    const std::wstring query = search_ != nullptr ? Lowercase(WindowText(search_)) : L"";
    filtered_.clear();
    for (size_t i = 0; i < sessions_.size(); ++i) {
        if (query.empty() || Lowercase(sessions_[i].title).find(query) != std::wstring::npos) {
            filtered_.push_back(i);
        }
    }
    firstRow_ = 0;
    selected_ = 0;
    PlaceChildren();
    Refresh();
}

void ChatWindow::MoveSelection(int delta) {
    if (filtered_.empty()) {
        return;
    }
    const auto next = std::clamp<long long>(static_cast<long long>(selected_) + delta, 0,
                                            static_cast<long long>(filtered_.size()) - 1);
    selected_ = static_cast<size_t>(next);
    if (selected_ < firstRow_) {
        firstRow_ = selected_;
    } else if (selected_ >= firstRow_ + kRowsShown) {
        firstRow_ = selected_ + 1 - kRowsShown;
    }
    Refresh();
}

void ChatWindow::OpenSession(size_t filteredIndex) {
    if (filteredIndex >= filtered_.size()) {
        return;
    }
    const SessionInfo session = sessions_[filtered_[filteredIndex]];
    ClosePopover();
    if (session.id == sessionId_) {
        return;
    }
    ResetChat();
    bool truncated = false;
    const std::vector<TranscriptEntry> entries = LoadTranscript(workspace_, session.id, kTranscriptLimit, truncated);
    if (truncated) {
        Add(Kind::Notice, std::format(L"Показаны последние {} сообщений.", kTranscriptLimit));
    }
    for (const TranscriptEntry& entry : entries) {
        switch (entry.kind) {
        case EntryKind::User:
            Add(Kind::User, entry.text);
            break;
        case EntryKind::Assistant:
            Add(Kind::Assistant, entry.text);
            break;
        case EntryKind::Tool:
            Add(Kind::Notice, L"инструмент: " + entry.text);
            break;
        }
    }
    sessionId_ = session.id;
    title_ = session.title;
    StartClaude();
}

std::wstring ChatWindow::ModelLabel() const {
    const std::string wanted = settings_.model.empty() ? "default" : settings_.model;
    std::wstring label = ToWide(wanted);
    for (const Json& model : models_) {
        if (JsonStr(model, "value") == wanted) {
            label = ToWide(JsonStr(model, "displayName"));
            if (const size_t cut = label.find(L" ("); cut != std::wstring::npos) {
                label.resize(cut);  // "Default (recommended)"
            }
        }
    }
    if (!settings_.effort.empty()) {
        label += L" · " + ToWide(settings_.effort);
    }
    return label;
}

void ChatWindow::ShowModelMenu(const Geometry& g) {
    constexpr UINT kModelBase = 100;
    constexpr UINT kEffortBase = 300;
    const std::string current = settings_.model.empty() ? "default" : settings_.model;
    const UINT disabled = busy_ ? MF_GRAYED : 0;

    wil::unique_hmenu menu(CreatePopupMenu());
    HMENU efforts = CreatePopupMenu();
    std::vector<std::string> levels = {"low", "medium", "high", "xhigh", "max"};
    for (size_t i = 0; i < models_.size(); ++i) {
        const Json& model = models_[i];
        const std::string value = JsonStr(model, "value");
        std::wstring text = ToWide(JsonStr(model, "displayName"));
        if (const std::string about = JsonStr(model, "description"); !about.empty()) {
            text += L"\t" + ToWide(about);
        }
        AppendMenuW(menu.get(), MF_STRING | disabled | (value == current ? MF_CHECKED : 0),
                    kModelBase + static_cast<UINT>(i), text.c_str());
        if (value == current && JsonAt(model, "supportedEffortLevels").is_array()) {
            levels.clear();
            for (const Json& level : JsonAt(model, "supportedEffortLevels")) {
                if (level.is_string()) {
                    levels.push_back(level.get<std::string>());
                }
            }
        }
    }
    if (models_.empty()) {
        AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, L"Список моделей ещё не пришёл от CLI");
    }
    AppendMenuW(efforts, MF_STRING | disabled | (settings_.effort.empty() ? MF_CHECKED : 0), kEffortBase,
                L"по умолчанию");
    for (size_t i = 0; i < levels.size(); ++i) {
        AppendMenuW(efforts, MF_STRING | disabled | (levels[i] == settings_.effort ? MF_CHECKED : 0),
                    kEffortBase + 1 + static_cast<UINT>(i), ToWide(levels[i]).c_str());
    }
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_POPUP, reinterpret_cast<UINT_PTR>(efforts), L"Effort");

    POINT at{ToPx(g.model.left), ToPx(g.model.top) - 4};
    ClientToScreen(hwnd_, &at);
    const UINT chosen = static_cast<UINT>(
        TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_BOTTOMALIGN | TPM_LEFTALIGN, at.x, at.y, 0, hwnd_, nullptr));
    if (chosen == 0) {
        return;
    }
    Settings next = settings_;
    if (chosen >= kModelBase && chosen < kModelBase + models_.size()) {
        const std::string value = JsonStr(models_[chosen - kModelBase], "value");
        next.model = value == "default" ? "" : value;
    } else if (chosen == kEffortBase) {
        next.effort.clear();
    } else if (chosen > kEffortBase && chosen <= kEffortBase + levels.size()) {
        next.effort = levels[chosen - kEffortBase - 1];
    }
    if (next.model == settings_.model && next.effort == settings_.effort) {
        return;
    }
    settings_ = next;
    SaveSettings(settings_);
    PlaceChildren();
    Restart();  // the same conversation goes on with the new model
}

bool ChatWindow::StartClaude() {
    ClaudeOptions options;
    options.exe = FindClaudeExe();
    if (options.exe.empty()) {
        Add(Kind::Error, L"Не нашёл claude.exe ни в %USERPROFILE%\\.local\\bin, ни в PATH.");
        return false;
    }
    options.cwd = workspace_;
    options.proxy = PickProxy();
    options.model = settings_.model;
    options.effort = settings_.effort;
    options.resume = sessionId_;  // a chat from the history, a restart, a crash: the same conversation
    route_ = options.proxy.empty() ? L"напрямую" : L"через " + options.proxy.substr(options.proxy.rfind(L'/') + 1);

    const WPARAM generation = ++generation_;
    const HWND hwnd = hwnd_;
    try {
        claude_.Start(
            options,
            [hwnd, generation](Json message) {
                auto payload = std::make_unique<Json>(std::move(message));
                if (PostMessageW(hwnd, kClaudeMessage, generation, reinterpret_cast<LPARAM>(payload.get()))) {
                    payload.release();
                }
            },
            [hwnd, generation](DWORD code, std::string stderrTail) {
                auto payload = std::make_unique<ExitInfo>(ExitInfo{code, std::move(stderrTail)});
                if (PostMessageW(hwnd, kClaudeExit, generation, reinterpret_cast<LPARAM>(payload.get()))) {
                    payload.release();
                }
            });
    } catch (const std::exception& e) {
        Add(Kind::Error, L"Не запустился claude.exe: " + ToWide(e.what()));
        return false;
    }
    running_ = true;
    Refresh();
    return true;
}

void ChatWindow::Restart() {
    ++generation_;
    claude_.Stop();
    running_ = false;
    busy_ = false;
    StartClaude();
}

void ChatWindow::ResetChat() {
    ++generation_;
    claude_.Stop();
    running_ = false;
    busy_ = false;
    sessionId_.clear();
    title_.clear();
    currentMessageId_.clear();
    streamed_.clear();
    feed_.clear();
    scroll_ = 0;
    stick_ = true;
}

void ChatWindow::NewChat() {
    ClosePopover();
    ResetChat();
    StartClaude();
}

void ChatWindow::Submit() {
    std::wstring text = NormalizeNewlines(WindowText(input_));
    const size_t first = text.find_first_not_of(L" \t\n");
    if (first == std::wstring::npos) {
        return;
    }
    text = text.substr(first, text.find_last_not_of(L" \t\n") - first + 1);
    if (!running_ && !StartClaude()) {
        return;
    }
    claude_.SendUserText(ToUtf8(text));
    Add(Kind::User, std::move(text));
    SetWindowTextW(input_, L"");
    busy_ = true;
    stick_ = true;
    Refresh();
}

void ChatWindow::OnClaude(const Json& message) {
    const std::string type = JsonStr(message, "type");
    if (type == "system") {
        if (JsonStr(message, "subtype") == "init") {
            sessionId_ = JsonStr(message, "session_id");
        }
    } else if (type == "stream_event") {
        const Json& event = JsonAt(message, "event");
        const std::string eventType = JsonStr(event, "type");
        const Json& index = JsonAt(event, "index");
        const int block = index.is_number_integer() ? index.get<int>() : -1;
        if (eventType == "message_start") {
            currentMessageId_ = JsonStr(JsonAt(event, "message"), "id");
        } else if (eventType == "content_block_start" && JsonStr(JsonAt(event, "content_block"), "type") == "text") {
            FeedItem item;
            item.kind = Kind::Assistant;
            item.messageId = currentMessageId_;
            item.block = block;
            feed_.push_back(std::move(item));
            streamed_.insert(currentMessageId_);
        } else if (eventType == "content_block_delta") {
            const Json& delta = JsonAt(event, "delta");
            if (JsonStr(delta, "type") == "text_delta") {
                for (auto it = feed_.rbegin(); it != feed_.rend(); ++it) {
                    if (it->kind == Kind::Assistant && it->messageId == currentMessageId_ && it->block == block) {
                        it->text += ToWide(JsonStr(delta, "text"));
                        it->layout.reset();
                        break;
                    }
                }
            }
        }
    } else if (type == "assistant") {
        const Json& body = JsonAt(message, "message");
        const std::string id = JsonStr(body, "id");
        const Json& content = JsonAt(body, "content");
        if (content.is_array()) {
            for (const Json& block : content) {
                const std::string blockType = JsonStr(block, "type");
                if (blockType == "text" && !streamed_.contains(id)) {
                    Add(Kind::Assistant, ToWide(JsonStr(block, "text")));
                } else if (blockType == "tool_use") {
                    Add(Kind::Notice, L"инструмент: " + ToWide(JsonStr(block, "name")));
                }
            }
        }
    } else if (type == "result") {
        busy_ = false;
        const Json& isError = JsonAt(message, "is_error");
        if ((isError.is_boolean() && isError.get<bool>()) || JsonStr(message, "subtype") != "success") {
            std::string text = JsonStr(message, "result");
            if (text.empty()) {
                text = JsonStr(message, "subtype");
            }
            Add(Kind::Error, ToWide(text));
        }
        if (!sessionId_.empty()) {
            if (std::wstring title = SessionTitle(workspace_, sessionId_); !title.empty()) {
                title_ = std::move(title);
            }
        }
    } else if (type == "rate_limit_event") {
        const Json& windows = JsonAt(JsonAt(message, "rate_limit_info"), "unifiedWindows");
        const auto percent = [](const Json& window) {
            const Json& used = JsonAt(window, "utilization");
            return used.is_number() ? static_cast<int>(std::lround(used.get<double>() * 100)) : -1;
        };
        const int fiveHours = percent(JsonAt(windows, "five_hour"));
        const int week = percent(JsonAt(windows, "seven_day"));
        limits_.clear();
        if (fiveHours >= 0) {
            limits_ = std::format(L"5 ч {}%", fiveHours);
        }
        if (week >= 0) {
            limits_ += std::format(L"{}неделя {}%", limits_.empty() ? L"" : L"  ·  ", week);
        }
    } else if (type == "control_response") {
        const Json& response = JsonAt(message, "response");
        if (JsonStr(response, "request_id") == "aic_init") {
            const Json& answer = JsonAt(response, "response");
            if (JsonAt(answer, "models").is_array()) {
                models_ = JsonAt(answer, "models");
            }
            const std::string email = JsonStr(JsonAt(answer, "account"), "email");
            userName_ = email.substr(0, email.find('@'));
            PlaceChildren();  // the model chip's width
        }
    } else if (type == "control_request") {
        const std::string id = JsonStr(message, "request_id");
        if (JsonStr(JsonAt(message, "request"), "subtype") == "can_use_tool") {
            claude_.Respond(id, Json{{"behavior", "deny"}, {"message", "Tools are not connected in this build yet."}});
        } else {
            claude_.RespondError(id, "Not supported in this build yet.");
        }
    } else if (type == "aic_raw") {
        Add(Kind::Notice, ToWide(JsonStr(message, "text")));
    }
    Refresh();
}

void ChatWindow::OnClaudeExit(const ExitInfo& exit) {
    running_ = false;
    busy_ = false;
    std::wstring text = std::format(L"claude.exe завершился (код {}).", exit.code);
    if (const std::wstring tail = LastLines(exit.stderrTail, 6); !tail.empty()) {
        text += L"\n" + tail;
    }
    Add(exit.code == 0 ? Kind::Notice : Kind::Error, std::move(text));
}

void ChatWindow::Add(Kind kind, std::wstring text) {
    FeedItem item;
    item.kind = kind;
    item.text = std::move(text);
    feed_.push_back(std::move(item));
    Refresh();
}

}  // namespace

int RunChatWindow(HINSTANCE instance, int show) {
    try {
        ChatWindow window(instance);
        return window.Run(show);
    } catch (const std::exception& e) {
        MessageBoxW(nullptr, ToWide(e.what()).c_str(), L"AIControls", MB_ICONERROR);
        return 1;
    }
}

}  // namespace aic
