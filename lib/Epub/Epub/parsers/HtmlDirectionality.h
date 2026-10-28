#pragma once

#include <string_view>

#include "../css/CssStyle.h"

// HTML's user-agent rules for directionality (HTML, Rendering, "Bidirectional text"), applied under the author's CSS:
// a property the book's CSS already set is left alone. They hold whether or not the reader uses the book's CSS, since
// dir is markup, not style.
//   an element with dir="ltr", "rtl" or "auto"   that direction, and unicode-bidi: isolate
//   <bdo>                                        unicode-bidi: isolate-override
//   <bdi>                                        unicode-bidi: isolate, and direction auto without a dir of its own
// Values are ASCII case-insensitive; any other value is as if the attribute were absent. An empty `dirAttr` means the
// element has none.
inline void applyHtmlDirectionality(CssStyle& style, const std::string_view tagName, const std::string_view dirAttr) {
  const auto is = [dirAttr](const std::string_view value) {
    if (dirAttr.size() != value.size()) return false;
    for (size_t i = 0; i < value.size(); ++i) {
      const char c = dirAttr[i] >= 'A' && dirAttr[i] <= 'Z' ? static_cast<char>(dirAttr[i] - 'A' + 'a') : dirAttr[i];
      if (c != value[i]) return false;
    }
    return true;
  };
  CssDirection direction = is("ltr")    ? CssDirection::Ltr
                           : is("rtl")  ? CssDirection::Rtl
                           : is("auto") ? CssDirection::Auto
                                        : CssDirection::Unset;
  const bool bdo = tagName == "bdo";
  const bool bdi = tagName == "bdi";
  CssUnicodeBidi bidi = CssUnicodeBidi::Unset;
  if (bdo) {
    bidi = CssUnicodeBidi::IsolateOverride;
  } else if (bdi || direction != CssDirection::Unset) {
    bidi = CssUnicodeBidi::Isolate;
  }
  if (bdi && direction == CssDirection::Unset) direction = CssDirection::Auto;
  if (!style.hasDirection()) style.direction = direction;
  if (!style.hasUnicodeBidi()) style.unicodeBidi = bidi;
}
