#include "app/chat_window.h"

#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <richedit.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <WebView2.h>
#include <wrl/event.h>

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
#include <filesystem>
#include <format>
#include <ctime>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "app/fonts.h"
#include "app/resource.h"
#include "core/app_update.h"
#include "core/attachment.h"
#include "core/auth.h"
#include "core/claude_install.h"
#include "core/claude_process.h"
#include "core/github.h"
#include "core/markdown.h"
#include "core/pty.h"
#include "core/secrets.h"
#include "core/store.h"
#include "core/tools.h"
#include "core/vt_screen.h"

namespace aic {
namespace {

// Posted from the reader thread; wParam is the chat's generation, lParam owns the payload.
constexpr UINT kClaudeMessage = WM_APP + 1;  // Json*
constexpr UINT kClaudeExit = WM_APP + 2;     // ExitInfo*
constexpr UINT kSecretKey = WM_APP + 3;      // from a secrets field: wParam the key, lParam the field's HWND
constexpr UINT kTermData = WM_APP + 4;       // std::string* of terminal output (wParam: the terminal's generation)
constexpr UINT kTermExit = WM_APP + 5;       // lParam: the shell's exit code
constexpr UINT kAuthStatus = WM_APP + 6;     // AuthStatus*: what `claude auth status` said
constexpr UINT kAuthUrl = WM_APP + 7;        // std::string*: the sign-in page, for when the browser did not open
constexpr UINT kAuthLogin = WM_APP + 8;      // LoginDone*: `claude auth login` ended
constexpr UINT kAuthLogout = WM_APP + 9;     // `claude auth logout` ended

struct LoginDone {
    bool cancelled = false;
    bool timedOut = false;
    bool started = true;
    DWORD exitCode = 0;
    std::string tail;  // the end of the CLI's output, for the failure note
};

constexpr UINT kSetupEvent = WM_APP + 12;      // SetupEvent*: the CLI's installer or updater
constexpr UINT kClaudeVersions = WM_APP + 13;  // ClaudeVersions*: what is installed and what is out

struct SetupEvent {
    unsigned percent = 0;
    bool installing = false;  // the download is over, `claude.exe install` runs
    bool finished = false;
    bool ok = false;
    bool update = false;  // an update, not a first install
    std::string error;
};

struct ClaudeVersions {
    std::string installed;
    std::string latest;
};

constexpr UINT kRepos = WM_APP + 10;      // ReposResult*: the GitHub repositories
constexpr UINT kCloneDone = WM_APP + 11;  // CloneDone*: `git clone` ended

struct ReposResult {
    std::vector<GithubRepo> repos;
    std::string error;
};

struct CloneDone {
    bool ok = false;
    std::wstring folder;
    std::wstring name;
    std::string output;
};
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kTipTimer = 2;  // the pointer has rested on something long enough for its hint
constexpr UINT_PTR kPreviewTimer = 3;  // the preview looks whether its file (or one beside it) has changed
constexpr UINT kPreviewCheck = 700;    // ms
constexpr UINT_PTR kUpdateTimer = 4;   // a newer AIControls may have been built
constexpr UINT kUpdateCheck = 30'000;  // ms
constexpr UINT_PTR kSubclassId = 1;
constexpr size_t kTranscriptLimit = 200;

// Layout, in DIPs.
constexpr float kHeader = 34.f;     // also the window's title bar
constexpr float kWinButton = 40.f;  // minimize, maximize, close: a little narrower than Windows' own, as in Claude's app
constexpr float kColumnMax = 720.f;
constexpr float kSide = 24.f;
constexpr float kGap = 14.f;
constexpr float kBubblePad = 10.f;
constexpr float kBoxPadY = 5.f;       // inside the input box, above and below its row
constexpr float kChipRow = 24.f;      // under the box: model, effort, usage ring
constexpr float kChipsBottom = 6.f;
constexpr float kInputButton = 26.f;  // "+" and send
constexpr float kRing = 20.f;
constexpr float kUsageWidth = 380.f;
constexpr float kPanelPad = 16.f;
constexpr float kSecretRow = 38.f;
constexpr float kFieldHeight = 34.f;
constexpr float kSecretsNote = 40.f;
constexpr float kUsageRow = 58.f;
constexpr float kTextMin = 22.f;
constexpr float kTextMax = 220.f;
constexpr float kButton = 26.f;
constexpr float kTerminal = 480.f;  // the right column: PowerShell (as wide by default)
constexpr float kMinTerminal = 300.f;
constexpr float kMinSidebar = 180.f;
constexpr float kMaxSidebar = 480.f;
constexpr uint32_t kTermFg = 0xd4d4d8;
constexpr float kSidebar = 252.f;  // the left column: search, secrets, the chats
constexpr float kHistoryRow = 34.f;
constexpr float kNavRow = 34.f;
constexpr float kMinChat = 420.f;  // the chat never gets narrower than this: side panels give way first
constexpr float kMinChatPreview = 340.f;  // ... but beside a preview it may be a strip

// Put into every page the preview loads: Ctrl shows the element under the pointer, Ctrl+click tells the window
// which one it is (a short CSS path and its text), to be named in the message being typed.
constexpr const wchar_t* kPickScript = LR"JS((() => {
  if (window.__aicPick) return;
  window.__aicPick = true;
  let box = null;
  const mark = (el) => {
    if (!box) {
      box = document.createElement('div');
      box.style.cssText = 'position:fixed;pointer-events:none;z-index:2147483647;border:2px solid #4c92ff;' +
                          'border-radius:3px;background:rgba(76,146,255,.12)';
      document.documentElement.appendChild(box);
    }
    const r = el.getBoundingClientRect();
    Object.assign(box.style, {left: r.left + 'px', top: r.top + 'px', width: r.width + 'px', height: r.height + 'px',
                              display: 'block'});
  };
  const hide = () => { if (box) box.style.display = 'none'; };
  const path = (el) => {
    const parts = [];
    while (el && el.nodeType === 1 && el !== document.body && el !== document.documentElement && parts.length < 4) {
      let part = el.tagName.toLowerCase();
      if (el.id) { parts.unshift(part + '#' + el.id); break; }
      const classes = [...el.classList].slice(0, 2);
      if (classes.length) part += '.' + classes.join('.');
      parts.unshift(part);
      el = el.parentElement;
    }
    return parts.join(' > ');
  };
  document.addEventListener('mousemove', (e) => { if (e.ctrlKey) mark(e.target); else hide(); }, true);
  document.addEventListener('keyup', (e) => { if (e.key === 'Control') hide(); }, true);
  document.addEventListener('click', (e) => {
    if (!e.ctrlKey) return;
    e.preventDefault();
    e.stopPropagation();
    hide();
    const text = (e.target.innerText || e.target.value || '').trim().replace(/\s+/g, ' ').slice(0, 80);
    window.chrome.webview.postMessage(JSON.stringify({selector: path(e.target), text}));
  }, true);
})();)JS";
constexpr float kMenuRow = 30.f;
constexpr float kModelsWidth = 270.f;
constexpr float kMoreWidth = 230.f;
constexpr float kEffortWidth = 250.f;
constexpr float kEffortHeight = 112.f;
constexpr float kAttachmentRow = 56.f;
constexpr float kFileChip = 190.f;
constexpr float kCardPad = 12.f;
constexpr float kCardTitle = 22.f;
constexpr float kCardButton = 30.f;
constexpr float kCardButtonWidth = 112.f;
constexpr float kTipPadX = 10.f;  // a hint's text inside its box
constexpr float kTipPadY = 5.f;
constexpr float kImageWidth = 360.f;   // a picture in the feed fits this box
constexpr float kImageHeight = 280.f;
constexpr float kImageGap = 6.f;
constexpr UINT kPreviewSide = 1024;    // px: sharp at 2x in that box, and quick to decode
constexpr UINT kViewerSide = 4096;

// Sovereign's palette (src/tray/ui_style.h): dark only, one blue accent.
constexpr uint32_t kWindow = 0x0e0e0f;
constexpr uint32_t kPanel = 0x121213;
constexpr uint32_t kCard = 0x18181a;
constexpr uint32_t kRaised = 0x232327;
constexpr uint32_t kUserBubble = 0x1d2738;
constexpr uint32_t kLine = 0x2c2c31;
constexpr uint32_t kPrimary = 0xf2f4f8;
constexpr uint32_t kSecondary = 0x9c9ea4;
constexpr uint32_t kTitle = 0xc4c6cc;  // the chat's title in the header: quieter than the messages
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

// The cross at a terminal tab's right end.
D2D1_RECT_F TabCloseRect(const D2D1_RECT_F& tab) {
    return D2D1::RectF(tab.right - 22, tab.top + 5, tab.right - 4, tab.bottom - 5);
}

bool SetClipboardText(HWND owner, const std::wstring& text);  // below, with the terminal

struct ExitInfo {
    DWORD code;
    std::string stderrTail;
};

enum class Kind { User, Assistant, Notice, Error, Approval };

// A picture sent with a message, shown above its bubble: the file it is kept in and a preview of it.
struct FeedImage {
    std::wstring path;
    wil::com_ptr<IWICBitmap> preview;  // decoded once, at most kPreviewSide px
    wil::com_ptr<ID2D1Bitmap> bitmap;  // ... on the render target; made again with a new target
    UINT generation = 0;               // the target it was made on
    float width = 0;   // its size in the feed's box, in DIPs
    float height = 0;
    D2D1_RECT_F at{};  // where it is laid out: x from the column's left edge, y from the item's top
};

// A tool call waiting for the user (Kind::Approval), and what became of it.
enum class Verdict { Pending, Allowed, Denied, Cancelled };

struct FeedItem {
    Kind kind = Kind::Notice;
    std::wstring text;
    std::string messageId;  // assistant text: the API message it streams into
    int block = -1;         // ... and its content block index
    wil::com_ptr<IDWriteTextLayout> layout;
    float layoutWidth = 0;
    float textWidth = 0;
    float layoutHeight = 0;  // the text's own
    float height = 0;
    float top = 0;  // where it was last painted, for hit tests
    std::vector<MdSpan> spans;  // Kind::Assistant: how the layout's runs are styled (the text is markdown)
    std::wstring shown;         // the text the layout was made of (markdown without its markup ...): selections index it
    // Kind::User: the pictures above the bubble, and where the bubble starts below them.
    std::vector<FeedImage> images;
    float bubbleTop = 0;
    std::string tool;  // Kind::Approval: the tool's own name (write_file ...)
    // Kind::Approval: `text` is what the call does, `title` what kind of call it is.
    std::wstring title;
    std::string requestId;  // the CLI's can_use_tool request, answered once
    std::string toolUseId;  // ... and the call it is about: its result comes back under this id
    Json input;
    Verdict verdict = Verdict::Pending;
    bool done = false;         // the result has come back
    bool outputError = false;
    std::wstring output;       // what the tool returned
    size_t outputStart = 0;    // where it begins in the card's layout
};

// A card of write_file or edit_file that wrote a file here (not on a server): it gets "open" and "show in folder".
bool HasFileActions(const FeedItem& item) {
    return item.kind == Kind::Approval && (item.tool == "write_file" || item.tool == "edit_file") &&
           item.verdict == Verdict::Allowed && item.done && !item.outputError && !JsonStr(item.input, "path").empty() &&
           JsonStr(item.input, "on").empty();
}

// ... and the file, a relative path being the chat folder's (as the tools resolve it).
std::wstring WrittenFile(const FeedItem& item, const std::wstring& workspace) {
    const std::filesystem::path path(ToWide(JsonStr(item.input, "path")));
    return (path.is_relative() ? std::filesystem::path(workspace) / path : path).lexically_normal().wstring();
}

// A path as written in an answer, to a file that is there: file.cpp:12 (and :12:5) is file.cpp. Empty if none.
std::wstring ExistingPath(std::wstring path) {
    std::error_code error;
    for (int i = 0; i < 3; ++i) {
        if (std::filesystem::exists(path, error)) {
            return path;
        }
        const size_t colon = path.find_last_of(L':');
        if (colon == std::wstring::npos || colon < 2 || colon + 1 == path.size() ||
            path.find_first_not_of(L"0123456789", colon + 1) != std::wstring::npos) {
            break;
        }
        path.resize(colon);
    }
    return {};
}

std::wstring LowerExtension(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    for (wchar_t& c : extension) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return extension;
}

// A page the preview shows (and opens by itself when a tool writes one).
bool IsPreviewable(const std::wstring& path) {
    const std::wstring extension = LowerExtension(path);
    return extension == L".html" || extension == L".htm" || extension == L".svg";
}

// The newest change to a page and the web files beside it (its CSS, scripts, pictures; not subfolders).
std::filesystem::file_time_type FolderStamp(const std::wstring& page) {
    namespace fs = std::filesystem;
    static constexpr std::wstring_view kWeb[] = {L".html", L".htm", L".css", L".js",  L".mjs", L".svg",
                                                 L".png",  L".jpg", L".jpeg", L".gif", L".webp", L".json"};
    std::error_code error;
    fs::file_time_type newest = fs::last_write_time(page, error);
    if (error) {
        newest = {};
    }
    size_t seen = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(page).parent_path(), error)) {
        if (++seen > 500) {
            break;
        }
        std::error_code entryError;
        const std::wstring extension = LowerExtension(entry.path().wstring());
        if (std::ranges::find(kWeb, extension) != std::end(kWeb) && entry.is_regular_file(entryError)) {
            newest = std::max(newest, entry.last_write_time(entryError));
        }
    }
    return newest;
}

// Explorer, at the file's folder with the file selected.
void RevealInFolder(const std::wstring& path) {
    PIDLIST_ABSOLUTE item = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr))) {
        SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        CoTaskMemFree(item);
    }
}

// The panels that open over the window; one at a time.
enum class Popup { None, Models, Effort, Usage, Image };  // Image: a picture over the whole window

// One of the plan's limit windows, from the CLI's rate_limit_event.
struct Limit {
    int percent = -1;  // -1: not reported yet
    long long resetsAt = 0;  // Unix seconds
};

// A window as it stands now: past its reset it is empty, until the CLI reports again after a turn.
Limit Current(const Limit& limit) {
    if (limit.resetsAt > 0 && limit.resetsAt <= static_cast<long long>(std::time(nullptr))) {
        return Limit{limit.percent >= 0 ? 0 : -1, 0};
    }
    return limit;
}

enum class Target {
    None,
    SidebarToggle,
    Update,  // the header's "update to ..." (only while there is one)
    SidebarSearch,
    FolderChip,     // the chat's folder, under the input: a menu to change it
    SplitSidebar,   // the edge between the sidebar and the chat: drag to resize
    SplitTerminal,  // ... and between the chat and the terminal
    WinMin,    // the window's own buttons, drawn in the header (the system caption is gone)
    WinMax,
    WinClose,
    SignIn,
    InstallClaude,  // the install screen's button, and the same button while it works
    CancelInstall,
    CancelLogin,
    CopyLoginUrl,
    Account,  // the sidebar's account row
    TerminalToggle,
    TermClose,
    TermTab,       // row: the tab
    TermTabClose,  // row: the same
    TermAdd,
    TermScreen,
    PreviewWide,   // the preview over the window, or back
    NewChat,
    Attach,
    AttachmentChip,    // row: an index into the attachments; shows its remove button
    RemoveAttachment,  // row: the same
    ModelChip,
    EffortChip,
    UsageChip,
    Secrets,       // the sidebar's entry that opens the secrets
    SecretsClose,  // ... and the way back to the chats
    SecretRow,     // row: an index into the secrets' names
    SecretDelete,  // row: the same
    SecretSave,
    SecretEye,     // show or hide the value being typed
    SecretCancel,  // leave editing a secret
    Send,
    HistoryRow,   // row: an index into the filtered history
    ModelRow,     // row: an index into the main models
    MoreModels,   // the row that opens the other models
    MoreRow,      // row: an index into the other models
    EffortTrack,
    EffortReset,
    ApprovalAllow,  // row: the feed item
    ApprovalDeny,   // row: the same
    CardPreview,    // row: a card that wrote a page: show it in the preview
    CardOpen,       // row: a card that wrote a file: open the file
    CardReveal,     // row: the same: show it in its folder
    FeedImage,      // row: the feed item, sub: its picture
    InsidePopup,  // on a panel, nothing to click
};

struct Hit {
    Target target = Target::None;
    size_t row = 0;
    size_t sub = 0;
    bool operator==(const Hit&) const = default;
};

// Every rectangle of the window for one client size, in DIPs.
struct Geometry {
    float width = 0;
    float height = 0;
    float column = 0;
    float areaLeft = 0;  // the chat's left edge: right of the sidebar
    float area = 0;      // ... and its right edge
    float left = 0;      // where the chat column starts
    D2D1_RECT_F sidebar{};
    D2D1_RECT_F splitSidebar{};   // a few DIPs either side of the sidebar's edge
    D2D1_RECT_F splitTerminal{};  // ... and of the terminal's
    D2D1_RECT_F term{};  // the terminal column (empty when closed)
    D2D1_RECT_F termClose{};
    D2D1_RECT_F termWide{};   // beside a preview: widen it over the window, or back
    D2D1_RECT_F termScreen{};
    std::vector<D2D1_RECT_F> termTabs;  // one per shell, in the panel's top row
    D2D1_RECT_F termAdd{};               // "+": another shell
    D2D1_RECT_F terminalBtn{};  // the header's button that shows or hides the terminal
    D2D1_RECT_F update{};       // ... and left of it, "update to ..." while a newer build is there
    D2D1_RECT_F winMin{};       // minimize, maximize, close: the header is the window's title bar
    D2D1_RECT_F winMax{};
    D2D1_RECT_F winClose{};
    D2D1_RECT_F sideToggle{};   // the header's button that shows or hides the sidebar
    D2D1_RECT_F secrets{};     // the sidebar's entry for the secrets
    D2D1_RECT_F panel{};       // the secrets view: the sidebar itself while it is open
    D2D1_RECT_F panelClose{};
    D2D1_RECT_F nameField{};
    D2D1_RECT_F valueField{};
    D2D1_RECT_F saveButton{};
    D2D1_RECT_F secretEye{};     // inside the value field, at its right
    D2D1_RECT_F secretCancel{};  // beside the save button while a secret is edited
    float secretsTop = 0;  // the first secret's row
    D2D1_RECT_F newItem{};  // the sidebar's "new chat" row
    D2D1_RECT_F account{};  // the sidebar's account row, at the bottom
    D2D1_RECT_F folder{};   // under the input, at the left: the folder the chat works in
    D2D1_RECT_F signIn{};   // the sign-in screen's buttons (in the middle of an empty chat)
    D2D1_RECT_F signAlt{};
    float listBottom = 0;
    D2D1_RECT_F feed{};
    D2D1_RECT_F box{};
    D2D1_RECT_F attachments{};  // the row of attached files above the text
    D2D1_RECT_F text{};
    D2D1_RECT_F attach{};
    D2D1_RECT_F usage{};  // the ring that opens the plan limits
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

// A tool's output for its card: whole if short, else its first line (the exit code) and its end.
std::wstring OutputPreview(const std::wstring& output) {
    constexpr size_t kTail = 12;
    constexpr size_t kLongestLine = 400;
    std::vector<std::wstring> lines;
    for (size_t start = 0; start <= output.size();) {
        size_t end = output.find(L'\n', start);
        if (end == std::wstring::npos) {
            end = output.size();
        }
        std::wstring line = output.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        if (line.size() > kLongestLine) {
            line = line.substr(0, kLongestLine) + L"…";
        }
        lines.push_back(std::move(line));
        start = end + 1;
    }
    std::wstring preview;
    const auto add = [&](const std::wstring& line) { preview += (preview.empty() ? L"" : L"\n") + line; };
    if (lines.size() <= kTail + 2) {
        for (const std::wstring& line : lines) {
            add(line);
        }
        return preview;
    }
    add(lines.front());
    add(std::format(L"… скрыто строк: {} …", lines.size() - 1 - kTail));
    for (size_t i = lines.size() - kTail; i < lines.size(); ++i) {
        add(lines[i]);
    }
    return preview;
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

// Dark native menus: uxtheme's SetPreferredAppMode(ForceDark), as in Sovereign.
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

    void Paint();
    void PaintFeed(const Geometry& g);
    void PaintHeader(const Geometry& g);
    void PaintWindowButtons(const Geometry& g);
    LRESULT HitTestFrame(LPARAM lParam);
    void PaintInput(const Geometry& g);
    void PaintPanel(const D2D1_RECT_F& rect);
    void PaintModels(const Geometry& g);
    void PaintEffort(const Geometry& g);
    void PaintUsage(const Geometry& g);

    // Selecting text in the feed with the mouse, across messages.
    struct FeedPoint {
        size_t item = 0;  // an index into feed_
        UINT32 pos = 0;   // in the item's shown text
    };
    D2D1_POINT_2F ItemOrigin(const Geometry& g, const FeedItem& item) const;
    std::optional<FeedPoint> FeedPointAt(const Geometry& g, float x, float y) const;
    std::wstring LinkAt(const Geometry& g, float x, float y) const;
    // The innermost markdown span with `flag` under a point of an answer (and its item), or null.
    const MdSpan* MarkdownSpanAt(const Geometry& g, float x, float y, unsigned flag, const FeedItem** owner) const;

    // Hints: what a button does, shown after the pointer rests on it (a link shows its address).
    std::wstring TooltipText();
    void UpdateTip();
    void HideTip();
    static LRESULT CALLBACK TipProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void ShowTip();
    void PaintTip();

    // Renaming in place (a terminal tab, a chat in the sidebar): the keys go to the field until Enter or Esc,
    // a click elsewhere keeps what is typed.
    enum class Rename { None, Tab, Chat };
    void BeginRename(Rename what, size_t index);
    void CommitRename();
    void CancelRename();
    void OnRenameKey(WPARAM key);
    void OnRenameChar(wchar_t c);
    void PaintRenameField(const D2D1_RECT_F& r);

    // Right-click menus.
    void ShowTabMenu(size_t index, POINT screen);
    void ShowSecretMenu(size_t index, POINT screen);
    void ShowAttachmentMenu(size_t index, POINT screen);
    void ShowTitleMenu(POINT screen);
    void ClearTerminalTab(size_t index);
    bool HasFeedSelection() const;
    bool FeedSelectionIn(size_t index, UINT32& from, UINT32& to) const;
    void PaintFeedSelection(size_t index, D2D1_POINT_2F origin);
    std::wstring FeedSelectionText() const;
    bool CopyFeedSelection();
    void ClearFeedSelection();

    // The chat's folder: the default, recent ones, any folder, or a GitHub repository (cloned first).
    void LoadRepos();
    void SwitchWorkspace(const std::wstring& folder);
    void PickWorkspace();
    void OpenRepo(size_t index);
    void OnCloneDone(const CloneDone& done);
    void ShowFolderMenu();
    std::wstring FolderLabel() const {
        return cloning_ ? L"Клонирую " + cloningName_ + L"…" : folderName_;
    }

    // The account: `claude auth status / login / logout`, each on its own thread.
    bool SignedOut() const { return auth_.known && !auth_.loggedIn; }
    void RefreshAuth();
    void SignIn();
    void CancelSignIn();
    void SignOut();
    void OnAuthStatus(const AuthStatus& status);
    void OnLoginDone(const LoginDone& done);
    void OnLogoutDone();
    void ShowAccountMenu();
    void PaintSignIn(const Geometry& g);

    // The CLI itself: fetched when it is missing, updated from the account menu.
    bool ClaudeUpdateAvailable() const { return ClaudeVersionLess(claudeVersion_, claudeLatest_); }
    void InstallClaudeCli();
    void UpdateClaudeCli();
    void CancelSetup() { setupCancel_ = true; }
    void OnSetupEvent(const SetupEvent& event);
    void CheckClaudeVersions();
    void OnClaudeVersions(const ClaudeVersions& versions);
    void PaintInstall(const Geometry& g);
    std::wstring AccountName() const;
    std::wstring AccountPlan() const;

    // The terminal at the right: PowerShell in a pseudo console, drawn from the emulator's screen.
    struct TermPoint {
        size_t line = 0;  // an index into the emulator's lines (the scrollback first)
        int col = 0;
    };
    // One shell per tab; its posts carry the tab's id, so a closed or restarted shell's late output is dropped.
    struct TermTab {
        std::unique_ptr<VtScreen> screen;
        std::unique_ptr<Pty> pty;  // its threads end (in its destructor) before the screen goes
        WPARAM id = 0;
        std::wstring name;         // given by the user; empty: the numbered default
        int number = 1;            // "Терминал 1", "Терминал 2" ...
        bool exited = false;
        size_t scroll = 0;         // lines scrolled back from the bottom
        std::wstring preview;      // the preview tab (one at most, no shell): the HTML file it shows
    };
    TermTab* Tab() const { return activeTab_ < tabs_.size() ? tabs_[activeTab_].get() : nullptr; }
    VtScreen* Screen() const { return Tab() != nullptr ? Tab()->screen.get() : nullptr; }
    TermTab* TabById(WPARAM id) const;
    void TerminalGrid(int& cols, int& rows) const;
    void AddTerminalTab();
    void StartShell(TermTab& tab);
    void CloseTerminalTab(size_t index);
    void SelectTerminalTab(size_t index);
    std::wstring TabLabel(const TermTab& tab) const;
    void PaintTerminal(const Geometry& g);
    void SetTerminalOpen(bool open);
    void SyncTerminalSize();
    void OnTermData(WPARAM id, const std::string& bytes);
    void OnTermExit(WPARAM id, DWORD code);
    void OnTermKey(WPARAM key);
    void OnTermChar(wchar_t c);
    void TermSend(std::string_view bytes);
    void PasteToTerminal();
    void CopyTermSelection();
    bool HasTermSelection() const;
    bool TermSelected(size_t line, int col) const;
    size_t TermFirstLine() const;
    TermPoint TermCellAt(const Geometry& g, float x, float y) const;
    void ScrollTerminal(long long lines);

    // The preview: an HTML file in a tab beside the shells (WebView2), loaded again whenever it or a file beside
    // it changes. Opens by itself when a tool writes such a file here.
    bool PreviewShown() const { return terminalOpen_ && Tab() != nullptr && !Tab()->preview.empty(); }
    TermTab* PreviewTab() const;
    void OpenPreview(const std::wstring& path);
    void EnsureWebView();
    void NavigatePreview();
    void PlacePreview();
    void CheckPreview();
    void ShowPreviewMenu(size_t index, POINT screen);
    void OnPreviewPick(const std::wstring& message);

    // A newer AIControls built here: a button in the header installs it, restarts and opens this chat again.
    void CheckAppUpdate();
    void StartUpdate();
    std::wstring UpdateLabel() const { return update_ ? L"Обновить до " + ToWide(update_->version) : std::wstring(); }  // Ctrl+click in the page: the element goes into the message
    // Wide: the sidebar goes and the chat keeps only a strip, the rest is the preview's; again: as it was.
    void SetPreviewWide(bool wide);
    float MinChat() const { return PreviewShown() ? kMinChatPreview : kMinChat; }

    // The sidebar: search, the way to the secrets, the chats (or the secrets view in their place).
    void PaintSidebar(const Geometry& g);
    static size_t SidebarRows(const Geometry& g);
    void SetSidebarOpen(bool open);
    void FocusSearch();
    void RefreshSessions();
    void OnSearchKey(WPARAM key);
    void OnSearchChar(wchar_t c);

    // The secrets view: names and masked values, delete, and a form to add or replace one.
    static LRESULT CALLBACK SecretEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                           DWORD_PTR self);
    HWND CreateSecretField(bool password, const wchar_t* hint);
    void SetSecretsOpen(bool open);
    void PlaceSecretFields();
    void SubmitSecret();
    void EditSecret(const std::string& name);
    void CancelSecretEdit();
    void SetShowValue(bool show);
    void DeleteSecret(const std::string& name);
    void PaintSecrets(const Geometry& g);
    static D2D1_RECT_F SecretRowRect(const Geometry& g, size_t index);
    static D2D1_RECT_F SecretDeleteRect(const D2D1_RECT_F& row);
    static size_t SecretRowsFit(const Geometry& g);
    // A list that is longer than its room: the first row shown is kept within the list, and a thin thumb on the
    // panel's edge shows where in the list the window is (brighter while the pointer is over the panel).
    static size_t ClampFirst(size_t first, size_t total, size_t rows) { return total > rows ? std::min(first, total - rows) : 0; }
    void PaintListThumb(float x, float top, float bottom, size_t first, size_t rows, size_t total, bool bright);
    std::wstring FieldText(HWND field) const;
    void Ring(D2D1_POINT_2F center, float radius, float fraction, uint32_t color);
    void Text(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, uint32_t color);
    void Fill(const D2D1_RECT_F& rect, float radius, uint32_t color, float alpha = 1.f);
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, uint32_t color, float width = 1.6f);
    void Check(D2D1_POINT_2F center, uint32_t color);
    void MeasureFeed(float column);
    float FeedHeight() const;
    void ScrollBy(float delta);
    void CopyItemAt(float y);
    void Flash(std::wstring status, UINT milliseconds = 1500);

    void AskApproval(const std::string& requestId, const Json& request);
    void OnToolResult(const Json& block);
    void Decide(size_t index, bool allow);
    void CancelApprovals();  // pending cards are answered "no" and marked as cancelled
    void StopTurn();
    static float ApprovalHeight(const FeedItem& item);
    static void ApprovalButtons(const Geometry& g, const FeedItem& item, D2D1_RECT_F& allow, D2D1_RECT_F& deny);
    void PaintApproval(const Geometry& g, const FeedItem& item, float left);
    void StyleMarkdown(FeedItem& item);
    void PaintMarkdown(const FeedItem& item, float x, float y, float left, float column);
    // A page gets "preview" first; other files have it empty.
    static void ApprovalFileButtons(const Geometry& g, const FeedItem& item, D2D1_RECT_F& preview, D2D1_RECT_F& open,
                                    D2D1_RECT_F& reveal);

    // Pictures in the feed: above the bubble of the message they went with; a click shows one over the window
    // (the arrows go through the others), the right button copies, opens or finds it.
    FeedImage MakeFeedImage(std::wstring path) const;
    static float LayoutImages(FeedItem& item, float column);  // the pictures' height
    D2D1_RECT_F ImageRect(const Geometry& g, const FeedItem& item, size_t index) const;
    ID2D1Bitmap* Bitmap(FeedImage& image);
    void PaintImages(const Geometry& g, FeedItem& item);
    void OpenViewer(size_t item, size_t image);
    void StepViewer(int delta);
    void PaintViewer(const Geometry& g);
    void ShowImageMenu(const std::wstring& path, POINT screen);
    void CopyImage(const std::wstring& path);
    void OpenFile(const std::wstring& path);  // with its app; a flash when it is gone

    D2D1_RECT_F AttachmentRect(const Geometry& g, size_t index) const;
    static D2D1_RECT_F RemoveRect(const D2D1_RECT_F& chip);
    void PaintAttachments(const Geometry& g);
    void PickFiles();
    void AttachFile(const std::wstring& path);
    void AttachClipboardImage();
    void PasteClipboard();
    void ShowInputMenu(POINT screen);
    void ShowCopyMenu(POINT screen, D2D1_POINT_2F at);
    void ShowChatMenu(size_t filteredIndex, POINT screen);
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
    std::optional<FeedPoint> feedAnchor_;  // where a selection in the feed began
    std::optional<FeedPoint> feedFocus_;   // ... and where it ends now
    bool feedSelecting_ = false;           // the mouse is dragging one
    std::vector<GithubRepo> repos_;  // as GitHub listed them for the GITHUB secret
    std::wstring reposError_;
    bool reposLoading_ = false;
    std::thread reposThread_;
    bool cloning_ = false;
    std::wstring cloningName_;
    std::thread cloneThread_;
    std::atomic<bool> cloneCancel_ = false;
    AuthStatus auth_;                // as last asked; auth_.known is false until the first answer
    bool authWorking_ = false;       // a CLI call (status, login, logout) is running
    bool loginWaiting_ = false;      // `claude auth login` runs: the user is in the browser
    bool restartAfterAuth_ = false;  // signed in: the CLI starts over to pick the account up
    std::string loginUrl_;
    std::wstring authNote_;          // why the last attempt failed
    std::thread authThread_;
    std::atomic<bool> authCancel_ = false;
    bool claudeMissing_ = false;     // no claude.exe was found: the install screen takes the empty chat
    bool setupBusy_ = false;         // the installer or the updater runs
    bool setupInstalling_ = false;   // ... and has the build already
    unsigned setupPercent_ = 0;
    std::wstring setupNote_;         // why the last attempt failed
    std::thread setupThread_;
    std::atomic<bool> setupCancel_ = false;
    std::string claudeVersion_;      // as last asked; empty until the first answer
    std::string claudeLatest_;
    std::string announcedVersion_;   // the newest version already mentioned in a flash
    std::thread versionThread_;
    bool terminalOpen_ = false;
    bool termFocus_ = false;         // typing goes to the shell
    bool termSwallowChar_ = false;   // the key that was just handled would also make a character
    bool selecting_ = false;
    wchar_t pendingHigh_ = 0;        // a UTF-16 high surrogate waiting for its pair
    float cellW_ = 8.f;
    float cellH_ = 18.f;
    std::optional<TermPoint> selAnchor_;  // in the active tab
    std::optional<TermPoint> selEnd_;
    std::vector<std::unique_ptr<TermTab>> tabs_;
    size_t activeTab_ = 0;
    WPARAM nextTabId_ = 0;
    wil::com_ptr<ICoreWebView2Controller> webController_;
    wil::com_ptr<ICoreWebView2> webView_;
    bool webCreating_ = false;
    std::optional<AppUpdate> update_;
    bool updatePending_ = false;  // asked while Claude was answering: done when the turn ends
    bool previewWide_ = false;
    float wideSavedTerminal_ = kTerminal;  // the widths to go back to
    bool wideSavedSidebar_ = true;
    std::wstring webShown_;  // the file the preview has loaded
    std::filesystem::file_time_type previewStamp_{};  // ... and the newest change in its folder then
    wil::com_ptr<IDWriteTextFormat> termFont_;
    wil::com_ptr<IDWriteTextFormat> termBold_;
    float sidebarWidth_ = kSidebar;   // as dragged; kept in the settings
    float terminalWidth_ = kTerminal;
    Target dragging_ = Target::None;  // a splitter being dragged
    bool sidebarOpen_ = true;
    bool searchFocus_ = false;  // typing goes to the sidebar's search (drawn here, no caret)
    bool secretsOpen_ = false;  // the sidebar shows the secrets instead of the chats
    std::string editingSecret_;  // the secret the form holds (its old name: renaming replaces it); empty: a new one
    bool showValue_ = false;     // the value field shows its text instead of dots
    HWND nameEdit_ = nullptr;   // the panel's two fields; hidden while it is closed
    HWND valueEdit_ = nullptr;  // (a password field: the value never shows)
    wil::unique_hbrush fieldBrush_;

    wil::com_ptr<ID2D1Factory> d2d_;
    wil::com_ptr<IDWriteFactory> dwrite_;
    wil::com_ptr<IWICImagingFactory> wic_;
    wil::com_ptr<IDWriteFontCollection> fonts_;
    wil::com_ptr<ID2D1HwndRenderTarget> target_;
    UINT targetGeneration_ = 0;  // bumped with each new target: bitmaps made on an old one are made again
    wil::com_ptr<ID2D1SolidColorBrush> brush_;
    wil::com_ptr<ID2D1StrokeStyle> round_;
    wil::com_ptr<IDWriteTextFormat> body_;
    wil::com_ptr<IDWriteTextFormat> notice_;
    wil::com_ptr<IDWriteTextFormat> header_;
    wil::com_ptr<IDWriteTextFormat> ui_;
    wil::com_ptr<IDWriteTextFormat> uiBold_;
    wil::com_ptr<IDWriteTextFormat> uiRight_;
    wil::com_ptr<IDWriteTextFormat> badge_;
    wil::com_ptr<IDWriteTextFormat> greeting_;
    wil::com_ptr<IDWriteTextFormat> small_;  // wrapping, left-aligned notes
    wil::com_ptr<IDWriteTextFormat> mono_;
    wil::com_ptr<IDWriteTextFormat> button_;

    std::vector<FeedItem> feed_;
    float scroll_ = 0;
    float maxScroll_ = 0;
    bool stick_ = true;  // follow the end of the feed as it grows

    float textHeight_ = kTextMin;  // the input's text area, grows with what is typed
    bool inputFocused_ = false;
    bool hasText_ = false;
    std::vector<Attachment> attachments_;  // go with the next message
    // The picture shown over the window (Popup::Image): which one, and its larger image.
    size_t viewerItem_ = 0;
    size_t viewerImage_ = 0;
    FeedImage viewer_;
    FeedImage mascot_;  // sits on the input box, as the one in Claude's app
    Hit hover_;
    bool trackingMouse_ = false;

    Popup popup_ = Popup::None;
    // History.
    std::vector<SessionInfo> sessions_;
    std::vector<size_t> filtered_;
    std::wstring searchText_;
    size_t firstRow_ = 0;
    size_t secretsFirst_ = 0;  // the first secret shown when they do not all fit
    float wheelRemainder_ = 0;  // wheel movement not yet worth a whole row
    std::wstring tipText_;      // the hint for what is under the pointer ("" for nothing)
    HWND tip_ = nullptr;        // ... shown in a window of its own, made when first needed
    wil::com_ptr<ID2D1HwndRenderTarget> tipTarget_;
    wil::com_ptr<IDWriteTextLayout> tipLayout_;
    bool tipShown_ = false;
    D2D1_POINT_2F tipAt_{};     // where the pointer was when it appeared
    Rename renaming_ = Rename::None;
    WPARAM renameTab_ = 0;      // the tab's id
    std::string renameChat_;    // the chat's session id
    std::wstring renameText_;
    D2D1_POINT_2F mouse_{-1.f, -1.f};  // the pointer in DIPs; off the window after it leaves
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
    std::wstring folderName_;  // its last component, for the chip under the input
    Settings settings_;
    Json models_ = Json::array();
    std::string userName_;

    ClaudeProcess claude_;
    // Declared after claude_: the tool threads answer through it, so they are joined first.
    SecretStore secrets_{SecretStore::DefaultPath()};
    Tools tools_{secrets_, {}};
    ToolHost host_{tools_, [this](const std::string& id, Json response) { claude_.Respond(id, std::move(response)); }};
    WPARAM generation_ = 0;  // bumped per process; stale posts are dropped
    bool running_ = false;
    bool busy_ = false;
    bool stateEvents_ = false;  // the CLI reports its session state; else a result ends the turn
    std::string sessionId_;
    std::wstring title_;
    std::string currentMessageId_;
    std::set<std::string> streamed_;  // messages whose text came as deltas
    Limit session_;  // the five-hour window
    Limit week_;
    std::wstring route_;
    std::wstring status_;
};

