#pragma once

#include <windows.h>
#include <dwrite.h>

#include <wil/com.h>

namespace aic {

// The families of the embedded fonts, as DirectWrite and GDI know them.
inline constexpr wchar_t kDisplayFont[] = L"Unbounded";   // the greeting, the header
inline constexpr wchar_t kTextFont[] = L"Golos Text";     // everything else
inline constexpr wchar_t kMonoFont[] = L"JetBrains Mono"; // code

// A collection of the three fonts, built from the exe's resources (Windows 10 1809+).
// Null when it cannot be built: formats then fall back to the system's default font.
wil::com_ptr<IDWriteFontCollection> LoadFontCollection(IDWriteFactory* factory);

// Registers the text font for this process's GDI (the RichEdit input).
void RegisterGdiFonts(HINSTANCE instance);

}  // namespace aic
