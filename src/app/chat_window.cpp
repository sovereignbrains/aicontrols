#include "app/chat_window.h"

#include <windowsx.h>
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

namespace aic {
namespace {

// Posted from the reader thread; wParam is the chat's generation, lParam owns the payload.
constexpr UINT kClaudeMessage = WM_APP + 1;  // Json*
constexpr UINT kClaudeExit = WM_APP + 2;     // ExitInfo*
constexpr UINT_PTR kStatusTimer = 1;

// Layout, in DIPs.
constexpr float kTopBar = 34.f;
constexpr float kPad = 16.f;
constexpr float kGap = 12.f;
constexpr float kBubblePad = 10.f;
constexpr float kInputArea = 104.f;
constexpr float kInputMargin = 12.f;
constexpr float kInputInset = 10.f;
constexpr float kColumn = 760.f;

// Colors, 0xRRGGBB.
constexpr uint32_t kBackground = 0x1b1c1f;
constexpr uint32_t kBar = 0x232428;
constexpr uint32_t kUserBubble = 0x2d3b55;
constexpr uint32_t kText = 0xe8e8ea;
constexpr uint32_t kMuted = 0x8d9097;
constexpr uint32_t kErrorText = 0xff7b72;
constexpr uint32_t kInputBackground = 0x26272b;
constexpr uint32_t kBorder = 0x3a3c42;

constexpr COLORREF ToColorRef(uint32_t rgb) {
    return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
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

class ChatWindow {
public:
    explicit ChatWindow(HINSTANCE instance) : instance_(instance) {}
    int Run(int show);

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT OnInputFilter(const MSGFILTER& filter);

    float Dpi() const { return static_cast<float>(GetDpiForWindow(hwnd_)); }
    float ToDip(int px) const { return static_cast<float>(px) * 96.f / Dpi(); }
    int ToPx(float dip) const { return static_cast<int>(std::lround(dip * Dpi() / 96.f)); }
    D2D1_SIZE_F ClientDip() const;
    float ColumnWidth(float clientWidth) const { return std::min(clientWidth - 2 * kPad, kColumn); }

    void CreateInput();
    void StyleInput();
    void PlaceInput();
    void EnsureTarget();
    void Paint();
    void MeasureFeed(float column);
    float FeedHeight() const;
    void ScrollBy(float delta);
    void CopyItemAt(float y);
    void Flash(std::wstring status);

    bool StartClaude();
    void NewChat();
    void Submit();
    void OnClaude(const Json& message);
    void OnClaudeExit(const ExitInfo& exit);
    void Add(Kind kind, std::wstring text);
    void Refresh() { InvalidateRect(hwnd_, nullptr, FALSE); }

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    HWND input_ = nullptr;
    wil::unique_hmodule richEdit_;
    wil::com_ptr<ID2D1Factory> d2d_;
    wil::com_ptr<IDWriteFactory> dwrite_;
    wil::com_ptr<ID2D1HwndRenderTarget> target_;
    wil::com_ptr<ID2D1SolidColorBrush> brush_;
    wil::com_ptr<IDWriteTextFormat> body_;
    wil::com_ptr<IDWriteTextFormat> notice_;
    wil::com_ptr<IDWriteTextFormat> bar_;

    std::vector<FeedItem> feed_;
    float scroll_ = 0;
    float maxScroll_ = 0;
    bool stick_ = true;  // follow the end of the feed as it grows

    ClaudeProcess claude_;
    WPARAM generation_ = 0;  // bumped per process; stale posts are dropped
    bool running_ = false;
    bool busy_ = false;
    std::string sessionId_;
    std::string model_;
    std::string currentMessageId_;
    std::set<std::string> streamed_;  // messages whose text came as deltas
    std::wstring limits_;
    std::wstring route_;
    std::wstring status_;
};

int ChatWindow::Run(int show) {
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put()));
    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite_.put())));
    THROW_IF_FAILED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.f,
                                              L"ru-ru", body_.put()));
    THROW_IF_FAILED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f,
                                              L"ru-ru", notice_.put()));
    notice_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    THROW_IF_FAILED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.f,
                                              L"ru-ru", bar_.put()));
    bar_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    bar_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

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
    SetWindowPos(hwnd_, nullptr, 0, 0, ToPx(980.f), ToPx(820.f), SWP_NOMOVE | SWP_NOZORDER);
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

