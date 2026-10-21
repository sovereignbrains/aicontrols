#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

namespace aic {

// What the menu is drawn with: the window's own factories and its UI font (single line).
struct MenuLook {
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dwrite = nullptr;
    IDWriteTextFormat* text = nullptr;
    float dpi = 96.f;
};

// A popup menu drawn in the app's style (as Claude's own: dark, rounded, as wide as its items, shortcuts and
// checks at the right, submenus beside it) for an ordinary HMENU: its items, "\t" before a shortcut, grayed,
// checked, separators and popups. The same contract as TrackPopupMenu with TPM_RETURNCMD: the chosen item's id,
// 0 when it was dismissed. `above`: the menu's bottom at `screen` (TPM_BOTTOMALIGN). Modal until it closes.
int ShowMenu(HWND owner, HMENU menu, POINT screen, bool above, const MenuLook& look);

}  // namespace aic