int ChatWindow::Run(int show) {
    workspace_ = DefaultWorkspace();
    folderName_ = std::filesystem::path(workspace_).filename().wstring();
    if (folderName_.empty()) {
        folderName_ = workspace_;
    }
    tools_.SetCwd(workspace_);
    secrets_.Load();
    settings_ = LoadSettings();
    if (std::error_code error; !settings_.workspace.empty() && std::filesystem::is_directory(settings_.workspace, error)) {
        workspace_ = settings_.workspace;  // the folder chosen last time
        folderName_ = std::filesystem::path(workspace_).filename().wstring();
        tools_.SetCwd(workspace_);
    }
    if (settings_.sidebarWidth > 0) {
        sidebarWidth_ = std::clamp(settings_.sidebarWidth, kMinSidebar, kMaxSidebar);
    }
    if (settings_.terminalWidth > 0) {
        terminalWidth_ = std::max(settings_.terminalWidth, kMinTerminal);
    }
    {  // the limits as last seen; a window that has reset since is empty again
        const long long now = static_cast<long long>(std::time(nullptr));
        const auto restore = [now](int percent, long long resetsAt) {
            return resetsAt > 0 && resetsAt <= now ? Limit{percent >= 0 ? 0 : -1, 0} : Limit{percent, resetsAt};
        };
        session_ = restore(settings_.sessionPercent, settings_.sessionResets);
        week_ = restore(settings_.weekPercent, settings_.weekResets);
    }
    UseDarkMenus();
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put()));
    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite_.put())));
    fonts_ = LoadFontCollection(dwrite_.get());
    RegisterGdiFonts(instance_);
    THROW_IF_FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(wic_.put())));
    if (HRSRC found = FindResourceW(instance_, MAKEINTRESOURCEW(IDR_MASCOT), RT_RCDATA); found != nullptr) {
        if (HGLOBAL loaded = LoadResource(instance_, found); loaded != nullptr) {  // a resource needs no freeing
            mascot_.preview = LoadImagePreview(wic_.get(), LockResource(loaded), SizeofResource(instance_, found), 128);
        }
    }
    D2D1_STROKE_STYLE_PROPERTIES stroke = D2D1::StrokeStyleProperties();
    stroke.startCap = stroke.endCap = D2D1_CAP_STYLE_ROUND;
    stroke.lineJoin = D2D1_LINE_JOIN_ROUND;
    THROW_IF_FAILED(d2d_->CreateStrokeStyle(stroke, nullptr, 0, round_.put()));
    body_ = MakeFormat(kTextFont, 15.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    notice_ = MakeFormat(kTextFont, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    header_ = MakeFormat(kTextFont, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    ui_ = MakeFormat(kTextFont, 13.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    uiBold_ = MakeFormat(kTextFont, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    uiRight_ = MakeFormat(kTextFont, 12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    badge_ = MakeFormat(kTextFont, 11.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    greeting_ = MakeFormat(kDisplayFont, 22.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    small_ = MakeFormat(kTextFont, 12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    mono_ = MakeFormat(kMonoFont, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    button_ = MakeFormat(kTextFont, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    termFont_ = MakeFormat(kMonoFont, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    termBold_ = MakeFormat(kMonoFont, 12.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    cellW_ = TextWidth(L"MMMMMMMMMM", termFont_.get()) / 10.f;
    cellH_ = 18.f;

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

    CreateWindowExW(0, windowClass.lpszClassName, L"AIControls", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance_, this);
    THROW_LAST_ERROR_IF_NULL(hwnd_);
    SetWindowPos(hwnd_, nullptr, 0, 0, ToPx(1000.f), ToPx(840.f), SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    if (!fonts_) {
        Add(Kind::Error, L"Встроенные шрифты не загрузились — показываю системным.");
    }
    RefreshSessions();
    RefreshAuth();
    LoadRepos();
    CheckClaudeVersions();
    StartClaude();  // warm up: the first answer does not wait for the CLI to start
    if (!settings_.reopen.empty()) {  // back from an update: the chat it was in
        const std::string id = std::exchange(settings_.reopen, std::string());
        SaveSettings(settings_);
        for (size_t i = 0; i < filtered_.size(); ++i) {
            if (sessions_[filtered_[i]].id == id) {
                OpenSession(i);
                break;
            }
        }
    }
    CheckAppUpdate();
    SetTimer(hwnd_, kUpdateTimer, kUpdateCheck, nullptr);

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
    // Rounded up and a DIP more: a box made exactly this wide draws the text a hair wider (pixel snapping),
    // and its last letter wrapped or turned into an ellipsis.
    return std::ceil(metrics.widthIncludingTrailingWhitespace) + 1.f;
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
    if (message == WM_CONTEXTMENU) {
        POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (at.x == -1 && at.y == -1) {  // the keyboard's menu key
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            at = {rect.left + 8, rect.top + 8};
        }
        reinterpret_cast<ChatWindow*>(self)->ShowInputMenu(at);
        return 0;
    }
    if (message == WM_PASTE) {
        reinterpret_cast<ChatWindow*>(self)->PasteClipboard();  // files and pictures too, not only text
        return 0;
    }
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
        // The frame is recalculated without the caption (WM_NCCALCSIZE).
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        return 0;
    }
    case WM_NCCALCSIZE:
        // No system caption: the header is the title bar. The side and bottom frames stay (they are the
        // invisible resize borders); a maximized window, which hangs over the screen by its frame, is kept inside.
        if (wParam == TRUE) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            const LONG top = params->rgrc[0].top;
            DefWindowProcW(hwnd_, WM_NCCALCSIZE, wParam, lParam);
            params->rgrc[0].top = top;
            if (IsZoomed(hwnd_)) {
                const UINT dpi = GetDpiForWindow(hwnd_);
                params->rgrc[0].top += GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                                       GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            }
            return 0;
        }
        break;
    case WM_NCHITTEST:
        return HitTestFrame(lParam);
    case WM_CTLCOLOREDIT:
        if (const auto field = reinterpret_cast<HWND>(lParam); field == nameEdit_ || field == valueEdit_) {
            const auto dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(kPrimary));
            SetBkColor(dc, ToColorRef(kRaised));
            return reinterpret_cast<LRESULT>(fieldBrush_.get());
        }
        break;
    case kAuthStatus: {
        const std::unique_ptr<AuthStatus> payload(reinterpret_cast<AuthStatus*>(lParam));
        OnAuthStatus(*payload);
        return 0;
    }
    case kAuthUrl: {
        const std::unique_ptr<std::string> payload(reinterpret_cast<std::string*>(lParam));
        if (loginWaiting_) {
            loginUrl_ = *payload;
            Refresh();
        }
        return 0;
    }
    case kAuthLogin: {
        const std::unique_ptr<LoginDone> payload(reinterpret_cast<LoginDone*>(lParam));
        OnLoginDone(*payload);
        return 0;
    }
    case kAuthLogout:
        OnLogoutDone();
        return 0;
    case kSetupEvent: {
        const std::unique_ptr<SetupEvent> payload(reinterpret_cast<SetupEvent*>(lParam));
        OnSetupEvent(*payload);
        return 0;
    }
    case kClaudeVersions: {
        const std::unique_ptr<ClaudeVersions> payload(reinterpret_cast<ClaudeVersions*>(lParam));
        OnClaudeVersions(*payload);
        return 0;
    }
    case kRepos: {
        const std::unique_ptr<ReposResult> payload(reinterpret_cast<ReposResult*>(lParam));
        reposLoading_ = false;
        repos_ = std::move(payload->repos);
        reposError_ = ToWide(payload->error);
        return 0;
    }
    case kCloneDone: {
        const std::unique_ptr<CloneDone> payload(reinterpret_cast<CloneDone*>(lParam));
        OnCloneDone(*payload);
        return 0;
    }
    case kTermData: {
        const std::unique_ptr<std::string> payload(reinterpret_cast<std::string*>(lParam));
        OnTermData(wParam, *payload);
        return 0;
    }
    case kTermExit:
        OnTermExit(wParam, static_cast<DWORD>(lParam));
        return 0;
    case kSecretKey: {
        const auto from = reinterpret_cast<HWND>(lParam);
        if (wParam == VK_ESCAPE) {
            if (!editingSecret_.empty()) {
                CancelSecretEdit();
            } else {
                SetSecretsOpen(false);
            }
        } else if (wParam == VK_TAB || (wParam == VK_RETURN && from == nameEdit_)) {
            SetFocus(from == nameEdit_ ? valueEdit_ : nameEdit_);
        } else {
            SubmitSecret();
        }
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
        mouse_ = at;
        if (draggingEffort_) {
            SetEffortAt(at.x);
            return 0;
        }
        if (dragging_ != Target::None) {  // a splitter: the panel follows the mouse, within limits
            const Geometry g = Layout();
            previewWide_ = false;  // dragged by hand: that is the width now
            if (dragging_ == Target::SplitSidebar) {
                const float most = std::max(kMinSidebar, g.width - (g.width - g.area) - MinChat());
                sidebarWidth_ = std::clamp(at.x, kMinSidebar, std::min(kMaxSidebar, most));
            } else {
                const float most = std::max(kMinTerminal, g.width - g.areaLeft - MinChat());
                terminalWidth_ = std::clamp(g.width - at.x, kMinTerminal, most);
            }
            PlaceChildren();
            Refresh();
            return 0;
        }
        if (feedSelecting_) {  // dragging a selection in the feed; past its edge the feed scrolls
            const Geometry g = Layout();
            if (at.y < g.feed.top) {
                ScrollBy(-24.f);
            } else if (at.y > g.feed.bottom) {
                ScrollBy(24.f);
            }
            if (const auto point = FeedPointAt(Layout(), at.x, at.y)) {
                feedFocus_ = point;
            }
            Refresh();
            return 0;
        }
        if (selecting_) {  // dragging a selection in the terminal; past its edge the view scrolls
            const Geometry g = Layout();
            if (at.y < g.termScreen.top) {
                ScrollTerminal(1);
            } else if (at.y > g.termScreen.bottom) {
                ScrollTerminal(-1);
            }
            selEnd_ = TermCellAt(g, at.x, at.y);
            Refresh();
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
        UpdateTip();
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        mouse_ = {-1.f, -1.f};
        HideTip();
        tipText_.clear();
        hover_ = {};
        Refresh();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && reinterpret_cast<HWND>(wParam) == hwnd_) {
            const bool clickable = hover_.target != Target::None && hover_.target != Target::InsidePopup &&
                                   hover_.target != Target::AttachmentChip && hover_.target != Target::TermScreen;
            bool text = hover_.target == Target::TermScreen;
            bool link = false;
            if (hover_.target == Target::None && popup_ == Popup::None && HasConversation()) {  // over the feed: text
                POINT cursor{};
                GetCursorPos(&cursor);
                ScreenToClient(hwnd_, &cursor);
                text = Inside(Layout().feed, ToDip(cursor.x), ToDip(cursor.y));
                if (text && !LinkAt(Layout(), ToDip(cursor.x), ToDip(cursor.y)).empty()) {
                    text = false;
                    link = true;
                }
            }
            const bool split = hover_.target == Target::SplitSidebar || hover_.target == Target::SplitTerminal;
            SetCursor(LoadCursorW(nullptr, split ? IDC_SIZEWE
                                                 : (link ? IDC_HAND : (text ? IDC_IBEAM : (clickable ? IDC_HAND : IDC_ARROW)))));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        const D2D1_POINT_2F at = CursorDip(lParam);
        HideTip();
        if (renaming_ != Rename::None) {  // a click elsewhere keeps what was typed
            CommitRename();
        }
        if (const Target target = HitTest(at.x, at.y).target;
            popup_ == Popup::None && (target == Target::SplitSidebar || target == Target::SplitTerminal)) {
            dragging_ = target;
            SetCapture(hwnd_);
            return 0;
        }
        if (popup_ == Popup::None && Screen() && HitTest(at.x, at.y).target == Target::TermScreen) {
            termFocus_ = true;  // a click into the terminal: keys go to the shell, a drag selects
            searchFocus_ = false;
            SetFocus(hwnd_);
            selEnd_ = TermCellAt(Layout(), at.x, at.y);
            selAnchor_ = selEnd_;
            selecting_ = true;
            SetCapture(hwnd_);
            Refresh();
            return 0;
        }
        if (popup_ == Popup::None && HitTest(at.x, at.y).target == Target::None) {
            const Geometry g = Layout();
            if (Inside(g.feed, at.x, at.y)) {  // a press on the feed's text starts a selection
                ClearFeedSelection();
                feedAnchor_ = FeedPointAt(g, at.x, at.y);
                feedFocus_ = feedAnchor_;
                feedSelecting_ = feedAnchor_.has_value();
                if (feedSelecting_) {
                    SetCapture(hwnd_);
                }
                Refresh();
                return 0;
            }
        }
        if (HitTest(at.x, at.y).target == Target::EffortTrack) {
            draggingEffort_ = true;
            SetCapture(hwnd_);
            SetEffortAt(at.x);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (dragging_ != Target::None) {
            dragging_ = Target::None;
            ReleaseCapture();
            settings_.sidebarWidth = sidebarWidth_;
            settings_.terminalWidth = terminalWidth_;
            SaveSettings(settings_);
            return 0;
        }
        if (feedSelecting_) {
            feedSelecting_ = false;
            ReleaseCapture();
            if (!HasFeedSelection()) {  // a plain click: nothing selected, a link opens
                ClearFeedSelection();
                const D2D1_POINT_2F at = CursorDip(lParam);
                const std::wstring url = LinkAt(Layout(), at.x, at.y);
                if (url.starts_with(L"http://") || url.starts_with(L"https://") || url.starts_with(L"mailto:")) {
                    ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                } else if (IsLocalPath(url) && IsPreviewable(url)) {
                    OpenPreview(url);
                } else if (IsLocalPath(url)) {
                    OpenFile(url);
                }
            }
            searchFocus_ = false;
            termFocus_ = false;
            ClosePopup();
            SetFocus(input_);  // Ctrl+C there copies the selection when the input has none of its own
            Refresh();
            return 0;
        }
        if (selecting_) {
            selecting_ = false;
            ReleaseCapture();
            if (!HasTermSelection()) {  // a plain click: no selection
                selAnchor_.reset();
                selEnd_.reset();
            }
            Refresh();
            return 0;
        }
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
        HideTip();
        POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &at);
        const float steps = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        const Geometry g = Layout();
        // A mouse with a free-spinning wheel or a touchpad sends deltas below 120: the part of a row that does not
        // move the list yet is kept for the next message.
        const auto wholeRows = [&](float perNotch) {
            wheelRemainder_ += steps * perNotch;
            const auto whole = static_cast<long long>(wheelRemainder_);
            wheelRemainder_ -= static_cast<float>(whole);
            return whole;
        };
        if (popup_ == Popup::None && Inside(g.term, ToDip(at.x), ToDip(at.y))) {
            ScrollTerminal(-static_cast<long long>(std::lround(steps * 3)));
        } else if (popup_ == Popup::None && secretsOpen_ && Inside(g.panel, ToDip(at.x), ToDip(at.y))) {
            const auto moved = static_cast<long long>(secretsFirst_) + wholeRows(2.f);
            secretsFirst_ = ClampFirst(static_cast<size_t>(std::max<long long>(moved, 0)), secrets_.Names().size(),
                                       SecretRowsFit(g));
            hover_ = HitTest(ToDip(at.x), ToDip(at.y));  // the rows moved under the pointer
            Refresh();
        } else if (popup_ == Popup::None && !secretsOpen_ && Inside(g.sidebar, ToDip(at.x), ToDip(at.y))) {
            const auto moved = static_cast<long long>(firstRow_) + wholeRows(3.f);
            firstRow_ = ClampFirst(static_cast<size_t>(std::max<long long>(moved, 0)), filtered_.size(), SidebarRows(g));
            hover_ = HitTest(ToDip(at.x), ToDip(at.y));
            Refresh();
        } else if (popup_ == Popup::None) {
            ScrollBy(steps * 64.f);
        }
        return 0;
    }
    case WM_RBUTTONUP:
        if (popup_ == Popup::None && Screen() &&
            HitTest(CursorDip(lParam).x, CursorDip(lParam).y).target == Target::TermScreen) {
            if (HasTermSelection()) {  // as in Windows Terminal: a right click copies a selection, else pastes
                CopyTermSelection();
            } else {
                PasteToTerminal();
            }
            return 0;
        }
        if (popup_ == Popup::Image) {
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd_, &screen);
            ShowImageMenu(viewer_.path, screen);
            return 0;
        }
        if (popup_ == Popup::None) {
            HideTip();
            if (renaming_ != Rename::None) {
                CommitRename();
            }
            const D2D1_POINT_2F at = CursorDip(lParam);
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd_, &screen);
            const Geometry g = Layout();
            switch (const Hit hit = HitTest(at.x, at.y); hit.target) {
            case Target::HistoryRow:
                ShowChatMenu(hit.row, screen);
                return 0;
            case Target::TermTab:
            case Target::TermTabClose:
                ShowTabMenu(hit.row, screen);
                return 0;
            case Target::SecretRow:
            case Target::SecretDelete:
                ShowSecretMenu(hit.row, screen);
                return 0;
            case Target::AttachmentChip:
            case Target::RemoveAttachment:
                ShowAttachmentMenu(hit.row, screen);
                return 0;
            case Target::FeedImage:
                ShowImageMenu(feed_[hit.row].images[hit.sub].path, screen);
                return 0;
            case Target::Account:
                ShowAccountMenu();
                return 0;
            case Target::None:
                break;
            default:
                return 0;  // a button: its left click is what it does
            }
            if (at.y < kHeader && at.x > g.areaLeft && at.x < g.terminalBtn.left) {
                ShowTitleMenu(screen);  // the chat's title
            } else if (!secretsOpen_ && Inside(g.sidebar, at.x, at.y)) {
                ShowTitleMenu(screen);  // the empty part of the chats' list: the same "new chat"
            } else if (Inside(g.box, at.x, at.y)) {
                ShowInputMenu(screen);  // the padding of the input card
            } else {
                ShowCopyMenu(screen, at);
            }
        }
        return 0;
    case WM_KEYDOWN:
        HideTip();
        if (renaming_ != Rename::None) {
            OnRenameKey(wParam);
        } else if (popup_ != Popup::None) {
            OnPopupKey(wParam);
        } else if (searchFocus_) {
            OnSearchKey(wParam);
        } else if (termFocus_) {
            OnTermKey(wParam);
        }
        return 0;
    case WM_CHAR:
        if (renaming_ != Rename::None) {
            OnRenameChar(static_cast<wchar_t>(wParam));
        } else if (popup_ != Popup::None) {
            OnPopupChar(static_cast<wchar_t>(wParam));
        } else if (searchFocus_) {
            OnSearchChar(static_cast<wchar_t>(wParam));
        } else if (termFocus_) {
            OnTermChar(static_cast<wchar_t>(wParam));
        }
        return 0;
    case WM_SYSCHAR:  // Alt+key in the shell
        if (termFocus_ && popup_ == Popup::None && Tab() != nullptr && !Tab()->exited) {
            TermSend("\x1b" + ToUtf8(std::wstring(1, static_cast<wchar_t>(wParam))));
            return 0;
        }
        break;
    case WM_SETFOCUS:
        if (popup_ == Popup::None && !searchFocus_ && !termFocus_ && renaming_ == Rename::None) {  // with a panel open, the search or the shell active the keys stay here
            SetFocus(input_);
        }
        return 0;
    case WM_TIMER:
        if (wParam == kStatusTimer) {
            KillTimer(hwnd_, kStatusTimer);
            status_.clear();
            Refresh();
        } else if (wParam == kPreviewTimer) {
            CheckPreview();
        } else if (wParam == kUpdateTimer) {
            CheckAppUpdate();
        } else if (wParam == kTipTimer) {
            KillTimer(hwnd_, kTipTimer);
            if (!tipText_.empty() && popup_ == Popup::None && dragging_ == Target::None && !feedSelecting_ &&
                !selecting_) {
                tipAt_ = mouse_;
                ShowTip();
            }
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
        if (webController_) {
            webController_->Close();  // the browser processes go with the window
            webController_.reset();
            webView_.reset();
        }
        authCancel_ = true;  // a sign-in in progress ends at once
        if (authThread_.joinable()) {
            authThread_.join();
        }
        setupCancel_ = true;  // so does an install or an update
        if (setupThread_.joinable()) {
            setupThread_.join();
        }
        if (versionThread_.joinable()) {
            versionThread_.join();
        }
        cloneCancel_ = true;
        if (cloneThread_.joinable()) {
            cloneThread_.join();
        }
        if (reposThread_.joinable()) {
            reposThread_.join();
        }
        ++generation_;
        tabs_.clear();  // every shell ends
        host_.CancelAll();
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
    bool keepFocus = false;  // the secrets view's, the search's and the terminal's own clicks keep focus where they put it
    if (hit.target != Target::SidebarSearch) {
        searchFocus_ = false;
    }
    termFocus_ = false;  // a click anywhere but into the screen (those never get here) ends typing into the shell
    if (popup_ != Popup::None) {
        switch (hit.target) {
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
        const bool sameButton = (hit.target == Target::ModelChip && was == Popup::Models) ||
                                (hit.target == Target::EffortChip && was == Popup::Effort) ||
                                (hit.target == Target::UsageChip && was == Popup::Usage);
        if (sameButton) {
            return;
        }
    }
    switch (hit.target) {
    case Target::SidebarToggle:
        SetSidebarOpen(!sidebarOpen_);
        break;
    case Target::WinMin:
        ShowWindow(hwnd_, SW_MINIMIZE);
        return;
    case Target::WinMax:
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        return;
    case Target::WinClose:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    case Target::SignIn:
        SignIn();
        break;
    case Target::InstallClaude:
        InstallClaudeCli();
        break;
    case Target::CancelInstall:
        CancelSetup();
        break;
    case Target::CancelLogin:
        CancelSignIn();
        break;
    case Target::CopyLoginUrl:
        if (SetClipboardText(hwnd_, ToWide(loginUrl_))) {
            Flash(L"Ссылка скопирована");
        }
        break;
    case Target::Account:
        if (SignedOut()) {
            SignIn();
        } else {
            ShowAccountMenu();
        }
        break;
    case Target::TerminalToggle:
        SetTerminalOpen(!terminalOpen_);
        keepFocus = true;
        break;
    case Target::TermClose:
        SetTerminalOpen(false);
        break;
    case Target::PreviewWide:
        SetPreviewWide(!previewWide_);
        break;
    case Target::Update:
        StartUpdate();
        break;
    case Target::TermTab:
        SelectTerminalTab(hit.row);
        keepFocus = true;
        break;
    case Target::TermTabClose:
        CloseTerminalTab(hit.row);
        keepFocus = terminalOpen_;
        if (terminalOpen_) {
            SelectTerminalTab(activeTab_);
        }
        break;
    case Target::TermAdd:
        AddTerminalTab();
        SyncTerminalSize();
        SelectTerminalTab(activeTab_);
        keepFocus = true;
        break;
    case Target::SidebarSearch:
        searchFocus_ = true;
        SetFocus(hwnd_);
        keepFocus = true;
        Refresh();
        break;
    case Target::HistoryRow:
        OpenSession(hit.row);
        break;
    case Target::ModelChip:
        OpenPopup(Popup::Models);
        break;
    case Target::EffortChip:
        OpenPopup(Popup::Effort);
        break;
    case Target::UsageChip:
        OpenPopup(Popup::Usage);
        break;
    case Target::FolderChip:
        ShowFolderMenu();
        keepFocus = true;
        break;
    case Target::Secrets:
        SetSecretsOpen(true);
        keepFocus = true;
        break;
    case Target::SecretsClose:
        SetSecretsOpen(false);
        break;
    case Target::SecretRow: {  // edit a secret: its name and value go into the form
        const std::vector<std::string> names = secrets_.Names();
        if (hit.row < names.size()) {
            EditSecret(names[hit.row]);
            keepFocus = true;
        }
        break;
    }
    case Target::SecretEye:
        SetShowValue(!showValue_);
        SetFocus(valueEdit_);
        keepFocus = true;
        break;
    case Target::SecretCancel:
        CancelSecretEdit();
        keepFocus = true;
        break;
    case Target::SecretDelete: {
        const std::vector<std::string> names = secrets_.Names();
        if (hit.row < names.size()) {
            DeleteSecret(names[hit.row]);
        }
        keepFocus = true;
        break;
    }
    case Target::SecretSave:
        SubmitSecret();
        keepFocus = true;
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
    case Target::ApprovalAllow:
        Decide(hit.row, true);
        break;
    case Target::ApprovalDeny:
        Decide(hit.row, false);
        break;
    case Target::CardPreview:
        OpenPreview(WrittenFile(feed_[hit.row], workspace_));
        return;
    case Target::CardOpen:
        OpenFile(WrittenFile(feed_[hit.row], workspace_));
        break;
    case Target::CardReveal:
        if (const std::wstring file = ExistingPath(WrittenFile(feed_[hit.row], workspace_)); !file.empty()) {
            RevealInFolder(file);
        } else {
            Flash(L"Файла больше нет", 3000);
        }
        break;
    case Target::FeedImage:
        OpenViewer(hit.row, hit.sub);
        return;
    case Target::Send:
        if (busy_ && !hasContent()) {
            StopTurn();
            Flash(L"останавливаю…");
        } else {
            Submit();  // also while Claude works: the message joins the running turn
        }
        break;
    default:
        break;
    }
    if (popup_ == Popup::None && !keepFocus) {
        SetFocus(input_);
    }
}

void ChatWindow::OnPopupKey(WPARAM key) {
    if (key == VK_ESCAPE) {
        ClosePopup();
        return;
    }
    switch (popup_) {
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
    case Popup::Image:
        if (key == VK_LEFT || key == VK_RIGHT) {
            StepViewer(key == VK_RIGHT ? 1 : -1);
        } else if (key == 'C' && GetKeyState(VK_CONTROL) < 0) {
            CopyImage(viewer_.path);
        } else if (key == VK_RETURN) {
            OpenFile(viewer_.path);
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
}

void ChatWindow::OnCommand(WPARAM wParam, LPARAM lParam) {
    if (const auto from = reinterpret_cast<HWND>(lParam); from == nameEdit_ || from == valueEdit_) {
        if (HIWORD(wParam) == EN_CHANGE) {
            Refresh();  // the button says "add" or "update" by the name
        }
        return;
    }
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
            searchFocus_ = false;
            termFocus_ = false;
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
            StopTurn();
            Flash(L"останавливаю…");
            return 1;
        }
        if (filter.wParam == 'N' && control) {
            NewChat();
            return 1;
        }
        if (filter.wParam == 'H' && control) {
            FocusSearch();
            return 1;
        }
        if ((filter.wParam == 'C' || filter.wParam == VK_INSERT) && control) {
            CHARRANGE own{};  // the input's own selection comes first
            SendMessageW(input_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&own));
            if (own.cpMax == own.cpMin && CopyFeedSelection()) {
                return 1;
            }
        }
        if (filter.wParam == 'U' && control) {
            PickFiles();
            return 1;
        }
        if ((filter.wParam == 'V' && control) || (filter.wParam == VK_INSERT && shift)) {
            PasteClipboard();
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
    // The sidebar docks at the left and the chat takes what is left, as in Claude's app.
    const float sidebarWidth = sidebarOpen_ ? std::min(sidebarWidth_, std::max(0.f, g.width - MinChat())) : 0.f;
    g.areaLeft = sidebarWidth;
    const float termWidth =
        terminalOpen_ ? std::min(terminalWidth_, std::max(0.f, g.width - sidebarWidth - MinChat())) : 0.f;
    if (sidebarWidth > 0) {
        g.splitSidebar = D2D1::RectF(sidebarWidth - 3, kHeader, sidebarWidth + 4, g.height);
    }
    if (termWidth > 0) {
        g.splitTerminal = D2D1::RectF(g.width - termWidth - 4, kHeader, g.width - termWidth + 3, g.height);
    }
    g.area = g.width - termWidth;
    g.column = std::max(0.f, std::min(g.area - g.areaLeft - 2 * kSide, kColumnMax));
    const float left = g.areaLeft + (g.area - g.areaLeft - g.column) / 2;
    g.left = left;
    const float right = left + g.column;

    const float buttonTop = (kHeader - kButton) / 2;
    g.winClose = D2D1::RectF(g.width - kWinButton, 0, g.width, kHeader);
    g.winMax = D2D1::RectF(g.winClose.left - kWinButton, 0, g.winClose.left, kHeader);
    g.winMin = D2D1::RectF(g.winMax.left - kWinButton, 0, g.winMax.left, kHeader);
    g.terminalBtn = D2D1::RectF(g.winMin.left - 10 - kButton, buttonTop, g.winMin.left - 10, buttonTop + kButton);
    g.sideToggle = D2D1::RectF(10, buttonTop, 10 + kButton, buttonTop + kButton);
    if (update_) {
        const float width = TextWidth(UpdateLabel(), ui_.get()) + 22.f;
        g.update = D2D1::RectF(g.terminalBtn.left - 8 - width, buttonTop, g.terminalBtn.left - 8, buttonTop + kButton);
    }

    // As in Claude's own app: the box holds "+", the text and send; the model, its effort and the
    // usage ring sit in a row under it, at the right.
    const float chipsBottom = g.height - kChipsBottom;
    const float chipsTop = chipsBottom - kChipRow;
    const float boxBottom = chipsTop - 4;
    const float attachmentsHeight = attachments_.empty() ? 0.f : kAttachmentRow + 8;
    const float rowHeight = std::max(textHeight_, kInputButton);
    const float boxTop = boxBottom - (2 * kBoxPadY + attachmentsHeight + rowHeight);
    g.box = D2D1::RectF(left, boxTop, right, boxBottom);
    g.attachments = D2D1::RectF(left + 12, boxTop + kBoxPadY, right - 12, boxTop + kBoxPadY + kAttachmentRow);
    const float rowTop = boxTop + kBoxPadY + attachmentsHeight;
    const float rowBottom = rowTop + rowHeight;
    g.attach = D2D1::RectF(left + 8, rowBottom - kInputButton, left + 8 + kInputButton, rowBottom);
    g.send = D2D1::RectF(right - 8 - kInputButton, rowBottom - kInputButton, right - 8, rowBottom);
    const float textTop = rowTop + (rowHeight - textHeight_) / 2;
    g.text = D2D1::RectF(g.attach.right + 4, textTop, g.send.left - 4, textTop + textHeight_);

    const float ringTop = chipsTop + (kChipRow - kRing) / 2;
    g.usage = D2D1::RectF(right - 6 - kRing, ringTop, right - 6, ringTop + kRing);
    const float chipTop = chipsTop + 2;
    const float chipBottom = chipsBottom - 2;
    float chipsRight = g.usage.left - 6;
    if (!EffortLevels().empty()) {
        const float width = TextWidth(EffortName(settings_.effort), ui_.get()) + 18;
        g.effort = D2D1::RectF(chipsRight - width, chipTop, chipsRight, chipBottom);
        chipsRight = g.effort.left - 2;
    }
    const float modelWidth = TextWidth(ModelName(), ui_.get()) + 18;
    g.model = D2D1::RectF(chipsRight - modelWidth, chipTop, chipsRight, chipBottom);
    const float folderWidth = std::min(TextWidth(FolderLabel(), ui_.get()) + 34.f, std::max(0.f, g.model.left - left - 16.f));
    g.folder = D2D1::RectF(left + 4, chipTop, left + 4 + folderWidth, chipBottom);
    g.feed = D2D1::RectF(g.areaLeft, kHeader, g.area, boxTop - 8);
    {
        const float centerX = left + g.column / 2;
        const float middle = (g.feed.top + g.feed.bottom) / 2;
        g.signIn = D2D1::RectF(centerX - 90, middle + 28, centerX + 90, middle + 28 + 38);
        g.signAlt = D2D1::RectF(centerX - 90, g.signIn.bottom + 10, centerX + 90, g.signIn.bottom + 10 + 34);
    }

    if (sidebarWidth > 0) {
        g.sidebar = D2D1::RectF(0, kHeader, sidebarWidth, g.height);
        g.search = D2D1::RectF(10, kHeader + 10, sidebarWidth - 10, kHeader + 10 + kFieldHeight);
        g.newItem = D2D1::RectF(6, g.search.bottom + 8, sidebarWidth - 6, g.search.bottom + 8 + kNavRow);
        g.secrets = D2D1::RectF(6, g.newItem.bottom + 2, sidebarWidth - 6, g.newItem.bottom + 2 + kNavRow);
        g.account = D2D1::RectF(6, g.height - 6 - 34, sidebarWidth - 6, g.height - 6);
        g.listTop = g.secrets.bottom + 8 + 26;  // under the "chats" label
        g.listBottom = g.account.top - 6;
    }
    if (termWidth > 0) {
        g.term = D2D1::RectF(g.area, kHeader, g.width, g.height);
        g.termClose = D2D1::RectF(g.term.right - 12 - 28, g.term.top + 8, g.term.right - 12, g.term.top + 36);
        g.termScreen = D2D1::RectF(g.term.left + 14, g.term.top + 44, g.term.right - 8, g.term.bottom - 12);
        float tabLeft = g.term.left + 10;
        for (const auto& tab : tabs_) {
            const float width = TextWidth(TabLabel(*tab), ui_.get()) + 16 + 20;
            g.termTabs.push_back(D2D1::RectF(tabLeft, g.term.top + 8, tabLeft + width, g.term.top + 36));
            tabLeft += width + 4;
        }
        g.termAdd = D2D1::RectF(tabLeft, g.term.top + 8, tabLeft + 28, g.term.top + 36);
        if (PreviewShown() || previewWide_) {
            g.termWide = D2D1::RectF(g.termClose.left - 4 - 28, g.termClose.top, g.termClose.left - 4, g.termClose.bottom);
        }
    }
    if (secretsOpen_ && sidebarWidth > 0) {
        g.panel = g.sidebar;
        g.panelClose = D2D1::RectF(g.panel.right - 12 - 28, g.panel.top + 10, g.panel.right - 12, g.panel.top + 38);
        const float fieldLeft = g.panel.left + kPanelPad;
        const float fieldRight = g.panel.right - kPanelPad;
        g.saveButton = D2D1::RectF(fieldLeft, g.panel.bottom - kPanelPad - kFieldHeight, fieldRight,
                                   g.panel.bottom - kPanelPad);
        g.valueField = D2D1::RectF(fieldLeft, g.saveButton.top - 8 - kFieldHeight, fieldRight, g.saveButton.top - 8);
        g.nameField = D2D1::RectF(fieldLeft, g.valueField.top - 8 - kFieldHeight, fieldRight, g.valueField.top - 8);
        g.secretEye = D2D1::RectF(g.valueField.right - 34, g.valueField.top + 3, g.valueField.right - 4, g.valueField.bottom - 3);
        if (!editingSecret_.empty()) {  // "save" and "cancel" side by side
            const float split = fieldLeft + (fieldRight - fieldLeft) * 0.6f;
            g.secretCancel = D2D1::RectF(split + 4, g.saveButton.top, fieldRight, g.saveButton.bottom);
            g.saveButton.right = split - 4;
        }
        g.secretsTop = g.panel.top + 46 + kSecretsNote;
    }

    switch (popup_) {
    case Popup::Models: {
        const float rows = static_cast<float>(mainModels_.size());
        const float height = 12 + rows * kMenuRow + (moreModels_.empty() ? 0 : 9 + kMenuRow);
        const float popupLeft = ClampTo(g.model.left, 10.f, g.area - 10 - kModelsWidth);
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
        const float popupRight = ClampTo(g.effort.right, 10.f + kEffortWidth, g.area - 10);
        g.popup = D2D1::RectF(popupRight - kEffortWidth, g.box.top - 6 - kEffortHeight, popupRight, g.box.top - 6);
        g.track = D2D1::RectF(g.popup.left + 14, g.popup.top + 62, g.popup.right - 14, g.popup.top + 86);
        const float resetWidth = TextWidth(L"по умолчанию", uiRight_.get());
        g.reset = D2D1::RectF(g.popup.right - 14 - resetWidth, g.popup.top + 10, g.popup.right - 14,
                              g.popup.top + 34);
        break;
    }
    case Popup::Usage: {
        const float popupRight = ClampTo(g.usage.right + 4, 10.f + kUsageWidth, g.area - 10);
        const float height = 14 + 24 + 2 * kUsageRow + 4;
        // Above the box, like the other panels: the input's own window would hide whatever lay over it.
        g.popup = D2D1::RectF(popupRight - kUsageWidth, g.box.top - 6 - height, popupRight, g.box.top - 6);
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
    case Popup::Usage:
        if (Inside(g.popup, x, y)) {
            return {Target::InsidePopup};
        }
        break;
    case Popup::Image:
        return {};  // over everything: a click anywhere closes it
    case Popup::None:
        break;
    }
    if (Inside(g.splitSidebar, x, y)) {
        return {Target::SplitSidebar};
    }
    if (Inside(g.splitTerminal, x, y)) {
        return {Target::SplitTerminal};
    }
    if (secretsOpen_ && Inside(g.panel, x, y)) {  // the sidebar in its secrets view
        if (Inside(g.panelClose, x, y)) {
            return {Target::SecretsClose};
        }
        if (Inside(g.saveButton, x, y)) {
            return {Target::SecretSave};
        }
        if (Inside(g.secretCancel, x, y)) {
            return {Target::SecretCancel};
        }
        if (Inside(g.secretEye, x, y)) {
            return {Target::SecretEye};
        }
        const size_t total = secrets_.Names().size();
        const size_t first = ClampFirst(secretsFirst_, total, SecretRowsFit(g));
        const size_t shown = std::min(total - first, SecretRowsFit(g));
        for (size_t i = 0; i < shown; ++i) {
            const D2D1_RECT_F row = SecretRowRect(g, i);
            if (Inside(SecretDeleteRect(row), x, y)) {
                return {Target::SecretDelete, first + i};
            }
            if (Inside(row, x, y)) {
                return {Target::SecretRow, first + i};
            }
        }
        return {};
    }
    if (Inside(g.sidebar, x, y)) {
        if (Inside(g.search, x, y)) {
            return {Target::SidebarSearch};
        }
        if (Inside(g.newItem, x, y)) {
            return {Target::NewChat};
        }
        if (Inside(g.account, x, y)) {
            return {Target::Account};
        }
        if (Inside(g.secrets, x, y)) {
            return {Target::Secrets};
        }
        if (y >= g.listTop && y < g.listBottom) {
            const auto row = firstRow_ + static_cast<size_t>((y - g.listTop) / kHistoryRow);
            if (row < filtered_.size() && row < firstRow_ + SidebarRows(g)) {
                return {Target::HistoryRow, row};
            }
        }
        return {};
    }
    if (Inside(g.term, x, y)) {
        if (Inside(g.termClose, x, y)) {
            return {Target::TermClose};
        }
        if (Inside(g.termWide, x, y)) {
            return {Target::PreviewWide};
        }
        for (size_t i = 0; i < g.termTabs.size(); ++i) {
            if (Inside(TabCloseRect(g.termTabs[i]), x, y)) {
                return {Target::TermTabClose, i};
            }
            if (Inside(g.termTabs[i], x, y)) {
                return {Target::TermTab, i};
            }
        }
        if (Inside(g.termAdd, x, y)) {
            return {Target::TermAdd};
        }
        return Inside(g.termScreen, x, y) ? Hit{Target::TermScreen} : Hit{};
    }
    if (Inside(g.winClose, x, y)) {
        return {Target::WinClose};
    }
    if (Inside(g.winMax, x, y)) {
        return {Target::WinMax};
    }
    if (Inside(g.winMin, x, y)) {
        return {Target::WinMin};
    }
    if (Inside(g.sideToggle, x, y)) {
        return {Target::SidebarToggle};
    }
    if (Inside(g.terminalBtn, x, y)) {
        return {Target::TerminalToggle};
    }
    if (Inside(g.update, x, y)) {
        return {Target::Update};
    }
    if (claudeMissing_ && !HasConversation() && Inside(g.signIn, x, y)) {
        return {setupBusy_ ? Target::CancelInstall : Target::InstallClaude};
    }
    if (SignedOut() && !HasConversation()) {
        if (Inside(g.signIn, x, y)) {
            return {loginWaiting_ ? Target::CancelLogin : Target::SignIn};
        }
        if (loginWaiting_ && !loginUrl_.empty() && Inside(g.signAlt, x, y)) {
            return {Target::CopyLoginUrl};
        }
    }
    if (Inside(g.feed, x, y)) {
        for (size_t i = 0; i < feed_.size(); ++i) {
            const FeedItem& item = feed_[i];
            if (y < item.top || y >= item.top + item.height) {
                continue;
            }
            for (size_t k = 0; k < item.images.size(); ++k) {
                if (Inside(ImageRect(g, item, k), x, y)) {
                    return {Target::FeedImage, i, k};
                }
            }
            if (HasFileActions(item)) {
                D2D1_RECT_F preview{}, open{}, reveal{};
                ApprovalFileButtons(g, item, preview, open, reveal);
                if (Inside(preview, x, y)) {
                    return {Target::CardPreview, i};
                }
                if (Inside(open, x, y)) {
                    return {Target::CardOpen, i};
                }
                if (Inside(reveal, x, y)) {
                    return {Target::CardReveal, i};
                }
            }
            if (item.kind != Kind::Approval || item.verdict != Verdict::Pending) {
                continue;
            }
            D2D1_RECT_F allow{}, deny{};
            ApprovalButtons(g, item, allow, deny);
            if (Inside(allow, x, y)) {
                return {Target::ApprovalAllow, i};
            }
            if (Inside(deny, x, y)) {
                return {Target::ApprovalDeny, i};
            }
        }
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
    if (Inside(g.folder, x, y)) {
        return {Target::FolderChip};
    }
    if (Inside(g.effort, x, y)) {
        return {Target::EffortChip};
    }
    if (Inside(g.usage, x, y)) {
        return {Target::UsageChip};
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
    fieldBrush_.reset(CreateSolidBrush(ToColorRef(kRaised)));
    nameEdit_ = CreateSecretField(false, L"Имя, например GITHUB");
    valueEdit_ = CreateSecretField(true, L"Значение");
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
    PlaceSecretFields();
    SyncTerminalSize();
    PlacePreview();
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
    ++targetGeneration_;
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

void ChatWindow::Check(D2D1_POINT_2F c, uint32_t color) {
    Line({c.x - 5.f, c.y}, {c.x - 1.5f, c.y + 3.5f}, color, 1.6f);
    Line({c.x - 1.5f, c.y + 3.5f}, {c.x + 5.f, c.y - 3.5f}, color, 1.6f);
}

void ChatWindow::MeasureFeed(float column) {
    for (FeedItem& item : feed_) {
        float width = item.kind == Kind::User ? column * 0.85f - 2 * kBubblePad : column;
        if (item.kind == Kind::Approval) {
            width = column - 2 * kCardPad;
        }
        if (item.layout && item.layoutWidth == width) {
            if (item.kind == Kind::Approval) {
                item.height = ApprovalHeight(item);  // the buttons go once it is decided
            } else if (item.kind == Kind::User) {
                const float pictures = LayoutImages(item, column);  // the same: the column may have changed
                item.bubbleTop = pictures > 0 && !item.text.empty() ? pictures + kImageGap : pictures;
                item.height = item.bubbleTop + (item.text.empty() ? 0.f : item.layoutHeight + 2 * kBubblePad);
            }
            continue;
        }
        IDWriteTextFormat* format = item.kind == Kind::Notice ? notice_.get() : body_.get();
        if (item.kind == Kind::Approval) {
            format = mono_.get();
        }
        item.layout.reset();
        std::wstring shown;  // an assistant message without its markup
        const std::wstring* source = &item.text;
        if (item.kind == Kind::Assistant) {
            MdText md = ParseMarkdown(item.text);
            shown = std::move(md.text);
            item.spans = std::move(md.spans);
            source = &shown;
        } else if (item.kind == Kind::Approval && !item.output.empty()) {
            shown = item.text + L"\n\n";
            item.outputStart = shown.size();
            shown += OutputPreview(item.output);
            source = &shown;
        }
        if (FAILED(dwrite_->CreateTextLayout(source->c_str(), static_cast<UINT32>(source->size()), format,
                                             std::max(width, 1.f), 1e6f, item.layout.put()))) {
            item.height = 0;
            continue;
        }
        if (item.kind == Kind::Assistant) {
            StyleMarkdown(item);
        }
        item.shown = *source;
        item.layoutWidth = width;
        DWRITE_TEXT_METRICS metrics{};
        item.layout->GetMetrics(&metrics);
        item.textWidth = metrics.widthIncludingTrailingWhitespace;
        item.layoutHeight = metrics.height;
        item.height = item.kind == Kind::Approval ? ApprovalHeight(item) : metrics.height;
        if (item.kind == Kind::User) {  // the pictures, then the bubble (none for pictures alone)
            const float pictures = LayoutImages(item, column);
            item.bubbleTop = pictures > 0 && !item.text.empty() ? pictures + kImageGap : pictures;
            item.height = item.bubbleTop + (item.text.empty() ? 0.f : metrics.height + 2 * kBubblePad);
        }
    }
}

float ChatWindow::LayoutImages(FeedItem& item, float column) {
    // Side by side while they fit in the bubble's room, rows right-aligned like the bubble.
    const float room = column * 0.85f;
    float top = 0;
    size_t first = 0;
    while (first < item.images.size()) {
        float rowWidth = 0;
        float rowHeight = 0;
        size_t end = first;
        for (; end < item.images.size(); ++end) {
            const FeedImage& image = item.images[end];
            const float scale = std::min(1.f, room / std::max(image.width, 1.f));
            const float width = image.width * scale;
            if (end > first && rowWidth + kImageGap + width > room) {
                break;
            }
            rowWidth += (end > first ? kImageGap : 0.f) + width;
            rowHeight = std::max(rowHeight, image.height * scale);
        }
        float x = column - rowWidth;
        for (size_t i = first; i < end; ++i) {
            FeedImage& image = item.images[i];
            const float scale = std::min(1.f, room / std::max(image.width, 1.f));
            image.at = D2D1::RectF(x, top + rowHeight - image.height * scale, x + image.width * scale, top + rowHeight);
            x = image.at.right + kImageGap;
        }
        top += rowHeight + kImageGap;
        first = end;
    }
    return top > 0 ? top - kImageGap : 0.f;
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
    PaintWindowButtons(g);
    PaintInput(g);
    PaintSidebar(g);
    PaintTerminal(g);
    for (const Target split : {Target::SplitSidebar, Target::SplitTerminal}) {  // the edge lights up to be dragged
        if (hover_.target == split || dragging_ == split) {
            const float x = split == Target::SplitSidebar ? g.areaLeft : g.area;
            Fill(D2D1::RectF(x - 1.f, kHeader, x + 1.f, g.height), 0.f, kAccent);
        }
    }
    switch (popup_) {
    case Popup::Models:
        PaintModels(g);
        break;
    case Popup::Effort:
        PaintEffort(g);
        break;
    case Popup::Usage:
        PaintUsage(g);
        break;
    case Popup::Image:
        PaintViewer(g);
        break;
    case Popup::None:
        break;
    }
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        target_.reset();
        brush_.reset();
    }
}

void ChatWindow::PaintFeed(const Geometry& g) {
    const float left = g.left;
    if (claudeMissing_ && !HasConversation()) {
        PaintInstall(g);
    } else if (SignedOut() && !HasConversation()) {
        PaintSignIn(g);
    } else if (!HasConversation()) {
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
        if (visible && item.kind == Kind::Approval) {
            PaintApproval(g, item, left);
        } else if (visible && !(item.kind == Kind::Notice && !HasConversation())) {
            uint32_t color = kPrimary;
            float x = left;
            float textY = y;
            if (item.kind == Kind::User) {
                PaintImages(g, item);
                const float width = item.textWidth + 2 * kBubblePad;
                x = left + g.column - width;
                if (!item.text.empty()) {
                    Fill(D2D1::RectF(x, y + item.bubbleTop, x + width, y + item.height), 10.f, kUserBubble);
                }
                x += kBubblePad;
                textY = y + item.bubbleTop + kBubblePad;
            } else if (item.kind == Kind::Notice) {
                color = kSecondary;
            } else if (item.kind == Kind::Error) {
                color = kDanger;
            }
            if (item.kind == Kind::Assistant) {
                PaintMarkdown(item, x, y, left, g.column);
            }
            PaintFeedSelection(static_cast<size_t>(&item - feed_.data()), {x, textY});
            brush_->SetColor(Color(color));
            target_->DrawTextLayout({x, textY}, item.layout.get(), brush_.get());
        }
        y += item.height + kGap;
    }
    if (maxScroll_ > 0) {  // a thin scroll thumb on the right edge
        const float track = g.feed.bottom - g.feed.top;
        const float thumb = std::max(24.f, track * track / (track + maxScroll_));
        const float top = g.feed.top + (track - thumb) * (scroll_ / maxScroll_);
        Fill(D2D1::RectF(g.area - 7.f, top, g.area - 3.f, top + thumb), 2.f, kLine);
    }
    target_->PopAxisAlignedClip();
}

void ChatWindow::StyleMarkdown(FeedItem& item) {
    IDWriteTextLayout* layout = item.layout.get();
    for (const MdSpan& span : item.spans) {
        const DWRITE_TEXT_RANGE range{static_cast<UINT32>(span.start), static_cast<UINT32>(span.length)};
        if ((span.flags & kMdBold) != 0) {
            layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
        }
        if ((span.flags & kMdItalic) != 0) {
            layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        }
        if ((span.flags & kMdLink) != 0) {
            layout->SetUnderline(TRUE, range);
        }
        if ((span.flags & kMdHeading) != 0) {
            layout->SetFontSize(15.f * span.scale, range);
        }
        if ((span.flags & kMdCode) != 0) {
            if (fonts_) {
                layout->SetFontCollection(fonts_.get(), range);
            }
            layout->SetFontFamilyName(kMonoFont, range);
            layout->SetFontSize(13.5f, range);
        }
    }
}

// Colors for links, quotes and code, and the panels under code; the text itself is drawn by the caller.
void ChatWindow::PaintMarkdown(const FeedItem& item, float x, float y, float left, float column) {
    constexpr uint32_t kCodeText = 0xdadde4;
    wil::com_ptr<ID2D1SolidColorBrush> link, quote, code;
    target_->CreateSolidColorBrush(Color(kAccent), link.put());
    target_->CreateSolidColorBrush(Color(kSecondary), quote.put());
    target_->CreateSolidColorBrush(Color(kCodeText), code.put());
    std::array<wil::com_ptr<ID2D1SolidColorBrush>, 8> tokens;  // by TokenKind, made when first needed
    const auto tokenBrush = [&](TokenKind kind) {
        constexpr std::array<uint32_t, 8> colors = {kCodeText, 0x7f8794, 0x9ccc8a, 0xe5b567,
                                                    0xc792ea,  0x7fbfe6, 0x7fd48a, 0xf07178};
        wil::com_ptr<ID2D1SolidColorBrush>& brush = tokens[static_cast<size_t>(kind)];
        if (!brush) {
            target_->CreateSolidColorBrush(Color(colors[static_cast<size_t>(kind)]), brush.put());
        }
        return brush.get();
    };
    for (const MdSpan& span : item.spans) {
        const DWRITE_TEXT_RANGE range{static_cast<UINT32>(span.start), static_cast<UINT32>(span.length)};
        if ((span.flags & kMdToken) != 0) {  // after its block's span: the color of one part of the code
            item.layout->SetDrawingEffect(tokenBrush(span.token), range);
            continue;
        }
        if ((span.flags & kMdLink) != 0) {
            item.layout->SetDrawingEffect(link.get(), range);
        }
        if ((span.flags & kMdQuote) != 0) {
            item.layout->SetDrawingEffect(quote.get(), range);
        }
        if ((span.flags & kMdCode) == 0) {
            continue;
        }
        item.layout->SetDrawingEffect(code.get(), range);
        UINT32 count = 0;
        item.layout->HitTestTextRange(range.startPosition, range.length, 0, 0, nullptr, 0, &count);
        std::vector<DWRITE_HIT_TEST_METRICS> boxes(count);
        if (count == 0 || FAILED(item.layout->HitTestTextRange(range.startPosition, range.length, 0, 0, boxes.data(),
                                                               count, &count))) {
            continue;
        }
        if (span.block) {  // one panel behind all its lines
            float top = boxes.front().top;
            float bottom = top;
            for (const DWRITE_HIT_TEST_METRICS& box : boxes) {
                top = std::min(top, box.top);
                bottom = std::max(bottom, box.top + box.height);
            }
            const D2D1_RECT_F panel = D2D1::RectF(left - 8.f, y + top - 6.f, left + column + 8.f, y + bottom + 6.f);
            Fill(panel, 8.f, kCard);
            brush_->SetColor(Color(kLine));
            target_->DrawRoundedRectangle(D2D1::RoundedRect(panel, 8.f, 8.f), brush_.get(), 1.f);
        } else {
            for (const DWRITE_HIT_TEST_METRICS& box : boxes) {
                Fill(D2D1::RectF(x + box.left - 2.f, y + box.top, x + box.left + box.width + 2.f, y + box.top + box.height),
                     4.f, kRaised);
            }
        }
    }
}

float ChatWindow::ApprovalHeight(const FeedItem& item) {
    if (!item.layout) {
        return 0;
    }
    DWRITE_TEXT_METRICS metrics{};
    item.layout->GetMetrics(&metrics);
    const float buttons = item.verdict == Verdict::Pending || HasFileActions(item) ? kCardPad + kCardButton : 0.f;
    return kCardPad + kCardTitle + 4.f + metrics.height + buttons + kCardPad;
}

void ChatWindow::ApprovalButtons(const Geometry& g, const FeedItem& item, D2D1_RECT_F& allow, D2D1_RECT_F& deny) {
    const float left = g.left + kCardPad;
    const float bottom = item.top + item.height - kCardPad;
    allow = D2D1::RectF(left, bottom - kCardButton, left + kCardButtonWidth, bottom);
    deny = D2D1::RectF(allow.right + 8.f, allow.top, allow.right + 8.f + kCardButtonWidth, bottom);
}

void ChatWindow::ApprovalFileButtons(const Geometry& g, const FeedItem& item, D2D1_RECT_F& preview, D2D1_RECT_F& open,
                                     D2D1_RECT_F& reveal) {
    D2D1_RECT_F first{}, second{};
    ApprovalButtons(g, item, first, second);  // where "allow" and "deny" were
    if (!IsPreviewable(ToWide(JsonStr(item.input, "path")))) {
        preview = {};
        open = first;
        reveal = second;
        return;
    }
    const float step = second.left - first.left;
    preview = first;
    open = second;
    reveal = D2D1::RectF(second.left + step, second.top, second.right + step, second.bottom);
}

void ChatWindow::PaintApproval(const Geometry& g, const FeedItem& item, float left) {
    const bool pending = item.verdict == Verdict::Pending;
    const D2D1_RECT_F card = D2D1::RectF(left, item.top, left + g.column, item.top + item.height);
    Fill(card, 10.f, kCard);
    brush_->SetColor(pending ? Color(kAccent, 0.6f) : Color(kLine));
    target_->DrawRoundedRectangle(D2D1::RoundedRect(card, 10.f, 10.f), brush_.get(), 1.f);

    const float inner = left + kCardPad;
    const float top = item.top + kCardPad;
    Text(item.title, uiBold_.get(), D2D1::RectF(inner, top, card.right - kCardPad - 110.f, top + kCardTitle), kPrimary);
    if (!pending) {
        std::wstring label;
        uint32_t labelColor = kDanger;
        if (item.verdict == Verdict::Allowed) {
            label = !item.done ? L"Выполняется…" : (item.outputError ? L"Ошибка" : L"Готово");
            labelColor = item.done && item.outputError ? kDanger : kSecondary;
        } else {
            label = item.verdict == Verdict::Denied ? L"Отклонено" : L"Отменено";
        }
        Text(label, uiRight_.get(), D2D1::RectF(card.right - kCardPad - 110.f, top, card.right - kCardPad, top + kCardTitle),
             labelColor);
    }
    if (!item.output.empty() && item.layout) {  // the output under the call, in the terminal's colour (red if it failed)
        wil::com_ptr<ID2D1SolidColorBrush> outputBrush;
        target_->CreateSolidColorBrush(Color(item.outputError ? kDanger : kTermFg), outputBrush.put());
        item.layout->SetDrawingEffect(outputBrush.get(), DWRITE_TEXT_RANGE{static_cast<UINT32>(item.outputStart), 0x7fffffff});
    }
    PaintFeedSelection(static_cast<size_t>(&item - feed_.data()), {inner, top + kCardTitle + 4.f});
    brush_->SetColor(Color(pending ? kPrimary : kSecondary));
    target_->DrawTextLayout({inner, top + kCardTitle + 4.f}, item.layout.get(), brush_.get());

    if (pending) {
        D2D1_RECT_F allow{}, deny{};
        ApprovalButtons(g, item, allow, deny);
        const bool onItem = hover_.row < feed_.size() && &feed_[hover_.row] == &item;
        Fill(allow, 7.f, kAccent, onItem && hover_.target == Target::ApprovalAllow ? 0.85f : 1.f);
        Text(L"Разрешить", button_.get(), allow, kPrimary);
        Fill(deny, 7.f, onItem && hover_.target == Target::ApprovalDeny ? kLine : kRaised);
        Text(L"Отклонить", button_.get(), deny, kPrimary);
    } else if (HasFileActions(item)) {
        D2D1_RECT_F preview{}, open{}, reveal{};
        ApprovalFileButtons(g, item, preview, open, reveal);
        const bool onItem = hover_.row < feed_.size() && &feed_[hover_.row] == &item;
        if (preview.right > preview.left) {
            Fill(preview, 7.f, kAccent, onItem && hover_.target == Target::CardPreview ? 0.85f : 1.f);
            Text(L"Превью", button_.get(), preview, kPrimary);
        }
        Fill(open, 7.f, onItem && hover_.target == Target::CardOpen ? kLine : kRaised);
        Text(L"Открыть", button_.get(), open, kPrimary);
        Fill(reveal, 7.f, onItem && hover_.target == Target::CardReveal ? kLine : kRaised);
        Text(L"В папке", button_.get(), reveal, kPrimary);
    }
}

void ChatWindow::PaintHeader(const Geometry& g) {
    // No strip of its own, as in Claude's app: over the chat the header is the chat's background, over the side
    // columns it is their top, their edge line running up to the window's top.
    for (const D2D1_RECT_F& column : {g.sidebar, g.term}) {
        if (column.right <= column.left) {
            continue;
        }
        Fill(D2D1::RectF(column.left, 0, column.right, kHeader), 0.f, kPanel);
        const float edge = column.left == 0.f ? column.right : column.left;
        Line({edge, 0}, {edge, kHeader}, kLine, 1.f);
    }

    // The title starts where the chat does (as in Claude's app), or after the toggle when the sidebar is hidden.
    const float titleLeft = std::max(g.areaLeft + 16.f, g.sideToggle.right + 12.f);
    const std::wstring title = title_.empty() ? L"Новый чат" : title_;
    const float titleRight = (update_ ? g.update.left : g.terminalBtn.left) - 12;
    if (update_) {
        Fill(g.update, 6.f, kAccent, hover_.target == Target::Update ? 0.85f : 1.f);
        Text(updatePending_ ? L"Обновлю после ответа" : UpdateLabel(), button_.get(), g.update, kPrimary);
    }
    Text(title, header_.get(), D2D1::RectF(titleLeft, 0, titleRight, kHeader), kTitle);
    std::wstring state = busy_ ? L"отвечает…" : L"";
    if (!status_.empty()) {
        state = status_;
    }
    if (!state.empty()) {
        const float titleWidth = std::min(TextWidth(title, header_.get()), titleRight - titleLeft);
        Text(state, ui_.get(), D2D1::RectF(titleLeft + titleWidth + 12, 0, titleRight, kHeader), kSecondary);
    }

    for (const Target button : {Target::SidebarToggle, Target::TerminalToggle}) {
        const D2D1_RECT_F& r = button == Target::SidebarToggle ? g.sideToggle : g.terminalBtn;
        const bool active = hover_.target == button || (button == Target::SidebarToggle && sidebarOpen_) ||
                            (button == Target::TerminalToggle && terminalOpen_);
        if (active) {
            Fill(r, 6.f, kRaised);
        }
        const D2D1_POINT_2F c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        const uint32_t color = active ? kPrimary : kSecondary;
        if (button == Target::SidebarToggle) {  // a window with its left column marked
            brush_->SetColor(Color(color));
            target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.x - 8.f, c.y - 6.5f, c.x + 8.f, c.y + 6.5f), 2.5f, 2.5f),
                                          brush_.get(), 1.4f);
            Line({c.x - 3.f, c.y - 6.5f}, {c.x - 3.f, c.y + 6.5f}, color, 1.4f);
        } else {  // the terminal: a prompt, > and a cursor
            Line({c.x - 6.f, c.y - 4.f}, {c.x - 1.5f, c.y}, color, 1.5f);
            Line({c.x - 1.5f, c.y}, {c.x - 6.f, c.y + 4.f}, color, 1.5f);
            Line({c.x + 1.f, c.y + 5.f}, {c.x + 7.f, c.y + 5.f}, color, 1.5f);
        }
    }
}

// The window's edges and its title bar, which is the header: the top border resizes, the header's free
// space drags the window (a double click maximizes, a right click opens the system menu), its buttons are ours.
LRESULT ChatWindow::HitTestFrame(LPARAM lParam) {
    const LRESULT standard = DefWindowProcW(hwnd_, WM_NCHITTEST, 0, lParam);
    if (standard != HTCLIENT) {
        return standard;  // the side and bottom borders
    }
    POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(hwnd_, &at);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const UINT dpi = GetDpiForWindow(hwnd_);
    const int border = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    if (!IsZoomed(hwnd_) && at.y < border) {
        if (at.x < border * 2) {
            return HTTOPLEFT;
        }
        if (at.x >= client.right - border * 2) {
            return HTTOPRIGHT;
        }
        return HTTOP;
    }
    const float x = ToDip(at.x);
    const float y = ToDip(at.y);
    if (y < kHeader && HitTest(x, y).target == Target::None) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

// Minimize, maximize (restore when maximized), close: drawn like Windows 11's own.
void ChatWindow::PaintWindowButtons(const Geometry& g) {
    for (const Target button : {Target::WinMin, Target::WinMax, Target::WinClose}) {
        const D2D1_RECT_F& r = button == Target::WinMin ? g.winMin : (button == Target::WinMax ? g.winMax : g.winClose);
        const bool hovered = hover_.target == button;
        if (hovered) {
            Fill(r, 0.f, button == Target::WinClose ? 0xc42b1c : kRaised);
        }
        const D2D1_POINT_2F c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        const uint32_t color = hovered ? kPrimary : kSecondary;
        brush_->SetColor(Color(color));
        if (button == Target::WinMin) {
            Line({c.x - 5.f, c.y}, {c.x + 5.f, c.y}, color, 1.f);
        } else if (button == Target::WinMax) {
            if (IsZoomed(hwnd_)) {  // two windows: restore
                target_->DrawRectangle(D2D1::RectF(c.x - 5.f, c.y - 3.f, c.x + 3.f, c.y + 5.f), brush_.get(), 1.f);
                Line({c.x - 3.f, c.y - 5.f}, {c.x + 5.f, c.y - 5.f}, color, 1.f);
                Line({c.x + 5.f, c.y - 5.f}, {c.x + 5.f, c.y + 3.f}, color, 1.f);
            } else {
                target_->DrawRectangle(D2D1::RectF(c.x - 5.f, c.y - 5.f, c.x + 5.f, c.y + 5.f), brush_.get(), 1.f);
            }
        } else {
            Line({c.x - 5.f, c.y - 5.f}, {c.x + 5.f, c.y + 5.f}, color, 1.f);
            Line({c.x - 5.f, c.y + 5.f}, {c.x + 5.f, c.y - 5.f}, color, 1.f);
        }
    }
}

void ChatWindow::PaintInput(const Geometry& g) {
    Fill(g.box, 12.f, kCard);
    brush_->SetColor(inputFocused_ ? Color(kAccent, 0.75f) : Color(kLine));
    target_->DrawRoundedRectangle(D2D1::RoundedRect(g.box, 12.f, 12.f), brush_.get(), 1.f);
    if (ID2D1Bitmap* mascot = Bitmap(mascot_); mascot != nullptr && g.box.right - g.box.left > 200.f) {
        constexpr float kMascot = 32.f;  // on the box's top edge, near its right end
        const D2D1_RECT_F at = D2D1::RectF(g.box.right - 24.f - kMascot, g.box.top - kMascot + 3.f, g.box.right - 24.f,
                                           g.box.top + 3.f);
        target_->DrawBitmap(mascot, at, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

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

    // The model, then its effort in a box of its own, as in Claude's app.
    if (g.folder.right > g.folder.left) {  // the folder the chat works in: a menu to change it
        const bool active = hover_.target == Target::FolderChip;
        if (active) {
            Fill(g.folder, 6.f, kRaised);
        }
        const uint32_t color = active ? kPrimary : kSecondary;
        const D2D1_POINT_2F c{g.folder.left + 12.f, (g.folder.top + g.folder.bottom) / 2};
        brush_->SetColor(Color(color));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.x - 5.5f, c.y - 3.5f, c.x + 5.5f, c.y + 4.5f), 1.5f, 1.5f),
                                      brush_.get(), 1.2f);
        Line({c.x - 5.5f, c.y - 3.5f}, {c.x - 3.5f, c.y - 5.5f}, color, 1.2f);
        Line({c.x - 3.5f, c.y - 5.5f}, {c.x - 1.f, c.y - 5.5f}, color, 1.2f);
        Text(FolderLabel(), ui_.get(), D2D1::RectF(g.folder.left + 26, g.folder.top, g.folder.right - 6, g.folder.bottom),
             color);
    }
    const bool modelActive = hover_.target == Target::ModelChip || popup_ == Popup::Models;
    if (modelActive) {
        Fill(g.model, 6.f, kRaised);
    }
    Text(ModelName(), ui_.get(), D2D1::RectF(g.model.left + 9, g.model.top, g.model.right, g.model.bottom),
         modelActive ? kPrimary : kSecondary);
    if (g.effort.right > g.effort.left) {
        const bool effortActive = hover_.target == Target::EffortChip || popup_ == Popup::Effort;
        if (effortActive) {
            Fill(g.effort, 6.f, kRaised);
        }
        Text(EffortName(settings_.effort), ui_.get(),
             D2D1::RectF(g.effort.left + 9, g.effort.top, g.effort.right, g.effort.bottom),
             effortActive ? kPrimary : kSecondary);
    }

    {  // the plan's limits: a ring filled to the session's share; a click opens the details
        const bool active = hover_.target == Target::UsageChip || popup_ == Popup::Usage;
        if (active) {
            Fill(g.usage, 6.f, kRaised);
        }
        const D2D1_POINT_2F center{(g.usage.left + g.usage.right) / 2, (g.usage.top + g.usage.bottom) / 2};
        brush_->SetColor(Color(kLine));
        target_->DrawEllipse(D2D1::Ellipse(center, 7.5f, 7.5f), brush_.get(), 2.f);
        if (const Limit session = Current(session_); session.percent >= 0) {
            Ring(center, 7.5f, static_cast<float>(session.percent) / 100.f, session.percent >= 90 ? kDanger : kAccent);
        }
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

// When a limit window resets: "через 3 ч 56 мин" for the short one, "вт 3:00" for the week.
std::wstring ResetText(const Limit& limit, bool weekday) {
    if (limit.resetsAt <= 0) {
        return {};
    }
    if (!weekday) {
        const long long now = static_cast<long long>(std::time(nullptr));
        const long long minutes = std::max(0LL, (limit.resetsAt - now + 59) / 60);
        return minutes >= 60 ? std::format(L"Сброс через {} ч {} мин", minutes / 60, minutes % 60)
                             : std::format(L"Сброс через {} мин", minutes);
    }
    constexpr long long kUnixToFileTime = 11644473600LL;
    ULARGE_INTEGER ticks{};
    ticks.QuadPart = static_cast<unsigned long long>(limit.resetsAt + kUnixToFileTime) * 10'000'000ULL;
    FILETIME utc{ticks.LowPart, ticks.HighPart};
    FILETIME local{};
    SYSTEMTIME time{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &time)) {
        return {};
    }
    static constexpr const wchar_t* days[] = {L"вс", L"пн", L"вт", L"ср", L"чт", L"пт", L"сб"};
    return std::format(L"Сброс {} {}:{:02}", days[time.wDayOfWeek % 7], time.wHour, time.wMinute);
}

// An arc from the top, clockwise, for `fraction` of the circle.
void ChatWindow::Ring(D2D1_POINT_2F center, float radius, float fraction, uint32_t color) {
    fraction = std::clamp(fraction, 0.f, 1.f);
    if (fraction <= 0.f) {
        return;
    }
    brush_->SetColor(Color(color));
    if (fraction >= 0.995f) {
        target_->DrawEllipse(D2D1::Ellipse(center, radius, radius), brush_.get(), 2.f);
        return;
    }
    const float angle = fraction * 2.f * std::numbers::pi_v<float>;
    wil::com_ptr<ID2D1PathGeometry> path;
    wil::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(d2d_->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) {
        return;
    }
    sink->BeginFigure({center.x, center.y - radius}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment({center.x + radius * std::sin(angle), center.y - radius * std::cos(angle)},
                                  D2D1::SizeF(radius, radius), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  fraction > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (SUCCEEDED(sink->Close())) {
        target_->DrawGeometry(path.get(), brush_.get(), 2.f, round_.get());
    }
}

void ChatWindow::PaintUsage(const Geometry& g) {
    PaintPanel(g.popup);
    const float left = g.popup.left + 16;
    const float right = g.popup.right - 16;
    Text(L"Лимиты использования", ui_.get(), D2D1::RectF(left, g.popup.top + 10, right, g.popup.top + 34), kSecondary);
    struct Row {
        const wchar_t* label;
        const Limit* limit;
        bool weekday;
    };
    const Row rows[] = {{L"Сессия · 5 ч", &session_, false}, {L"Неделя · все модели", &week_, true}};
    constexpr float kPercentWidth = 40.f;
    float top = g.popup.top + 14 + 24;
    for (const Row& row : rows) {
        const Limit limit = Current(*row.limit);
        const int percent = limit.percent;
        const float labelWidth = TextWidth(row.label, uiBold_.get()) + 12.f;
        Text(row.label, uiBold_.get(), D2D1::RectF(left, top, left + labelWidth, top + 20), kPrimary);
        Text(ResetText(limit, row.weekday), uiRight_.get(),
             D2D1::RectF(left + labelWidth, top, right - kPercentWidth - 8, top + 20), kSecondary);
        Text(percent >= 0 ? std::format(L"{}%", percent) : std::wstring(L"—"), uiRight_.get(),
             D2D1::RectF(right - kPercentWidth, top, right, top + 20), kSecondary);
        const D2D1_RECT_F bar = D2D1::RectF(left, top + 28, right, top + 34);
        Fill(bar, 3.f, kRaised);
        if (percent > 0) {
            const float width = std::max(6.f, (right - left) * static_cast<float>(std::min(percent, 100)) / 100.f);
            Fill(D2D1::RectF(left, bar.top, left + width, bar.bottom), 3.f, percent >= 90 ? kDanger : kAccent);
        }
        top += kUsageRow;
    }
}

LRESULT CALLBACK ChatWindow::SecretEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR,
                                            DWORD_PTR self) {
    auto* window = reinterpret_cast<ChatWindow*>(self);
    if (message == WM_KEYDOWN && (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB)) {
        PostMessageW(window->hwnd_, kSecretKey, wParam, reinterpret_cast<LPARAM>(hwnd));
        return 0;
    }
    if (message == WM_CHAR && (wParam == L'\r' || wParam == L'\t' || wParam == 0x1b)) {
        return 0;  // no beep for the keys handled above
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

HWND ChatWindow::CreateSecretField(bool password, const wchar_t* hint) {
    const DWORD style = WS_CHILD | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0);
    const HWND field = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
    THROW_LAST_ERROR_IF_NULL(field);
    SendMessageW(field, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(hint));
    SendMessageW(field, EM_SETLIMITTEXT, 8192, 0);
    SetWindowSubclass(field, &ChatWindow::SecretEditProc, kSubclassId, reinterpret_cast<DWORD_PTR>(this));
    return field;
}

std::wstring ChatWindow::FieldText(HWND field) const {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(field)), L'\0');
    if (!text.empty()) {
        GetWindowTextW(field, text.data(), static_cast<int>(text.size() + 1));
    }
    return text;
}

std::wstring Trimmed(std::wstring text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
}

// The fields sit over the panel's painted slots; while the panel is closed they are hidden.
void ChatWindow::PlaceSecretFields() {
    if (nameEdit_ == nullptr || valueEdit_ == nullptr) {
        return;
    }
    if (secretsOpen_) {
        const Geometry g = Layout();
        constexpr float kText = 20.f;
        constexpr float kInset = 10.f;
        for (const auto& [edit, slot] : {std::pair{nameEdit_, g.nameField}, std::pair{valueEdit_, g.valueField}}) {
            const float top = slot.top + (slot.bottom - slot.top - kText) / 2;
            const float right = slot.right - kInset - (edit == valueEdit_ ? 30.f : 0.f);  // room for the eye
            MoveChild(edit, D2D1::RectF(slot.left + kInset, top, right, top + kText));
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_.get()), TRUE);
        }
    }
    ShowWindow(nameEdit_, secretsOpen_ ? SW_SHOW : SW_HIDE);
    ShowWindow(valueEdit_, secretsOpen_ ? SW_SHOW : SW_HIDE);
}

void ChatWindow::SetSecretsOpen(bool open) {
    if (secretsOpen_ == open) {
        return;
    }
    ClosePopup();
    searchFocus_ = false;
    if (open) {
        sidebarOpen_ = true;  // the secrets live in the sidebar
    }
    secretsOpen_ = open;
    hover_ = {};
    if (!open) {
        editingSecret_.clear();
        SetShowValue(false);
        SetWindowTextW(nameEdit_, L"");
        SetWindowTextW(valueEdit_, L"");
    }
    PlaceChildren();  // the chat's column moves, the fields come or go
    SetFocus(open ? nameEdit_ : input_);
    Refresh();
}

void ChatWindow::SubmitSecret() {
    const std::wstring name = Trimmed(FieldText(nameEdit_));
    std::wstring value = Trimmed(FieldText(valueEdit_));
    const bool validName = !name.empty() && !std::iswdigit(name[0]) &&
                           std::ranges::all_of(name, [](wchar_t c) { return c < 128 && (std::iswalnum(c) || c == L'_'); });
    if (!validName) {
        Flash(L"Имя: латиница, цифры и _, как у переменной окружения", 3500);
        SetFocus(nameEdit_);
        return;
    }
    if (value.empty()) {
        Flash(L"Введите значение", 2500);
        SetFocus(valueEdit_);
        return;
    }
    const std::string key = ToUtf8(name);
    const bool renaming = !editingSecret_.empty() && editingSecret_ != key;
    if (renaming && secrets_.Has(key)) {
        Flash(L"Секрет с таким именем уже есть", 3000);
        SetFocus(nameEdit_);
        return;
    }
    const bool replaced = secrets_.Has(key) || renaming;
    secrets_.Set(key, ToUtf8(value));
    if (renaming) {
        secrets_.Remove(editingSecret_);
    }
    SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    if (!secrets_.Save()) {
        Flash(L"Не удалось сохранить файл секретов", 3500);
        return;
    }
    editingSecret_.clear();
    SetShowValue(false);
    SetWindowTextW(nameEdit_, L"");
    SetWindowTextW(valueEdit_, L"");
    PlaceSecretFields();
    SetFocus(nameEdit_);
    Flash(replaced ? L"Секрет обновлён" : L"Секрет добавлен");
    Refresh();
}

// A secret into the form: its name, and its value behind dots (the eye shows it). Saving replaces it, renamed or not.
void ChatWindow::EditSecret(const std::string& name) {
    std::optional<std::string> value = secrets_.Get(name);
    if (!value) {
        return;
    }
    editingSecret_ = name;
    SetShowValue(false);
    SetWindowTextW(nameEdit_, ToWide(name).c_str());
    std::wstring wide = ToWide(*value);
    SetWindowTextW(valueEdit_, wide.c_str());
    SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));
    SecureZeroMemory(value->data(), value->size());
    PlaceSecretFields();  // the save button makes room for "cancel"
    SetFocus(valueEdit_);
    SendMessageW(valueEdit_, EM_SETSEL, 0, -1);
    Refresh();
}

void ChatWindow::CancelSecretEdit() {
    editingSecret_.clear();
    SetShowValue(false);
    SetWindowTextW(nameEdit_, L"");
    SetWindowTextW(valueEdit_, L"");
    PlaceSecretFields();
    SetFocus(nameEdit_);
    Refresh();
}

void ChatWindow::SetShowValue(bool show) {
    showValue_ = show;
    if (valueEdit_ != nullptr) {
        SendMessageW(valueEdit_, EM_SETPASSWORDCHAR, show ? 0 : 0x25CF, 0);
        InvalidateRect(valueEdit_, nullptr, TRUE);
    }
    Refresh();
}

void ChatWindow::DeleteSecret(const std::string& name) {
    const std::wstring question = L"Удалить секрет " + ToWide(name) + L"?\nЭто нельзя отменить.";
    if (MessageBoxW(hwnd_, question.c_str(), L"AIControls", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    if (name == editingSecret_) {
        CancelSecretEdit();
    }
    secrets_.Remove(name);
    if (!secrets_.Save()) {
        Flash(L"Не удалось сохранить файл секретов", 3500);
    } else {
        Flash(L"Секрет удалён");
    }
    hover_ = {};
    Refresh();
}

D2D1_RECT_F ChatWindow::SecretRowRect(const Geometry& g, size_t index) {
    const float top = g.secretsTop + static_cast<float>(index) * kSecretRow;
    return D2D1::RectF(g.panel.left + 8, top, g.panel.right - 8, top + kSecretRow - 2);
}

D2D1_RECT_F ChatWindow::SecretDeleteRect(const D2D1_RECT_F& row) {
    return D2D1::RectF(row.right - 34, row.top + 3, row.right - 6, row.bottom - 3);
}

size_t ChatWindow::SecretRowsFit(const Geometry& g) {
    const float room = g.nameField.top - 12 - g.secretsTop;
    return room <= 0 ? 0 : static_cast<size_t>(room / kSecretRow);
}

void ChatWindow::PaintSecrets(const Geometry& g) {
    if (!secretsOpen_) {
        return;
    }
    Text(L"Секреты", uiBold_.get(),
         D2D1::RectF(g.panel.left + kPanelPad, g.panel.top + 10, g.panelClose.left - 8, g.panel.top + 38), kPrimary);
    {  // a cross
        const bool active = hover_.target == Target::SecretsClose;
        if (active) {
            Fill(g.panelClose, 6.f, kRaised);
        }
        const D2D1_POINT_2F c{(g.panelClose.left + g.panelClose.right) / 2, (g.panelClose.top + g.panelClose.bottom) / 2};
        const uint32_t color = active ? kPrimary : kSecondary;
        Line({c.x - 5.f, c.y - 5.f}, {c.x + 5.f, c.y + 5.f}, color, 1.5f);
        Line({c.x - 5.f, c.y + 5.f}, {c.x + 5.f, c.y - 5.f}, color, 1.5f);
    }
    Text(L"Значения хранятся зашифрованными. Claude видит только имена.",
         small_.get(),
         D2D1::RectF(g.panel.left + kPanelPad, g.panel.top + 44, g.panel.right - kPanelPad, g.panel.top + 44 + kSecretsNote),
         kSecondary);

    const std::vector<std::string> names = secrets_.Names();
    if (names.empty()) {
        Text(L"Секретов пока нет.", ui_.get(),
             D2D1::RectF(g.panel.left + kPanelPad, g.secretsTop, g.panel.right - kPanelPad, g.secretsTop + 28), kSecondary);
    }
    const size_t fit = SecretRowsFit(g);
    secretsFirst_ = ClampFirst(secretsFirst_, names.size(), fit);
    const size_t shown = std::min(names.size() - secretsFirst_, fit);
    PaintListThumb(g.panel.right - 5.f, g.secretsTop, g.secretsTop + static_cast<float>(fit) * kSecretRow, secretsFirst_, fit,
                   names.size(), Inside(g.panel, mouse_.x, mouse_.y));
    for (size_t slot = 0; slot < shown; ++slot) {
        const size_t i = secretsFirst_ + slot;
        const D2D1_RECT_F row = SecretRowRect(g, slot);
        const bool onRow = (hover_.target == Target::SecretRow || hover_.target == Target::SecretDelete) && hover_.row == i;
        if (onRow || names[i] == editingSecret_) {
            Fill(row, 8.f, kRaised);
        }
        if (names[i] == editingSecret_) {  // the one in the form: a mark at its left edge
            Fill(D2D1::RectF(row.left, row.top + 8, row.left + 3, row.bottom - 8), 1.5f, kAccent);
        }
        Text(ToWide(names[i]), ui_.get(), D2D1::RectF(row.left + 10, row.top, row.right - 100, row.bottom), kPrimary);
        Text(L"••••••••", uiRight_.get(), D2D1::RectF(row.right - 96, row.top, row.right - 40, row.bottom), kSecondary);
        if (onRow) {  // a bin
            const D2D1_RECT_F bin = SecretDeleteRect(row);
            const bool danger = hover_.target == Target::SecretDelete;
            const D2D1_POINT_2F c{(bin.left + bin.right) / 2, (bin.top + bin.bottom) / 2};
            const uint32_t color = danger ? kDanger : kSecondary;
            Line({c.x - 5.f, c.y - 3.5f}, {c.x + 5.f, c.y - 3.5f}, color, 1.4f);
            Line({c.x - 2.f, c.y - 5.5f}, {c.x + 2.f, c.y - 5.5f}, color, 1.4f);
            Line({c.x - 4.f, c.y - 3.5f}, {c.x - 3.2f, c.y + 5.f}, color, 1.4f);
            Line({c.x + 4.f, c.y - 3.5f}, {c.x + 3.2f, c.y + 5.f}, color, 1.4f);
            Line({c.x - 3.2f, c.y + 5.f}, {c.x + 3.2f, c.y + 5.f}, color, 1.4f);
        }
    }

    Fill(g.nameField, 8.f, kRaised);
    Fill(g.valueField, 8.f, kRaised);
    const std::wstring name = Trimmed(FieldText(nameEdit_));
    const bool ready = !name.empty();
    const bool editing = !editingSecret_.empty();
    const bool replacing = ready && (editing || secrets_.Has(ToUtf8(name)));
    Fill(g.saveButton, 8.f, ready ? kAccent : kRaised, ready && hover_.target == Target::SecretSave ? 0.85f : 1.f);
    Text(editing ? L"Сохранить" : (replacing ? L"Обновить" : L"Добавить"), button_.get(), g.saveButton,
         ready ? kPrimary : kSecondary);
    if (editing) {
        Fill(g.secretCancel, 8.f, hover_.target == Target::SecretCancel ? kLine : kRaised);
        Text(L"Отмена", button_.get(), g.secretCancel, kPrimary);
    }
    {  // the eye: shows the value, struck through while it is hidden
        const bool active = hover_.target == Target::SecretEye;
        if (active) {
            Fill(g.secretEye, 6.f, kLine);
        }
        const D2D1_POINT_2F c{(g.secretEye.left + g.secretEye.right) / 2, (g.secretEye.top + g.secretEye.bottom) / 2};
        const uint32_t color = active || showValue_ ? kPrimary : kSecondary;
        brush_->SetColor(Color(color));
        target_->DrawEllipse(D2D1::Ellipse(c, 7.f, 4.f), brush_.get(), 1.3f);
        target_->FillEllipse(D2D1::Ellipse(c, 1.8f, 1.8f), brush_.get());
        if (!showValue_) {
            Line({c.x - 6.f, c.y + 5.f}, {c.x + 6.f, c.y - 5.f}, color, 1.3f);
        }
    }
}

// ---- The terminal -------------------------------------------------------------------------------

bool SetClipboardText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return false;
    }
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    bool ok = false;
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes); memory != nullptr) {
        if (void* data = GlobalLock(memory); data != nullptr) {
            std::memcpy(data, text.c_str(), bytes);
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok) {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    return ok;
}

uint32_t Mix(uint32_t from, uint32_t to, float amount) {
    const auto channel = [&](int shift) {
        const float a = static_cast<float>((from >> shift) & 0xff);
        const float b = static_cast<float>((to >> shift) & 0xff);
        return static_cast<uint32_t>(a + (b - a) * amount) & 0xff;
    };
    return channel(16) << 16 | channel(8) << 8 | channel(0);
}

ChatWindow::TermTab* ChatWindow::TabById(WPARAM id) const {
    for (const auto& tab : tabs_) {
        if (tab->id == id) {
            return tab.get();
        }
    }
    return nullptr;
}

// As many columns and rows as fit the panel's screen area.
void ChatWindow::TerminalGrid(int& cols, int& rows) const {
    const Geometry g = Layout();
    cols = std::max(10, static_cast<int>((g.termScreen.right - g.termScreen.left) / cellW_));
    rows = std::max(3, static_cast<int>((g.termScreen.bottom - g.termScreen.top) / cellH_));
}

std::wstring ChatWindow::TabLabel(const TermTab& tab) const {
    if (!tab.preview.empty()) {
        return L"Превью · " + std::filesystem::path(tab.preview).filename().wstring();
    }
    if (!tab.name.empty()) {
        return tab.name;
    }
    return std::format(L"Терминал {}", tab.number);
}

void ChatWindow::AddTerminalTab() {
    auto tab = std::make_unique<TermTab>();
    int number = 1;  // the lowest number not in use
    while (std::ranges::any_of(tabs_, [&](const auto& other) { return other->number == number; })) {
        ++number;
    }
    tab->number = number;
    tabs_.push_back(std::move(tab));
    activeTab_ = tabs_.size() - 1;
    selAnchor_.reset();
    selEnd_.reset();
    StartShell(*tabs_.back());
    PlacePreview();
    Refresh();
}

// A fresh PowerShell in the tab (also after the old one has exited).
void ChatWindow::StartShell(TermTab& tab) {
    int cols = 0;
    int rows = 0;
    TerminalGrid(cols, rows);
    tab.pty.reset();  // stops the old shell and its threads
    tab.pty = std::make_unique<Pty>();
    tab.screen = std::make_unique<VtScreen>(cols, rows);
    Pty* pty = tab.pty.get();
    tab.screen->SetReply([pty](std::string answer) { pty->Write(answer); });
    tab.exited = false;
    tab.scroll = 0;
    tab.id = ++nextTabId_;
    const WPARAM id = tab.id;
    const HWND hwnd = hwnd_;
    wchar_t profile[MAX_PATH]{};
    GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    try {
        tab.pty->Start(
            cols, rows, L"powershell.exe -NoLogo", profile,
            [hwnd, id](std::string bytes) {
                auto payload = std::make_unique<std::string>(std::move(bytes));
                if (PostMessageW(hwnd, kTermData, id, reinterpret_cast<LPARAM>(payload.get()))) {
                    payload.release();
                }
            },
            [hwnd, id](DWORD code) { PostMessageW(hwnd, kTermExit, id, static_cast<LPARAM>(code)); });
    } catch (const std::exception& e) {
        tab.screen->Feed("\x1b[31m" + ToUtf8(L"Не удалось запустить PowerShell: ") + e.what() + "\x1b[0m");
        tab.exited = true;
    }
}

void ChatWindow::CloseTerminalTab(size_t index) {
    if (index >= tabs_.size()) {
        return;
    }
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));  // the shell ends with it
    selAnchor_.reset();
    selEnd_.reset();
    hover_ = {};
    if (PreviewTab() == nullptr) {
        webShown_.clear();
    }
    PlacePreview();
    if (tabs_.empty()) {
        activeTab_ = 0;
        SetTerminalOpen(false);
        return;
    }
    if (activeTab_ >= tabs_.size() || index < activeTab_) {
        activeTab_ = activeTab_ == 0 ? 0 : activeTab_ - 1;
    }
    activeTab_ = std::min(activeTab_, tabs_.size() - 1);
    PlacePreview();
    Refresh();
}

void ChatWindow::SelectTerminalTab(size_t index) {
    if (index < tabs_.size() && index != activeTab_) {
        activeTab_ = index;
        selAnchor_.reset();
        selEnd_.reset();
    }
    termFocus_ = !PreviewShown();  // the page takes its own clicks and keys
    searchFocus_ = false;
    SetFocus(hwnd_);
    PlacePreview();
    Refresh();
}

void ChatWindow::SetTerminalOpen(bool open) {
    if (terminalOpen_ == open) {
        return;
    }
    ClosePopup();
    terminalOpen_ = open;
    hover_ = {};
    PlaceChildren();  // the chat's column moves
    if (open) {
        if (tabs_.empty()) {
            AddTerminalTab();
        }
        SyncTerminalSize();
        termFocus_ = true;
        searchFocus_ = false;
        SetFocus(hwnd_);
    } else {
        termFocus_ = false;
        selecting_ = false;
        SetFocus(input_);
    }
    Refresh();
}

// Every tab's grid follows the panel.
void ChatWindow::SyncTerminalSize() {
    if (!terminalOpen_ || tabs_.empty()) {
        return;
    }
    const Geometry g = Layout();
    if (g.termScreen.right <= g.termScreen.left) {
        return;
    }
    int cols = 0;
    int rows = 0;
    TerminalGrid(cols, rows);
    for (const auto& tab : tabs_) {
        if (tab->screen && (cols != tab->screen->Cols() || rows != tab->screen->Rows())) {
            tab->screen->Resize(cols, rows);
            if (tab->pty) {
                tab->pty->Resize(cols, rows);
            }
            tab->scroll = std::min(tab->scroll, tab->screen->ScrollbackSize());
        }
    }
}

void ChatWindow::OnTermData(WPARAM id, const std::string& bytes) {
    TermTab* tab = TabById(id);
    if (tab == nullptr || !tab->screen) {
        return;
    }
    const size_t before = tab->screen->ScrollbackSize();
    tab->screen->Feed(bytes);
    if (tab->scroll > 0) {  // reading the scrollback: the view stays on its lines while new ones arrive
        const size_t after = tab->screen->ScrollbackSize();
        tab->scroll = std::min(tab->scroll + (after - std::min(before, after)), after);
    }
    if (tab == Tab()) {
        Refresh();
    }
}

void ChatWindow::OnTermExit(WPARAM id, DWORD code) {
    TermTab* tab = TabById(id);
    if (tab == nullptr) {
        return;
    }
    tab->exited = true;
    if (tab->screen) {
        tab->screen->Feed("\r\n\x1b[90m" +
                          ToUtf8(std::format(L"[процесс завершён, код {}; любая клавиша запускает заново]", code)) + "\x1b[0m");
    }
    Refresh();
}

void ChatWindow::TermSend(std::string_view bytes) {
    TermTab* tab = Tab();
    if (tab == nullptr || !tab->pty) {
        return;
    }
    tab->pty->Write(bytes);
    tab->scroll = 0;
    selAnchor_.reset();
    selEnd_.reset();
    Refresh();
}

void ChatWindow::OnTermChar(wchar_t c) {
    if (termSwallowChar_) {
        termSwallowChar_ = false;
        return;
    }
    if (Tab() == nullptr) {
        return;
    }
    if (Tab()->exited) {
        StartShell(*Tab());
        return;
    }
    if (c >= 0xd800 && c <= 0xdbff) {
        pendingHigh_ = c;
        return;
    }
    std::wstring text;
    if (pendingHigh_ != 0) {
        text += pendingHigh_;
        pendingHigh_ = 0;
    }
    text += c;
    if (c == 0x08) {
        TermSend("\x7f");  // Backspace
    } else if (c == 0x7f) {
        TermSend("\x17");  // Ctrl+Backspace: delete the word
    } else if (c == 0) {
        TermSend(std::string(1, '\0'));
    } else {
        TermSend(ToUtf8(text));
    }
}

void ChatWindow::OnTermKey(WPARAM key) {
    if (Tab() == nullptr) {
        return;
    }
    if (Tab()->exited) {
        if (key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU) {
            StartShell(*Tab());
            termSwallowChar_ = true;
        }
        return;
    }
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (control && key == 'C' && (shift || HasTermSelection())) {
        CopyTermSelection();
        termSwallowChar_ = true;
        return;
    }
    if ((control && key == 'V') || (shift && key == VK_INSERT)) {
        PasteToTerminal();
        termSwallowChar_ = true;
        return;
    }
    if (control && key == VK_INSERT) {
        CopyTermSelection();
        return;
    }
    if (shift && (key == VK_PRIOR || key == VK_NEXT)) {
        ScrollTerminal((key == VK_PRIOR ? 1 : -1) * std::max(1, Screen() ? Screen()->Rows() - 1 : 1));
        return;
    }
    if (key == VK_TAB && shift) {
        TermSend("\x1b[Z");
        termSwallowChar_ = true;
        return;
    }
    const int modifier = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (control ? 4 : 0);
    const auto arrow = [&](char final) {
        if (modifier == 1) {
            return std::string(Screen() && Screen()->AppCursorKeys() ? "\x1bO" : "\x1b[") + final;
        }
        return "\x1b[1;" + std::to_string(modifier) + final;
    };
    const auto tilde = [&](int code) {
        return "\x1b[" + std::to_string(code) + (modifier == 1 ? "" : ";" + std::to_string(modifier)) + "~";
    };
    switch (key) {
    case VK_UP:
        TermSend(arrow('A'));
        break;
    case VK_DOWN:
        TermSend(arrow('B'));
        break;
    case VK_RIGHT:
        TermSend(arrow('C'));
        break;
    case VK_LEFT:
        TermSend(arrow('D'));
        break;
    case VK_HOME:
        TermSend(arrow('H'));
        break;
    case VK_END:
        TermSend(arrow('F'));
        break;
    case VK_INSERT:
        TermSend(tilde(2));
        break;
    case VK_DELETE:
        TermSend(tilde(3));
        break;
    case VK_PRIOR:
        TermSend(tilde(5));
        break;
    case VK_NEXT:
        TermSend(tilde(6));
        break;
    case VK_F1:
    case VK_F2:
    case VK_F3:
    case VK_F4:
        TermSend(std::string("\x1bO") + static_cast<char>('P' + (key - VK_F1)));
        break;
    case VK_F5:
        TermSend(tilde(15));
        break;
    case VK_F6:
    case VK_F7:
    case VK_F8:
        TermSend(tilde(17 + static_cast<int>(key - VK_F6)));
        break;
    case VK_F9:
    case VK_F10:
        TermSend(tilde(20 + static_cast<int>(key - VK_F9)));
        break;
    case VK_F11:
    case VK_F12:
        TermSend(tilde(23 + static_cast<int>(key - VK_F11)));
        break;
    default:
        break;
    }
}

void ChatWindow::PasteToTerminal() {
    if (!Screen() || Tab()->exited) {
        return;
    }
    std::wstring text = ClipboardText(hwnd_);
    if (text.empty()) {
        return;
    }
    std::wstring flat;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') {
            continue;
        }
        if (text[i] != 0x1b) {  // an escape in pasted text could end the bracket early
            flat += text[i] == L'\n' ? L'\r' : text[i];
        }
    }
    std::string bytes = ToUtf8(flat);
    if (Screen()->BracketedPaste()) {
        bytes = "\x1b[200~" + bytes + "\x1b[201~";
    }
    TermSend(bytes);
}

bool ChatWindow::HasTermSelection() const {
    return selAnchor_ && selEnd_ && (selAnchor_->line != selEnd_->line || selAnchor_->col != selEnd_->col);
}

bool ChatWindow::TermSelected(size_t line, int col) const {
    if (!HasTermSelection()) {
        return false;
    }
    TermPoint a = *selAnchor_;
    TermPoint b = *selEnd_;
    if (a.line > b.line || (a.line == b.line && a.col > b.col)) {
        std::swap(a, b);
    }
    const bool afterStart = line > a.line || (line == a.line && col >= a.col);
    const bool beforeEnd = line < b.line || (line == b.line && col <= b.col);
    return afterStart && beforeEnd;
}

void ChatWindow::CopyTermSelection() {
    if (!Screen() || !HasTermSelection()) {
        return;
    }
    TermPoint a = *selAnchor_;
    TermPoint b = *selEnd_;
    if (a.line > b.line || (a.line == b.line && a.col > b.col)) {
        std::swap(a, b);
    }
    if (SetClipboardText(hwnd_, Screen()->TextBetween(a.line, a.col, b.line, b.col + 1))) {
        Flash(L"Скопировано");
    }
    selAnchor_.reset();
    selEnd_.reset();
    Refresh();
}

// The index of the line shown at the top of the screen area.
size_t ChatWindow::TermFirstLine() const {
    if (!Screen()) {
        return 0;
    }
    const size_t rows = static_cast<size_t>(Screen()->Rows());
    const size_t scroll = std::min(Tab()->scroll, Screen()->ScrollbackSize());
    return Screen()->LineCount() - rows - scroll;
}

ChatWindow::TermPoint ChatWindow::TermCellAt(const Geometry& g, float x, float y) const {
    TermPoint point;
    if (!Screen()) {
        return point;
    }
    const int row = std::clamp(static_cast<int>(std::floor((y - g.termScreen.top) / cellH_)), 0, Screen()->Rows() - 1);
    point.col = std::clamp(static_cast<int>(std::floor((x - g.termScreen.left) / cellW_)), 0, Screen()->Cols() - 1);
    point.line = TermFirstLine() + static_cast<size_t>(row);
    return point;
}

void ChatWindow::ScrollTerminal(long long lines) {
    if (!Screen()) {
        return;
    }
    const long long most = static_cast<long long>(Screen()->ScrollbackSize());
    Tab()->scroll = static_cast<size_t>(std::clamp(static_cast<long long>(Tab()->scroll) + lines, 0LL, most));
    Refresh();
}

void ChatWindow::PaintTerminal(const Geometry& g) {
    if (g.term.right <= g.term.left) {
        return;
    }
    Fill(g.term, 0.f, kPanel);
    Line({g.term.left, g.term.top}, {g.term.left, g.term.bottom}, kLine, 1.f);
    for (size_t i = 0; i < tabs_.size() && i < g.termTabs.size(); ++i) {  // the tabs, as in Claude's app
        const D2D1_RECT_F& r = g.termTabs[i];
        const bool active = i == activeTab_;
        const bool hovered = (hover_.target == Target::TermTab || hover_.target == Target::TermTabClose) && hover_.row == i;
        if (renaming_ == Rename::Tab && tabs_[i]->id == renameTab_) {
            PaintRenameField(r);
            continue;
        }
        if (active || hovered) {
            Fill(r, 6.f, active ? kRaised : kCard);
        }
        Text(TabLabel(*tabs_[i]), ui_.get(), D2D1::RectF(r.left + 10, r.top, r.right - 22, r.bottom),
             active ? kPrimary : kSecondary);
        if (active || hovered) {
            const D2D1_RECT_F x = TabCloseRect(r);
            const D2D1_POINT_2F c{(x.left + x.right) / 2, (x.top + x.bottom) / 2};
            const uint32_t color = hover_.target == Target::TermTabClose && hover_.row == i ? kPrimary : kSecondary;
            Line({c.x - 3.5f, c.y - 3.5f}, {c.x + 3.5f, c.y + 3.5f}, color, 1.3f);
            Line({c.x - 3.5f, c.y + 3.5f}, {c.x + 3.5f, c.y - 3.5f}, color, 1.3f);
        }
    }
    {  // "+"
        const bool active = hover_.target == Target::TermAdd;
        if (active) {
            Fill(g.termAdd, 6.f, kRaised);
        }
        const D2D1_POINT_2F c{(g.termAdd.left + g.termAdd.right) / 2, (g.termAdd.top + g.termAdd.bottom) / 2};
        const uint32_t color = active ? kPrimary : kSecondary;
        Line({c.x - 5.f, c.y}, {c.x + 5.f, c.y}, color, 1.5f);
        Line({c.x, c.y - 5.f}, {c.x, c.y + 5.f}, color, 1.5f);
    }
    {  // a cross
        const bool active = hover_.target == Target::TermClose;
        if (active) {
            Fill(g.termClose, 6.f, kRaised);
        }
        const D2D1_POINT_2F c{(g.termClose.left + g.termClose.right) / 2, (g.termClose.top + g.termClose.bottom) / 2};
        const uint32_t color = active ? kPrimary : kSecondary;
        Line({c.x - 5.f, c.y - 5.f}, {c.x + 5.f, c.y + 5.f}, color, 1.5f);
        Line({c.x - 5.f, c.y + 5.f}, {c.x + 5.f, c.y - 5.f}, color, 1.5f);
    }
    if (g.termWide.right > g.termWide.left) {  // arrows out to the corners (widen), or in (back)
        const bool active = hover_.target == Target::PreviewWide;
        if (active || previewWide_) {
            Fill(g.termWide, 6.f, kRaised);
        }
        const D2D1_POINT_2F c{(g.termWide.left + g.termWide.right) / 2, (g.termWide.top + g.termWide.bottom) / 2};
        const uint32_t color = active ? kPrimary : kSecondary;
        const float in = previewWide_ ? 1.5f : 5.5f;  // where each arrow's tip is, from the centre
        const float out = previewWide_ ? 5.5f : 1.5f;
        for (const float sx : {-1.f, 1.f}) {
            const float sy = -sx;  // top right and bottom left
            const D2D1_POINT_2F tip{c.x + sx * in, c.y + sy * in};
            Line({c.x + sx * out, c.y + sy * out}, tip, color, 1.4f);
            const float back = previewWide_ ? 1.f : -1.f;  // the arrowhead's two strokes point back along it
            Line(tip, {tip.x + back * sx * 3.5f, tip.y}, color, 1.4f);
            Line(tip, {tip.x, tip.y + back * sy * 3.5f}, color, 1.4f);
        }
    }
    if (!Screen()) {
        return;
    }

    target_->PushAxisAlignedClip(D2D1::RectF(g.term.left + 1, g.termScreen.top, g.term.right, g.term.bottom - 4),
                                 D2D1_ANTIALIAS_MODE_ALIASED);
    const int cols = Screen()->Cols();
    const int rows = Screen()->Rows();
    const size_t first = TermFirstLine();
    for (int r = 0; r < rows; ++r) {
        const size_t index = first + static_cast<size_t>(r);
        const VtLine& line = Screen()->Line(index);
        const float y = g.termScreen.top + static_cast<float>(r) * cellH_;
        const auto colors = [&](const VtCell& cell, size_t col, uint32_t& fg, uint32_t& bg, bool& hasBg) {
            fg = cell.fg == kVtDefault ? kTermFg : cell.fg;
            bg = cell.bg == kVtDefault ? kPanel : cell.bg;
            hasBg = cell.bg != kVtDefault;
            if ((cell.attrs & kVtInverse) != 0) {
                std::swap(fg, bg);
                hasBg = true;
            }
            if ((cell.attrs & kVtDim) != 0) {
                fg = Mix(fg, bg, 0.45f);
            }
            if (TermSelected(index, static_cast<int>(col))) {
                bg = Mix(bg, kAccent, 0.45f);
                hasBg = true;
            }
        };

        // Backgrounds first, in runs.
        for (int col = 0; col < cols;) {
            uint32_t fg = 0, bg = 0;
            bool hasBg = false;
            colors(line[static_cast<size_t>(col)], static_cast<size_t>(col), fg, bg, hasBg);
            int end = col + 1;
            while (end < cols) {
                uint32_t fg2 = 0, bg2 = 0;
                bool hasBg2 = false;
                colors(line[static_cast<size_t>(end)], static_cast<size_t>(end), fg2, bg2, hasBg2);
                if (hasBg2 != hasBg || bg2 != bg) {
                    break;
                }
                ++end;
            }
            if (hasBg) {
                Fill(D2D1::RectF(g.termScreen.left + static_cast<float>(col) * cellW_, y,
                                 g.termScreen.left + static_cast<float>(end) * cellW_, y + cellH_),
                     0.f, bg);
            }
            col = end;
        }

        // Then the text: a run is a stretch of narrow characters with the same look.
        std::wstring run;
        float runX = 0;
        uint32_t runFg = 0;
        uint8_t runAttrs = 0;
        const auto flush = [&] {
            if (!run.empty() && run.find_first_not_of(L' ') != std::wstring::npos) {
                brush_->SetColor(Color(runFg));
                IDWriteTextFormat* format = (runAttrs & kVtBold) != 0 ? termBold_.get() : termFont_.get();
                const float width = cellW_ * static_cast<float>(run.size() + 1);
                target_->DrawText(run.c_str(), static_cast<UINT32>(run.size()), format,
                                  D2D1::RectF(runX, y, runX + width, y + cellH_), brush_.get());
                if ((runAttrs & kVtUnderline) != 0) {
                    Line({runX, y + cellH_ - 2.f}, {runX + cellW_ * static_cast<float>(run.size()), y + cellH_ - 2.f},
                         runFg, 1.f);
                }
            }
            run.clear();
        };
        for (int col = 0; col < cols; ++col) {
            const VtCell& cell = line[static_cast<size_t>(col)];
            if (cell.ch == 0) {
                continue;
            }
            uint32_t fg = 0, bg = 0;
            bool hasBg = false;
            colors(cell, static_cast<size_t>(col), fg, bg, hasBg);
            const float x = g.termScreen.left + static_cast<float>(col) * cellW_;
            if (VtCharWidth(cell.ch) == 2 || cell.ch > 0xffff) {  // wide: alone, at its own column
                flush();
                std::wstring glyph;
                if (cell.ch > 0xffff) {
                    const char32_t v = cell.ch - 0x10000;
                    glyph += static_cast<wchar_t>(0xd800 + (v >> 10));
                    glyph += static_cast<wchar_t>(0xdc00 + (v & 0x3ff));
                } else {
                    glyph += static_cast<wchar_t>(cell.ch);
                }
                brush_->SetColor(Color(fg));
                target_->DrawText(glyph.c_str(), static_cast<UINT32>(glyph.size()), termFont_.get(),
                                  D2D1::RectF(x, y, x + 3 * cellW_, y + cellH_), brush_.get());
                continue;
            }
            if (!run.empty() && (fg != runFg || (cell.attrs & (kVtBold | kVtUnderline)) != (runAttrs & (kVtBold | kVtUnderline)))) {
                flush();
            }
            if (run.empty()) {
                runX = x;
                runFg = fg;
                runAttrs = cell.attrs;
            }
            run += static_cast<wchar_t>(cell.ch);
        }
        flush();
    }

    if (Screen()->CursorVisible() && Tab()->scroll == 0 && !Tab()->exited) {  // a block while typing goes here, an outline otherwise
        const D2D1_RECT_F block = D2D1::RectF(g.termScreen.left + static_cast<float>(Screen()->CursorCol()) * cellW_,
                                              g.termScreen.top + static_cast<float>(Screen()->CursorRow()) * cellH_,
                                              g.termScreen.left + static_cast<float>(Screen()->CursorCol() + 1) * cellW_,
                                              g.termScreen.top + static_cast<float>(Screen()->CursorRow() + 1) * cellH_);
        if (termFocus_) {
            Fill(block, 0.f, kTermFg, 0.55f);
        } else {
            brush_->SetColor(Color(kSecondary));
            target_->DrawRectangle(block, brush_.get(), 1.f);
        }
    }
    if (Screen()->ScrollbackSize() > 0) {  // where the view is within the scrollback
        const float track = g.termScreen.bottom - g.termScreen.top;
        const float total = static_cast<float>(Screen()->LineCount());
        const float thumb = std::max(20.f, track * static_cast<float>(rows) / total);
        const float shown = static_cast<float>(first) / std::max(1.f, total - static_cast<float>(rows));
        const float top = g.termScreen.top + (track - thumb) * std::clamp(shown, 0.f, 1.f);
        Fill(D2D1::RectF(g.term.right - 6.f, top, g.term.right - 3.f, top + thumb), 1.5f, kLine);
    }
    target_->PopAxisAlignedClip();
}

// ---- The account --------------------------------------------------------------------------------

std::wstring ChatWindow::AccountName() const {
    std::string email = auth_.email;
    if (email.empty()) {
        return userName_.empty() ? std::wstring(L"Аккаунт") : ToWide(userName_);
    }
    return ToWide(email.substr(0, email.find('@')));
}

std::wstring ChatWindow::AccountPlan() const {
    if (auth_.plan.empty()) {
        return {};
    }
    std::wstring plan = ToWide(auth_.plan);
    plan[0] = static_cast<wchar_t>(std::towupper(plan[0]));
    return plan;
}

void ChatWindow::RefreshAuth() {
    if (authWorking_) {
        return;
    }
    if (authThread_.joinable()) {
        authThread_.join();
    }
    authWorking_ = true;
    const HWND hwnd = hwnd_;
    authThread_ = std::thread([hwnd] {
        auto status = std::make_unique<AuthStatus>(QueryAuthStatus(FindClaudeExe(), PickProxy()));
        if (PostMessageW(hwnd, kAuthStatus, 0, reinterpret_cast<LPARAM>(status.get()))) {
            status.release();
        }
    });
}

void ChatWindow::OnAuthStatus(const AuthStatus& status) {
    authWorking_ = false;
    auth_ = status;
    if (status.known && status.loggedIn && !status.email.empty()) {
        userName_ = status.email.substr(0, status.email.find('@'));
    }
    if (restartAfterAuth_) {
        restartAfterAuth_ = false;
        if (status.loggedIn) {
            Restart();  // the CLI starts over and finds the account
        }
    }
    Refresh();
}

// `claude auth login` opens the browser itself and ends when the page has handed the code back.
void ChatWindow::SignIn() {
    if (authWorking_) {
        return;
    }
    const std::wstring exe = FindClaudeExe();
    if (exe.empty()) {
        authNote_ = L"Не найден claude.exe";
        Refresh();
        return;
    }
    if (authThread_.joinable()) {
        authThread_.join();
    }
    authWorking_ = true;
    loginWaiting_ = true;
    loginUrl_.clear();
    authNote_.clear();
    authCancel_ = false;
    const HWND hwnd = hwnd_;
    const std::atomic<bool>* cancel = &authCancel_;
    authThread_ = std::thread([hwnd, exe, cancel] {
        std::string seen;
        bool urlSent = false;
        const CliResult run = RunCli(
            L"\"" + exe + L"\" auth login --claudeai", PickProxy(), 600, cancel, [&](const std::string& text) {
                seen += text;
                if (urlSent) {
                    return;
                }
                if (std::string url = FindLoginUrl(seen); !url.empty()) {
                    urlSent = true;
                    auto payload = std::make_unique<std::string>(std::move(url));
                    if (PostMessageW(hwnd, kAuthUrl, 0, reinterpret_cast<LPARAM>(payload.get()))) {
                        payload.release();
                    }
                }
            });
        auto done = std::make_unique<LoginDone>();
        done->cancelled = run.cancelled;
        done->timedOut = run.timedOut;
        done->started = run.started;
        done->exitCode = run.exitCode;
        done->tail = seen.size() > 400 ? seen.substr(seen.size() - 400) : seen;
        if (PostMessageW(hwnd, kAuthLogin, 0, reinterpret_cast<LPARAM>(done.get()))) {
            done.release();
        }
    });
    Refresh();
}

void ChatWindow::CancelSignIn() {
    authCancel_ = true;  // the thread ends the CLI and reports back
}

void ChatWindow::OnLoginDone(const LoginDone& done) {
    loginWaiting_ = false;
    authWorking_ = false;
    loginUrl_.clear();
    if (done.cancelled) {
        authNote_ = L"Вход отменён.";
    } else if (done.timedOut) {
        authNote_ = L"Время ожидания входа вышло.";
    } else if (!done.started) {
        authNote_ = L"Не удалось запустить claude.exe.";
    } else if (done.exitCode != 0) {
        const std::wstring tail = LastLines(done.tail, 2);
        authNote_ = L"Вход не удался" + (tail.empty() ? std::wstring(L".") : L": " + tail);
    } else {
        authNote_.clear();
        restartAfterAuth_ = true;
    }
    RefreshAuth();
}

void ChatWindow::SignOut() {
    if (authWorking_) {
        return;
    }
    const std::wstring exe = FindClaudeExe();
    if (exe.empty()) {
        return;
    }
    if (MessageBoxW(hwnd_, L"Выйти из аккаунта?\nЧтобы снова пользоваться Claude, придётся войти заново.", L"AIControls",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    if (authThread_.joinable()) {
        authThread_.join();
    }
    authWorking_ = true;
    const HWND hwnd = hwnd_;
    authThread_ = std::thread([hwnd, exe] {
        RunCli(L"\"" + exe + L"\" auth logout", PickProxy(), 30);
        PostMessageW(hwnd, kAuthLogout, 0, 0);
    });
}

void ChatWindow::OnLogoutDone() {
    authWorking_ = false;
    ++generation_;  // the running CLI still holds the old account: it goes
    claude_.Stop();
    running_ = false;
    busy_ = false;
    RefreshAuth();
}

void ChatWindow::ShowAccountMenu() {
    const Geometry g = Layout();
    POINT screen{ToPx(g.account.left + 8.f), ToPx(g.account.top)};
    ClientToScreen(hwnd_, &screen);
    wil::unique_hmenu menu(CreatePopupMenu());
    const std::wstring who = auth_.email.empty() ? AccountName() : ToWide(auth_.email);
    AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 1, who.c_str());
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    const std::wstring version = claudeVersion_.empty() ? std::wstring(L"Claude: версия неизвестна")
                                                        : L"Claude " + ToWide(claudeVersion_);
    AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 3, version.c_str());
    const std::wstring update = L"Обновить Claude до " + ToWide(claudeLatest_);
    AppendMenuW(menu.get(), MF_STRING | (setupBusy_ ? MF_GRAYED : 0), ClaudeUpdateAvailable() ? 4 : 5,
                ClaudeUpdateAvailable() ? update.c_str() : L"Проверить обновления Claude");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, 2, L"Выйти из аккаунта");
    switch (TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, screen.x, screen.y, 0, hwnd_,
                           nullptr)) {
    case 2:
        SignOut();
        break;
    case 4:
        UpdateClaudeCli();
        break;
    case 5:
        CheckClaudeVersions();
        Flash(L"Проверяю, нет ли новой версии Claude…");
        break;
    default:
        break;
    }
}

// ---- The CLI itself ------------------------------------------------------------------------------------

namespace {

void PostSetup(HWND hwnd, SetupEvent event) {
    auto payload = std::make_unique<SetupEvent>(std::move(event));
    if (PostMessageW(hwnd, kSetupEvent, 0, reinterpret_cast<LPARAM>(payload.get()))) {
        payload.release();
    }
}

}  // namespace

// What the official installer does: the newest build, its checksum checked, `claude.exe install`.
void ChatWindow::InstallClaudeCli() {
    if (setupBusy_) {
        return;
    }
    if (setupThread_.joinable()) {
        setupThread_.join();
    }
    setupBusy_ = true;
    setupInstalling_ = false;
    setupPercent_ = 0;
    setupNote_.clear();
    setupCancel_ = false;
    const HWND hwnd = hwnd_;
    const std::atomic<bool>* cancel = &setupCancel_;
    setupThread_ = std::thread([hwnd, cancel] {
        std::string error;
        const bool ok = InstallClaude(
            PickProxy(), cancel,
            [hwnd](unsigned percent, bool installing) {
                PostSetup(hwnd, SetupEvent{percent, installing, false, false, false, {}});
            },
            error);
        PostSetup(hwnd, SetupEvent{100, false, true, ok, false, std::move(error)});
    });
    Refresh();
}

// `claude update`, with the running CLI stopped first: it holds its own file.
void ChatWindow::UpdateClaudeCli() {
    const std::wstring exe = FindClaudeExe();
    if (setupBusy_ || exe.empty()) {
        return;
    }
    if (busy_ && MessageBoxW(hwnd_, L"Сейчас Claude отвечает. Обновление прервёт ответ. Продолжить?", L"AIControls",
                             MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    if (setupThread_.joinable()) {
        setupThread_.join();
    }
    ++generation_;
    host_.CancelAll();
    for (FeedItem& item : feed_) {
        if (item.kind == Kind::Approval && item.verdict == Verdict::Pending) {
            item.verdict = Verdict::Cancelled;
        }
    }
    claude_.Stop();
    running_ = false;
    busy_ = false;
    setupBusy_ = true;
    setupInstalling_ = true;
    setupNote_.clear();
    setupCancel_ = false;
    Flash(L"Обновляю Claude…", 60000);
    const HWND hwnd = hwnd_;
    const std::atomic<bool>* cancel = &setupCancel_;
    setupThread_ = std::thread([hwnd, exe, cancel] {
        const CliResult run = RunCli(L"\"" + exe + L"\" update", PickProxy(), 300, cancel);
        std::string error;
        if (run.cancelled || run.timedOut || !run.started || run.exitCode != 0) {
            error = run.timedOut ? "Не дождался конца обновления"
                                 : (run.output.size() > 300 ? run.output.substr(run.output.size() - 300) : run.output);
            if (error.empty()) {
                error = "Обновление не удалось (код " + std::to_string(run.exitCode) + ")";
            }
        }
        PostSetup(hwnd, SetupEvent{100, false, true, error.empty(), true, std::move(error)});
    });
    Refresh();
}

void ChatWindow::OnSetupEvent(const SetupEvent& event) {
    if (!event.finished) {
        setupPercent_ = event.percent;
        setupInstalling_ = event.installing;
        Refresh();
        return;
    }
    setupBusy_ = false;
    setupInstalling_ = false;
    if (event.ok) {
        setupNote_.clear();
        claudeMissing_ = FindClaudeExe().empty();
        CheckClaudeVersions();
        RefreshAuth();
        StartClaude();
        Flash(event.update ? L"Claude обновлён" : L"Claude установлен", 5000);
    } else {
        setupNote_ = event.error == "Cancelled" ? std::wstring(L"Отменено.") : ToWide(event.error);
        if (event.update) {  // the old CLI is still there: back to work with it
            Flash(L"Не удалось обновить Claude: " + setupNote_, 8000);
            StartClaude();
        }
    }
    Refresh();
}

void ChatWindow::CheckClaudeVersions() {
    if (versionThread_.joinable()) {
        versionThread_.join();
    }
    const HWND hwnd = hwnd_;
    versionThread_ = std::thread([hwnd] {
        auto versions = std::make_unique<ClaudeVersions>();
        versions->installed = InstalledClaudeVersion(FindClaudeExe());
        versions->latest = LatestClaudeRelease(PickProxy()).version;
        if (PostMessageW(hwnd, kClaudeVersions, 0, reinterpret_cast<LPARAM>(versions.get()))) {
            versions.release();
        }
    });
}

void ChatWindow::OnClaudeVersions(const ClaudeVersions& versions) {
    claudeVersion_ = versions.installed;
    if (!versions.latest.empty()) {
        claudeLatest_ = versions.latest;
    }
    if (ClaudeUpdateAvailable() && announcedVersion_ != claudeLatest_) {
        announcedVersion_ = claudeLatest_;
        Flash(L"Вышла новая версия Claude " + ToWide(claudeLatest_) + L": обновить можно в меню аккаунта", 10000);
    }
    Refresh();
}

// In place of the greeting while claude.exe is missing: what it is, and the button that fetches it.
void ChatWindow::PaintInstall(const Geometry& g) {
    const float middle = (g.feed.top + g.feed.bottom) / 2;
    Text(L"Нужен Claude", greeting_.get(), D2D1::RectF(g.left, middle - 40, g.left + g.column, middle), kPrimary);
    std::wstring hint = L"Клиенту нужна программа claude.exe. Скачаю её с downloads.claude.ai и проверю контрольную сумму.";
    if (setupBusy_) {
        hint = setupInstalling_ ? std::wstring(L"Устанавливаю…") : L"Загружаю… " + std::to_wstring(setupPercent_) + L"%";
    }
    Text(hint, notice_.get(), D2D1::RectF(g.left, middle + 2, g.left + g.column, middle + 24), kSecondary);
    const bool onMain = hover_.target == Target::InstallClaude || hover_.target == Target::CancelInstall;
    if (setupBusy_) {
        Fill(g.signIn, 9.f, onMain ? kLine : kRaised);
        Text(L"Отмена", button_.get(), g.signIn, kPrimary);
    } else {
        Fill(g.signIn, 9.f, kAccent, onMain ? 0.85f : 1.f);
        Text(L"Установить", button_.get(), g.signIn, kPrimary);
    }
    if (!setupNote_.empty()) {
        Text(setupNote_, notice_.get(), D2D1::RectF(g.left, g.signIn.bottom + 12, g.left + g.column, g.signIn.bottom + 52),
             kDanger);
    }
}

// In place of the greeting while nobody is signed in: what to do, and the sign-in button.
void ChatWindow::PaintSignIn(const Geometry& g) {
    const float middle = (g.feed.top + g.feed.bottom) / 2;
    Text(L"Войдите в аккаунт Claude", greeting_.get(), D2D1::RectF(g.left, middle - 40, g.left + g.column, middle),
         kPrimary);
    const std::wstring hint = loginWaiting_ ? L"Откройте страницу входа в браузере и подтвердите вход."
                                            : L"Нужна подписка Claude (Pro или Max): без входа чат не отвечает.";
    Text(hint, notice_.get(), D2D1::RectF(g.left, middle + 2, g.left + g.column, middle + 24), kSecondary);

    const bool onMain = hover_.target == Target::SignIn || hover_.target == Target::CancelLogin;
    if (loginWaiting_) {
        Fill(g.signIn, 9.f, onMain ? kLine : kRaised);
        Text(L"Отмена", button_.get(), g.signIn, kPrimary);
        if (!loginUrl_.empty()) {
            const bool onAlt = hover_.target == Target::CopyLoginUrl;
            Fill(g.signAlt, 9.f, onAlt ? kLine : kRaised);
            Text(L"Копировать ссылку", button_.get(), g.signAlt, kPrimary);
        }
    } else {
        Fill(g.signIn, 9.f, kAccent, onMain ? 0.85f : 1.f);
        Text(authWorking_ ? L"Проверяю…" : L"Войти", button_.get(), g.signIn, kPrimary);
    }
    if (!authNote_.empty()) {
        const float top = (loginWaiting_ && !loginUrl_.empty() ? g.signAlt.bottom : g.signIn.bottom) + 12;
        Text(authNote_, notice_.get(), D2D1::RectF(g.left, top, g.left + g.column, top + 40), kDanger);
    }
}

// ---- Selecting text in the feed ---------------------------------------------------------------------

// Where an item's text layout is drawn (as PaintFeed and PaintApproval place it).
D2D1_POINT_2F ChatWindow::ItemOrigin(const Geometry& g, const FeedItem& item) const {
    switch (item.kind) {
    case Kind::User:
        return {g.left + g.column - (item.textWidth + 2 * kBubblePad) + kBubblePad, item.top + item.bubbleTop + kBubblePad};
    case Kind::Approval:
        return {g.left + kCardPad, item.top + kCardPad + kCardTitle + 4.f};
    default:
        return {g.left, item.top};
    }
}

// The text position under a point; above an item it is the item's start, below the last one its end.
std::optional<ChatWindow::FeedPoint> ChatWindow::FeedPointAt(const Geometry& g, float x, float y) const {
    std::optional<FeedPoint> found;
    for (size_t i = 0; i < feed_.size(); ++i) {
        const FeedItem& item = feed_[i];
        if (!item.layout || (item.kind == Kind::Notice && !HasConversation())) {
            continue;
        }
        if (y < item.top) {
            return found ? found : FeedPoint{i, 0};
        }
        if (y < item.top + item.height) {
            const D2D1_POINT_2F origin = ItemOrigin(g, item);
            BOOL trailing = FALSE;
            BOOL inside = FALSE;
            DWRITE_HIT_TEST_METRICS metrics{};
            item.layout->HitTestPoint(x - origin.x, y - origin.y, &trailing, &inside, &metrics);
            const UINT32 position = metrics.textPosition + (trailing ? metrics.length : 0);
            return FeedPoint{i, std::min<UINT32>(position, static_cast<UINT32>(item.shown.size()))};
        }
        found = FeedPoint{i, static_cast<UINT32>(item.shown.size())};  // past this one: its end, unless another follows
    }
    return found;
}

// The address of the link right under a point (the innermost one), or empty.
std::wstring ChatWindow::LinkAt(const Geometry& g, float x, float y) const {
    const MdSpan* link = MarkdownSpanAt(g, x, y, kMdLink, nullptr);
    return link != nullptr ? link->url : std::wstring();
}

const MdSpan* ChatWindow::MarkdownSpanAt(const Geometry& g, float x, float y, unsigned flag,
                                         const FeedItem** owner) const {
    if (!Inside(g.feed, x, y)) {
        return nullptr;
    }
    for (const FeedItem& item : feed_) {
        if (item.kind != Kind::Assistant || !item.layout || y < item.top || y >= item.top + item.height) {
            continue;
        }
        const D2D1_POINT_2F origin = ItemOrigin(g, item);
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        item.layout->HitTestPoint(x - origin.x, y - origin.y, &trailing, &inside, &metrics);
        // A link has to be under the pointer itself; a code block counts anywhere on its lines (its panel).
        if (!inside && flag != kMdCode) {
            return nullptr;
        }
        const MdSpan* best = nullptr;
        for (const MdSpan& span : item.spans) {
            if ((span.flags & flag) != 0 && (flag != kMdLink || !span.url.empty()) &&
                metrics.textPosition >= span.start && metrics.textPosition < span.start + span.length &&
                (!best || span.length < best->length)) {
                best = &span;
            }
        }
        if (best != nullptr && owner != nullptr) {
            *owner = &item;
        }
        return best;
    }
    return nullptr;
}

bool ChatWindow::HasFeedSelection() const {
    return feedAnchor_ && feedFocus_ && (feedAnchor_->item != feedFocus_->item || feedAnchor_->pos != feedFocus_->pos);
}

// The selected part of item `index`, as [from, to) in its layout's text; false when none of it is selected.
bool ChatWindow::FeedSelectionIn(size_t index, UINT32& from, UINT32& to) const {
    if (!HasFeedSelection() || index >= feed_.size()) {
        return false;
    }
    FeedPoint a = *feedAnchor_;
    FeedPoint b = *feedFocus_;
    if (a.item > b.item || (a.item == b.item && a.pos > b.pos)) {
        std::swap(a, b);
    }
    if (index < a.item || index > b.item) {
        return false;
    }
    const auto size = static_cast<UINT32>(feed_[index].shown.size());
    from = index == a.item ? std::min(a.pos, size) : 0;
    to = index == b.item ? std::min(b.pos, size) : size;
    return to > from;
}

void ChatWindow::PaintFeedSelection(size_t index, D2D1_POINT_2F origin) {
    UINT32 from = 0;
    UINT32 to = 0;
    if (!FeedSelectionIn(index, from, to) || !feed_[index].layout) {
        return;
    }
    IDWriteTextLayout* layout = feed_[index].layout.get();
    UINT32 count = 0;
    layout->HitTestTextRange(from, to - from, 0, 0, nullptr, 0, &count);
    std::vector<DWRITE_HIT_TEST_METRICS> boxes(count);
    if (count == 0 || FAILED(layout->HitTestTextRange(from, to - from, 0, 0, boxes.data(), count, &count))) {
        return;
    }
    for (const DWRITE_HIT_TEST_METRICS& box : boxes) {
        Fill(D2D1::RectF(origin.x + box.left, origin.y + box.top, origin.x + box.left + std::max(box.width, 4.f),
                         origin.y + box.top + box.height),
             0.f, kAccent, 0.38f);
    }
}

std::wstring ChatWindow::FeedSelectionText() const {
    std::wstring text;
    for (size_t i = 0; i < feed_.size(); ++i) {
        UINT32 from = 0;
        UINT32 to = 0;
        if (FeedSelectionIn(i, from, to)) {
            text += (text.empty() ? L"" : L"\n\n") + feed_[i].shown.substr(from, to - from);
        }
    }
    return text;
}

bool ChatWindow::CopyFeedSelection() {
    if (!HasFeedSelection()) {
        return false;
    }
    if (SetClipboardText(hwnd_, FeedSelectionText())) {
        Flash(L"Скопировано");
    }
    return true;
}

void ChatWindow::ClearFeedSelection() {
    feedAnchor_.reset();
    feedFocus_.reset();
    feedSelecting_ = false;
}

// ---- The chat's folder: the default one, others picked before, any folder, a GitHub repository -------

void ChatWindow::LoadRepos() {
    if (reposLoading_) {
        return;
    }
    const std::string token = secrets_.Get("GITHUB").value_or("");
    if (token.empty()) {
        reposError_ = L"нет секрета GITHUB";
        return;
    }
    if (reposThread_.joinable()) {
        reposThread_.join();
    }
    reposLoading_ = true;
    const HWND hwnd = hwnd_;
    reposThread_ = std::thread([hwnd, token] {
        auto result = std::make_unique<ReposResult>();
        result->repos = ListGithubRepos(token, result->error);
        if (PostMessageW(hwnd, kRepos, 0, reinterpret_cast<LPARAM>(result.get()))) {
            result.release();
        }
    });
}

// The chats now work in `folder`: a new chat starts there, with that folder's history in the sidebar.
void ChatWindow::SwitchWorkspace(const std::wstring& folder) {
    if (folder.empty() || folder == workspace_) {
        return;
    }
    if (busy_ && MessageBoxW(hwnd_, L"Claude сейчас отвечает. Сменить папку и начать новый чат?", L"AIControls",
                             MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    workspace_ = folder;
    folderName_ = std::filesystem::path(folder).filename().wstring();
    if (folderName_.empty()) {
        folderName_ = folder;
    }
    tools_.SetCwd(folder);
    settings_.workspace = folder == DefaultWorkspace() ? std::wstring() : folder;
    std::erase(settings_.recentFolders, folder);
    settings_.recentFolders.insert(settings_.recentFolders.begin(), folder);
    if (settings_.recentFolders.size() > 8) {
        settings_.recentFolders.resize(8);
    }
    SaveSettings(settings_);
    ResetChat();
    StartClaude();
    firstRow_ = 0;
    selected_ = 0;
    RefreshSessions();
    PlaceChildren();
    Flash(L"Папка чата: " + folderName_, 2500);
    Refresh();
}

void ChatWindow::PickWorkspace() {
    wil::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
        return;
    }
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Папка, в которой работает чат");
    wil::com_ptr<IShellItem> current;
    if (SUCCEEDED(SHCreateItemFromParsingName(workspace_.c_str(), nullptr, IID_PPV_ARGS(current.put())))) {
        dialog->SetFolder(current.get());
    }
    if (FAILED(dialog->Show(hwnd_))) {
        return;  // cancelled
    }
    wil::com_ptr<IShellItem> item;
    wil::unique_cotaskmem_string path;
    if (SUCCEEDED(dialog->GetResult(item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, path.put()))) {
        SwitchWorkspace(path.get());
    }
}

void ChatWindow::OpenRepo(size_t index) {
    if (index >= repos_.size() || cloning_) {
        return;
    }
    const GithubRepo repo = repos_[index];
    const std::wstring folder = RepoFolder(repo);
    std::error_code error;
    if (std::filesystem::exists(std::filesystem::path(folder) / L".git", error)) {
        SwitchWorkspace(folder);  // cloned before: just work there
        return;
    }
    if (std::filesystem::exists(folder, error) && !std::filesystem::is_empty(folder, error)) {
        MessageBoxW(hwnd_, (L"Папка " + folder + L" уже есть, но это не клон репозитория.\nПереименуйте её или выберите её через «Выбрать папку…».").c_str(),
                    L"AIControls", MB_ICONWARNING);
        return;
    }
    const std::string token = secrets_.Get("GITHUB").value_or("");
    if (cloneThread_.joinable()) {
        cloneThread_.join();
    }
    cloning_ = true;
    cloningName_ = ToWide(repo.name);
    cloneCancel_ = false;
    const HWND hwnd = hwnd_;
    const std::atomic<bool>* cancel = &cloneCancel_;
    cloneThread_ = std::thread([hwnd, repo, folder, token, cancel] {
        auto done = std::make_unique<CloneDone>();
        done->folder = folder;
        done->name = ToWide(repo.name);
        done->ok = CloneGithubRepo(repo, folder, token, cancel, done->output);
        if (PostMessageW(hwnd, kCloneDone, 0, reinterpret_cast<LPARAM>(done.get()))) {
            done.release();
        }
    });
    Refresh();
}

void ChatWindow::OnCloneDone(const CloneDone& done) {
    cloning_ = false;
    cloningName_.clear();
    if (done.ok) {
        SwitchWorkspace(done.folder);
    } else {
        const std::wstring tail = LastLines(done.output, 3);
        Add(Kind::Error, L"Не удалось клонировать " + done.name + (tail.empty() ? L"." : L":\n" + tail));
    }
    Refresh();
}

void ChatWindow::ShowFolderMenu() {
    constexpr UINT kDefault = 2;
    constexpr UINT kPick = 3;
    constexpr UINT kReload = 4;
    constexpr UINT kRecent = 100;
    constexpr UINT kRepo = 1000;
    const std::wstring standard = DefaultWorkspace();
    wil::unique_hmenu menu(CreatePopupMenu());
    AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 1, (L"Сейчас: " + workspace_).c_str());
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING | (workspace_ == standard ? MF_CHECKED : 0), kDefault,
                (std::filesystem::path(standard).filename().wstring() + L"  (по умолчанию)").c_str());
    std::vector<std::wstring> recent;
    for (const std::wstring& folder : settings_.recentFolders) {
        std::error_code error;
        if (folder != standard && recent.size() < 8 && std::filesystem::is_directory(folder, error)) {
            recent.push_back(folder);
            AppendMenuW(menu.get(), MF_STRING | (folder == workspace_ ? MF_CHECKED : 0),
                        kRecent + static_cast<UINT>(recent.size() - 1), folder.c_str());
        }
    }
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, kPick, L"Выбрать папку…");

    wil::unique_hmenu repoMenu(CreatePopupMenu());
    if (repos_.empty()) {
        const std::wstring note = reposLoading_ ? L"Загружаю…" : (reposError_.empty() ? L"Репозиториев нет" : reposError_);
        AppendMenuW(repoMenu.get(), MF_STRING | MF_GRAYED, 0, note.c_str());
    }
    for (size_t i = 0; i < repos_.size(); ++i) {
        std::error_code error;
        const bool local = std::filesystem::exists(std::filesystem::path(RepoFolder(repos_[i])) / L".git", error);
        std::wstring label = ToWide(repos_[i].fullName);
        if (repos_[i].isPrivate) {
            label += L"  · приватный";
        }
        label += local ? L"  · склонирован" : L"";
        AppendMenuW(repoMenu.get(), MF_STRING | (RepoFolder(repos_[i]) == workspace_ ? MF_CHECKED : 0),
                    kRepo + static_cast<UINT>(i), label.c_str());
    }
    AppendMenuW(repoMenu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(repoMenu.get(), MF_STRING | (reposLoading_ ? MF_GRAYED : 0), kReload, L"Обновить список");
    AppendMenuW(menu.get(), MF_POPUP | (cloning_ ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(repoMenu.release()),
                cloning_ ? (L"Клонирую " + cloningName_ + L"…").c_str() : L"Репозитории GitHub");

    const Geometry g = Layout();
    POINT screen{ToPx(g.folder.left), ToPx(g.folder.top)};
    ClientToScreen(hwnd_, &screen);
    const auto chosen = static_cast<UINT>(TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                                         screen.x, screen.y, 0, hwnd_, nullptr));
    if (chosen == kDefault) {
        SwitchWorkspace(standard);
    } else if (chosen == kPick) {
        PickWorkspace();
    } else if (chosen == kReload) {
        repos_.clear();
        LoadRepos();
        Flash(L"Обновляю список репозиториев…");
    } else if (chosen >= kRepo && chosen - kRepo < repos_.size()) {
        OpenRepo(chosen - kRepo);
    } else if (chosen >= kRecent && chosen - kRecent < recent.size()) {
        SwitchWorkspace(recent[chosen - kRecent]);
    }
    SetFocus(input_);
}

void ChatWindow::PaintPanel(const D2D1_RECT_F& rect) {
    Fill(D2D1::RectF(rect.left + 2, rect.top + 4, rect.right + 2, rect.bottom + 6), 10.f, 0x000000, 0.35f);
    Fill(rect, 10.f, kCard);
    brush_->SetColor(Color(kLine));
    target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, 10.f, 10.f), brush_.get(), 1.f);
}

size_t ChatWindow::SidebarRows(const Geometry& g) {
    const float room = g.listBottom - g.listTop;
    return room < kHistoryRow ? 0 : static_cast<size_t>(room / kHistoryRow);
}

void ChatWindow::PaintSidebar(const Geometry& g) {
    if (g.sidebar.right <= g.sidebar.left) {
        return;
    }
    Fill(g.sidebar, 0.f, kPanel);
    Line({g.sidebar.right, g.sidebar.top}, {g.sidebar.right, g.sidebar.bottom}, kLine, 1.f);
    if (secretsOpen_) {
        PaintSecrets(g);
        return;
    }

    // Search: drawn here, so there is no blinking caret.
    Fill(g.search, 8.f, kRaised);
    if (searchFocus_) {
        brush_->SetColor(Color(kAccent, 0.75f));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(g.search, 8.f, 8.f), brush_.get(), 1.f);
    }
    {  // a magnifier
        const D2D1_POINT_2F c{g.search.left + 18.f, (g.search.top + g.search.bottom) / 2};
        brush_->SetColor(Color(kSecondary));
        target_->DrawEllipse(D2D1::Ellipse({c.x - 1.f, c.y - 1.f}, 4.5f, 4.5f), brush_.get(), 1.4f);
        Line({c.x + 2.5f, c.y + 2.5f}, {c.x + 5.5f, c.y + 5.5f}, kSecondary, 1.4f);
    }
    const D2D1_RECT_F field = D2D1::RectF(g.search.left + 34, g.search.top, g.search.right - 10, g.search.bottom);
    if (searchText_.empty()) {
        Text(L"Поиск", ui_.get(), field, kSecondary);
    } else {
        Text(searchText_, ui_.get(), field, kPrimary);
    }

    {  // a new chat: lit while the open chat is empty, as "New" is in Claude's app
        const bool active = hover_.target == Target::NewChat || !HasConversation();
        if (active) {
            Fill(g.newItem, 8.f, kRaised);
        }
        const uint32_t color = active ? kPrimary : kSecondary;
        const D2D1_POINT_2F c{g.newItem.left + 22.f, (g.newItem.top + g.newItem.bottom) / 2};
        Line({c.x - 5.5f, c.y}, {c.x + 5.5f, c.y}, color, 1.5f);
        Line({c.x, c.y - 5.5f}, {c.x, c.y + 5.5f}, color, 1.5f);
        Text(L"Новый чат", ui_.get(),
             D2D1::RectF(g.newItem.left + 42, g.newItem.top, g.newItem.right - 8, g.newItem.bottom), color);
    }

    {  // the account, at the bottom: who is signed in and the plan, or the way to sign in
        Line({g.sidebar.left, g.account.top - 4.f}, {g.sidebar.right, g.account.top - 4.f}, kLine, 1.f);
        const bool hovered = hover_.target == Target::Account;
        if (hovered) {
            Fill(g.account, 8.f, kRaised);
        }
        const D2D1_POINT_2F c{g.account.left + 22.f, (g.account.top + g.account.bottom) / 2};
        if (SignedOut()) {
            brush_->SetColor(Color(kAccent));
            target_->FillEllipse(D2D1::Ellipse(c, 12.f, 12.f), brush_.get());
            Line({c.x - 4.f, c.y}, {c.x + 4.f, c.y}, kPrimary, 1.6f);  // an arrow into the door
            Line({c.x + 1.f, c.y - 3.f}, {c.x + 4.f, c.y}, kPrimary, 1.6f);
            Line({c.x + 1.f, c.y + 3.f}, {c.x + 4.f, c.y}, kPrimary, 1.6f);
            Text(L"Войти в аккаунт", ui_.get(),
                 D2D1::RectF(g.account.left + 44, g.account.top, g.account.right - 8, g.account.bottom), kAccent);
        } else {
            // One line, as in Claude's app: "name · plan", the plan quieter.
            const std::wstring name = AccountName();
            brush_->SetColor(Color(0x7a5af8));
            target_->FillEllipse(D2D1::Ellipse(c, 10.f, 10.f), brush_.get());
            Text(std::wstring(1, static_cast<wchar_t>(std::towupper(name[0]))), badge_.get(),
                 D2D1::RectF(c.x - 10, c.y - 10, c.x + 10, c.y + 10), kPrimary);
            const std::wstring plan = AccountPlan();
            const float nameLeft = g.account.left + 40;
            const float room = g.account.right - 8 - nameLeft;
            const std::wstring tail = plan.empty() ? std::wstring() : L" · " + plan;
            const float tailWidth = tail.empty() ? 0.f : TextWidth(tail, ui_.get());
            const float nameWidth = std::min(TextWidth(name, ui_.get()), std::max(0.f, room - tailWidth));
            Text(name, ui_.get(), D2D1::RectF(nameLeft, g.account.top, nameLeft + nameWidth, g.account.bottom), kPrimary);
            if (!tail.empty()) {
                Text(tail, ui_.get(),
                     D2D1::RectF(nameLeft + nameWidth, g.account.top, nameLeft + nameWidth + tailWidth, g.account.bottom),
                     kSecondary);
            }
        }
    }

    {  // the way to the secrets
        const bool active = hover_.target == Target::Secrets;
        if (active) {
            Fill(g.secrets, 8.f, kRaised);
        }
        const uint32_t color = active ? kPrimary : kSecondary;
        const D2D1_POINT_2F c{g.secrets.left + 22.f, (g.secrets.top + g.secrets.bottom) / 2};
        brush_->SetColor(Color(color));
        target_->DrawEllipse(D2D1::Ellipse({c.x - 3.5f, c.y + 3.5f}, 3.5f, 3.5f), brush_.get(), 1.5f);
        Line({c.x - 1.f, c.y + 1.f}, {c.x + 6.5f, c.y - 6.5f}, color, 1.5f);
        Line({c.x + 3.5f, c.y - 3.f}, {c.x + 6.f, c.y - 0.5f}, color, 1.5f);
        Line({c.x + 5.f, c.y - 4.5f}, {c.x + 7.5f, c.y - 2.f}, color, 1.5f);
        Text(L"Секреты", ui_.get(),
             D2D1::RectF(g.secrets.left + 42, g.secrets.top, g.secrets.right - 8, g.secrets.bottom), color);
    }

    Text(L"Чаты", ui_.get(),
         D2D1::RectF(g.sidebar.left + 16, g.secrets.bottom + 8, g.sidebar.right - 10, g.secrets.bottom + 8 + 22),
         kSecondary);
    if (filtered_.empty()) {
        const std::wstring empty = sessions_.empty() ? L"Чатов пока нет" : L"Ничего не найдено";
        Text(empty, ui_.get(),
             D2D1::RectF(g.sidebar.left + 16, g.listTop, g.sidebar.right - 10, g.listTop + kHistoryRow), kSecondary);
        return;
    }
    firstRow_ = ClampFirst(firstRow_, filtered_.size(), SidebarRows(g));
    const size_t last = std::min(filtered_.size(), firstRow_ + SidebarRows(g));
    PaintListThumb(g.sidebar.right - 5.f, g.listTop, g.listBottom, firstRow_, SidebarRows(g), filtered_.size(),
                   Inside(g.sidebar, mouse_.x, mouse_.y));
    for (size_t row = firstRow_; row < last; ++row) {
        const SessionInfo& session = sessions_[filtered_[row]];
        const float top = g.listTop + static_cast<float>(row - firstRow_) * kHistoryRow;
        const D2D1_RECT_F r = D2D1::RectF(g.sidebar.left + 6, top, g.sidebar.right - 6, top + kHistoryRow);
        if (renaming_ == Rename::Chat && session.id == renameChat_) {
            PaintRenameField(r);
            continue;
        }
        const bool hovered = hover_.target == Target::HistoryRow && hover_.row == row;
        if (hovered || (searchFocus_ && row == selected_)) {
            Fill(r, 8.f, kRaised);
        }
        const bool current = session.id == sessionId_;
        Text(session.title.empty() ? L"Без названия" : session.title, ui_.get(),
             D2D1::RectF(r.left + 10, r.top, r.right - 8, r.bottom), current ? kAccent : kPrimary);
    }
}

void ChatWindow::PaintListThumb(float x, float top, float bottom, size_t first, size_t rows, size_t total, bool bright) {
    if (rows == 0 || total <= rows || bottom <= top) {
        return;
    }
    const float track = bottom - top;
    const float thumb = std::max(24.f, track * static_cast<float>(rows) / static_cast<float>(total));
    const float at = top + (track - thumb) * static_cast<float>(first) / static_cast<float>(total - rows);
    Fill(D2D1::RectF(x, at, x + 3.f, at + thumb), 1.5f, bright ? kSecondary : kLine);
}

void ChatWindow::SetSidebarOpen(bool open) {
    if (sidebarOpen_ == open) {
        return;
    }
    if (!open) {
        if (secretsOpen_) {
            SetSecretsOpen(false);
        }
        searchFocus_ = false;
        SetFocus(input_);
    }
    sidebarOpen_ = open;
    hover_ = {};
    PlaceChildren();  // the chat's column moves
    if (open) {
        RefreshSessions();
    }
    Refresh();
}

// Ctrl+H or a click on the search: the keys go to it until Esc, Enter or a click elsewhere.
void ChatWindow::FocusSearch() {
    ClosePopup();
    SetSidebarOpen(true);
    if (secretsOpen_) {
        SetSecretsOpen(false);
    }
    searchFocus_ = true;
    SetFocus(hwnd_);
    Refresh();
}

void ChatWindow::RefreshSessions() {
    const size_t first = firstRow_;
    const size_t selected = selected_;
    sessions_ = ListSessions(workspace_);
    FilterSessions();
    const size_t most = filtered_.empty() ? 0 : filtered_.size() - 1;
    firstRow_ = std::min(first, most);
    selected_ = std::min(selected, most);
}

void ChatWindow::OnSearchKey(WPARAM key) {
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (key == VK_ESCAPE) {
        searchFocus_ = false;
        if (!searchText_.empty()) {
            searchText_.clear();
            FilterSessions();
        }
        SetFocus(input_);
        Refresh();
    } else if (key == VK_RETURN) {
        if (selected_ < filtered_.size()) {
            searchFocus_ = false;
            OpenSession(selected_);
            SetFocus(input_);
        }
    } else if (key == VK_DOWN || key == VK_UP) {
        MoveSelection(key == VK_DOWN ? 1 : -1);
    } else if (key == 'V' && control) {
        searchText_ += ClipboardText(hwnd_);
        std::erase_if(searchText_, [](wchar_t c) { return c == L'\r' || c == L'\n'; });
        FilterSessions();
    }
}

void ChatWindow::OnSearchChar(wchar_t c) {
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
        // A tool's card copies with its whole output, not the shortened one it shows.
        if (!SetClipboardText(hwnd_, item.output.empty() ? item.text : item.text + L"\n\n" + item.output)) {
            return;
        }
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

void ChatWindow::PasteClipboard() {
    // Files copied in Explorer become attachments; a picture alone (a screenshot) too; else plain text.
    std::vector<std::wstring> files;
    if (IsClipboardFormatAvailable(CF_HDROP) && OpenClipboard(hwnd_)) {
        if (const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP)); drop != nullptr) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                std::wstring path(DragQueryFileW(drop, i, nullptr, 0), L'\0');
                DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size() + 1));
                files.push_back(std::move(path));
            }
        }
        CloseClipboard();
    }
    if (!files.empty()) {
        for (const std::wstring& path : files) {
            AttachFile(path);
        }
    } else if (!IsClipboardFormatAvailable(CF_UNICODETEXT) && ClipboardHasImage()) {
        AttachClipboardImage();
    } else {
        SendMessageW(input_, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
    }
}

