#include "app/chat_window.h"

#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <richedit.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "app/fonts.h"
#include "app/resource.h"
#include "core/attachment.h"
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
constexpr float kLogo = 28.f;
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
constexpr float kHistoryWidth = 380.f;
constexpr float kHistoryRow = 36.f;
constexpr size_t kHistoryRows = 12;
constexpr float kMenuRow = 30.f;
constexpr float kModelsWidth = 270.f;
constexpr float kMoreWidth = 230.f;
constexpr float kEffortWidth = 250.f;
constexpr float kEffortHeight = 112.f;
constexpr float kAttachmentRow = 56.f;
constexpr float kFileChip = 190.f;

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

// std::clamp asserts when lo > hi, which a narrow or minimized window makes of the panels' bounds.
float ClampTo(float value, float lo, float hi) {
    return std::clamp(value, lo, std::max(lo, hi));
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

// The panels that open over the window; one at a time.
enum class Popup { None, History, Models, Effort };

enum class Target {
    None,
    History,
    NewChat,
    Attach,
    AttachmentChip,    // row: an index into the attachments; shows its remove button
    RemoveAttachment,  // row: the same
    ModelChip,
    EffortChip,
    Send,
    HistoryRow,   // row: an index into the filtered history
    ModelRow,     // row: an index into the main models
    MoreModels,   // the row that opens the other models
    MoreRow,      // row: an index into the other models
    EffortTrack,
    EffortReset,
    InsidePopup,  // on a panel, nothing to click
};

struct Hit {
    Target target = Target::None;
    size_t row = 0;
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
    D2D1_RECT_F attachments{};  // the row of attached files above the text
    D2D1_RECT_F text{};
    D2D1_RECT_F attach{};
    D2D1_RECT_F limits{};
    D2D1_RECT_F model{};
    D2D1_RECT_F effort{};  // empty when the model has no effort levels
    D2D1_RECT_F send{};
    D2D1_RECT_F popup{};   // the open panel
    // History.
    D2D1_RECT_F search{};
    float listTop = 0;
    // Models.
    float modelsTop = 0;
    D2D1_RECT_F more{};
    D2D1_RECT_F submenu{};
    // Effort.
    D2D1_RECT_F track{};
    D2D1_RECT_F reset{};
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

std::wstring ClipboardText(HWND owner) {
    std::wstring text;
    if (!OpenClipboard(owner)) {
        return text;
    }
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT); data != nullptr) {
        if (const auto* chars = static_cast<const wchar_t*>(GlobalLock(data)); chars != nullptr) {
            text = chars;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
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

std::wstring EffortName(const std::string& level) {
    if (level.empty()) {
        return L"Auto";
    }
    if (level == "xhigh") {
        return L"XHigh";
    }
    std::wstring name = ToWide(level);
    name[0] = static_cast<wchar_t>(std::towupper(name[0]));
    return name;
}

class ChatWindow {
public:
    explicit ChatWindow(HINSTANCE instance) : instance_(instance) {}
    int Run(int show);

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK InputProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                      DWORD_PTR self);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT OnInputFilter(const MSGFILTER& filter);
    void OnCommand(WPARAM wParam, LPARAM lParam);
    void OnClick(float x, float y);
    void OnPopupKey(WPARAM key);
    void OnPopupChar(wchar_t c);

    float Dpi() const { return static_cast<float>(GetDpiForWindow(hwnd_)); }
    float ToDip(int px) const { return static_cast<float>(px) * 96.f / Dpi(); }
    int ToPx(float dip) const { return static_cast<int>(std::lround(dip * Dpi() / 96.f)); }
    D2D1_POINT_2F CursorDip(LPARAM lParam) const {
        return {ToDip(GET_X_LPARAM(lParam)), ToDip(GET_Y_LPARAM(lParam))};
    }
    Geometry Layout() const;
    Hit HitTest(float x, float y) const;
    void MoveChild(HWND child, const D2D1_RECT_F& rect) const;

    wil::com_ptr<IDWriteTextFormat> MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                               DWRITE_TEXT_ALIGNMENT align, bool singleLine) const;
    float TextWidth(const std::wstring& text, IDWriteTextFormat* format) const;
    void CreateFonts();
    void CreateInput();
    void StyleInput();
    void PlaceChildren();
    void EnsureTarget();
    void LoadLogo();

    void Paint();
    void PaintFeed(const Geometry& g);
    void PaintHeader(const Geometry& g);
    void PaintInput(const Geometry& g);
    void PaintPanel(const D2D1_RECT_F& rect);
    void PaintHistory(const Geometry& g);
    void PaintModels(const Geometry& g);
    void PaintEffort(const Geometry& g);
    void Text(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, uint32_t color);
    void Fill(const D2D1_RECT_F& rect, float radius, uint32_t color, float alpha = 1.f);
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, uint32_t color, float width = 1.6f);
    void Check(D2D1_POINT_2F center, uint32_t color);
    void MeasureFeed(float column);
    float FeedHeight() const;
    void ScrollBy(float delta);
    void CopyItemAt(float y);
    void Flash(std::wstring status, UINT milliseconds = 1500);

    D2D1_RECT_F AttachmentRect(const Geometry& g, size_t index) const;
    static D2D1_RECT_F RemoveRect(const D2D1_RECT_F& chip);
    void PaintAttachments(const Geometry& g);
    void PickFiles();
    void AttachFile(const std::wstring& path);
    void AttachClipboardImage();
    void Attached();

    void OpenPopup(Popup popup);
    void ClosePopup();
    void FilterSessions();
    void MoveSelection(int delta);
    void OpenSession(size_t filteredIndex);

    void IndexModels();
    const Json* CurrentModel() const;
    std::wstring ModelName() const;
    std::vector<std::string> EffortLevels() const;
    void SetEffortAt(float x);
    void ChooseModel(size_t index);
    void Apply(const Settings& next);

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
    bool hasContent() const { return hasText_ || !attachments_.empty(); }

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    HWND input_ = nullptr;
    wil::unique_hmodule richEdit_;
    wil::unique_hfont uiFont_;

    wil::com_ptr<ID2D1Factory> d2d_;
    wil::com_ptr<IDWriteFactory> dwrite_;
    wil::com_ptr<IWICImagingFactory> wic_;
    wil::com_ptr<IDWriteFontCollection> fonts_;
    wil::com_ptr<ID2D1HwndRenderTarget> target_;
    wil::com_ptr<ID2D1SolidColorBrush> brush_;
    wil::com_ptr<IWICBitmap> logoSource_;
    wil::com_ptr<ID2D1Bitmap> logo_;  // per render target
    wil::com_ptr<ID2D1StrokeStyle> round_;
    wil::com_ptr<IDWriteTextFormat> body_;
    wil::com_ptr<IDWriteTextFormat> notice_;
    wil::com_ptr<IDWriteTextFormat> header_;
    wil::com_ptr<IDWriteTextFormat> ui_;
    wil::com_ptr<IDWriteTextFormat> uiBold_;
    wil::com_ptr<IDWriteTextFormat> uiRight_;
    wil::com_ptr<IDWriteTextFormat> badge_;
    wil::com_ptr<IDWriteTextFormat> greeting_;

    std::vector<FeedItem> feed_;
    float scroll_ = 0;
    float maxScroll_ = 0;
    bool stick_ = true;  // follow the end of the feed as it grows

    float textHeight_ = kTextMin;  // the input's text area, grows with what is typed
    bool inputFocused_ = false;
    bool hasText_ = false;
    std::vector<Attachment> attachments_;  // go with the next message
    Hit hover_;
    bool trackingMouse_ = false;

    Popup popup_ = Popup::None;
    // History.
    std::vector<SessionInfo> sessions_;
    std::vector<size_t> filtered_;
    std::wstring searchText_;
    size_t firstRow_ = 0;
    size_t selected_ = 0;
    // Models: indexes into models_.
    std::vector<size_t> mainModels_;
    std::vector<size_t> moreModels_;
    std::string defaultResolved_;  // what the CLI's "default" stands for
    bool moreOpen_ = false;
    // Effort, applied when its panel closes.
    std::string pendingEffort_;
    bool draggingEffort_ = false;

    std::wstring workspace_;
    Settings settings_;
    Json models_ = Json::array();
    std::string userName_;

    ClaudeProcess claude_;
    WPARAM generation_ = 0;  // bumped per process; stale posts are dropped
    bool running_ = false;
    bool busy_ = false;
    bool stateEvents_ = false;  // the CLI reports its session state; else a result ends the turn
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
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put()));
    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite_.put())));
    fonts_ = LoadFontCollection(dwrite_.get());
    RegisterGdiFonts(instance_);
    THROW_IF_FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(wic_.put())));
    D2D1_STROKE_STYLE_PROPERTIES stroke = D2D1::StrokeStyleProperties();
    stroke.startCap = stroke.endCap = D2D1_CAP_STYLE_ROUND;
    stroke.lineJoin = D2D1_LINE_JOIN_ROUND;
    THROW_IF_FAILED(d2d_->CreateStrokeStyle(stroke, nullptr, 0, round_.put()));
    body_ = MakeFormat(kTextFont, 15.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    notice_ = MakeFormat(kTextFont, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    header_ = MakeFormat(kDisplayFont, 12.f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    ui_ = MakeFormat(kTextFont, 13.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    uiBold_ = MakeFormat(kTextFont, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    uiRight_ = MakeFormat(kTextFont, 12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    badge_ = MakeFormat(kTextFont, 11.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    greeting_ = MakeFormat(kDisplayFont, 22.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &ChatWindow::Proc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                        LR_DEFAULTCOLOR));
    windowClass.lpszClassName = L"AIControlsWindow";
    RegisterClassExW(&windowClass);
    LoadLogo();

    CreateWindowExW(0, windowClass.lpszClassName, L"AIControls", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance_, this);
    THROW_LAST_ERROR_IF_NULL(hwnd_);
    SetWindowPos(hwnd_, nullptr, 0, 0, ToPx(1000.f), ToPx(840.f), SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    if (!fonts_) {
        Add(Kind::Error, L"Встроенные шрифты не загрузились — показываю системным.");
    }
    StartClaude();  // warm up: the first answer does not wait for the CLI to start

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

wil::com_ptr<IDWriteTextFormat> ChatWindow::MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                                       DWRITE_TEXT_ALIGNMENT align, bool singleLine) const {
    wil::com_ptr<IDWriteTextFormat> format;
    THROW_IF_FAILED(dwrite_->CreateTextFormat(family, fonts_.get(), weight, DWRITE_FONT_STYLE_NORMAL,
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
        DragAcceptFiles(hwnd_, TRUE);
        return 0;
    }
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            std::wstring path(DragQueryFileW(drop, i, nullptr, 0), L'\0');
            DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size() + 1));
            AttachFile(path);
        }
        DragFinish(drop);
        SetForegroundWindow(hwnd_);
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
        logo_.reset();
        CreateFonts();
        StyleInput();
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        PlaceChildren();
        return 0;
    }
    case WM_GETMINMAXINFO:
        if (hwnd_ != nullptr) {
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {ToPx(560.f), ToPx(480.f)};
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
    case WM_MOUSEMOVE: {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd_, 0};
            trackingMouse_ = TrackMouseEvent(&track) != FALSE;
        }
        const D2D1_POINT_2F at = CursorDip(lParam);
        if (draggingEffort_) {
            SetEffortAt(at.x);
            return 0;
        }
        if (const Hit hit = HitTest(at.x, at.y); !(hit == hover_)) {
            hover_ = hit;
            if (hit.target == Target::HistoryRow) {
                selected_ = hit.row;
            } else if (hit.target == Target::MoreModels) {
                moreOpen_ = true;
            } else if (hit.target == Target::ModelRow) {
                moreOpen_ = false;
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
            const bool clickable = hover_.target != Target::None && hover_.target != Target::InsidePopup &&
                                   hover_.target != Target::AttachmentChip;
            SetCursor(LoadCursorW(nullptr, clickable ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        const D2D1_POINT_2F at = CursorDip(lParam);
        if (HitTest(at.x, at.y).target == Target::EffortTrack) {
            draggingEffort_ = true;
            SetCapture(hwnd_);
            SetEffortAt(at.x);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (draggingEffort_) {
            draggingEffort_ = false;
            ReleaseCapture();
            return 0;
        }
        const D2D1_POINT_2F at = CursorDip(lParam);
        OnClick(at.x, at.y);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &at);
        const float steps = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        const Geometry g = Layout();
        if (popup_ == Popup::History && Inside(g.popup, ToDip(at.x), ToDip(at.y))) {
            const size_t maxFirst = filtered_.size() > kHistoryRows ? filtered_.size() - kHistoryRows : 0;
            const auto moved = static_cast<long long>(firstRow_) + static_cast<long long>(std::lround(steps * 3));
            firstRow_ = static_cast<size_t>(std::clamp<long long>(moved, 0, static_cast<long long>(maxFirst)));
            Refresh();
        } else if (popup_ == Popup::None) {
            ScrollBy(steps * 64.f);
        }
        return 0;
    }
    case WM_RBUTTONUP:
        if (popup_ == Popup::None) {
            CopyItemAt(CursorDip(lParam).y);
        }
        return 0;
    case WM_KEYDOWN:
        if (popup_ != Popup::None) {
            OnPopupKey(wParam);
        }
        return 0;
    case WM_CHAR:
        if (popup_ != Popup::None) {
            OnPopupChar(static_cast<wchar_t>(wParam));
        }
        return 0;
    case WM_SETFOCUS:
        if (popup_ == Popup::None) {  // with a panel open the keys stay here
            SetFocus(input_);
        }
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

void ChatWindow::OnClick(float x, float y) {
    const Hit hit = HitTest(x, y);
    if (popup_ != Popup::None) {
        switch (hit.target) {
        case Target::HistoryRow:
            OpenSession(hit.row);
            return;
        case Target::ModelRow:
            ChooseModel(mainModels_[hit.row]);
            return;
        case Target::MoreRow:
            ChooseModel(moreModels_[hit.row]);
            return;
        case Target::MoreModels:
            moreOpen_ = !moreOpen_;
            Refresh();
            return;
        case Target::EffortReset:
            pendingEffort_.clear();
            Refresh();
            return;
        case Target::EffortTrack:
        case Target::InsidePopup:
            return;
        default:
            break;
        }
        // A click outside closes the panel; on the button that opened it, that is all it does.
        const Popup was = popup_;
        ClosePopup();
        const bool sameButton = (hit.target == Target::History && was == Popup::History) ||
                                (hit.target == Target::ModelChip && was == Popup::Models) ||
                                (hit.target == Target::EffortChip && was == Popup::Effort);
        if (sameButton) {
            return;
        }
    }
    switch (hit.target) {
    case Target::History:
        OpenPopup(Popup::History);
        break;
    case Target::ModelChip:
        OpenPopup(Popup::Models);
        break;
    case Target::EffortChip:
        OpenPopup(Popup::Effort);
        break;
    case Target::NewChat:
        NewChat();
        break;
    case Target::Attach:
        PickFiles();
        break;
    case Target::RemoveAttachment:
        attachments_.erase(attachments_.begin() + static_cast<std::ptrdiff_t>(hit.row));
        hover_ = {};
        Attached();
        break;
    case Target::Send:
        if (busy_ && !hasContent()) {
            claude_.Interrupt();
            Flash(L"останавливаю…");
        } else {
            Submit();  // also while Claude works: the message joins the running turn
        }
        break;
    default:
        break;
    }
    if (popup_ == Popup::None) {
        SetFocus(input_);
    }
}

void ChatWindow::OnPopupKey(WPARAM key) {
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (key == VK_ESCAPE) {
        ClosePopup();
        return;
    }
    switch (popup_) {
    case Popup::History:
        if (key == VK_RETURN && selected_ < filtered_.size()) {
            OpenSession(selected_);
        } else if (key == VK_DOWN || key == VK_UP) {
            MoveSelection(key == VK_DOWN ? 1 : -1);
        } else if (key == 'V' && control) {
            searchText_ += ClipboardText(hwnd_);
            std::erase_if(searchText_, [](wchar_t c) { return c == L'\r' || c == L'\n'; });
            FilterSessions();
        }
        break;
    case Popup::Effort:
        if (key == VK_RETURN) {
            ClosePopup();
        } else if (key == VK_LEFT || key == VK_RIGHT) {
            const std::vector<std::string> levels = EffortLevels();
            if (levels.empty()) {
                break;
            }
            const auto at = std::ranges::find(levels, pendingEffort_);
            long long index = at == levels.end() ? -1 : at - levels.begin();
            index = std::clamp<long long>(index + (key == VK_RIGHT ? 1 : -1), 0,
                                          static_cast<long long>(levels.size()) - 1);
            pendingEffort_ = levels[static_cast<size_t>(index)];
            Refresh();
        }
        break;
    default:
        break;
    }
}

void ChatWindow::OnPopupChar(wchar_t c) {
    if (popup_ == Popup::Models && c >= L'1' && c <= L'9') {
        if (const auto index = static_cast<size_t>(c - L'1'); index < mainModels_.size()) {
            ChooseModel(mainModels_[index]);
        }
        return;
    }
    if (popup_ != Popup::History) {
        return;
    }
    if (c == L'\b') {
        if (!searchText_.empty()) {
            searchText_.pop_back();
        }
    } else if (c == 0x7f) {  // Ctrl+Backspace
        searchText_.clear();
    } else if (c >= L' ') {
        searchText_ += c;
    } else {
        return;
    }
    FilterSessions();
}

void ChatWindow::OnCommand(WPARAM wParam, LPARAM lParam) {
    if (reinterpret_cast<HWND>(lParam) != input_) {
        return;
    }
    const WORD code = HIWORD(wParam);
    if (code == EN_CHANGE) {
        hasText_ = GetWindowTextLengthW(input_) > 0;
        InvalidateRect(input_, nullptr, TRUE);  // the placeholder comes and goes
        Refresh();
    } else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
        inputFocused_ = code == EN_SETFOCUS;
        if (inputFocused_) {
            ClosePopup();  // a click into the input
        }
        Refresh();
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
            OpenPopup(Popup::History);
            return 1;
        }
        if (filter.wParam == 'U' && control) {
            PickFiles();
            return 1;
        }
        if (filter.wParam == 'V' && control && ClipboardHasImage()) {
            AttachClipboardImage();  // a screenshot becomes an attachment, not text
            return 1;
        }
    }
    if (filter.msg == WM_CHAR && filter.wParam == L'\r' && !shift) {
        return 1;  // the Enter that sent the message
    }
    if (filter.msg == WM_CHAR && filter.wParam == 0x15) {
        return 1;  // Ctrl+U's character
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
    const float attachmentsHeight = attachments_.empty() ? 0.f : kAttachmentRow + 8;
    const float boxTop = boxBottom - (kBoxPad + attachmentsHeight + textHeight_ + 4 + kToolbar);
    g.box = D2D1::RectF(left, boxTop, right, boxBottom);
    g.attachments = D2D1::RectF(left + 12, boxTop + kBoxPad, right - 12, boxTop + kBoxPad + kAttachmentRow);
    const float textTop = boxTop + kBoxPad + attachmentsHeight;
    g.text = D2D1::RectF(left + 14, textTop, right - 14, textTop + textHeight_);
    const float toolTop = boxBottom - kToolbar + 4;
    const float toolBottom = boxBottom - 6;
    g.attach = D2D1::RectF(left + 8, toolTop, left + 8 + (toolBottom - toolTop), toolBottom);
    g.send = D2D1::RectF(right - 8 - (toolBottom - toolTop), toolTop, right - 8, toolBottom);
    float chipsRight = g.send.left - 8;
    if (!EffortLevels().empty()) {
        const float width = TextWidth(EffortName(settings_.effort), ui_.get()) + 18;
        g.effort = D2D1::RectF(chipsRight - width, toolTop + 3, chipsRight, toolBottom - 3);
        chipsRight = g.effort.left - 4;
    }
    const float modelWidth = TextWidth(ModelName(), ui_.get()) + 18;
    g.model = D2D1::RectF(chipsRight - modelWidth, toolTop + 3, chipsRight, toolBottom - 3);
    g.limits = D2D1::RectF(g.attach.right + 10, toolTop, g.model.left - 10, toolBottom);
    g.feed = D2D1::RectF(0, kHeader, g.width, boxTop - 8);

    switch (popup_) {
    case Popup::History: {
        const float width = std::min(kHistoryWidth, g.width - 20);
        const float top = kHeader + 4;
        g.search = D2D1::RectF(g.width - 10 - width + 10, top + 10, g.width - 20, top + 42);
        g.listTop = g.search.bottom + 8;
        const size_t rows = std::max<size_t>(1, std::min(kHistoryRows, filtered_.size()));
        g.popup = D2D1::RectF(g.width - 10 - width, top, g.width - 10,
                              g.listTop + static_cast<float>(rows) * kHistoryRow + 8);
        break;
    }
    case Popup::Models: {
        const float rows = static_cast<float>(mainModels_.size());
        const float height = 12 + rows * kMenuRow + (moreModels_.empty() ? 0 : 9 + kMenuRow);
        const float popupLeft = ClampTo(g.model.left, 10.f, g.width - 10 - kModelsWidth);
        g.popup = D2D1::RectF(popupLeft, g.box.top - 6 - height, popupLeft + kModelsWidth, g.box.top - 6);
        g.modelsTop = g.popup.top + 6;
        if (!moreModels_.empty()) {
            const float moreTop = g.modelsTop + rows * kMenuRow + 9;
            g.more = D2D1::RectF(g.popup.left + 6, moreTop, g.popup.right - 6, moreTop + kMenuRow);
            const float subHeight = 12 + static_cast<float>(moreModels_.size()) * kMenuRow;
            const bool roomLeft = g.popup.left - 4 - kMoreWidth >= 10;
            const float subLeft = roomLeft ? g.popup.left - 4 - kMoreWidth : g.popup.right + 4;
            const float subBottom = g.more.bottom + 6;
            const float subTop = std::max(kHeader + 4, subBottom - subHeight);
            g.submenu = D2D1::RectF(subLeft, subTop, subLeft + kMoreWidth, subTop + subHeight);
        }
        break;
    }
    case Popup::Effort: {
        const float popupRight = ClampTo(g.effort.right, 10.f + kEffortWidth, g.width - 10);
        g.popup = D2D1::RectF(popupRight - kEffortWidth, g.box.top - 6 - kEffortHeight, popupRight, g.box.top - 6);
        g.track = D2D1::RectF(g.popup.left + 14, g.popup.top + 62, g.popup.right - 14, g.popup.top + 86);
        const float resetWidth = TextWidth(L"по умолчанию", uiRight_.get());
        g.reset = D2D1::RectF(g.popup.right - 14 - resetWidth, g.popup.top + 10, g.popup.right - 14,
                              g.popup.top + 34);
        break;
    }
    case Popup::None:
        break;
    }
    return g;
}

Hit ChatWindow::HitTest(float x, float y) const {
    const Geometry g = Layout();
    switch (popup_) {
    case Popup::History:
        if (Inside(g.popup, x, y)) {
            if (y >= g.listTop) {
                const auto row = firstRow_ + static_cast<size_t>((y - g.listTop) / kHistoryRow);
                if (row < filtered_.size() && row < firstRow_ + kHistoryRows) {
                    return {Target::HistoryRow, row};
                }
            }
            return {Target::InsidePopup};
        }
        break;
    case Popup::Models:
        if (moreOpen_ && Inside(g.submenu, x, y)) {
            const auto row = static_cast<size_t>(std::max(0.f, y - g.submenu.top - 6) / kMenuRow);
            return row < moreModels_.size() ? Hit{Target::MoreRow, row} : Hit{Target::InsidePopup};
        }
        if (Inside(g.popup, x, y)) {
            if (Inside(g.more, x, y)) {
                return {Target::MoreModels};
            }
            if (y >= g.modelsTop) {
                const auto row = static_cast<size_t>((y - g.modelsTop) / kMenuRow);
                if (row < mainModels_.size()) {
                    return {Target::ModelRow, row};
                }
            }
            return {Target::InsidePopup};
        }
        break;
    case Popup::Effort:
        if (Inside(g.popup, x, y)) {
            if (!pendingEffort_.empty() && Inside(g.reset, x, y)) {
                return {Target::EffortReset};
            }
            const D2D1_RECT_F grab = D2D1::RectF(g.track.left, g.track.top - 8, g.track.right, g.track.bottom + 8);
            return Inside(grab, x, y) ? Hit{Target::EffortTrack} : Hit{Target::InsidePopup};
        }
        break;
    case Popup::None:
        break;
    }
    if (Inside(g.history, x, y)) {
        return {Target::History};
    }
    if (Inside(g.newChat, x, y)) {
        return {Target::NewChat};
    }
    if (Inside(g.attach, x, y)) {
        return {Target::Attach};
    }
    for (size_t i = 0; i < attachments_.size(); ++i) {
        const D2D1_RECT_F chip = AttachmentRect(g, i);
        if (Inside(RemoveRect(chip), x, y)) {
            return {Target::RemoveAttachment, i};
        }
        if (Inside(chip, x, y)) {
            return {Target::AttachmentChip, i};
        }
    }
    if (Inside(g.model, x, y)) {
        return {Target::ModelChip};
    }
    if (Inside(g.effort, x, y)) {
        return {Target::EffortChip};
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
    wcscpy_s(font.lfFaceName, kTextFont);
    uiFont_.reset(CreateFontIndirectW(&font));
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
    wcscpy_s(format.szFaceName, kTextFont);
    SendMessageW(input_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    SendMessageW(input_, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));
}

void ChatWindow::PlaceChildren() {
    if (input_ == nullptr) {
        return;
    }
    MoveChild(input_, Layout().text);
    SendMessageW(input_, EM_REQUESTRESIZE, 0, 0);
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
    if (logoSource_) {
        target_->CreateBitmapFromWicBitmap(logoSource_.get(), nullptr, logo_.put());
    }
}

// The logo from the exe's resources, decoded once; the Direct2D bitmap is made per render target.
void ChatWindow::LoadLogo() {
    HRSRC found = FindResourceW(instance_, MAKEINTRESOURCEW(IDR_LOGO), RT_RCDATA);
    HGLOBAL loaded = found != nullptr ? LoadResource(instance_, found) : nullptr;
    const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (data == nullptr) {
        return;
    }
    wil::com_ptr<IWICStream> stream;
    wil::com_ptr<IWICBitmapDecoder> decoder;
    wil::com_ptr<IWICBitmapFrameDecode> frame;
    wil::com_ptr<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)),
                                            SizeofResource(instance_, found))) ||
        FAILED(wic_->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(wic_->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom))) {
        return;
    }
    wic_->CreateBitmapFromSource(converter.get(), WICBitmapCacheOnLoad, logoSource_.put());
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

void ChatWindow::Check(D2D1_POINT_2F c, uint32_t color) {
    Line({c.x - 5.f, c.y}, {c.x - 1.5f, c.y + 3.5f}, color, 1.6f);
    Line({c.x - 1.5f, c.y + 3.5f}, {c.x + 5.f, c.y - 3.5f}, color, 1.6f);
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
    switch (popup_) {
    case Popup::History:
        PaintHistory(g);
        break;
    case Popup::Models:
        PaintModels(g);
        break;
    case Popup::Effort:
        PaintEffort(g);
        break;
    case Popup::None:
        break;
    }
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        target_.reset();
        brush_.reset();
        logo_.reset();
    }
}

void ChatWindow::PaintFeed(const Geometry& g) {
    const float left = (g.width - g.column) / 2;
    if (!HasConversation()) {
        const std::wstring hello = userName_.empty() ? L"Что дальше?" : L"Что дальше, " + ToWide(userName_) + L"?";
        const float middle = (g.feed.top + g.feed.bottom) / 2;
        Text(hello, greeting_.get(), D2D1::RectF(left, middle - 40, left + g.column, middle), kPrimary);
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

    float titleLeft = 16;
    if (logo_) {
        const float top = (kHeader - kLogo) / 2;
        target_->DrawBitmap(logo_.get(), D2D1::RectF(14, top, 14 + kLogo, top + kLogo), 1.f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        titleLeft = 14 + kLogo + 10;
    }
    const std::wstring title = title_.empty() ? L"Новый чат" : title_;
    const float titleRight = g.history.left - 12;
    Text(title, header_.get(), D2D1::RectF(titleLeft, 0, titleRight, kHeader), kPrimary);
    std::wstring state = busy_ ? L"отвечает…" : L"";
    if (!status_.empty()) {
        state = status_;
    }
    if (!state.empty()) {
        const float titleWidth = std::min(TextWidth(title, header_.get()), titleRight - titleLeft);
        Text(state, ui_.get(), D2D1::RectF(titleLeft + titleWidth + 12, 0, titleRight, kHeader), kSecondary);
    }

    for (const Target button : {Target::History, Target::NewChat}) {
        const D2D1_RECT_F& r = button == Target::History ? g.history : g.newChat;
        const bool active = hover_.target == button || (button == Target::History && popup_ == Popup::History);
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

    PaintAttachments(g);
    {  // "+": attach files or photos
        const bool active = hover_.target == Target::Attach;
        if (active) {
            Fill(g.attach, 6.f, kRaised);
        }
        const D2D1_POINT_2F c{(g.attach.left + g.attach.right) / 2, (g.attach.top + g.attach.bottom) / 2};
        const uint32_t color = active ? kPrimary : kSecondary;
        Line({c.x - 6.f, c.y}, {c.x + 6.f, c.y}, color, 1.6f);
        Line({c.x, c.y - 6.f}, {c.x, c.y + 6.f}, color, 1.6f);
    }

    if (!limits_.empty()) {
        Text(limits_, ui_.get(), g.limits, kSecondary);
    }

    // The model, then its effort in a box of its own, as in Claude's app.
    const bool modelActive = hover_.target == Target::ModelChip || popup_ == Popup::Models;
    if (modelActive) {
        Fill(g.model, 6.f, kRaised);
    }
    Text(ModelName(), ui_.get(), D2D1::RectF(g.model.left + 9, g.model.top, g.model.right, g.model.bottom),
         modelActive ? kPrimary : kSecondary);
    if (g.effort.right > g.effort.left) {
        const bool effortActive = hover_.target == Target::EffortChip || popup_ == Popup::Effort;
        Fill(g.effort, 6.f, kRaised);
        if (effortActive) {
            brush_->SetColor(Color(kLine));
            target_->DrawRoundedRectangle(D2D1::RoundedRect(g.effort, 6.f, 6.f), brush_.get(), 1.f);
        }
        Text(EffortName(settings_.effort), ui_.get(),
             D2D1::RectF(g.effort.left + 9, g.effort.top, g.effort.right, g.effort.bottom),
             effortActive ? kPrimary : kSecondary);
    }

    const bool live = hasContent() || busy_;
    const D2D1_POINT_2F c{(g.send.left + g.send.right) / 2, (g.send.top + g.send.bottom) / 2};
    const float radius = (g.send.right - g.send.left) / 2;
    brush_->SetColor(Color(live ? kAccent : kRaised, live && hover_.target == Target::Send ? 0.85f : 1.f));
    target_->FillEllipse(D2D1::Ellipse(c, radius, radius), brush_.get());
    const uint32_t mark = live ? kPrimary : kSecondary;
    if (busy_ && !hasContent()) {  // stop
        Fill(D2D1::RectF(c.x - 4.5f, c.y - 4.5f, c.x + 4.5f, c.y + 4.5f), 1.5f, mark);
    } else {  // an arrow up
        Line({c.x, c.y + 5.5f}, {c.x, c.y - 5.5f}, mark, 1.8f);
        Line({c.x - 4.5f, c.y - 1.f}, {c.x, c.y - 5.5f}, mark, 1.8f);
        Line({c.x + 4.5f, c.y - 1.f}, {c.x, c.y - 5.5f}, mark, 1.8f);
    }
}

void ChatWindow::PaintPanel(const D2D1_RECT_F& rect) {
    Fill(D2D1::RectF(rect.left + 2, rect.top + 4, rect.right + 2, rect.bottom + 6), 10.f, 0x000000, 0.35f);
    Fill(rect, 10.f, kCard);
    brush_->SetColor(Color(kLine));
    target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, 10.f, 10.f), brush_.get(), 1.f);
}

void ChatWindow::PaintHistory(const Geometry& g) {
    PaintPanel(g.popup);
    Fill(g.search, 6.f, kRaised);
    const D2D1_RECT_F field = D2D1::RectF(g.search.left + 10, g.search.top, g.search.right - 10, g.search.bottom);
    if (searchText_.empty()) {
        Text(L"Поиск по чатам — просто начни печатать", ui_.get(), field, kSecondary);
    } else {
        Text(searchText_, ui_.get(), field, kPrimary);
    }

    if (filtered_.empty()) {
        const std::wstring empty = sessions_.empty() ? L"Чатов пока нет" : L"Ничего не нашлось";
        Text(empty, ui_.get(), D2D1::RectF(g.search.left + 10, g.listTop, g.search.right, g.listTop + kHistoryRow),
             kSecondary);
        return;
    }
    const size_t last = std::min(filtered_.size(), firstRow_ + kHistoryRows);
    for (size_t row = firstRow_; row < last; ++row) {
        const SessionInfo& session = sessions_[filtered_[row]];
        const float top = g.listTop + static_cast<float>(row - firstRow_) * kHistoryRow;
        const D2D1_RECT_F r = D2D1::RectF(g.popup.left + 6, top, g.popup.right - 6, top + kHistoryRow);
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

void ChatWindow::PaintModels(const Geometry& g) {
    const Json* current = CurrentModel();
    const auto rowFor = [&](size_t modelIndex, const D2D1_RECT_F& r, bool hovered, const std::wstring& key) {
        const Json& model = models_[modelIndex];
        if (hovered) {
            Fill(r, 6.f, kRaised);
        }
        const std::wstring name = ToWide(JsonStr(model, "displayName"));
        Text(name, ui_.get(), D2D1::RectF(r.left + 10, r.top, r.right - 30, r.bottom), kPrimary);
        if (JsonStr(model, "resolvedModel") == defaultResolved_ && !defaultResolved_.empty()) {
            const float x = r.left + 10 + TextWidth(name, ui_.get()) + 8;
            const float width = TextWidth(L"Default", badge_.get()) + 12;
            const D2D1_RECT_F badge = D2D1::RectF(x, r.top + 7, x + width, r.bottom - 7);
            Fill(badge, 4.f, kRaised);
            Text(L"Default", badge_.get(), badge, kSecondary);
        }
        if (&model == current) {
            Check({r.right - 16, (r.top + r.bottom) / 2}, kAccent);
        } else if (!key.empty()) {
            Text(key, uiRight_.get(), D2D1::RectF(r.left, r.top, r.right - 10, r.bottom), kSecondary);
        }
    };

    PaintPanel(g.popup);
    for (size_t i = 0; i < mainModels_.size(); ++i) {
        const float top = g.modelsTop + static_cast<float>(i) * kMenuRow;
        const D2D1_RECT_F r = D2D1::RectF(g.popup.left + 6, top, g.popup.right - 6, top + kMenuRow);
        const bool hovered = hover_.target == Target::ModelRow && hover_.row == i;
        rowFor(mainModels_[i], r, hovered, i < 9 ? std::to_wstring(i + 1) : L"");
    }
    if (moreModels_.empty()) {
        return;
    }
    brush_->SetColor(Color(kLine));
    target_->FillRectangle(D2D1::RectF(g.popup.left + 10, g.more.top - 5, g.popup.right - 10, g.more.top - 4),
                           brush_.get());
    if (moreOpen_ || hover_.target == Target::MoreModels) {
        Fill(g.more, 6.f, kRaised);
    }
    Text(L"Ещё модели", ui_.get(), D2D1::RectF(g.more.left + 10, g.more.top, g.more.right - 30, g.more.bottom),
         kPrimary);
    const D2D1_POINT_2F c{g.more.right - 16, (g.more.top + g.more.bottom) / 2};
    Line({c.x - 2.f, c.y - 4.f}, {c.x + 2.f, c.y}, kSecondary, 1.5f);
    Line({c.x + 2.f, c.y}, {c.x - 2.f, c.y + 4.f}, kSecondary, 1.5f);

    if (moreOpen_) {
        PaintPanel(g.submenu);
        for (size_t i = 0; i < moreModels_.size(); ++i) {
            const float top = g.submenu.top + 6 + static_cast<float>(i) * kMenuRow;
            const D2D1_RECT_F r = D2D1::RectF(g.submenu.left + 6, top, g.submenu.right - 6, top + kMenuRow);
            rowFor(moreModels_[i], r, hover_.target == Target::MoreRow && hover_.row == i, L"");
        }
    }
}

void ChatWindow::PaintEffort(const Geometry& g) {
    PaintPanel(g.popup);
    const float titleLeft = g.popup.left + 14;
    const D2D1_RECT_F titleRow = D2D1::RectF(titleLeft, g.popup.top + 10, g.reset.left - 8, g.popup.top + 34);
    Text(L"Effort", ui_.get(), titleRow, kSecondary);
    const float labelLeft = titleLeft + TextWidth(L"Effort", ui_.get()) + 8;
    Text(EffortName(pendingEffort_), uiBold_.get(), D2D1::RectF(labelLeft, titleRow.top, titleRow.right, titleRow.bottom),
         kPrimary);
    if (!pendingEffort_.empty()) {
        Text(L"по умолчанию", uiRight_.get(), g.reset, hover_.target == Target::EffortReset ? kPrimary : kSecondary);
    }
    Text(L"Быстрее", ui_.get(), D2D1::RectF(titleLeft, g.popup.top + 38, g.popup.right - 14, g.popup.top + 56),
         kSecondary);
    Text(L"Умнее", uiRight_.get(), D2D1::RectF(titleLeft, g.popup.top + 38, g.popup.right - 14, g.popup.top + 56),
         kSecondary);

    Fill(g.track, 7.f, kRaised);
    const std::vector<std::string> levels = EffortLevels();
    const float first = g.track.left + 14;
    const float span = g.track.right - 14 - first;
    const float middle = (g.track.top + g.track.bottom) / 2;
    for (size_t i = 0; i < levels.size(); ++i) {
        const float x = levels.size() > 1 ? first + span * static_cast<float>(i) / static_cast<float>(levels.size() - 1)
                                          : first + span / 2;
        if (levels[i] == pendingEffort_) {
            Fill(D2D1::RectF(x - 7, g.track.top + 3, x + 7, g.track.bottom - 3), 5.f, kPrimary);
        } else {
            brush_->SetColor(Color(kSecondary));
            target_->FillEllipse(D2D1::Ellipse({x, middle}, 2.f, 2.f), brush_.get());
        }
    }
    const std::wstring note = pendingEffort_.empty() ? L"Auto — решает модель"
                                                     : L"Применится при закрытии";
    Text(note, ui_.get(), D2D1::RectF(titleLeft, g.track.bottom + 4, g.popup.right - 14, g.popup.bottom - 4),
         kSecondary);
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

void ChatWindow::Flash(std::wstring status, UINT milliseconds) {
    status_ = std::move(status);
    SetTimer(hwnd_, kStatusTimer, milliseconds, nullptr);
    Refresh();
}

D2D1_RECT_F ChatWindow::AttachmentRect(const Geometry& g, size_t index) const {
    float x = g.attachments.left;
    for (size_t i = 0; i <= index && i < attachments_.size(); ++i) {
        const float width = attachments_[i].type == Attachment::Type::Image ? kAttachmentRow : kFileChip;
        if (i == index) {
            return D2D1::RectF(x, g.attachments.top, x + width, g.attachments.bottom);
        }
        x += width + 8;
    }
    return {};
}

D2D1_RECT_F ChatWindow::RemoveRect(const D2D1_RECT_F& chip) {
    return D2D1::RectF(chip.right - 20, chip.top + 2, chip.right - 2, chip.top + 20);
}

void ChatWindow::PaintAttachments(const Geometry& g) {
    for (size_t i = 0; i < attachments_.size(); ++i) {
        const Attachment& attachment = attachments_[i];
        const D2D1_RECT_F chip = AttachmentRect(g, i);
        if (chip.right > g.attachments.right) {
            break;  // no room left in the row
        }
        Fill(chip, 8.f, kRaised);
        wil::com_ptr<ID2D1Bitmap> bitmap;
        if (attachment.thumbnail &&
            SUCCEEDED(target_->CreateBitmapFromWicBitmap(attachment.thumbnail.get(), nullptr, bitmap.put()))) {
            // Cover the square: the middle of the picture.
            const D2D1_SIZE_F size = bitmap->GetSize();
            const float side = std::min(size.width, size.height);
            const D2D1_RECT_F source = D2D1::RectF((size.width - side) / 2, (size.height - side) / 2,
                                                   (size.width + side) / 2, (size.height + side) / 2);
            target_->DrawBitmap(bitmap.get(), chip, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &source);
        } else {
            const std::wstring kind = attachment.type == Attachment::Type::Document ? L"PDF" : L"текст";
            Text(attachment.name, ui_.get(), D2D1::RectF(chip.left + 12, chip.top + 6, chip.right - 24, chip.top + 30),
                 kPrimary);
            Text(kind, ui_.get(), D2D1::RectF(chip.left + 12, chip.top + 28, chip.right - 12, chip.bottom - 6),
                 kSecondary);
        }
        const bool hovered = (hover_.target == Target::AttachmentChip || hover_.target == Target::RemoveAttachment) &&
                             hover_.row == i;
        if (hovered) {
            const D2D1_RECT_F close = RemoveRect(chip);
            const D2D1_POINT_2F c{(close.left + close.right) / 2, (close.top + close.bottom) / 2};
            brush_->SetColor(Color(kWindow, 0.85f));
            target_->FillEllipse(D2D1::Ellipse(c, 8.f, 8.f), brush_.get());
            const uint32_t color = hover_.target == Target::RemoveAttachment ? kPrimary : kSecondary;
            Line({c.x - 3.f, c.y - 3.f}, {c.x + 3.f, c.y + 3.f}, color, 1.4f);
            Line({c.x + 3.f, c.y - 3.f}, {c.x - 3.f, c.y + 3.f}, color, 1.4f);
        }
    }
}

void ChatWindow::PickFiles() {
    wil::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
        return;
    }
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    const std::array<COMDLG_FILTERSPEC, 2> filters{{
        {L"Фото и файлы",
         L"*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.pdf;*.txt;*.md;*.json;*.log;*.csv;*.xml;*.yml;*.yaml;*.toml;"
         L"*.ini;*.cfg;*.conf;*.cpp;*.h;*.hpp;*.c;*.py;*.js;*.ts;*.go;*.rs;*.ps1;*.cmd;*.sh"},
        {L"Все файлы", L"*.*"},
    }};
    dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
    dialog->SetTitle(L"Прикрепить фото или файлы");
    if (FAILED(dialog->Show(hwnd_))) {
        return;  // cancelled
    }
    wil::com_ptr<IShellItemArray> items;
    DWORD count = 0;
    if (FAILED(dialog->GetResults(items.put())) || FAILED(items->GetCount(&count))) {
        return;
    }
    for (DWORD i = 0; i < count; ++i) {
        wil::com_ptr<IShellItem> item;
        wil::unique_cotaskmem_string path;
        if (SUCCEEDED(items->GetItemAt(i, item.put())) &&
            SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, path.put()))) {
            AttachFile(path.get());
        }
    }
}

void ChatWindow::AttachFile(const std::wstring& path) {
    std::wstring error;
    if (std::optional<Attachment> attachment = LoadAttachment(wic_.get(), path, error)) {
        attachments_.push_back(std::move(*attachment));
        Attached();
    } else {
        Flash(error, 5000);
    }
}

void ChatWindow::AttachClipboardImage() {
    std::wstring error;
    if (std::optional<Attachment> attachment = ClipboardImage(wic_.get(), hwnd_, error)) {
        attachments_.push_back(std::move(*attachment));
        Attached();
    } else if (!error.empty()) {
        Flash(error, 5000);
    }
}

void ChatWindow::Attached() {
    PlaceChildren();
    SetFocus(input_);
    Refresh();
}

void ChatWindow::OpenPopup(Popup popup) {
    if ((popup == Popup::Models || popup == Popup::Effort) && busy_) {
        Flash(L"модель меняется между ответами");
        return;
    }
    ClosePopup();
    popup_ = popup;
    hover_ = {};
    switch (popup) {
    case Popup::History:
        sessions_ = ListSessions(workspace_);
        searchText_.clear();
        FilterSessions();
        break;
    case Popup::Models:
        moreOpen_ = false;
        break;
    case Popup::Effort:
        pendingEffort_ = settings_.effort;
        break;
    case Popup::None:
        break;
    }
    SetFocus(hwnd_);  // the panel's keys: typing to search, digits, arrows
    Refresh();
}

void ChatWindow::ClosePopup() {
    if (popup_ == Popup::None) {
        return;
    }
    const Popup was = popup_;
    popup_ = Popup::None;
    moreOpen_ = false;
    hover_ = {};
    if (was == Popup::Effort && pendingEffort_ != settings_.effort) {
        Settings next = settings_;
        next.effort = pendingEffort_;
        Apply(next);
    }
    SetFocus(input_);
    Refresh();
}

void ChatWindow::FilterSessions() {
    const std::wstring query = Lowercase(searchText_);
    filtered_.clear();
    for (size_t i = 0; i < sessions_.size(); ++i) {
        if (query.empty() || Lowercase(sessions_[i].title).find(query) != std::wstring::npos) {
            filtered_.push_back(i);
        }
    }
    firstRow_ = 0;
    selected_ = 0;
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
    } else if (selected_ >= firstRow_ + kHistoryRows) {
        firstRow_ = selected_ + 1 - kHistoryRows;
    }
    Refresh();
}

void ChatWindow::OpenSession(size_t filteredIndex) {
    if (filteredIndex >= filtered_.size()) {
        return;
    }
    const SessionInfo session = sessions_[filtered_[filteredIndex]];
    ClosePopup();
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

// The CLI lists aliases (opus, sonnet…) and pinned versions (claude-opus-4-8…): the aliases are the
// main list, the versions go under "more"; "default" itself is shown as a badge on what it means.
void ChatWindow::IndexModels() {
    mainModels_.clear();
    moreModels_.clear();
    defaultResolved_.clear();
    for (size_t i = 0; i < models_.size(); ++i) {
        const std::string value = JsonStr(models_[i], "value");
        if (value == "default") {
            defaultResolved_ = JsonStr(models_[i], "resolvedModel");
        } else if (value.find('-') == std::string::npos) {
            mainModels_.push_back(i);
        } else {
            moreModels_.push_back(i);
        }
    }
}

const Json* ChatWindow::CurrentModel() const {
    for (const size_t i : mainModels_) {
        const Json& model = models_[i];
        if (settings_.model.empty() ? JsonStr(model, "resolvedModel") == defaultResolved_
                                    : JsonStr(model, "value") == settings_.model) {
            return &model;
        }
    }
    for (const size_t i : moreModels_) {
        if (JsonStr(models_[i], "value") == settings_.model) {
            return &models_[i];
        }
    }
    return nullptr;
}

std::wstring ChatWindow::ModelName() const {
    if (const Json* model = CurrentModel(); model != nullptr) {
        return ToWide(JsonStr(*model, "displayName"));
    }
    return settings_.model.empty() ? L"Модель" : ToWide(settings_.model);
}

std::vector<std::string> ChatWindow::EffortLevels() const {
    std::vector<std::string> levels;
    const Json* model = CurrentModel();
    if (model == nullptr) {
        return models_.empty() ? std::vector<std::string>{"low", "medium", "high", "xhigh", "max"} : levels;
    }
    for (const Json& level : JsonAt(*model, "supportedEffortLevels")) {
        if (level.is_string()) {
            levels.push_back(level.get<std::string>());
        }
    }
    return levels;
}

void ChatWindow::SetEffortAt(float x) {
    const std::vector<std::string> levels = EffortLevels();
    if (levels.empty()) {
        return;
    }
    const Geometry g = Layout();
    const float first = g.track.left + 14;
    const float span = std::max(1.f, g.track.right - 14 - first);
    const float position = std::clamp((x - first) / span, 0.f, 1.f) * static_cast<float>(levels.size() - 1);
    const std::string& level = levels[static_cast<size_t>(std::lround(position))];
    if (level != pendingEffort_) {
        pendingEffort_ = level;
        Refresh();
    }
}

void ChatWindow::ChooseModel(size_t index) {
    Settings next = settings_;
    next.model = JsonStr(models_[index], "value");
    // An effort the new model does not have would be refused: back to its default.
    bool supported = false;
    for (const Json& level : JsonAt(models_[index], "supportedEffortLevels")) {
        supported = supported || (level.is_string() && level.get<std::string>() == next.effort);
    }
    if (!supported) {
        next.effort.clear();
    }
    ClosePopup();
    Apply(next);
}

void ChatWindow::Apply(const Settings& next) {
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
    stateEvents_ = false;
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
    ClosePopup();
    ResetChat();
    StartClaude();
}

void ChatWindow::Submit() {
    std::wstring text = NormalizeNewlines(WindowText(input_));
    const size_t first = text.find_first_not_of(L" \t\n");
    text = first == std::wstring::npos ? L"" : text.substr(first, text.find_last_not_of(L" \t\n") - first + 1);
    if (text.empty() && attachments_.empty()) {
        return;
    }
    if (!running_ && !StartClaude()) {
        return;
    }
    // Attachments first, then the words about them, as Claude's own apps send it.
    Json content = Json::array();
    std::wstring shown;
    for (const Attachment& attachment : attachments_) {
        content.push_back(ContentBlock(attachment));
        shown += (shown.empty() ? L"" : L"\n") + (L"📎 " + attachment.name);
    }
    if (!text.empty()) {
        content.push_back(Json{{"type", "text"}, {"text", ToUtf8(text)}});
        shown += (shown.empty() ? L"" : L"\n\n") + text;
    }
    claude_.SendUserContent(std::move(content));
    Add(Kind::User, std::move(shown));
    attachments_.clear();
    SetWindowTextW(input_, L"");
    PlaceChildren();
    busy_ = true;
    stick_ = true;
    Refresh();
}

void ChatWindow::OnClaude(const Json& message) {
    const std::string type = JsonStr(message, "type");
    if (type == "system") {
        const std::string subtype = JsonStr(message, "subtype");
        if (subtype == "init") {
            sessionId_ = JsonStr(message, "session_id");
        } else if (subtype == "session_state_changed") {
            stateEvents_ = true;
            busy_ = JsonStr(message, "state") != "idle";
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
        if (!stateEvents_) {
            busy_ = false;
        }
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
                IndexModels();
            }
            const std::string email = JsonStr(JsonAt(answer, "account"), "email");
            userName_ = email.substr(0, email.find('@'));
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
    // The file dialog and WIC are COM; plain CoInitialize (not OleInitialize) also keeps RichEdit
    // from registering itself as a drop target, so dropped files reach the window.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return 1;
    }
    auto uninitialize = wil::scope_exit([] { CoUninitialize(); });
    try {
        ChatWindow window(instance);
        return window.Run(show);
    } catch (const std::exception& e) {
        MessageBoxW(nullptr, ToWide(e.what()).c_str(), L"AIControls", MB_ICONERROR);
        return 1;
    }
}

}  // namespace aic
