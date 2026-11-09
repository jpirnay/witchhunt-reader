// Every key of every keyboard layout, every layer, drawn by the SDK's keyboard() as KeyboardEntryActivity sets it up
// at an X3's and an X4's keyboard width, measured in the UI faces the device draws it in: the built-in faces with the
// Arabic UI pack behind them, the label formed as drawText forms it (BidiUtils::arabicUiForms).
//
// What it holds: a key's character is drawn whole -- in the body face if it fits the key, else in the small face,
// within the key's outline -- never as the ellipsis the target cuts a too-wide string to; and every character a key or
// a layout's name draws, formed, is in the UI face or the pack. The X3 showed the first going wrong for the wide Arabic
// isolated forms, and the second for Hamza, which the mapper left unformed while the pack holds only forms.
#include <Utf8.h>
#include <components/keyboard/keyboard.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ArabicUiPack.h"
#include "BidiUtils.h"
#include "EpdFont.h"
#include "FallbackFaceProvider.h"
#include "SdCardFont.h"
#include "UiFaceProvider.h"
#include "activities/util/KeyboardLayoutSet.h"
#include "builtinFonts/inter_ui_10_regular.h"
#include "builtinFonts/inter_ui_12_regular.h"
#include "builtinFonts/inter_ui_14_regular.h"
#include "builtinFonts/notosans_8_regular.h"

namespace fui = freeink::ui;

namespace {

std::string packPath(const unsigned points) {
  return std::string(ARABIC_UI_DIR) + "/ArabicUI_" + std::to_string(points) + ".cpfont";
}

// A built-in UI face with the pack's file of its size behind it, as CjkUiFonts::attach() binds it.
class UiFace {
 public:
  UiFace(const EpdFontData& face, const unsigned points) {
    loaded = pack.load(packPath(points).c_str());
    UiFaceProvider::Pack hooks;
    hooks.face = [](void* ctx) -> const EpdFontData* { return static_cast<SdCardFont*>(ctx)->getEpdFont(0)->data; };
    hooks.covers = &arabic_ui_pack::covers;
    hooks.installed = [](void*) { return true; };
    hooks.prepare = [](void*, const char*) {};
    hooks.ctx = &pack;
    provider.bind(&shared, hooks);
    data = face;
    data.provider = &provider;
  }
  UiFace(const UiFace&) = delete;
  UiFace& operator=(const UiFace&) = delete;

  // What GfxRenderer::getTextWidth gives: the formed string's extent.
  int width(const char* text) const {
    const std::string formed = BidiUtils::arabicUiForms(text);
    int w = 0, h = 0;
    EpdFont(&data).getTextDimensions(formed.c_str(), &w, &h);
    return w;
  }
  bool holds(const uint32_t cp) const { return static_cast<bool>(EpdFont(&data).findGlyph(cp)); }
  int16_t lineHeight() const { return static_cast<int16_t>(data.advanceY); }

  bool loaded = false;

 private:
  SdCardFont pack;
  FallbackFaceProvider shared;
  UiFaceProvider provider;
  EpdFontData data{};
};

// A DrawTarget measuring in two faces, the body and the small slot, that records what is drawn as text.
class FaceTarget : public fui::DrawTarget {
 public:
  struct Text {
    fui::Rect rect;
    std::string text;
    fui::FontId font;
  };
  struct Key {
    fui::Rect rect;  // its outline
    std::vector<Text> texts;
  };

  FaceTarget(const UiFace& body, const UiFace& small) : body(body), small(small) {}

  const UiFace& face(const fui::FontId font) const { return font == fui::FONT_SLOT_BODY ? body : small; }
  fui::Size measureText(const fui::FontId font, const char* text, fui::TextStyle) const override {
    return fui::Size{static_cast<int16_t>(face(font).width(text)), lineHeight(font)};
  }
  int16_t lineHeight(const fui::FontId font) const override { return face(font).lineHeight(); }
  void fill(fui::Rect, fui::Paint, uint8_t, uint8_t) override {}
  // Each key's button strokes its outline before it draws the key's text.
  void stroke(const fui::Rect rect, fui::Paint, uint8_t, uint8_t, uint8_t) override { keys.push_back(Key{rect, {}}); }
  void line(fui::Point, fui::Point, uint8_t, fui::Paint) override {}
  void triangle(fui::Point, fui::Point, fui::Point, fui::Paint) override {}
  void text(const fui::Rect rect, const char* text, const fui::TextStyle style) override {
    if (!keys.empty()) keys.back().texts.push_back(Text{rect, text, style.font});
  }
  void bitmap(fui::Rect, fui::BitmapRef, fui::BitmapMode, fui::Paint, fui::Rotation) override {}

