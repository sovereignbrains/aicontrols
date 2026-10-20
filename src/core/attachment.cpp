#include "core/attachment.h"

#include <wincrypt.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>

#include <wil/resource.h>

namespace aic {
namespace {

namespace fs = std::filesystem;

// The API's own limits: 5 MB per image as sent; past 1568 px the model sees it scaled down anyway.
constexpr uint64_t kImageBytes = 3'700'000;  // ~5 MB once in base64
constexpr UINT kImageSide = 1568;
constexpr UINT kThumbnailSide = 112;
constexpr uint64_t kTextBytes = 256 * 1024;
constexpr uint64_t kPdfBytes = 24 * 1024 * 1024;

std::string Base64(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD length = 0;
    CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), flags, nullptr, &length);
    std::string out(length, '\0');
    if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), flags, out.data(), &length)) {
        return {};
    }
    out.resize(length);
    return out;
}

std::vector<uint8_t> ReadBytes(const fs::path& path, uint64_t limit, bool& tooBig) {
    std::error_code error;
    const uint64_t size = fs::file_size(path, error);
    tooBig = !error && size > limit;
    if (error || tooBig) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    bytes.resize(static_cast<size_t>(in.gcount()));
    return bytes;
}

std::vector<uint8_t> StreamBytes(IStream* stream) {
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
    const LARGE_INTEGER start{};
    stream->Seek(start, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    if (FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read))) {
        return {};
    }
    bytes.resize(read);
    return bytes;
}

wil::com_ptr<IWICBitmapSource> Fit(IWICImagingFactory* wic, IWICBitmapSource* source, UINT side) {
    UINT width = 0;
    UINT height = 0;
    source->GetSize(&width, &height);
    if (width <= side && height <= side) {
        return wil::com_ptr<IWICBitmapSource>(source);
    }
    const double scale = static_cast<double>(side) / std::max(width, height);
    wil::com_ptr<IWICBitmapScaler> scaler;
    THROW_IF_FAILED(wic->CreateBitmapScaler(scaler.put()));
    THROW_IF_FAILED(scaler->Initialize(source, std::max(1U, static_cast<UINT>(width * scale)),
                                       std::max(1U, static_cast<UINT>(height * scale)),
                                       WICBitmapInterpolationModeHighQualityCubic));
    return scaler.query<IWICBitmapSource>();
}

wil::com_ptr<IWICBitmapSource> Convert(IWICImagingFactory* wic, IWICBitmapSource* source, REFWICPixelFormatGUID format) {
    wil::com_ptr<IWICFormatConverter> converter;
    THROW_IF_FAILED(wic->CreateFormatConverter(converter.put()));
    THROW_IF_FAILED(converter->Initialize(source, format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                          WICBitmapPaletteTypeCustom));
    return converter.query<IWICBitmapSource>();
}

std::vector<uint8_t> Encode(IWICImagingFactory* wic, IWICBitmapSource* source, REFGUID container) {
    wil::com_ptr<IStream> stream;
    THROW_IF_FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    wil::com_ptr<IWICBitmapEncoder> encoder;
    THROW_IF_FAILED(wic->CreateEncoder(container, nullptr, encoder.put()));
    THROW_IF_FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    wil::com_ptr<IWICBitmapFrameEncode> frame;
    wil::com_ptr<IPropertyBag2> options;
    THROW_IF_FAILED(encoder->CreateNewFrame(frame.put(), options.put()));
    if (container == GUID_ContainerFormatJpeg) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT quality{};
        quality.vt = VT_R4;
        quality.fltVal = 0.9f;
        options->Write(1, &option, &quality);
    }
    THROW_IF_FAILED(frame->Initialize(options.get()));
    UINT width = 0;
    UINT height = 0;
    source->GetSize(&width, &height);
    THROW_IF_FAILED(frame->SetSize(width, height));
    WICPixelFormatGUID format{};
    source->GetPixelFormat(&format);
    THROW_IF_FAILED(frame->SetPixelFormat(&format));
    THROW_IF_FAILED(frame->WriteSource(source, nullptr));
    THROW_IF_FAILED(frame->Commit());
    THROW_IF_FAILED(encoder->Commit());
    return StreamBytes(stream.get());
}

