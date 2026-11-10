#pragma once

// Which keyboard layouts the language key cycles through, and which one a
// keyboard opens on.
//
// Ported from crosspoint-reader (PR #2858 by winst0niuss, with Uri Tauber).
//
// The set is SETTINGS.keyboardLayouts, chosen on the Keyboard Layouts screen.
// Until that is configured it is the UI language's layout plus English, so a
// reader whose interface is in Russian gets ЙЦУКЕН and QWERTY without visiting
// the screen, and one reading Russian books under an English interface switches
// ЙЦУКЕН on there.
//
// Hebrew, Arabic and Greek are scripts the interface is not translated into:
// they have no UI language, so only that screen switches them on, and a
// keyboard never opens on one. Their keys and the text typed with them are
// drawn through the UI text path, which orders right-to-left text: Hebrew and
// Greek from the UI faces, Arabic from the Arabic UI pack (UiScriptPack) or the
// last book's font, joined where that font has the presentation forms.

#include <I18n.h>
#include <components/keyboard/keyboard.h>

#include <cstdint>

namespace keyboard_layouts {

// A layout for a script the interface is not translated into.
inline constexpr Language NO_UI_LANGUAGE = Language::_COUNT;

struct LayoutInfo {
  freeink::ui::KeyboardLayoutId id;
  // The UI language the layout is for: the one it opens on, and the one that names it.
  Language language;
  // A layout with NO_UI_LANGUAGE is named here, in its own language, as I18n names the UI languages.
  const char* nativeName;
};

// Table position is the bit a layout occupies in the enabled() mask, kept from the
// first build that wrote it: a new layout is appended, never inserted.
inline constexpr LayoutInfo ALL[] = {
    {freeink::ui::KeyboardLayoutId::QwertyEn, Language::EN, nullptr},
    {freeink::ui::KeyboardLayoutId::AzertyFr, Language::FR, nullptr},
    {freeink::ui::KeyboardLayoutId::QwertzDe, Language::DE, nullptr},
    {freeink::ui::KeyboardLayoutId::SpanishEs, Language::ES, nullptr},
    {freeink::ui::KeyboardLayoutId::CyrillicRu, Language::RU, nullptr},
    {freeink::ui::KeyboardLayoutId::CyrillicUk, Language::UK, nullptr},
    {freeink::ui::KeyboardLayoutId::CyrillicBe, Language::BE, nullptr},
    {freeink::ui::KeyboardLayoutId::CyrillicKk, Language::KK, nullptr},
    {freeink::ui::KeyboardLayoutId::HebrewIl, NO_UI_LANGUAGE, "עברית"},
    {freeink::ui::KeyboardLayoutId::ArabicAr, NO_UI_LANGUAGE, "العربية"},
    {freeink::ui::KeyboardLayoutId::GreekEl, NO_UI_LANGUAGE, "Ελληνικά"},
};
inline constexpr uint8_t COUNT = sizeof(ALL) / sizeof(ALL[0]);
static_assert(COUNT <= 16, "the enabled-layout mask is uint16_t");

inline constexpr uint16_t bitAt(const uint8_t i) { return static_cast<uint16_t>(1u << i); }
// The layouts that can type a URL or a Wi-Fi password. One of them is always enabled.
inline constexpr uint16_t LATIN_BITS = bitAt(0) | bitAt(1) | bitAt(2) | bitAt(3);

// Bit i set: ALL[i] is reachable. Always includes a Latin layout, which is what
// URL and password fields need whatever else is chosen.
uint16_t enabled();

// The UI language's layout (English when the language has none), or the next
// enabled one when that has been switched off.
freeink::ui::KeyboardLayoutId startingLayout();

// The enabled layout after `current`, wrapping; `current` when it is the only one.
freeink::ui::KeyboardLayoutId next(freeink::ui::KeyboardLayoutId current);

// ALL[i]'s name on the Keyboard Layouts screen: its language's, in that language.
const char* name(uint8_t i);

}  // namespace keyboard_layouts
