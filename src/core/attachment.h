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

// The message content block the API expects for it.
Json ContentBlock(const Attachment& attachment);

}  // namespace aic