  std::vector<Key> keys;

 private:
  const UiFace& body;
  const UiFace& small;
};

std::vector<uint32_t> formedCodepoints(const char* text) {
  std::vector<uint32_t> cps;
  const std::string formed = BidiUtils::arabicUiForms(text);
  const auto* p = reinterpret_cast<const uint8_t*>(formed.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&p)) cps.push_back(cp);
  return cps;
}

constexpr auto LAST_LAYOUT = fui::KeyboardLayoutId::GreekEl;

// The keyboard's width on each C3 board, portrait: the content rect between the side hint columns
// (sideButtonHintsWidth 30), on both sides on the X3, on the right on the X4.
struct Board {
  const char* name;
  int16_t width;
};
constexpr Board BOARDS[] = {{"X3", 528 - 2 * 30}, {"X4", 480 - 30}};

// The UI sizes: Normal binds UI_12 to Inter 12 and the small slot to Noto Sans 8; Large, Inter 14 and Inter 10.
struct UiSize {
  const char* name;
  const EpdFontData& body;
  unsigned bodyPoints;
  const EpdFontData& small;
  unsigned smallPoints;
};
const UiSize SIZES[] = {{"Normal", inter_ui_12_regular, 12, notosans_8_regular, 8},
                        {"Large", inter_ui_14_regular, 14, inter_ui_10_regular, 10}};

}  // namespace

TEST(KeyboardLabels, EveryCharacterIsDrawnWholeWithinItsKey) {
  size_t characters = 0;
  size_t inTheSmallFace = 0;
  for (const UiSize& size : SIZES) {
    const UiFace body(size.body, size.bodyPoints);
    const UiFace small(size.small, size.smallPoints);
    ASSERT_TRUE(body.loaded && small.loaded) << size.name;
    for (const Board& board : BOARDS) {
      for (uint8_t raw = 0; raw <= static_cast<uint8_t>(LAST_LAYOUT); ++raw) {
        for (uint8_t flags = 0; flags < 16; ++flags) {
          const bool shifted = flags & 1, symbols = flags & 2, numbers = flags & 4, lang = flags & 8;
          fui::KeyboardLayoutBuffer buffer;
          const fui::KeyboardLayout& layout = fui::buildKeyboardLayout(buffer, static_cast<fui::KeyboardLayoutId>(raw),
                                                                       shifted, symbols, numbers, lang);

          FaceTarget target(body, small);
          fui::DeviceContext device;
          device.width = board.width;
          device.height = 800;
          device.hasTouch = true;
          const fui::InputSnapshot input{};
          fui::InteractionBuffer<64> interactions;
          fui::Frame<64> frame(target, device, input, interactions);
          // KeyboardEntryActivity::render's props on these boards.
          fui::KeyboardProps props;
          props.layout = &layout;
          props.keyAction = 1;
          props.labelText.font = fui::FONT_SLOT_BODY;
          props.altText.font = fui::FONT_SLOT_SMALL;
          props.gap = props.rowGap = 6;
          const int16_t height = static_cast<int16_t>(layout.rowCount * 42 + (layout.rowCount - 1) * 6 + 8);
          fui::keyboard(frame, fui::Rect{0, 0, board.width, height}, props);

          size_t keyCount = 0;
          for (uint8_t r = 0; r < layout.rowCount; ++r) keyCount += layout.rows[r].count;
          ASSERT_EQ(target.keys.size(), keyCount) << "one outline a key";
          size_t index = 0;
          for (uint8_t r = 0; r < layout.rowCount; ++r) {
            for (uint8_t c = 0; c < layout.rows[r].count; ++c, ++index) {
              const fui::KeyboardKey& key = layout.rows[r].keys[c];
              if (key.kind != fui::KeyKind::Normal || !fui::keyboardSingleCharacter(key.label)) continue;
              const FaceTarget::Key& drawn = target.keys[index];
              // The label's room: inside the key's padding, or, under a corner hint, 2 px in from either side
              // (keyboard()'s labelContentWidth).
              const int contentWidth =
                  key.alt ? drawn.rect.width - 4 : drawn.rect.width - props.keyPadding.left - props.keyPadding.right;
              for (const char* character : {key.label, key.alt}) {
                if (!character) continue;
                const std::string where = std::string(size.name) + " " + board.name + " layout " + std::to_string(raw) +
                                          " flags " + std::to_string(flags) + " " + character;
                const FaceTarget::Text* text = nullptr;
                for (const auto& t : drawn.texts) {
                  if (t.text == character) text = &t;
                }
                ASSERT_NE(text, nullptr) << where << ": not drawn as itself";
                const int width = target.face(text->font).width(character);
                // The target cuts a string wider than its rect: the rect has to hold it.
                EXPECT_LE(width, text->rect.width) << where;
                if (character != key.label) continue;
                ++characters;
                // Within the key: in the body face inside its padding, or, when that is too wide, in the small one
                // inside its outline. At the Large UI size the small face is Inter 10 with the pack's 10 pt, whose
                // widest isolated forms (ض, 27 px) take up to 5 px of a 12-key row's padding.
                if (text->font == fui::FONT_SLOT_SMALL) {
                  ++inTheSmallFace;
                  EXPECT_GT(body.width(character), contentWidth) << where;
                  EXPECT_LE(width, drawn.rect.width) << where;
                } else {
                  EXPECT_LE(width, contentWidth) << where;
                }
              }
            }
          }
        }
      }
    }
  }
  EXPECT_GT(characters, 10000u);
  EXPECT_GT(inTheSmallFace, 0u);  // the Arabic isolated forms the X3 cut to "…"
}