void ChatWindow::ShowInputMenu(POINT screen) {
    constexpr UINT kUndo = 1;
    constexpr UINT kCut = 2;
    constexpr UINT kCopy = 3;
    constexpr UINT kPaste = 4;
    constexpr UINT kSelectAll = 5;
    CHARRANGE selection{};
    SendMessageW(input_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    const bool selected = selection.cpMax > selection.cpMin;
    const bool canPaste = IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_HDROP) ||
                          ClipboardHasImage();
    wil::unique_hmenu menu(CreatePopupMenu());
    AppendMenuW(menu.get(), MF_STRING | (SendMessageW(input_, EM_CANUNDO, 0, 0) != 0 ? 0 : MF_GRAYED), kUndo,
                L"Отменить\tCtrl+Z");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING | (selected ? 0 : MF_GRAYED), kCut, L"Вырезать\tCtrl+X");
    AppendMenuW(menu.get(), MF_STRING | (selected ? 0 : MF_GRAYED), kCopy, L"Копировать\tCtrl+C");
    AppendMenuW(menu.get(), MF_STRING | (canPaste ? 0 : MF_GRAYED), kPaste, L"Вставить\tCtrl+V");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING | (GetWindowTextLengthW(input_) > 0 ? 0 : MF_GRAYED), kSelectAll,
                L"Выделить всё\tCtrl+A");
    const UINT chosen = static_cast<UINT>(
        TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr));
    switch (chosen) {
    case kUndo:
        SendMessageW(input_, EM_UNDO, 0, 0);
        break;
    case kCut:
        SendMessageW(input_, WM_CUT, 0, 0);
        break;
    case kCopy:
        SendMessageW(input_, WM_COPY, 0, 0);
        break;
    case kPaste:
        PasteClipboard();
        break;
    case kSelectAll: {
        const CHARRANGE all{0, -1};
        SendMessageW(input_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&all));
        break;
    }
    default:
        break;
    }
    SetFocus(input_);
}