// A decoded picture into an attachment: the original bytes when they already fit the API,
// else scaled to 1568 px and re-encoded (PNG, or JPEG when PNG is still too big).
Attachment FromDecoder(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, std::wstring name,
                       const std::vector<uint8_t>& original, const std::string& originalType) {
    wil::com_ptr<IWICBitmapFrameDecode> frame;
    THROW_IF_FAILED(decoder->GetFrame(0, frame.put()));
    UINT width = 0;
    UINT height = 0;
    THROW_IF_FAILED(frame->GetSize(&width, &height));

    Attachment attachment;
    attachment.type = Attachment::Type::Image;
    attachment.name = std::move(name);
    if (!originalType.empty() && original.size() <= kImageBytes && width <= kImageSide && height <= kImageSide) {
        attachment.mediaType = originalType;
        attachment.base64 = Base64(original);
    } else {
        const wil::com_ptr<IWICBitmapSource> fitted = Fit(wic, frame.get(), kImageSide);
        std::vector<uint8_t> bytes =
            Encode(wic, Convert(wic, fitted.get(), GUID_WICPixelFormat32bppBGRA).get(), GUID_ContainerFormatPng);
        attachment.mediaType = "image/png";
        if (bytes.size() > kImageBytes) {
            bytes = Encode(wic, Convert(wic, fitted.get(), GUID_WICPixelFormat24bppBGR).get(),
                           GUID_ContainerFormatJpeg);
            attachment.mediaType = "image/jpeg";
        }
        attachment.base64 = Base64(bytes);
    }

    const wil::com_ptr<IWICBitmapSource> preview =
        Convert(wic, Fit(wic, frame.get(), kThumbnailSide).get(), GUID_WICPixelFormat32bppPBGRA);
    THROW_IF_FAILED(wic->CreateBitmapFromSource(preview.get(), WICBitmapCacheOnLoad, attachment.thumbnail.put()));
    return attachment;
}

std::string ImageType(const std::wstring& extension) {
    if (extension == L".png") {
        return "image/png";
    }
    if (extension == L".jpg" || extension == L".jpeg") {
        return "image/jpeg";
    }
    if (extension == L".gif") {
        return "image/gif";
    }
    if (extension == L".webp") {
        return "image/webp";
    }
    return {};
}

bool IsUtf8Text(const std::vector<uint8_t>& bytes) {
    if (std::ranges::find(bytes, uint8_t{0}) != bytes.end()) {
        return false;
    }
    return bytes.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<int>(bytes.size()), nullptr, 0) > 0;
}

std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return text;
}

std::vector<uint8_t> FromBase64(const std::string& text) {
    DWORD length = 0;
    if (text.empty() || !CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                                              nullptr, &length, nullptr, nullptr)) {
        return {};
    }
    std::vector<uint8_t> bytes(length);
    if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, bytes.data(), &length,
                              nullptr, nullptr)) {
        return {};
    }
    bytes.resize(length);
    return bytes;
}

fs::path ImagesDir() {
    wchar_t local[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return fs::path(local) / L"AIControls" / L"images";
}

}  // namespace

std::optional<Attachment> LoadAttachment(IWICImagingFactory* wic, const std::wstring& path, std::wstring& error) {
    const fs::path file(path);
    const std::wstring name = file.filename().wstring();
    const std::wstring extension = Lower(file.extension().wstring());
    try {
        if (extension == L".pdf") {
            bool tooBig = false;
            const std::vector<uint8_t> bytes = ReadBytes(file, kPdfBytes, tooBig);
            if (tooBig || bytes.empty()) {
                error = tooBig ? name + L": PDF больше 24 МБ" : name + L": не прочитался";
                return std::nullopt;
            }
            return Attachment{Attachment::Type::Document, name, path, "application/pdf", Base64(bytes), {}, {}};
        }

        wil::com_ptr<IWICBitmapDecoder> decoder;
        if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, decoder.put()))) {
            bool tooBig = false;
            const std::string type = ImageType(extension);
            const std::vector<uint8_t> original = type.empty() ? std::vector<uint8_t>{}
                                                               : ReadBytes(file, kImageBytes, tooBig);
            Attachment attachment =
                FromDecoder(wic, decoder.get(), name, original, original.empty() ? std::string{} : type);
            attachment.path = path;
            return attachment;
        }

        bool tooBig = false;
        const std::vector<uint8_t> bytes = ReadBytes(file, kTextBytes, tooBig);
        if (tooBig) {
            error = name + L": текстовый файл больше 256 КБ";
            return std::nullopt;
        }
        if (!IsUtf8Text(bytes)) {
            error = name + L": не картинка, не PDF и не текст в UTF-8";
            return std::nullopt;
        }
        Attachment attachment;
        attachment.type = Attachment::Type::Text;
        attachment.name = name;
        attachment.text.assign(bytes.begin(), bytes.end());
        return attachment;
    } catch (const std::exception&) {
        error = name + L": не получилось прочитать";
        return std::nullopt;
    }
}