LRESULT ChatWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        constexpr DWORD kImmersiveDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, kImmersiveDarkMode, &dark, sizeof(dark));
        CreateInput();
        Add(Kind::Notice, L"Enter — отправить · Shift+Enter — новая строка · Esc — остановить ответ · "
                          L"Ctrl+N — новый чат · правый клик по сообщению — копировать");
        return 0;
    }
    case WM_SIZE:
        if (target_) {
            target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        PlaceInput();
        Refresh();
        return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        target_.reset();
        brush_.reset();
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        StyleInput();
        PlaceInput();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        if (hwnd_ != nullptr) {
            info->ptMinTrackSize = {ToPx(480.f), ToPx(420.f)};
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        Paint();
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_MOUSEWHEEL:
        ScrollBy(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA * 64.f);
        return 0;
    case WM_RBUTTONUP:
        CopyItemAt(ToDip(GET_Y_LPARAM(lParam)));
        return 0;
    case WM_SETFOCUS:
        SetFocus(input_);
        return 0;
    case WM_TIMER:
        if (wParam == kStatusTimer) {
            KillTimer(hwnd_, kStatusTimer);
            status_.clear();
            Refresh();
        }
        return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->hwndFrom == input_ && header->code == EN_MSGFILTER) {
            return OnInputFilter(*reinterpret_cast<const MSGFILTER*>(lParam));
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

// Keys typed into the input: Enter sends, Esc stops the answer, Ctrl+N starts over.
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
    }
    if (filter.msg == WM_CHAR && (filter.wParam == L'\r' && !shift)) {
        return 1;  // the Enter that sent the message
    }
    return 0;
}

D2D1_SIZE_F ChatWindow::ClientDip() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    return {ToDip(client.right), ToDip(client.bottom)};
}

void ChatWindow::CreateInput() {
    richEdit_.reset(LoadLibraryW(L"Msftedit.dll"));
    THROW_LAST_ERROR_IF_NULL(richEdit_.get());
    input_ = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL, 0, 0,
                             0, 0, hwnd_, nullptr, instance_, nullptr);
    THROW_LAST_ERROR_IF_NULL(input_);
    SendMessageW(input_, EM_SETTEXTMODE, TM_PLAINTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE, 0);
    SendMessageW(input_, EM_SETEVENTMASK, 0, ENM_KEYEVENTS);
    StyleInput();
    PlaceInput();
    SetFocus(input_);
}

void ChatWindow::StyleInput() {
    SendMessageW(input_, EM_SETBKGNDCOLOR, 0, ToColorRef(kInputBackground));
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
    format.crTextColor = ToColorRef(kText);
    format.yHeight = 11 * 20;  // twips
    wcscpy_s(format.szFaceName, L"Segoe UI");
    SendMessageW(input_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    SendMessageW(input_, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));
}

void ChatWindow::PlaceInput() {
    if (input_ == nullptr) {
        return;
    }
    const D2D1_SIZE_F size = ClientDip();
    const float left = kPad + kInputInset;
    const float right = size.width - kPad - kInputInset;
    const float top = size.height - kInputArea + kInputMargin + kInputInset;
    const float bottom = size.height - kInputMargin - kInputInset;
    MoveWindow(input_, ToPx(left), ToPx(top), std::max(0, ToPx(right) - ToPx(left)),
               std::max(0, ToPx(bottom) - ToPx(top)), TRUE);
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
    target_->CreateSolidColorBrush(D2D1::ColorF(kText), brush_.put());
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
    float total = 2 * kPad;
    for (const FeedItem& item : feed_) {
        total += item.height + kGap;
    }
    return total;
}