// A chat's menu in the sidebar: delete it (to the Recycle Bin, so it can still be brought back).
void ChatWindow::ShowChatMenu(size_t filteredIndex, POINT screen) {
    if (filteredIndex >= filtered_.size()) {
        return;
    }
    const SessionInfo session = sessions_[filtered_[filteredIndex]];
    wil::unique_hmenu menu(CreatePopupMenu());
    AppendMenuW(menu.get(), MF_STRING, 1, L"Открыть");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, 2, L"Удалить чат");
    InsertMenuW(menu.get(), 1, MF_BYPOSITION | MF_STRING, 3, L"Переименовать");
    const auto chosen = TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
    if (chosen == 3) {
        BeginRename(Rename::Chat, filteredIndex);
        return;
    }
    if (chosen == 1) {
        OpenSession(filteredIndex);
        SetFocus(input_);
        return;
    }
    if (chosen != 2) {
        return;
    }
    const std::wstring title = session.title.empty() ? L"без названия" : session.title;
    const std::wstring question = L"Удалить чат «" + title + L"»?\nОн уйдёт в корзину Windows, оттуда его можно вернуть.";
    if (MessageBoxW(hwnd_, question.c_str(), L"AIControls", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    const bool current = session.id == sessionId_;
    if (current) {  // the CLI holds the file open: it goes first
        ResetChat();
    }
    if (DeleteSession(workspace_, session.id)) {
        Flash(L"Чат удалён");
    } else {
        Flash(L"Не удалось удалить чат", 3000);
    }
    if (current) {
        StartClaude();
    }
    RefreshSessions();
    hover_ = {};
    Refresh();
}

// The feed's menu: what is under the pointer can be taken: a selection, a link, a code block or table,
// the whole message.
void ChatWindow::ShowCopyMenu(POINT screen, D2D1_POINT_2F at) {
    constexpr UINT kSelection = 1, kOpenLink = 2, kCopyLink = 3, kCopyCode = 4, kCopyMessage = 5, kReveal = 6,
                   kPreview = 7;
    const Geometry g = Layout();
    const float y = at.y;
    const FeedItem* codeOwner = nullptr;
    const MdSpan* link = MarkdownSpanAt(g, at.x, at.y, kMdLink, nullptr);
    const MdSpan* code = MarkdownSpanAt(g, at.x, at.y, kMdCode, &codeOwner);
    const bool onMessage = std::ranges::any_of(feed_, [y](const FeedItem& item) {
        return y >= item.top && y < item.top + item.height && !item.text.empty();
    });
    wil::unique_hmenu menu(CreatePopupMenu());
    const auto separate = [&] {
        if (GetMenuItemCount(menu.get()) > 0) {
            AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        }
    };
    if (HasFeedSelection()) {
        AppendMenuW(menu.get(), MF_STRING, kSelection, L"Копировать выделенное\tCtrl+C");
    }
    const bool path = link != nullptr && IsLocalPath(link->url);
    if (path) {  // a file on this computer
        separate();
        if (IsPreviewable(link->url)) {
            AppendMenuW(menu.get(), MF_STRING, kPreview, L"Превью");
        }
        AppendMenuW(menu.get(), MF_STRING, kOpenLink, IsPreviewable(link->url) ? L"Открыть в браузере" : L"Открыть");
        AppendMenuW(menu.get(), MF_STRING, kReveal, L"Показать в папке");
        AppendMenuW(menu.get(), MF_STRING, kCopyLink, L"Копировать путь");
    } else if (link != nullptr && !link->url.empty()) {
        separate();
        AppendMenuW(menu.get(), MF_STRING, kOpenLink, L"Открыть ссылку");
        AppendMenuW(menu.get(), MF_STRING, kCopyLink, L"Копировать ссылку");
    }
    if (code != nullptr && codeOwner != nullptr) {
        separate();
        AppendMenuW(menu.get(), MF_STRING, kCopyCode,
                    (code->flags & kMdTable) != 0 ? L"Копировать таблицу" : L"Копировать код");
    }
    if (onMessage) {
        separate();
        AppendMenuW(menu.get(), MF_STRING, kCopyMessage, L"Копировать сообщение");
    }
    if (GetMenuItemCount(menu.get()) == 0) {
        return;
    }
    switch (TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr)) {
    case kSelection:
        CopyFeedSelection();
        break;
    case kOpenLink:
        if (path) {
            OpenFile(link->url);
        } else if (link != nullptr && (link->url.starts_with(L"http://") || link->url.starts_with(L"https://") ||
                                       link->url.starts_with(L"mailto:"))) {
            ShellExecuteW(hwnd_, L"open", link->url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    case kPreview:
        if (path) {
            OpenPreview(link->url);
        }
        break;
    case kReveal:
        if (const std::wstring file = path ? ExistingPath(link->url) : std::wstring(); !file.empty()) {
            RevealInFolder(file);
        } else {
            Flash(L"Нет такого файла", 3000);
        }
        break;
    case kCopyLink:
        if (link != nullptr && SetClipboardText(hwnd_, link->url)) {
            Flash(L"Ссылка скопирована");
        }
        break;
    case kCopyCode:
        if (code != nullptr && codeOwner != nullptr && SetClipboardText(hwnd_, codeOwner->shown.substr(code->start, code->length))) {
            Flash((code->flags & kMdTable) != 0 ? L"Таблица скопирована" : L"Код скопирован");
        }
        break;
    case kCopyMessage:
        CopyItemAt(y);
        break;
    default:
        break;
    }
}

// ---- The preview -----------------------------------------------------------------------------------

ChatWindow::TermTab* ChatWindow::PreviewTab() const {
    for (const auto& tab : tabs_) {
        if (!tab->preview.empty()) {
            return tab.get();
        }
    }
    return nullptr;
}

void ChatWindow::OpenPreview(const std::wstring& path) {
    const std::wstring file = ExistingPath(path);
    if (file.empty()) {
        Flash(L"Нет такого файла: " + path, 4000);
        return;
    }
    TermTab* tab = PreviewTab();
    if (tab == nullptr) {
        auto created = std::make_unique<TermTab>();
        created->id = ++nextTabId_;
        tabs_.push_back(std::move(created));
        tab = tabs_.back().get();
    }
    tab->preview = file;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].get() == tab) {
            activeTab_ = i;
        }
    }
    selAnchor_.reset();
    selEnd_.reset();
    SetTerminalOpen(true);
    termFocus_ = false;  // typing stays in the chat
    SetFocus(input_);
    EnsureWebView();
    if (webShown_ != file) {
        NavigatePreview();
    }
    PlacePreview();
    SetTimer(hwnd_, kPreviewTimer, kPreviewCheck, nullptr);
    Refresh();
}