bool ClipboardHasImage() {
    return IsClipboardFormatAvailable(RegisterClipboardFormatW(L"PNG")) != FALSE ||
           IsClipboardFormatAvailable(CF_DIB) != FALSE;
}

std::optional<Attachment> ClipboardImage(IWICImagingFactory* wic, HWND owner, std::wstring& error) {
    if (!ClipboardHasImage() || !OpenClipboard(owner)) {
        return std::nullopt;
    }
    auto close = wil::scope_exit([] { CloseClipboard(); });
    // PNG first (screenshot tools and browsers put it there, alpha intact), else the DIB as a BMP file.
    std::vector<uint8_t> bytes;
    if (HANDLE png = GetClipboardData(RegisterClipboardFormatW(L"PNG")); png != nullptr) {
        if (const void* data = GlobalLock(png); data != nullptr) {
            const auto* begin = static_cast<const uint8_t*>(data);
            bytes.assign(begin, begin + GlobalSize(png));
            GlobalUnlock(png);
        }
    } else if (HANDLE dib = GetClipboardData(CF_DIB); dib != nullptr) {
        if (const void* data = GlobalLock(dib); data != nullptr) {
            const SIZE_T size = GlobalSize(dib);
            const auto* info = static_cast<const BITMAPINFOHEADER*>(data);
            DWORD offset = sizeof(BITMAPFILEHEADER) + info->biSize + info->biClrUsed * sizeof(RGBQUAD);
            if (info->biCompression == BI_BITFIELDS && info->biSize == sizeof(BITMAPINFOHEADER)) {
                offset += 3 * sizeof(DWORD);
            }
            BITMAPFILEHEADER header{};
            header.bfType = 0x4d42;  // "BM"
            header.bfSize = static_cast<DWORD>(sizeof(header) + size);
            header.bfOffBits = offset;
            bytes.resize(sizeof(header) + size);
            std::memcpy(bytes.data(), &header, sizeof(header));
            std::memcpy(bytes.data() + sizeof(header), data, size);
            GlobalUnlock(dib);
        }
    }
    close.reset();
    if (bytes.empty()) {
        return std::nullopt;
    }
    try {
        wil::com_ptr<IWICStream> stream;
        THROW_IF_FAILED(wic->CreateStream(stream.put()));
        THROW_IF_FAILED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size())));
        wil::com_ptr<IWICBitmapDecoder> decoder;
        THROW_IF_FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                     decoder.put()));
        // Re-encoded either way: a BMP is not something the API takes.
        return FromDecoder(wic, decoder.get(), L"снимок.png", {}, {});
    } catch (const std::exception&) {
        error = L"Картинку из буфера не получилось прочитать";
        return std::nullopt;
    }
}

Json ContentBlock(const Attachment& attachment) {
    switch (attachment.type) {
    case Attachment::Type::Image:
        return Json{{"type", "image"},
                    {"source", {{"type", "base64"}, {"media_type", attachment.mediaType}, {"data", attachment.base64}}}};
    case Attachment::Type::Document:
        return Json{{"type", "document"},
                    {"source", {{"type", "base64"}, {"media_type", attachment.mediaType}, {"data", attachment.base64}}},
                    {"title", ToUtf8(attachment.name)}};
    case Attachment::Type::Text:
        break;
    }
    return Json{{"type", "text"},
                {"text", "Файл «" + ToUtf8(attachment.name) + "»:\n\n" + attachment.text}};
}

std::wstring SaveChatImage(const std::string& base64, const std::string& mediaType) {
    uint64_t hash = 14695981039346656037ULL;  // FNV-1a: the name tells the same picture again
    for (const char c : base64) {
        hash = (hash ^ static_cast<uint8_t>(c)) * 1099511628211ULL;
    }
    std::wstring extension = L".png";
    if (mediaType == "image/jpeg") {
        extension = L".jpg";
    } else if (mediaType == "image/gif") {
        extension = L".gif";
    } else if (mediaType == "image/webp") {
        extension = L".webp";
    }
    const fs::path dir = ImagesDir();
    const fs::path file = dir / (std::format(L"{:016x}", hash) + extension);
    std::error_code error;
    if (fs::exists(file, error)) {
        return file.wstring();
    }
    const std::vector<uint8_t> bytes = FromBase64(base64);
    if (bytes.empty()) {
        return {};
    }
    fs::create_directories(dir, error);
    const fs::path partial = fs::path(file).concat(L".part");
    {
        std::ofstream out(partial, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) {
            return {};
        }
    }
    fs::rename(partial, file, error);
    return error ? std::wstring() : file.wstring();
}