TEST(KeyboardLabels, EveryCharacterAndLayoutNameIsInAUiFaceOrThePack) {
  for (const UiSize& size : SIZES) {
    for (const auto& [faceData, points] : {std::pair<const EpdFontData&, unsigned>{size.body, size.bodyPoints},
                                           std::pair<const EpdFontData&, unsigned>{size.small, size.smallPoints}}) {
      const UiFace face(faceData, points);
      ASSERT_TRUE(face.loaded);
      for (uint8_t raw = 0; raw <= static_cast<uint8_t>(LAST_LAYOUT); ++raw) {
        for (uint8_t flags = 0; flags < 16; ++flags) {
          fui::KeyboardLayoutBuffer buffer;
          const fui::KeyboardLayout& layout = fui::buildKeyboardLayout(buffer, static_cast<fui::KeyboardLayoutId>(raw),
                                                                       flags & 1, flags & 2, flags & 4, flags & 8);
          for (uint8_t r = 0; r < layout.rowCount; ++r) {
            for (uint8_t c = 0; c < layout.rows[r].count; ++c) {
              const fui::KeyboardKey& key = layout.rows[r].keys[c];
              if (key.kind != fui::KeyKind::Normal) continue;
              for (const char* text : {key.label, key.alt}) {
                if (!text) continue;
                for (const uint32_t cp : formedCodepoints(text))
                  EXPECT_TRUE(face.holds(cp))
                      << size.name << " " << points << " pt: " << text << " U+" << std::hex << cp;
              }
            }
          }
        }
      }
      for (const auto& info : keyboard_layouts::ALL) {
        if (!info.nativeName) continue;
        for (const uint32_t cp : formedCodepoints(info.nativeName)) {
          EXPECT_TRUE(face.holds(cp)) << points << " pt: " << info.nativeName << " U+" << std::hex << cp;
        }
      }
    }
  }
}

// The Hamza key: the letter the mapper used to leave as it is, which no UI face nor the pack draws.
TEST(KeyboardLabels, TheHamzaKeyDrawsItsIsolatedForm) {
  EXPECT_EQ(formedCodepoints("\xD8\xA1"), (std::vector<uint32_t>{0xFE80}));
  const UiFace face(inter_ui_12_regular, 12);
  EXPECT_TRUE(face.holds(0xFE80));
  EXPECT_FALSE(face.holds(0x0621));
}