void ChatWindow::EnsureWebView() {
    if (webController_ || webCreating_) {
        return;
    }
    webCreating_ = true;
    const auto failed = [this] {
        webCreating_ = false;
        Flash(L"Превью не запустилось: нужен WebView2 (Microsoft Edge)", 5000);
    };
    wchar_t local[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const std::wstring data = (std::filesystem::path(local) / L"AIControls" / L"WebView2").wstring();
    const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, failed](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || environment == nullptr) {
                    failed();
                    return S_OK;
                }
                const HRESULT creating = environment->CreateCoreWebView2Controller(
                    hwnd_, Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                               [this, failed](HRESULT created, ICoreWebView2Controller* controller) -> HRESULT {
                                   if (FAILED(created) || controller == nullptr) {
                                       failed();
                                       return S_OK;
                                   }
                                   webCreating_ = false;
                                   webController_ = controller;
                                   webController_->get_CoreWebView2(webView_.put());
                                   if (const auto colored = webController_.try_query<ICoreWebView2Controller2>()) {
                                       colored->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 0x12, 0x12, 0x13});
                                   }
                                   wil::com_ptr<ICoreWebView2Settings> settings;
                                   if (webView_ && SUCCEEDED(webView_->get_Settings(settings.put()))) {
                                       settings->put_IsStatusBarEnabled(FALSE);
                                   }
                                   if (webView_) {
                                       webView_->AddScriptToExecuteOnDocumentCreated(kPickScript, nullptr);
                                       EventRegistrationToken token{};
                                       webView_->add_WebMessageReceived(
                                           Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                               [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
                                                   wil::unique_cotaskmem_string message;
                                                   if (SUCCEEDED(args->TryGetWebMessageAsString(&message)) && message) {
                                                       OnPreviewPick(message.get());
                                                   }
                                                   return S_OK;
                                               })
                                               .Get(),
                                           &token);
                                   }
                                   webShown_.clear();
                                   NavigatePreview();
                                   PlacePreview();
                                   return S_OK;
                               })
                               .Get());
                if (FAILED(creating)) {
                    failed();
                }
                return S_OK;
            })
            .Get());
    if (FAILED(started)) {
        failed();
    }
}