wil::com_ptr<IWICBitmap> LoadImagePreview(IWICImagingFactory* wic, const std::wstring& path, UINT side, UINT& width,
                                          UINT& height) {
    width = height = 0;
    try {
        wil::com_ptr<IWICBitmapDecoder> decoder;
        THROW_IF_FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnDemand, decoder.put()));
        wil::com_ptr<IWICBitmapFrameDecode> frame;
        THROW_IF_FAILED(decoder->GetFrame(0, frame.put()));
        THROW_IF_FAILED(frame->GetSize(&width, &height));
        const wil::com_ptr<IWICBitmapSource> fitted =
            Convert(wic, Fit(wic, frame.get(), side).get(), GUID_WICPixelFormat32bppPBGRA);
        wil::com_ptr<IWICBitmap> bitmap;
        THROW_IF_FAILED(wic->CreateBitmapFromSource(fitted.get(), WICBitmapCacheOnLoad, bitmap.put()));
        return bitmap;
    } catch (const std::exception&) {
        width = height = 0;
        return nullptr;
    }
}

wil::com_ptr<IWICBitmap> LoadImagePreview(IWICImagingFactory* wic, const void* data, size_t size, UINT side) {
    try {
        wil::com_ptr<IWICStream> stream;
        THROW_IF_FAILED(wic->CreateStream(stream.put()));
        THROW_IF_FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), static_cast<DWORD>(size)));
        wil::com_ptr<IWICBitmapDecoder> decoder;
        THROW_IF_FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put()));
        wil::com_ptr<IWICBitmapFrameDecode> frame;
        THROW_IF_FAILED(decoder->GetFrame(0, frame.put()));
        const wil::com_ptr<IWICBitmapSource> fitted =
            Convert(wic, Fit(wic, frame.get(), side).get(), GUID_WICPixelFormat32bppPBGRA);
        wil::com_ptr<IWICBitmap> bitmap;
        THROW_IF_FAILED(wic->CreateBitmapFromSource(fitted.get(), WICBitmapCacheOnLoad, bitmap.put()));
        return bitmap;
    } catch (const std::exception&) {
        return nullptr;
    }
}

bool CopyImageToClipboard(IWICImagingFactory* wic, HWND owner, const std::wstring& path) {
    std::vector<uint8_t> dib;
    std::vector<uint8_t> png;
    try {
        wil::com_ptr<IWICBitmapDecoder> decoder;
        THROW_IF_FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnDemand, decoder.put()));
        wil::com_ptr<IWICBitmapFrameDecode> frame;
        THROW_IF_FAILED(decoder->GetFrame(0, frame.put()));
        const wil::com_ptr<IWICBitmapSource> pixels = Convert(wic, frame.get(), GUID_WICPixelFormat32bppBGRA);
        UINT width = 0;
        UINT height = 0;
        THROW_IF_FAILED(pixels->GetSize(&width, &height));
        const UINT stride = width * 4;
        std::vector<uint8_t> rows(static_cast<size_t>(stride) * height);
        THROW_IF_FAILED(pixels->CopyPixels(nullptr, stride, static_cast<UINT>(rows.size()), rows.data()));
        // CF_DIB: a header and the rows bottom-up.
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = static_cast<LONG>(width);
        header.biHeight = static_cast<LONG>(height);
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;
        header.biSizeImage = static_cast<DWORD>(rows.size());
        dib.resize(sizeof(header) + rows.size());
        std::memcpy(dib.data(), &header, sizeof(header));
        for (UINT y = 0; y < height; ++y) {
            std::memcpy(dib.data() + sizeof(header) + static_cast<size_t>(height - 1 - y) * stride,
                        rows.data() + static_cast<size_t>(y) * stride, stride);
        }
        png = Encode(wic, pixels.get(), GUID_ContainerFormatPng);
    } catch (const std::exception&) {
        return false;
    }
    if (!OpenClipboard(owner)) {
        return false;
    }
    auto close = wil::scope_exit([] { CloseClipboard(); });
    EmptyClipboard();
    const auto put = [](UINT format, const std::vector<uint8_t>& bytes) {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
        if (memory == nullptr) {
            return false;
        }
        void* data = GlobalLock(memory);
        if (data == nullptr) {
            GlobalFree(memory);
            return false;
        }
        std::memcpy(data, bytes.data(), bytes.size());
        GlobalUnlock(memory);
        if (SetClipboardData(format, memory) == nullptr) {
            GlobalFree(memory);
            return false;
        }
        return true;
    };
    const bool bitmap = put(CF_DIB, dib);
    const bool encoded = !png.empty() && put(RegisterClipboardFormatW(L"PNG"), png);
    return bitmap || encoded;
}

}  // namespace aic