void ChatWindow::Paint() {
    EnsureTarget();
    if (!target_) {
        return;
    }
    const D2D1_SIZE_F size = target_->GetSize();
    const float feedTop = kTopBar;
    const float feedBottom = size.height - kInputArea;
    const float column = ColumnWidth(size.width);
    const float left = (size.width - column) / 2;

    MeasureFeed(column);
    maxScroll_ = std::max(0.f, FeedHeight() - (feedBottom - feedTop));
    if (stick_) {
        scroll_ = maxScroll_;
    }
    scroll_ = std::clamp(scroll_, 0.f, maxScroll_);

    target_->BeginDraw();
    target_->Clear(D2D1::ColorF(kBackground));

    target_->PushAxisAlignedClip(D2D1::RectF(0, feedTop, size.width, feedBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = feedTop + kPad - scroll_;
    for (FeedItem& item : feed_) {
        item.top = y;
        if (item.layout && y + item.height >= feedTop && y <= feedBottom) {
            switch (item.kind) {
            case Kind::User: {
                const float width = item.textWidth + 2 * kBubblePad;
                const float x = left + column - width;
                brush_->SetColor(D2D1::ColorF(kUserBubble));
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(x, y, x + width, y + item.height), 8.f, 8.f), brush_.get());
                brush_->SetColor(D2D1::ColorF(kText));
                target_->DrawTextLayout({x + kBubblePad, y + kBubblePad}, item.layout.get(), brush_.get());
                break;
            }
            case Kind::Assistant:
                brush_->SetColor(D2D1::ColorF(kText));
                target_->DrawTextLayout({left, y}, item.layout.get(), brush_.get());
                break;
            case Kind::Notice:
                brush_->SetColor(D2D1::ColorF(kMuted));
                target_->DrawTextLayout({left, y}, item.layout.get(), brush_.get());
                break;
            case Kind::Error:
                brush_->SetColor(D2D1::ColorF(kErrorText));
                target_->DrawTextLayout({left, y}, item.layout.get(), brush_.get());
                break;
            }
        }
        y += item.height + kGap;
    }
    if (maxScroll_ > 0) {  // a thin scroll thumb on the right edge
        const float track = feedBottom - feedTop;
        const float thumb = std::max(24.f, track * track / (track + maxScroll_));
        const float thumbTop = feedTop + (track - thumb) * (scroll_ / maxScroll_);
        brush_->SetColor(D2D1::ColorF(kBorder));
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(size.width - 7.f, thumbTop, size.width - 3.f, thumbTop + thumb), 2.f, 2.f),
            brush_.get());
    }
    target_->PopAxisAlignedClip();

    brush_->SetColor(D2D1::ColorF(kBar));
    target_->FillRectangle(D2D1::RectF(0, 0, size.width, kTopBar), brush_.get());
    std::wstring title = L"AIControls";
    if (!model_.empty()) {
        title += L"  ·  " + ToWide(model_);
    }
    if (busy_) {
        title += L"  ·  отвечает (Esc — стоп)";
    }
    if (!status_.empty()) {
        title += L"  ·  " + status_;
    }
    brush_->SetColor(D2D1::ColorF(kMuted));
    target_->DrawText(title.c_str(), static_cast<UINT32>(title.size()), bar_.get(),
                      D2D1::RectF(kPad, 0, size.width - kPad, kTopBar), brush_.get());
    std::wstring right = limits_;
    if (!route_.empty()) {
        right += (right.empty() ? L"" : L"  ·  ") + route_;
    }
    wil::com_ptr<IDWriteTextLayout> rightLayout;
    if (!right.empty() &&
        SUCCEEDED(dwrite_->CreateTextLayout(right.c_str(), static_cast<UINT32>(right.size()), bar_.get(),
                                            std::max(size.width - 2 * kPad, 1.f), kTopBar, rightLayout.put()))) {
        rightLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        target_->DrawTextLayout({kPad, 0}, rightLayout.get(), brush_.get());
    }

    brush_->SetColor(D2D1::ColorF(kBar));
    target_->FillRectangle(D2D1::RectF(0, feedBottom, size.width, size.height), brush_.get());
    const D2D1_ROUNDED_RECT box = D2D1::RoundedRect(
        D2D1::RectF(kPad, feedBottom + kInputMargin, size.width - kPad, size.height - kInputMargin), 8.f, 8.f);
    brush_->SetColor(D2D1::ColorF(kInputBackground));
    target_->FillRoundedRectangle(box, brush_.get());
    brush_->SetColor(D2D1::ColorF(kBorder));
    target_->DrawRoundedRectangle(box, brush_.get());

    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        target_.reset();
        brush_.reset();
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

bool ChatWindow::StartClaude() {
    ClaudeOptions options;
    options.exe = FindClaudeExe();
    if (options.exe.empty()) {
        Add(Kind::Error, L"Не нашёл claude.exe ни в %USERPROFILE%\\.local\\bin, ни в PATH.");
        return false;
    }
    std::array<wchar_t, MAX_PATH> home{};
    if (const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", home.data(), MAX_PATH);
        length > 0 && length < MAX_PATH) {
        options.cwd.assign(home.data(), length);
    }
    options.proxy = PickProxy();
    options.resume = sessionId_;  // after a crash: the same conversation, not a blank one
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

void ChatWindow::NewChat() {
    ++generation_;
    claude_.Stop();
    running_ = false;
    busy_ = false;
    sessionId_.clear();
    model_.clear();
    currentMessageId_.clear();
    streamed_.clear();
    feed_.clear();
    stick_ = true;
    Add(Kind::Notice, L"Новый чат.");
    StartClaude();
}

void ChatWindow::Submit() {
    const int length = GetWindowTextLengthW(input_);
    std::wstring raw(static_cast<size_t>(length), L'\0');
    GetWindowTextW(input_, raw.data(), length + 1);
    std::wstring text = NormalizeNewlines(raw);
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
            model_ = JsonStr(message, "model");
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
