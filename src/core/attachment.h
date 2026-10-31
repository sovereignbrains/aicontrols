#pragma once

#include <windows.h>
#include <wincodec.h>

#include <optional>
#include <string>

#include <wil/com.h>

#include "core/claude_process.h"

namespace aic {

// Something sent along with a message: a picture, a PDF, or a text file's content.
struct Attachment {
    enum class Type { Image, Document, Text };
    Type type = Type::Image;
    std::wstring name;
    std::wstring path;      // the file it was read from; empty for a pasted picture
    std::string mediaType;  // image/png, image/jpeg, application/pdf
    std::string base64;     // Image, Document
    std::string text;       // Text: the file's UTF-8 content
    wil::com_ptr<IWICBitmap> thumbnail;  // Image: small, 32bppPBGRA, for Direct2D
};

// COM must be initialized on the calling thread. On failure: nullopt and a message in `error`.
std::optional<Attachment> LoadAttachment(IWICImagingFactory* wic, const std::wstring& path, std::wstring& error);
// A picture on the clipboard (a screenshot); nullopt with an empty `error` when there is none.
std::optional<Attachment> ClipboardImage(IWICImagingFactory* wic, HWND owner, std::wstring& error);
bool ClipboardHasImage();

// A screenshot as PNG (JPEG when too big) in base64, scaled to the API's 1568 px: the window whose title holds
// `window` (case-insensitive), else the whole virtual screen when `window` is empty. Initializes COM itself, so any
// thread. On failure: empty and a message in `error`.
std::string CaptureScreenBase64(const std::wstring& window, std::wstring& error);

// The message content block the API expects for it.
Json ContentBlock(const Attachment& attachment);

// Pictures shown in the chat are kept as files, to be opened, copied and found again later:
// %LOCALAPPDATA%\AIControls\images\<hash>.<ext>, the same picture always the same file. Empty on failure.
std::wstring SaveChatImage(const std::string& base64, const std::string& mediaType);
// A picture file decoded for Direct2D (32bppPBGRA), at most `side` px on its longer side; `width` and
// `height` get the picture's own size. Null if it cannot be read.
wil::com_ptr<IWICBitmap> LoadImagePreview(IWICImagingFactory* wic, const std::wstring& path, UINT side, UINT& width,
                                          UINT& height);
// The same from a picture in memory (a resource).
wil::com_ptr<IWICBitmap> LoadImagePreview(IWICImagingFactory* wic, const void* data, size_t size, UINT side);
// The picture on the clipboard as a bitmap and as PNG, for any app to paste.
bool CopyImageToClipboard(IWICImagingFactory* wic, HWND owner, const std::wstring& path);

}  // namespace aic