void ChatWindow::NavigatePreview() {
    const TermTab* tab = PreviewTab();
    if (!webView_ || tab == nullptr) {
        return;
    }
    std::wstring url(4096, L'\0');
    DWORD length = static_cast<DWORD>(url.size());
    if (FAILED(UrlCreateFromPathW(tab->preview.c_str(), url.data(), &length, 0))) {
        return;
    }
    url.resize(length);
    webView_->Navigate(url.c_str());
    webShown_ = tab->preview;
    previewStamp_ = FolderStamp(tab->preview);
}

// The page covers the panel under the tabs while its tab is the active one (and no picture is over the window).
void ChatWindow::PlacePreview() {
    if (!webController_) {
        return;
    }
    const bool shown = PreviewShown() && popup_ != Popup::Image;
    if (shown) {
        const Geometry g = Layout();
        const RECT bounds{ToPx(g.term.left + 1.f), ToPx(g.termScreen.top), ToPx(g.term.right), ToPx(g.term.bottom)};
        webController_->put_Bounds(bounds);
    }
    webController_->put_IsVisible(shown ? TRUE : FALSE);
}

void ChatWindow::CheckPreview() {
    const TermTab* tab = PreviewTab();
    if (tab == nullptr) {
        KillTimer(hwnd_, kPreviewTimer);
        return;
    }
    if (!webView_ || !PreviewShown()) {
        return;  // looked at again once it shows
    }
    if (webShown_ != tab->preview) {
        NavigatePreview();
    } else if (const auto stamp = FolderStamp(tab->preview); stamp != previewStamp_) {
        previewStamp_ = stamp;
        webView_->Reload();
    }
}

