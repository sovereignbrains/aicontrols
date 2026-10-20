#include "app/fonts.h"

#include <dwrite_3.h>

#include <array>

#include "app/resource.h"

namespace aic {
namespace {

struct Blob {
    const void* data = nullptr;
    DWORD size = 0;
};

Blob FontResource(HINSTANCE instance, int id) {
    HRSRC found = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (found == nullptr) {
        return {};
    }
    HGLOBAL loaded = LoadResource(instance, found);
    return loaded == nullptr ? Blob{} : Blob{LockResource(loaded), SizeofResource(instance, found)};
}

constexpr std::array<int, 3> kFonts{IDR_FONT_UNBOUNDED, IDR_FONT_GOLOS, IDR_FONT_MONO};

}  // namespace

wil::com_ptr<IDWriteFontCollection> LoadFontCollection(IDWriteFactory* factory) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    wil::com_ptr<IDWriteFactory5> factory5;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(factory5.put())))) {
        return nullptr;
    }
    // The loader and the resource memory must outlive every format that uses the collection.
    static wil::com_ptr<IDWriteInMemoryFontFileLoader> loader;
    if (!loader) {
        if (FAILED(factory5->CreateInMemoryFontFileLoader(loader.put())) ||
            FAILED(factory5->RegisterFontFileLoader(loader.get()))) {
            loader.reset();
            return nullptr;
        }
    }
    wil::com_ptr<IDWriteFontSetBuilder1> builder;
    if (FAILED(factory5->CreateFontSetBuilder(builder.put()))) {
        return nullptr;
    }
    for (const int id : kFonts) {
        const Blob blob = FontResource(instance, id);
        wil::com_ptr<IDWriteFontFile> file;
        if (blob.data == nullptr ||
            FAILED(loader->CreateInMemoryFontFileReference(factory5.get(), blob.data, blob.size, nullptr,
                                                            file.put())) ||
            FAILED(builder->AddFontFile(file.get()))) {
            return nullptr;
        }
    }
    wil::com_ptr<IDWriteFontSet> set;
    wil::com_ptr<IDWriteFontCollection1> collection;
    if (FAILED(builder->CreateFontSet(set.put())) ||
        FAILED(factory5->CreateFontCollectionFromFontSet(set.get(), collection.put()))) {
        return nullptr;
    }
    return collection.query<IDWriteFontCollection>();
}

void RegisterGdiFonts(HINSTANCE instance) {
    const Blob blob = FontResource(instance, IDR_FONT_GOLOS);
    if (blob.data != nullptr) {
        DWORD count = 0;
        AddFontMemResourceEx(const_cast<void*>(blob.data), blob.size, nullptr, &count);  // lives with the process
    }
}

}  // namespace aic