void ChatWindow::ShowPreviewMenu(size_t index, POINT screen) {
    const std::wstring page = tabs_[index]->preview;
    constexpr UINT kReload = 1, kBrowser = 2, kReveal = 3, kCopyPath = 4, kClose = 5;
    wil::unique_hmenu menu(CreatePopupMenu());
    AppendMenuW(menu.get(), MF_STRING, kReload, L"Обновить");
    AppendMenuW(menu.get(), MF_STRING, kBrowser, L"Открыть в браузере");
    AppendMenuW(menu.get(), MF_STRING, kReveal, L"Показать в папке");
    AppendMenuW(menu.get(), MF_STRING, kCopyPath, L"Копировать путь");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, kClose, L"Закрыть");
    switch (TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr)) {
    case kReload:
        if (webView_) {
            previewStamp_ = FolderStamp(page);
            webView_->Reload();
        }
        break;
    case kBrowser:
        OpenFile(page);
        break;
    case kReveal:
        RevealInFolder(page);
        break;
    case kCopyPath:
        if (SetClipboardText(hwnd_, page)) {
            Flash(L"Путь скопирован");
        }
        break;
    case kClose:
        CloseTerminalTab(index);
        break;
    default:
        break;
    }
}

void ChatWindow::OnPreviewPick(const std::wstring& message) {
    const Json pick = Json::parse(ToUtf8(message), nullptr, false);
    const TermTab* tab = PreviewTab();
    if (pick.is_discarded() || tab == nullptr) {
        return;
    }
    // "[mockup.html: div.card > button.edit «Изменить»] ": enough for the model to find it in the file.
    std::wstring reference = L"[" + std::filesystem::path(tab->preview).filename().wstring() + L": " +
                             ToWide(JsonStr(pick, "selector"));
    if (const std::wstring text = ToWide(JsonStr(pick, "text")); !text.empty()) {
        reference += L" «" + text + L"»";
    }
    reference += L"] ";
    termFocus_ = false;
    SetFocus(input_);
    SendMessageW(input_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(reference.c_str()));
    Flash(L"Элемент в сообщении: допиши, что с ним сделать");
}

void ChatWindow::CheckAppUpdate() {
    std::optional<AppUpdate> found = FindAppUpdate(AppUpdateFolders(), InstalledAppVersion());
    const bool changed = found.has_value() != update_.has_value() || (found && found->version != update_->version);
    update_ = std::move(found);
    if (changed) {
        hover_ = {};
        Refresh();
    }
}

void ChatWindow::StartUpdate() {
    if (!update_) {
        return;
    }
    if (busy_) {  // the answer would be cut off: the update waits for the turn's end
        updatePending_ = true;
        Flash(L"Обновлю, как только Claude закончит ответ", 4000);
        return;
    }
    updatePending_ = false;
    settings_.reopen = sessionId_;
    SaveSettings(settings_);
    // Silent, and /RELAUNCH brings AIControls back once the files are in (installer\aicontrols.iss).
    const auto started = reinterpret_cast<INT_PTR>(ShellExecuteW(
        hwnd_, L"open", update_->installer.c_str(), L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /RELAUNCH", nullptr,
        SW_SHOWNORMAL));
    if (started <= 32) {
        settings_.reopen.clear();
        SaveSettings(settings_);
        Flash(L"Не запустился установщик: " + update_->installer, 5000);
        return;
    }
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);  // the installer replaces this exe: out of its way
}

void ChatWindow::SetPreviewWide(bool wide) {
    if (wide == previewWide_) {
        return;
    }
    previewWide_ = wide;
    if (wide) {
        wideSavedTerminal_ = terminalWidth_;
        wideSavedSidebar_ = sidebarOpen_;
        terminalWidth_ = 1e5f;  // the layout leaves the chat its least
        SetSidebarOpen(false);
    } else {
        terminalWidth_ = wideSavedTerminal_;
        SetSidebarOpen(wideSavedSidebar_);
    }
    hover_ = {};
    PlaceChildren();
    Refresh();
}

// ---- Pictures in the feed --------------------------------------------------------------------------

FeedImage ChatWindow::MakeFeedImage(std::wstring path) const {
    FeedImage image;
    UINT width = 0;
    UINT height = 0;
    image.preview = LoadImagePreview(wic_.get(), path, kPreviewSide, width, height);
    image.path = std::move(path);
    if (image.preview && width > 0 && height > 0) {
        const float scale = std::min({1.f, kImageWidth / static_cast<float>(width), kImageHeight / static_cast<float>(height)});
        image.width = static_cast<float>(width) * scale;
        image.height = static_cast<float>(height) * scale;
    }
    return image;
}

D2D1_RECT_F ChatWindow::ImageRect(const Geometry& g, const FeedItem& item, size_t index) const {
    const D2D1_RECT_F& at = item.images[index].at;
    return D2D1::RectF(g.left + at.left, item.top + at.top, g.left + at.right, item.top + at.bottom);
}

ID2D1Bitmap* ChatWindow::Bitmap(FeedImage& image) {
    if (!image.preview) {
        return nullptr;
    }
    if (!image.bitmap || image.generation != targetGeneration_) {
        image.bitmap.reset();
        if (FAILED(target_->CreateBitmapFromWicBitmap(image.preview.get(), nullptr, image.bitmap.put()))) {
            return nullptr;
        }
        image.generation = targetGeneration_;
    }
    return image.bitmap.get();
}

void ChatWindow::PaintImages(const Geometry& g, FeedItem& item) {
    const size_t index = static_cast<size_t>(&item - feed_.data());
    for (size_t k = 0; k < item.images.size(); ++k) {
        const D2D1_RECT_F rect = ImageRect(g, item, k);
        if (rect.bottom < g.feed.top || rect.top > g.feed.bottom) {
            continue;
        }
        if (ID2D1Bitmap* bitmap = Bitmap(item.images[k]); bitmap != nullptr) {
            target_->DrawBitmap(bitmap, rect, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        const bool hovered = hover_.target == Target::FeedImage && hover_.row == index && hover_.sub == k;
        brush_->SetColor(Color(hovered ? kAccent : kLine));
        target_->DrawRectangle(rect, brush_.get(), 1.f);
    }
}

void ChatWindow::OpenViewer(size_t item, size_t image) {
    if (item >= feed_.size() || image >= feed_[item].images.size()) {
        return;
    }
    if (popup_ != Popup::Image) {
        HideTip();
        OpenPopup(Popup::Image);
    }
    viewerItem_ = item;
    viewerImage_ = image;
    viewer_ = {};
    viewer_.path = feed_[item].images[image].path;
    UINT width = 0;
    UINT height = 0;
    viewer_.preview = LoadImagePreview(wic_.get(), viewer_.path, kViewerSide, width, height);
    viewer_.width = static_cast<float>(width);  // here: the picture's own pixels
    viewer_.height = static_cast<float>(height);
    if (!viewer_.preview) {
        const std::wstring path = viewer_.path;
        ClosePopup();
        Flash(L"Не открывается картинка: " + path, 4000);
        return;
    }
    Refresh();
}

// The arrows: the picture before or after this one, across all messages.
void ChatWindow::StepViewer(int delta) {
    std::vector<std::pair<size_t, size_t>> all;
    size_t at = 0;
    for (size_t i = 0; i < feed_.size(); ++i) {
        for (size_t k = 0; k < feed_[i].images.size(); ++k) {
            if (i == viewerItem_ && k == viewerImage_) {
                at = all.size();
            }
            all.emplace_back(i, k);
        }
    }
    const long long next = static_cast<long long>(at) + delta;
    if (next >= 0 && next < static_cast<long long>(all.size())) {
        OpenViewer(all[static_cast<size_t>(next)].first, all[static_cast<size_t>(next)].second);
    }
}

void ChatWindow::PaintViewer(const Geometry& g) {
    Fill(D2D1::RectF(0, 0, g.width, g.height), 0.f, 0x000000, 0.9f);
    constexpr float kMargin = 28.f;
    constexpr float kBar = 44.f;
    if (ID2D1Bitmap* bitmap = Bitmap(viewer_); bitmap != nullptr) {
        const float natural = 96.f / Dpi();  // a pixel of the picture as one of the screen
        const float width = viewer_.width * natural;
        const float height = viewer_.height * natural;
        const float scale = std::min({1.f, (g.width - 2 * kMargin) / width, (g.height - 2 * kMargin - kBar) / height});
        const float w = width * scale;
        const float h = height * scale;
        const float x = (g.width - w) / 2;
        const float y = kMargin + (g.height - 2 * kMargin - kBar - h) / 2;
        target_->DrawBitmap(bitmap, D2D1::RectF(x, y, x + w, y + h), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    size_t total = 0;
    size_t number = 0;
    for (size_t i = 0; i < feed_.size(); ++i) {
        for (size_t k = 0; k < feed_[i].images.size(); ++k) {
            ++total;
            if (i == viewerItem_ && k == viewerImage_) {
                number = total;
            }
        }
    }
    std::wstring line = std::filesystem::path(viewer_.path).filename().wstring();
    if (total > 1) {
        line += std::format(L"   {} из {}   ← →", number, total);
    }
    line += L"   ·   Ctrl+C — копировать   ·   Enter — открыть в программе   ·   Esc — закрыть";
    Text(line, button_.get(), D2D1::RectF(kMargin, g.height - kMargin - kBar + 12.f, g.width - kMargin, g.height - kMargin),
         kSecondary);
}

void ChatWindow::ShowImageMenu(const std::wstring& path, POINT screen) {
    constexpr UINT kCopy = 1, kOpen = 2, kReveal = 3, kCopyPath = 4;
    wil::unique_hmenu menu(CreatePopupMenu());
    AppendMenuW(menu.get(), MF_STRING, kCopy, L"Копировать картинку\tCtrl+C");
    AppendMenuW(menu.get(), MF_STRING, kOpen, L"Открыть в программе");
    AppendMenuW(menu.get(), MF_STRING, kReveal, L"Показать в папке");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, kCopyPath, L"Копировать путь");
    switch (TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr)) {
    case kCopy:
        CopyImage(path);
        break;
    case kOpen:
        OpenFile(path);
        break;
    case kReveal:
        RevealInFolder(path);
        break;
    case kCopyPath:
        if (SetClipboardText(hwnd_, path)) {
            Flash(L"Путь скопирован");
        }
        break;
    default:
        break;
    }
}

void ChatWindow::CopyImage(const std::wstring& path) {
    Flash(CopyImageToClipboard(wic_.get(), hwnd_, path) ? L"Картинка скопирована" : L"Не удалось скопировать картинку");
}

void ChatWindow::OpenFile(const std::wstring& path) {
    const std::wstring file = ExistingPath(path);
    if (file.empty()) {
        Flash(L"Нет такого файла: " + path, 4000);
        return;
    }
    // A file no app is set for gets Windows' "open with".
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        ShellExecuteW(hwnd_, L"openas", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ---- Hints -----------------------------------------------------------------------------------------

std::wstring ChatWindow::TooltipText() {
    switch (hover_.target) {
    case Target::SidebarToggle:
        return sidebarOpen_ ? L"Скрыть боковую панель" : L"Показать боковую панель";
    case Target::TerminalToggle:
        return terminalOpen_ ? L"Скрыть терминал" : L"Показать терминал";
    case Target::WinMin:
        return L"Свернуть";
    case Target::WinMax:
        return IsZoomed(hwnd_) ? L"Восстановить" : L"Развернуть";
    case Target::WinClose:
        return L"Закрыть";
    case Target::SidebarSearch:
        return L"Поиск по чатам (Ctrl+H)";
    case Target::FolderChip:
        return L"Папка чата: " + workspace_;
    case Target::Account:
        return ClaudeUpdateAvailable() ? L"Аккаунт · вышел Claude " + ToWide(claudeLatest_)
                                       : std::wstring(L"Аккаунт, версия Claude, выход");
    case Target::TermAdd:
        return L"Новый терминал";
    case Target::TermClose:
        return L"Скрыть терминал";
    case Target::TermTabClose:
        return L"Закрыть вкладку";
    case Target::Update:
        return L"Поставить AIControls " + (update_ ? ToWide(update_->version) : std::wstring()) +
               L" и перезапустить. Этот чат откроется снова";
    case Target::PreviewWide:
        return previewWide_ ? L"Вернуть как было" : L"Превью на всё окно (чат остаётся полосой слева)";
    case Target::TermTab:
        if (hover_.row < tabs_.size() && !tabs_[hover_.row]->preview.empty()) {
            return L"Ctrl+клик по элементу страницы — вставить его в сообщение. Правая кнопка — меню";
        }
        return L"Правый клик: переименовать, очистить";
    case Target::Attach:
        return L"Прикрепить файлы или фото (Ctrl+U)";
    case Target::AttachmentChip:
        return hover_.row < attachments_.size() ? attachments_[hover_.row].name : std::wstring();
    case Target::RemoveAttachment:
        return L"Убрать";
    case Target::FeedImage:
        return L"Открыть здесь. Правая кнопка: копировать, открыть в программе, показать в папке";
    case Target::CardPreview:
    case Target::CardOpen:
    case Target::CardReveal:
        return hover_.row < feed_.size() ? WrittenFile(feed_[hover_.row], workspace_) : std::wstring();
    case Target::ModelChip:
        return L"Модель";
    case Target::EffortChip:
        return L"Усилие: насколько глубоко Claude думает";
    case Target::UsageChip:
        return L"Лимиты использования";
    case Target::SecretsClose:
        return L"Назад к чатам";
    case Target::SecretEye:
        return showValue_ ? L"Скрыть значение" : L"Показать значение";
    case Target::SecretDelete:
        return L"Удалить секрет";
    case Target::EffortReset:
        return L"Вернуть как было";
    case Target::Send:
        if (busy_ && !hasContent()) {
            return L"Остановить ответ (Esc)";
        }
        return busy_ ? L"Отправить сейчас: Claude учтёт по ходу (Enter)" : L"Отправить (Enter)";
    case Target::HistoryRow: {  // the whole title, when the row cuts it
        if (hover_.row >= filtered_.size()) {
            return {};
        }
        const SessionInfo& session = sessions_[filtered_[hover_.row]];
        const Geometry g = Layout();
        const float room = (g.sidebar.right - 14.f) - (g.sidebar.left + 16.f);
        return !session.title.empty() && TextWidth(session.title, ui_.get()) > room ? session.title : std::wstring();
    }
    case Target::None: {  // a link: where it goes
        std::wstring url = LinkAt(Layout(), mouse_.x, mouse_.y);
        if (url.size() > 140) {
            url = url.substr(0, 137) + L"…";
        }
        return url;
    }
    default:
        return {};
    }
}

void ChatWindow::UpdateTip() {
    std::wstring text;
    if (popup_ == Popup::None && dragging_ == Target::None && !feedSelecting_ && !selecting_ &&
        renaming_ == Rename::None) {
        text = TooltipText();
    }
    if (text == tipText_) {
        return;
    }
    tipText_ = std::move(text);
    KillTimer(hwnd_, kTipTimer);
    if (tipShown_) {
        tipShown_ = false;
        ShowWindow(tip_, SW_HIDE);
    }
    if (!tipText_.empty()) {
        SetTimer(hwnd_, kTipTimer, 600, nullptr);
    }
}

// Gone until the pointer moves to something else (a click or a key does not bring it back on the same spot).
void ChatWindow::HideTip() {
    KillTimer(hwnd_, kTipTimer);
    if (tipShown_) {
        tipShown_ = false;
        ShowWindow(tip_, SW_HIDE);
    }
}

// A window of its own, over everything: drawn into the main one, the hint went under the input box and the
// preview (child windows of their own).
LRESULT CALLBACK ChatWindow::TipProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<ChatWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;  // the pointer is the main window's: the hint is only shown
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        if (self != nullptr) {
            self->PaintTip();
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void ChatWindow::ShowTip() {
    if (tipText_.empty() || popup_ != Popup::None) {
        return;
    }
    if (tip_ == nullptr) {
        WNDCLASSEXW tipClass{};
        tipClass.cbSize = sizeof(tipClass);
        tipClass.lpfnWndProc = &ChatWindow::TipProc;
        tipClass.hInstance = instance_;
        tipClass.lpszClassName = L"AIControlsTip";
        RegisterClassExW(&tipClass);
        tip_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"AIControlsTip", L"", WS_POPUP,
                               0, 0, 1, 1, hwnd_, nullptr, instance_, nullptr);
        if (tip_ == nullptr) {
            return;
        }
        SetWindowLongPtrW(tip_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUNDSMALL;
        DwmSetWindowAttribute(tip_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        const COLORREF border = ToColorRef(kLine);
        DwmSetWindowAttribute(tip_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    }
    // Laid out once and drawn as laid out; a long hint wraps at most this wide.
    const Geometry g = Layout();
    const float most = std::max(80.f, std::min(420.f, g.width - 16.f - 2 * kTipPadX));
    tipLayout_.reset();
    if (FAILED(dwrite_->CreateTextLayout(tipText_.c_str(), static_cast<UINT32>(tipText_.size()), small_.get(), most,
                                         1000.f, tipLayout_.put()))) {
        return;
    }
    DWRITE_TEXT_METRICS metrics{};
    tipLayout_->GetMetrics(&metrics);
    const float textWidth = std::ceil(metrics.widthIncludingTrailingWhitespace) + 1.f;
    tipLayout_->SetMaxWidth(textWidth);  // the box hugs the text
    const float width = textWidth + 2 * kTipPadX;
    const float height = std::ceil(metrics.height) + 2 * kTipPadY;
    const float left = std::clamp(tipAt_.x - width / 2, 8.f, std::max(8.f, g.width - width - 8.f));
    float top = tipAt_.y + 22.f;  // under the pointer, or above it at the window's bottom
    if (top + height > g.height - 6.f) {
        top = tipAt_.y - height - 10.f;
    }
    POINT at{ToPx(left), ToPx(top)};
    ClientToScreen(hwnd_, &at);
    const int widthPx = ToPx(width);
    const int heightPx = ToPx(height);
    SetWindowPos(tip_, HWND_TOP, at.x, at.y, widthPx, heightPx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (tipTarget_) {
        tipTarget_->Resize(D2D1::SizeU(static_cast<UINT32>(widthPx), static_cast<UINT32>(heightPx)));
    } else {
        D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties();
        properties.dpiX = properties.dpiY = Dpi();
        d2d_->CreateHwndRenderTarget(
            properties,
            D2D1::HwndRenderTargetProperties(tip_, D2D1::SizeU(static_cast<UINT32>(widthPx), static_cast<UINT32>(heightPx))),
            tipTarget_.put());
    }
    tipShown_ = true;
    InvalidateRect(tip_, nullptr, FALSE);
}

void ChatWindow::PaintTip() {
    if (!tipTarget_ || !tipLayout_) {
        return;
    }
    tipTarget_->SetDpi(Dpi(), Dpi());
    tipTarget_->BeginDraw();
    tipTarget_->Clear(Color(kRaised));
    wil::com_ptr<ID2D1SolidColorBrush> brush;
    tipTarget_->CreateSolidColorBrush(Color(kPrimary), brush.put());
    if (brush) {
        tipTarget_->DrawTextLayout({kTipPadX, kTipPadY}, tipLayout_.get(), brush.get());
    }
    if (tipTarget_->EndDraw() == D2DERR_RECREATE_TARGET) {
        tipTarget_.reset();
    }
}

// ---- Renaming in place --------------------------------------------------------------------------------

void ChatWindow::BeginRename(Rename what, size_t index) {
    if (what == Rename::Tab) {
        if (index >= tabs_.size()) {
            return;
        }
        renameTab_ = tabs_[index]->id;
        renameText_ = TabLabel(*tabs_[index]);
        SetTerminalOpen(true);
    } else if (what == Rename::Chat) {
        if (index >= filtered_.size()) {
            return;
        }
        const SessionInfo& session = sessions_[filtered_[index]];
        renameChat_ = session.id;
        renameText_ = session.title;
        if (secretsOpen_) {
            SetSecretsOpen(false);
        }
        SetSidebarOpen(true);
        const size_t rows = std::max<size_t>(1, SidebarRows(Layout()));  // the row in view
        if (index < firstRow_) {
            firstRow_ = index;
        } else if (index >= firstRow_ + rows) {
            firstRow_ = index + 1 - rows;
        }
    } else {
        return;
    }
    renaming_ = what;
    searchFocus_ = false;
    termFocus_ = false;
    HideTip();
    SetFocus(hwnd_);
    Refresh();
}

void ChatWindow::CommitRename() {
    const Rename what = renaming_;
    renaming_ = Rename::None;
    std::wstring text = renameText_;
    renameText_.clear();
    const size_t first = text.find_first_not_of(L" \t");
    text = first == std::wstring::npos ? L"" : text.substr(first, text.find_last_not_of(L" \t") - first + 1);
    if (what == Rename::Tab) {
        if (TermTab* tab = TabById(renameTab_)) {
            tab->name.clear();  // empty, or the default label typed back: the numbered default
            if (!text.empty() && text != TabLabel(*tab)) {
                tab->name = text;
            }
        }
    } else if (what == Rename::Chat && !text.empty()) {
        if (RenameSession(workspace_, renameChat_, text)) {
            if (renameChat_ == sessionId_) {
                title_ = text;
            }
            RefreshSessions();
        } else {
            Flash(L"Не удалось переименовать чат", 3000);
        }
    }
    SetFocus(input_);
    Refresh();
}

void ChatWindow::CancelRename() {
    renaming_ = Rename::None;
    renameText_.clear();
    SetFocus(input_);
    Refresh();
}

void ChatWindow::OnRenameKey(WPARAM key) {
    if (key == VK_RETURN) {
        CommitRename();
    } else if (key == VK_ESCAPE) {
        CancelRename();
    } else if (key == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) != 0 && OpenClipboard(hwnd_)) {
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
                std::wstring pasted(text);
                GlobalUnlock(data);
                pasted = pasted.substr(0, pasted.find_first_of(L"\r\n"));
                renameText_ += pasted.substr(0, 100 - std::min<size_t>(100, renameText_.size()));
            }
        }
        CloseClipboard();
        Refresh();
    }
}

void ChatWindow::OnRenameChar(wchar_t c) {
    if (c == L'\b') {
        if (!renameText_.empty()) {
            renameText_.pop_back();
        }
    } else if (c == 0x7f) {  // Ctrl+Backspace
        renameText_.clear();
    } else if (c >= L' ' && renameText_.size() < 100) {
        renameText_ += c;
    } else {
        return;
    }
    Refresh();
}

// The field drawn in place of a tab's or a chat's label: the end of the text stays in view, a still caret.
void ChatWindow::PaintRenameField(const D2D1_RECT_F& r) {
    Fill(r, 6.f, kWindow);
    brush_->SetColor(Color(kAccent));
    target_->DrawRoundedRectangle(D2D1::RoundedRect(r, 6.f, 6.f), brush_.get(), 1.f);
    const float room = r.right - r.left - 20.f;
    std::wstring shown = renameText_;
    while (!shown.empty() && TextWidth(shown, ui_.get()) > room) {
        shown.erase(0, 1);
    }
    Text(shown, ui_.get(), D2D1::RectF(r.left + 8, r.top, r.right - 8, r.bottom), kPrimary);
    const float caret = r.left + 9.f + TextWidth(shown, ui_.get());
    Line({caret, r.top + 7.f}, {caret, r.bottom - 7.f}, kAccent, 1.2f);
}

// ---- Right-click menus --------------------------------------------------------------------------------

void ChatWindow::ShowTabMenu(size_t index, POINT screen) {
    if (index >= tabs_.size()) {
        return;
    }
    if (!tabs_[index]->preview.empty()) {
        ShowPreviewMenu(index, screen);
        return;
    }
    constexpr UINT kRename = 1, kClear = 2, kNew = 3, kClose = 4;
    wil::unique_hmenu menu(CreatePopupMenu());
    AppendMenuW(menu.get(), MF_STRING, kRename, L"Переименовать");
    AppendMenuW(menu.get(), MF_STRING, kClear, L"Очистить");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, kNew, L"Новый терминал");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, kClose, L"Закрыть");
    switch (TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr)) {
    case kRename:
        BeginRename(Rename::Tab, index);
        break;
    case kClear:
        ClearTerminalTab(index);
        break;
    case kNew:
        AddTerminalTab();
        break;
    case kClose:
        CloseTerminalTab(index);
        if (terminalOpen_) {
            SelectTerminalTab(activeTab_);
        }
        break;
    default:
        break;
    }
}

// As Windows Terminal's "clear": the scrollback goes, and the shell redraws its prompt at the top (Ctrl+L).
void ChatWindow::ClearTerminalTab(size_t index) {
    if (index >= tabs_.size()) {
        return;
    }
    SelectTerminalTab(index);
    TermTab& tab = *tabs_[index];
    tab.scroll = 0;
    if (tab.screen) {
        tab.screen->ClearScrollback();
    }
    if (!tab.exited && tab.pty) {
        TermSend("\x0c");
    } else if (tab.screen) {
        tab.screen->Feed("\x1b[2J\x1b[H");
    }
    Refresh();
}

void ChatWindow::ShowSecretMenu(size_t index, POINT screen) {
    const std::vector<std::string> names = secrets_.Names();
    if (index >= names.size()) {
        return;
    }
    constexpr UINT kEdit = 1, kCopyName = 2, kDelete = 3;
    wil::unique_hmenu menu(CreatePopupMenu());
    AppendMenuW(menu.get(), MF_STRING, kEdit, L"Изменить");
    AppendMenuW(menu.get(), MF_STRING, kCopyName, L"Копировать имя");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, kDelete, L"Удалить");
    switch (TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr)) {
    case kEdit:
        EditSecret(names[index]);
        break;
    case kCopyName:
        if (SetClipboardText(hwnd_, ToWide(names[index]))) {
            Flash(L"Имя скопировано");
        }
        break;
    case kDelete:
        DeleteSecret(names[index]);
        break;
    default:
        break;
    }
}

void ChatWindow::ShowAttachmentMenu(size_t index, POINT screen) {
    if (index >= attachments_.size()) {
        return;
    }
    wil::unique_hmenu menu(CreatePopupMenu());
    AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 2, attachments_[index].name.c_str());
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, 1, L"Убрать");
    if (TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr) == 1) {
        attachments_.erase(attachments_.begin() + static_cast<std::ptrdiff_t>(index));
        hover_ = {};
        Attached();
    }
}

// The chat's title in the header (and the empty part of the chats' list): rename it, or start a new one.
void ChatWindow::ShowTitleMenu(POINT screen) {
    constexpr UINT kRename = 1, kNew = 2;
    wil::unique_hmenu menu(CreatePopupMenu());
    AppendMenuW(menu.get(), MF_STRING | (sessionId_.empty() ? MF_GRAYED : 0), kRename, L"Переименовать чат");
    AppendMenuW(menu.get(), MF_STRING, kNew, L"Новый чат");
    switch (TrackPopupMenu(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr)) {
    case kRename: {
        const auto find = [&] {
            return std::ranges::find_if(filtered_, [&](size_t i) { return sessions_[i].id == sessionId_; });
        };
        RefreshSessions();
        auto at = find();
        if (at == filtered_.end() && !searchText_.empty()) {  // the search hides it
            searchText_.clear();
            FilterSessions();
            at = find();
        }
        if (at != filtered_.end()) {
            BeginRename(Rename::Chat, static_cast<size_t>(at - filtered_.begin()));
        } else {
            Flash(L"Чат ещё не сохранён: сначала напишите что-нибудь", 3000);
        }
        break;
    }
    case kNew:
        NewChat();
        break;
    default:
        break;
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
    case Popup::Models:
        moreOpen_ = false;
        break;
    case Popup::Effort:
        pendingEffort_ = settings_.effort;
        break;
    case Popup::Usage:
    case Popup::Image:
    case Popup::None:
        break;
    }
    searchFocus_ = false;
    termFocus_ = false;
    SetFocus(hwnd_);  // the panel's keys: digits, arrows
    PlacePreview();   // a picture over the window covers the page too
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
    if (was == Popup::Image) {
        viewer_ = {};
        PlacePreview();
    }
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
    } else if (const size_t rows = std::max<size_t>(1, SidebarRows(Layout())); selected_ >= firstRow_ + rows) {
        firstRow_ = selected_ + 1 - rows;
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
            for (const TranscriptImage& picture : entry.images) {
                if (FeedImage image = MakeFeedImage(SaveChatImage(picture.base64, picture.mediaType)); image.preview) {
                    feed_.back().images.push_back(std::move(image));
                }
            }
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
    claudeMissing_ = options.exe.empty();
    if (options.exe.empty()) {
        Refresh();  // an empty chat offers to install it; a chat that has words says so in the feed
        if (!HasConversation()) {
            return false;
        }
        Add(Kind::Error, L"Не нашёл claude.exe ни в %USERPROFILE%\\.local\\bin, ни в PATH.");
        return false;
    }
    options.cwd = workspace_;
    options.appendSystemPrompt = Tools::SystemPrompt();
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
    host_.CancelAll();
    for (FeedItem& item : feed_) {  // the process that asked is gone
        if (item.kind == Kind::Approval && item.verdict == Verdict::Pending) {
            item.verdict = Verdict::Cancelled;
        }
    }
    claude_.Stop();
    running_ = false;
    busy_ = false;
    StartClaude();
}

void ChatWindow::ResetChat() {
    ++generation_;
    host_.CancelAll();
    claude_.Stop();
    running_ = false;
    busy_ = false;
    sessionId_.clear();
    title_.clear();
    currentMessageId_.clear();
    streamed_.clear();
    feed_.clear();
    ClearFeedSelection();
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
    if (SignedOut()) {
        Flash(L"Сначала войдите в аккаунт", 3000);
        return;
    }
    if (!running_ && !StartClaude()) {
        return;
    }
    // Attachments first, then the words about them, as Claude's own apps send it.
    Json content = Json::array();
    std::wstring shown;
    std::vector<FeedImage> images;  // shown as pictures above the bubble, not as lines in it
    for (const Attachment& attachment : attachments_) {
        content.push_back(ContentBlock(attachment));
        if (attachment.type == Attachment::Type::Image) {
            const std::wstring path =
                attachment.path.empty() ? SaveChatImage(attachment.base64, attachment.mediaType) : attachment.path;
            if (FeedImage image = MakeFeedImage(path); image.preview) {
                images.push_back(std::move(image));
                continue;
            }
        }
        shown += (shown.empty() ? L"" : L"\n") + (L"📎 " + attachment.name);
    }
    if (!text.empty()) {
        content.push_back(Json{{"type", "text"}, {"text", ToUtf8(text)}});
        shown += (shown.empty() ? L"" : L"\n\n") + text;
    }
    claude_.SendUserContent(std::move(content));
    Add(Kind::User, std::move(shown));
    feed_.back().images = std::move(images);
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
                } else if (blockType == "tool_use" && OwnToolName(JsonStr(block, "name")).empty()) {
                    Add(Kind::Notice, L"инструмент: " + ToWide(JsonStr(block, "name")));
                }
            }
        }
    } else if (type == "user") {  // the CLI's echo of tool results
        const Json& content = JsonAt(JsonAt(message, "message"), "content");
        if (content.is_array()) {
            for (const Json& block : content) {
                if (JsonStr(block, "type") == "tool_result") {
                    OnToolResult(block);
                }
            }
        }
    } else if (type == "result") {
        for (FeedItem& item : feed_) {  // a turn that ended (or was stopped) leaves no call "running"
            if (item.kind == Kind::Approval && item.verdict == Verdict::Allowed && !item.done) {
                item.done = true;
            }
        }
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
            RefreshSessions();  // a new chat appears in the list, a title may have changed
        }
    } else if (type == "rate_limit_event") {
        const Json& info = JsonAt(message, "rate_limit_info");
        const Json& windows = JsonAt(info, "unifiedWindows");
        const auto update = [](Limit& limit, const Json& window) {
            const Json& used = JsonAt(window, "utilization");
            if (!used.is_number()) {
                return;  // not in this event: what was known stays
            }
            const Json& resets = JsonAt(window, "resetsAt");
            limit = Limit{static_cast<int>(std::lround(used.get<double>() * 100)),
                          resets.is_number() ? resets.get<long long>() : 0};
        };
        if (windows.is_object()) {
            update(session_, JsonAt(windows, "five_hour"));
            update(week_, JsonAt(windows, "seven_day"));
        } else if (const std::string window = JsonStr(info, "rateLimitType"); window == "five_hour") {
            update(session_, info);  // a CLI without unifiedWindows: only the window the event is about
        } else if (window == "seven_day") {
            update(week_, info);
        }
        if (settings_.sessionPercent != session_.percent || settings_.sessionResets != session_.resetsAt ||
            settings_.weekPercent != week_.percent || settings_.weekResets != week_.resetsAt) {
            settings_.sessionPercent = session_.percent;
            settings_.sessionResets = session_.resetsAt;
            settings_.weekPercent = week_.percent;
            settings_.weekResets = week_.resetsAt;
            SaveSettings(settings_);
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
        const Json& request = JsonAt(message, "request");
        const std::string subtype = JsonStr(request, "subtype");
        if (subtype == "can_use_tool") {
            AskApproval(id, request);
        } else if (subtype == "mcp_message") {
            host_.HandleMcp(id, request);
        } else {
            claude_.RespondError(id, "Not supported in this build yet.");
        }
    } else if (type == "aic_raw") {
        Add(Kind::Notice, ToWide(JsonStr(message, "text")));
    }
    if (updatePending_ && !busy_) {  // the turn the update waited for has ended
        StartUpdate();
        return;
    }
    Refresh();
}

// The model wants to call a tool. Reading runs at once (a note in the feed); anything that changes
// something waits on a card for the user's word.
void ChatWindow::AskApproval(const std::string& requestId, const Json& request) {
    const std::string name = OwnToolName(JsonStr(request, "tool_name"));
    const Json& input = JsonAt(request, "input");
    if (name.empty()) {
        claude_.Respond(requestId, Json{{"behavior", "deny"}, {"message", "Only AIControls tools are available here."}});
        return;
    }
    ToolSummary summary = SummarizeTool(name, input);
    if (!ToolNeedsApproval(name, input)) {
        Add(Kind::Notice, summary.title + L": " + summary.detail);
        claude_.Respond(requestId, Json{{"behavior", "allow"}, {"updatedInput", input}});
        return;
    }
    FeedItem item;
    item.kind = Kind::Approval;
    item.title = std::move(summary.title);
    item.text = std::move(summary.detail);
    item.requestId = requestId;
    item.toolUseId = JsonStr(request, "tool_use_id");
    item.tool = name;
    item.input = input;
    feed_.push_back(std::move(item));
    stick_ = true;
}

// A tool's result, as the CLI echoes it back: the card of that call shows it.
void ChatWindow::OnToolResult(const Json& block) {
    const std::string id = JsonStr(block, "tool_use_id");
    for (FeedItem& item : feed_) {
        if (item.kind != Kind::Approval || item.toolUseId.empty() || item.toolUseId != id) {
            continue;
        }
        const Json& content = JsonAt(block, "content");
        std::wstring text;
        if (content.is_string()) {
            text = ToWide(content.get<std::string>());
        } else if (content.is_array()) {
            for (const Json& part : content) {
                if (JsonStr(part, "type") == "text") {
                    text += (text.empty() ? L"" : L"\n") + ToWide(JsonStr(part, "text"));
                }
            }
        }
        const size_t end = text.find_last_not_of(L" \t\r\n");
        text.resize(end == std::wstring::npos ? 0 : end + 1);
        item.output = std::move(text);
        item.outputError = JsonAt(block, "is_error").is_boolean() && JsonAt(block, "is_error").get<bool>();
        item.done = true;
        item.layout.reset();
        if (HasFileActions(item) && IsPreviewable(WrittenFile(item, workspace_))) {
            OpenPreview(WrittenFile(item, workspace_));  // a page just written: shown at once, as it changes
        }
        return;
    }
}

void ChatWindow::Decide(size_t index, bool allow) {
    if (index >= feed_.size() || feed_[index].kind != Kind::Approval || feed_[index].verdict != Verdict::Pending) {
        return;
    }
    FeedItem& item = feed_[index];
    item.verdict = allow ? Verdict::Allowed : Verdict::Denied;
    if (allow) {
        claude_.Respond(item.requestId, Json{{"behavior", "allow"}, {"updatedInput", item.input}});
    } else {
        claude_.Respond(item.requestId, Json{{"behavior", "deny"}, {"message", "The user declined this call."}});
    }
    hover_ = {};
    Refresh();
}

void ChatWindow::CancelApprovals() {
    for (FeedItem& item : feed_) {
        if (item.kind == Kind::Approval && item.verdict == Verdict::Pending) {
            claude_.Respond(item.requestId, Json{{"behavior", "deny"}, {"message", "The turn was stopped."}});
            item.verdict = Verdict::Cancelled;
        }
    }
}

void ChatWindow::StopTurn() {
    CancelApprovals();
    host_.CancelAll();
    claude_.Interrupt();
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
