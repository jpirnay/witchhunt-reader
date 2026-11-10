#include "SerialTestApi.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "BootDiagnostics.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/home/LibraryTab.h"
#include "util/ScreenshotUtil.h"
#include "util/SerialCommandGrammar.h"

#if SERIAL_TEST_API
#include <ArabicScripts.h>
#include <EpdFontFamily.h>
#include <Epub/Page.h>
#include <FileReplace.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <GlyphKey.h>
#include <GlyphProvider.h>
#include <GlyphRun.h>
#include <HalPowerManager.h>
#include <HalSystem.h>
#include <IndicScripts.h>
#include <Memory.h>
#include <OtShaper.h>
#include <ProtectedPaths.h>
#include <SerialTransfer/SerialTransferProtocol.h>
#include <ShapingFace.h>
#include <Utf8.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ttf/TtfFont.h>

#include <cstring>

#include "ButtonEventManager.h"
#include "CjkUiFonts.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SdCardFontGlobals.h"
#include "SettingsByKey.h"
#include "TouchUi.h"  // CP_TOUCH_UI
#include "UiLanguage.h"
#include "util/CjkLookupRun.h"
#include "util/Dictionary.h"
#include "util/IdleGate.h"
#endif

#if SERIAL_TEST_API
extern GfxRenderer renderer;  // main.cpp: CMD:LAB draws into its frame buffer
#endif

namespace SerialTestApi {

namespace {

using SerialCommandGrammar::appendField;

// ---- Output ---------------------------------------------------------------------------------

// logSerial.write() returns short when the USB-CDC TX ring stays full past its timeout (1 ms
// outside a transfer), so every line is written in a loop until the host has taken it. Yields
// rather than spins: a tight loop on the C3 starves the USB-CDC task being waited on. Gives up
// after `stallMs` without progress (the host stopped reading). `waiting` runs on each wait: a long stream feeds the
// task watchdog there.
bool writeAll(const uint8_t* data, const size_t len, const unsigned long stallMs, void (*waiting)() = nullptr) {
  size_t sent = 0;
  unsigned long lastProgressMs = millis();
  while (sent < len) {
    const size_t n = logSerial.write(data + sent, len - sent);
    if (n > 0) {
      sent += n;
      lastProgressMs = millis();
      continue;
    }
    if (millis() - lastProgressMs > stallMs) return false;
    if (waiting) waiting();
    delay(1);
  }
  return true;
}

bool writeText(const std::string_view text, const unsigned long stallMs) {
  return writeAll(reinterpret_cast<const uint8_t*>(text.data()), text.size(), stallMs);
}

// A reply waits up to 2 s for the host to read it.
constexpr unsigned long kReplyStallMs = 2000;

// One reply line: `head`, then `tail` (an argument echoed back), then the line end.
void reply(const std::string_view head, const std::string_view tail = {}, const unsigned long stallMs = kReplyStallMs) {
  if (writeText(head, stallMs) && writeText(tail, stallMs)) writeText("\n", stallMs);
}

void replyf(const char* format, ...) {
  char line[96];
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (n > 0) reply(std::string_view(line, std::min(static_cast<size_t>(n), sizeof(line) - 1)));
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

// "NAME" and "rest" of "NAME:rest".
void splitHead(const std::string_view text, std::string_view& head, std::string_view& rest) {
  const size_t colon = text.find(':');
  head = text.substr(0, colon);
  rest = colon == std::string_view::npos ? std::string_view() : text.substr(colon + 1);
}

#if SERIAL_TEST_API
// ---- Probes ---------------------------------------------------------------------------------

// Filled from whichever task draws (render task, loop task), read on the loop task. The critical
// section covers a copy of a few dozen bytes. `activity` is ActivityManager::activityChangeCount()
// when noted: what is noted on one screen is never reported for the next. `renderPass` is
// ActivityManager::renderPassCount(): a popup is on screen until the next pass redraws it.
portMUX_TYPE probeMux = portMUX_INITIALIZER_UNLOCKED;
constexpr size_t kProbeText = 64;  // a popup message or a row label; longer ones are cut

struct TextProbe {
  char text[kProbeText] = {};
  int index = -1;
  uint32_t activity = 0;
  uint32_t renderPass = 0;
  uint32_t atMs = 0;
  uint32_t sequence = 0;  // counts notes, so a host-side change detector sees a repeat too
  bool set = false;
};
TextProbe popupProbe;
TextProbe selectionProbe;

void noteText(TextProbe& probe, const int index, const char* text) {
  const uint32_t activity = activityManager.activityChangeCount();
  const uint32_t renderPass = activityManager.renderPassCount();
  const uint32_t now = millis();
  portENTER_CRITICAL(&probeMux);
  size_t i = 0;
  // One line on the wire: control characters (a popup's line breaks) become spaces.
  for (; text && text[i] != '\0' && i + 1 < kProbeText; ++i) {
    probe.text[i] = static_cast<unsigned char>(text[i]) < 0x20 ? ' ' : text[i];
  }
  // A text cut short must not end in half a UTF-8 character: drop its continuation bytes and lead.
  if (text && text[i] != '\0') {
    while (i > 0 && (static_cast<unsigned char>(probe.text[i - 1]) & 0xC0) == 0x80) --i;
    if (i > 0 && static_cast<unsigned char>(probe.text[i - 1]) >= 0xC0) --i;
  }
  probe.text[i] = '\0';
  probe.index = index;
  probe.activity = activity;
  probe.renderPass = renderPass;
  probe.atMs = now;
  probe.sequence++;
  probe.set = true;
  portEXIT_CRITICAL(&probeMux);
}

TextProbe readProbe(const TextProbe& probe) {
  portENTER_CRITICAL(&probeMux);
  const TextProbe copy = probe;
  portEXIT_CRITICAL(&probeMux);
  return copy;
}

// Section builds finished since service() last looked. Four, as with the touch ring: a backlog
// deeper than that means nobody is reading the events anyway.
struct BuildDone {
  int16_t spine;
  uint16_t pages;
};
constexpr int kBuildRing = 4;
BuildDone buildRing[kBuildRing] = {};
uint8_t buildHead = 0;
uint8_t buildTail = 0;

// Font downloads that ended, for EVT:FONT_INSTALLED.
struct FontInstalled {
  char family[32];
  bool ok;
};
constexpr int kFontRing = 4;
FontInstalled fontRing[kFontRing] = {};
uint8_t fontHead = 0;
uint8_t fontTail = 0;

// ---- State shared by the commands and service() ---------------------------------------------

GfxRenderer* testRenderer = nullptr;
bool eventsOn = false;
uint32_t reportedActivity = 0;
uint32_t reportedPopup = 0;
uint32_t reportedFills = 0;
uint32_t reportedWipes = 0;
uint32_t reportedRooms = 0;
// An event line (sent unasked once EVENTS:ON) waits only 50 ms, so a host that enabled events and
// went away cannot hold the loop up.
constexpr unsigned long kEventStallMs = 50;

IdleGate idleGate;
bool idleScreenOnly = false;
constexpr uint32_t kIdleDefaultTimeoutMs = 10000;
constexpr uint32_t kIdleMaxTimeoutMs = 600000;

// The last command meant to change the screen, which CMD:IDLE waits to see through (IdleGate).
// Input: a press or touch, handed over once HalGPIO has delivered it and the classifier is done
// with it. Screen: an ACT, handed over once the activity on top has changed.
enum class Effect : uint8_t { Input, Screen };
Effect effect = Effect::Input;
uint32_t effectActivityBase = 0;

void noteEffect(const Effect kind) {
  effect = kind;
  effectActivityBase = activityManager.activityChangeCount();
  idleGate.noteEffect();
}

bool effectHandled() {
  switch (effect) {
    case Effect::Input:
      return !gpio.injectedInputPending() && !globalButtonEvents().isMidGesture();
    case Effect::Screen:
      return activityManager.activityChangeCount() != effectActivityBase;
  }
  return true;
}

PowerRequest powerRequest = PowerRequest::None;
#else
enum class Effect : uint8_t { Input, Screen };
inline void noteEffect(Effect) {}
#endif  // SERIAL_TEST_API

// ---- BTN ------------------------------------------------------------------------------------

void cmdButton(const std::string_view arg) {
  using SerialCommandGrammar::PressKind;
  std::string_view name;
  PressKind kind = PressKind::Short;
  if (!SerialCommandGrammar::parseButtonArg(arg, name, kind)) {
    reply("BTN_ERROR:bad press ", arg);
    return;
  }
  struct NamedButton {
    const char* name;
    uint8_t index;
  };
  static constexpr NamedButton kButtons[] = {{"BACK", HalGPIO::BTN_BACK},  {"CONFIRM", HalGPIO::BTN_CONFIRM},
                                             {"LEFT", HalGPIO::BTN_LEFT},  {"RIGHT", HalGPIO::BTN_RIGHT},
                                             {"UP", HalGPIO::BTN_UP},      {"DOWN", HalGPIO::BTN_DOWN},
#if SERIAL_TEST_API
                                             {"POWER", HalGPIO::BTN_POWER}
#endif
  };
  for (const NamedButton& button : kButtons) {
    if (name != button.name) continue;
    switch (kind) {
      case PressKind::Short:
        gpio.injectPress(button.index);
        break;
      case PressKind::Long:
#if SERIAL_TEST_API
        if (button.index == HalGPIO::BTN_POWER) {
          // Holding power sleeps the device, and that is main.cpp's hold timer reading the live
          // pin, which an injected press never reaches. Do what the hold does.
          powerRequest = PowerRequest::Sleep;
          reply("BTN_OK");
          return;
        }
#endif
        gpio.injectPress(button.index, /*longPress=*/true);
        break;
      case PressKind::Double: {
#if SERIAL_TEST_API
        // Two taps inside ButtonEventManager::DOUBLE_WINDOW_MS (300 ms), dated apart so the
        // classifier sees two presses. A button with no double action configured takes them as two
        // short presses, as it would from a finger.
        constexpr uint32_t kTapGapMs = 120;
        const uint32_t now = millis();
        gpio.injectPress(button.index, false, now > kTapGapMs ? now - kTapGapMs : 0);
        gpio.injectPress(button.index, false, now);
        break;
#else
        reply("BTN_ERROR:bad press ", arg);
        return;
#endif
      }
    }
    noteEffect(Effect::Input);
    reply("BTN_OK");
    return;
  }
  reply("BTN_ERROR:unknown button ", name);
}

// ---- ACT ------------------------------------------------------------------------------------

bool openReader(const std::string_view arg) {
#if SERIAL_TEST_API
  SerialCommandGrammar::ReaderTarget target;
  if (!SerialCommandGrammar::parseReaderTarget(arg, target)) {
    reply("ACT_ERROR:bad position ", arg);
    return false;
  }
  std::string path(target.path);
#else
  std::string path(arg);
#endif
  if (!Storage.exists(path.c_str())) {
    reply("ACT_ERROR:no such file ", path);
    return false;
  }
#if SERIAL_TEST_API
  using Kind = SerialCommandGrammar::ReaderTarget::Kind;
  switch (target.kind) {
    case Kind::Saved:
      break;
    case Kind::SpinePage: {
      // The hand-off the bookmark list uses (0-based); the reader applies it in onEnter().
      auto& jump = APP_STATE.pendingBookmarkJump;
      jump.active = true;
      jump.bookPath = path;
      jump.spineIndex = static_cast<uint16_t>(target.spine - 1);
      jump.pageNumber = static_cast<uint16_t>(target.page - 1);
      APP_STATE.saveToFile();
      break;
    }
    case Kind::Percent: {
      auto& jump = APP_STATE.pendingPercentJump;
      jump.active = true;
      jump.bookPath = path;
      jump.percent = static_cast<uint8_t>(target.percent);
      break;
    }
  }
#endif
  // Opened as from a list, refresh included, so a test sees the open a reader sees.
  activityManager.openBook(std::move(path));
  return true;
}

// A keyword's position in `names`, or -1.
int indexOf(const std::string_view name, const char* const* names, const int count) {
  for (int i = 0; i < count; ++i) {
    if (name == names[i]) return i;
  }
  return -1;
}

#if SERIAL_TEST_API
// CMD:ACT:DICT:<word>. The dictionary screen's lookup without the screen: open the configured
// dictionary, build its index if it needs one, look the word up, and say what came of it.
void lookUpWord(const std::string_view wordView) {
  if (wordView.empty()) {
    reply("DICT_ERROR:no word");
    return;
  }
  const std::string text(wordView);  // not `word`: Arduino.h typedefs that
  const char* dictionaryName = SETTINGS.dictionaryName;
  const char* outcome = "no_dictionary";
  std::string definition;
  std::string headword;
  bool found = false;
  // A word that starts with Han, kana or Hangul takes the prefix search, as the word screen does.
  const bool cjk = CjkLookupRun::scriptOf(text.c_str(), text.size()) != CjkLookupRun::Script::Other;
  CjkLookupRun run;
  if (cjk) run.append(text.c_str(), text.size());
  DictPrefixMatch match;
  long searchMs = 0;
  long exactMs = -1;  // -1: no exact baseline ran
  if (dictionaryName[0] != '\0') {
    // The object carries scan buffers and an inflate window between lookups; on the heap for the
    // length of this command only, and makeUniqueNoThrow so a fragmented heap is an answer, not
    // an abort.
    auto dict = makeUniqueNoThrow<Dictionary>();
    if (!dict) {
      outcome = "low_memory";
    } else if (!dict->open(dictionaryName)) {
      outcome = "open_failed";
    } else if (dict->needsIndex() && !dict->buildIndex([](void*) { vTaskDelay(1); })) {
      outcome = "index_failed";
    } else {
      Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
      if (cjk) {
        // Run unabortable, so the time is the search's and not a keypress's.
        const unsigned long searchStart = millis();
        found = dict->resolvePrefixes(run.text(), match, &result);
        searchMs = static_cast<long>(millis() - searchStart);
        if (found) {
          // The old path for the same headword, after the search so any cache warmth favours it.
          DictEntries exactEntries;
          std::string exactHeadword;
          const unsigned long exactStart = millis();
          dict->resolve(match.hits[0].headword.c_str(), exactEntries, exactHeadword);
          exactMs = static_cast<long>(millis() - exactStart);
          found = dict->readPrefixDefinitions(match, definition, &result);
          headword = match.hits[0].headword;
        }
      } else {
        found = dict->lookup(text.c_str(), definition, headword, &result);
      }
      outcome = Dictionary::resultName(result);
    }
  }
  Dictionary::logLookup(dictionaryName, cjk ? run.text() : text.c_str(), outcome, found ? headword.c_str() : nullptr);
  std::string line = "DICT:result=";
  line += outcome;
  appendField(line, "dict", dictionaryName);
  appendField(line, "word", text);
  if (found) {
    appendField(line, "headword", headword);
    appendField(line, "bytes", static_cast<long>(definition.size()));
  }
  if (cjk) {
    long matched = 0;
    std::string hits;
    std::string entries;
    for (uint8_t i = 0; i < match.count; ++i) {
      if (i > 0) {
        hits += ',';
        entries += ',';
      }
      hits += match.hits[i].headword;
      entries += std::to_string(match.hits[i].entryCount);
    }
    if (match.count > 0) {
      const auto* cursor = reinterpret_cast<const unsigned char*>(run.text());
      const auto* const end = cursor + match.hits[0].runBytes;
      while (cursor < end && utf8NextCodepoint(&cursor) != 0) ++matched;
    }
    appendField(line, "run", run.text());
    appendField(line, "matched", matched);
    appendField(line, "hits", hits);
    appendField(line, "entries", entries);
    appendField(line, "probes", static_cast<long>(match.probes));
    appendField(line, "reads", static_cast<long>(match.reads));
    appendField(line, "ms", searchMs);
    if (exactMs >= 0) appendField(line, "exact_ms", exactMs);
  }
  reply(line);
}
#endif  // SERIAL_TEST_API

void cmdActivity(const std::string_view arg) {
  std::string_view target;
  std::string_view rest;
  splitHead(arg, target, rest);
  static constexpr const char* kTargets[] = {"HOME",    "READER", "SETTINGS",      "BOOKMARKS",   "FILES",
                                             "LIBRARY", "DICT",   "FILE_TRANSFER", "USB_TRANSFER"};
#if SERIAL_TEST_API
  constexpr int kTargetCount = 9;
#else
  constexpr int kTargetCount = 6;  // the original targets; the rest come with SERIAL_TEST_API
#endif
  switch (indexOf(target, kTargets, kTargetCount)) {
    case 0:
      activityManager.goHome();
      break;
    case 1:
      if (!openReader(rest)) return;
      break;
    case 2: {
      int tab = 0;
#if SERIAL_TEST_API
      // SettingsActivity's tabs, in its categoryNames order, or their numbers.
      static constexpr const char* kTabs[] = {"DISPLAY", "READER", "CONTROLS", "SYSTEM"};
      if (!rest.empty()) {
        tab = indexOf(rest, kTabs, 4);
        if (tab < 0 && (!SerialCommandGrammar::parseInt(rest, tab) || tab < 0 || tab > 3)) {
          reply("ACT_ERROR:unknown settings tab ", rest);
          return;
        }
      }
#endif
      activityManager.goToSettings(tab);
      break;
    }
    case 3:
      activityManager.goToGlobalBookmarks();
      break;
    case 4:
      activityManager.goToLibrary(LibraryTab::Books, std::string(rest));
      break;
    case 5: {
      // LibraryTab's order.
      static constexpr const char* kTabs[] = {"BOOKS", "RECENT", "NEW", "AUTHORS"};
      const int tab = rest.empty() ? 0 : indexOf(rest, kTabs, 4);
      if (tab < 0) {
        reply("ACT_ERROR:unknown library tab ", rest);
        return;
      }
      activityManager.goToLibrary(static_cast<LibraryTab>(tab));
      break;
    }
#if SERIAL_TEST_API
    case 6:
      lookUpWord(rest);
      return;
    case 7:
      activityManager.goToFileTransfer();
      break;
    case 8:
      // Leaving the transfer screen reboots (SerialTransferActivity::onExit), so replacing it with
      // a fresh one would land on Home: already there is already done.
      if (std::strcmp(activityManager.currentActivityName(), "SerialTransfer") == 0) {
        reply("ACT_OK");
        return;
      }
      activityManager.goToSerialTransfer();
      break;
#endif
    default:
      reply("ACT_ERROR:unknown activity ", target);
      return;
  }
  noteEffect(Effect::Screen);
  reply("ACT_OK");
}

// ---- STATE ----------------------------------------------------------------------------------

void cmdState(std::string_view /*arg*/) {
  std::string state = "STATE:";
  state.reserve(320);
  {
    RenderLock lock;  // the activity's state is also read by the render task
    activityManager.describeState(state);
  }
#if SERIAL_TEST_API
  const uint32_t activity = activityManager.activityChangeCount();
  const TextProbe selection = readProbe(selectionProbe);
  // Screens that know their own selection (Home) wrote sel= already.
  if (selection.set && selection.activity == activity && state.find(" sel=") == std::string::npos) {
    appendField(state, "sel", static_cast<long>(selection.index));
    if (selection.text[0] != '\0') appendField(state, "label", selection.text);
  }
  const TextProbe popup = readProbe(popupProbe);
  if (popup.set && popup.activity == activity && popup.renderPass == activityManager.renderPassCount()) {
    appendField(state, "popup", popup.text);
    appendField(state, "popup_ms", static_cast<long>(millis() - popup.atMs));
  }
  // The TrueType family behind the UI faces (CjkUiFonts::setReaderFont), when one is.
  if (CjkUiFonts::hasReaderFont()) appendField(state, "ui_font", sdFontSystem.loadedFamily().c_str());
  appendField(state, "heap", static_cast<long>(ESP.getFreeHeap()));
  appendField(state, "maxblock",
              static_cast<long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
#endif
  reply(state);
}

#if SERIAL_TEST_API
// ---- IDLE / EVENTS --------------------------------------------------------------------------

void cmdIdle(const std::string_view arg) {
  // [<timeout_ms>][:SCREEN]
  std::string_view first;
  std::string_view second;
  splitHead(arg, first, second);
  int timeout = static_cast<int>(kIdleDefaultTimeoutMs);
  if (first == "SCREEN") {
    second = first;
  } else if (!first.empty() && (!SerialCommandGrammar::parseInt(first, timeout) || timeout <= 0)) {
    reply("IDLE_ERROR:bad timeout ", first);
    return;
  }
  if (!second.empty() && second != "SCREEN") {
    reply("IDLE_ERROR:bad mode ", second);
    return;
  }
  idleScreenOnly = !second.empty();
  idleGate.begin(millis(), std::min(static_cast<uint32_t>(timeout), kIdleMaxTimeoutMs));
}

void cmdEvents(const std::string_view arg) {
  if (arg == "ON" || arg == "OFF") {
    eventsOn = arg == "ON";
  } else if (!arg.empty()) {
    reply("EVENTS_ERROR:expected ON or OFF, got ", arg);
    return;
  }
  replyf("EVENTS_OK:%s", eventsOn ? "on" : "off");
}

// ---- GET / SET ------------------------------------------------------------------------------

// "<key>=<value>[ label=<option label>]" for a GET or SET_OK reply.
void appendSetting(std::string& line, const SettingInfo& s) {
  line += s.key;
  line += '=';
  int number = 0;
  if (SettingsByKey::readNumber(s, number)) {
    char digits[12];
    snprintf(digits, sizeof(digits), "%d", number);
    line += digits;
    if (s.type == SettingType::ENUM && number >= 0 && number < SettingsByKey::optionCount(s)) {
      appendField(line, "label", SettingsByKey::optionLabel(s, static_cast<uint8_t>(number)));
    }
  } else if (s.type != SettingType::STRING) {
    line += "(none)";
  } else if (s.obfuscated) {
    line += "(hidden)";  // credentials never leave the device, as on the web settings page
  } else if (s.stringGetter) {
    SerialCommandGrammar::appendValue(line, s.callStringGetter());
  } else {
    SerialCommandGrammar::appendValue(
        line, s.stringMaxLen > 0 ? reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset : "");
  }
}

// GET and GET:* both go through SettingsByKey::visitKeyed(), one walk with the web settings API.
void replyGetRow(const SettingInfo& s, void* /*ctx*/) {
  if (!SettingsByKey::optionsKnown(s)) {
    // The family the setting names is not among the listed ones: its index and label would be wrong.
    reply("GET_ERROR:font families not loaded on this screen ", s.key);
    return;
  }
  std::string line = "GET:";
  appendSetting(line, s);
  reply(line);
}

// The UI language is not a setting: I18n keeps it in a file of its own, and the web settings API has
// no row for it. GET and SET take it by this key all the same.
constexpr std::string_view LANGUAGE_KEY = "language";

// "language=<code> label=<name>", as a GET or SET_OK reply shows it.
void appendLanguage(std::string& line) {
  const Language language = I18N.getLanguage();
  line += LANGUAGE_KEY;
  line += '=';
  line += I18N.getLanguageCode(language);
  appendField(line, "label", I18N.getLanguageName(language));
}

void cmdGet(const std::string_view key) {
  if (key == "*") {
    const int count = SettingsByKey::visitKeyed({}, &replyGetRow, nullptr);
    std::string line = "GET:";
    appendLanguage(line);
    reply(line);
    replyf("GET_END:%d", count + 1);
  } else if (key == LANGUAGE_KEY) {
    std::string line = "GET:";
    appendLanguage(line);
    reply(line);
  } else if (key.empty() || SettingsByKey::visitKeyed(key, &replyGetRow, nullptr) == 0) {
    reply("GET_ERROR:unknown key ", key);
  }
}

struct SetRequest {
  std::string_view value;
  const char* error;     // the reply's head when the write did not happen
  bool errorNamesValue;  // the error is about the value, not the key
};

void writeSetRow(const SettingInfo& s, void* ctx) {
  auto& request = *static_cast<SetRequest*>(ctx);
  SettingsByKey::WriteResult result = SettingsByKey::WriteResult::NotWritable;
  if (!SettingsByKey::optionsKnown(s)) {
    // On the boot straight into USB Transfer the card's font families are not listed: a name would
    // be refused as a bad value, and an index would store whatever it means among the built-ins.
    request.error = "SET_ERROR:font families not loaded on this screen ";
    return;
  }
  if (s.type == SettingType::STRING) {
    result = SettingsByKey::writeString(s, std::string(request.value));
  } else {
    int number = 0;
    bool parsed = SerialCommandGrammar::parseInt(request.value, number);
    // An ENUM row also takes one of its option labels, as GET shows them.
    for (uint8_t i = 0; !parsed && s.type == SettingType::ENUM && i < SettingsByKey::optionCount(s); ++i) {
      if (request.value == SettingsByKey::optionLabel(s, i)) {
        number = i;
        parsed = true;
      }
    }
    if (!parsed) {
      request.error = "SET_ERROR:bad value ";
      request.errorNamesValue = true;
      return;
    }
    result = SettingsByKey::writeNumber(s, number);
  }
  switch (result) {
    case SettingsByKey::WriteResult::Applied:
      request.error = nullptr;
      return;
    case SettingsByKey::WriteResult::OutOfRange:
      request.error = "SET_ERROR:out of range ";
      request.errorNamesValue = true;
      return;
    case SettingsByKey::WriteResult::NotWritable:
      request.error = "SET_ERROR:not writable ";
      return;
  }
}

void replySetRow(const SettingInfo& s, void* /*ctx*/) {
  std::string line = "SET_OK:";
  appendSetting(line, s);
  reply(line);
}

// SET:language=<code|name|index>: the switch the language picker makes, then a redraw of the screen
// on display so it shows the new strings.
void setLanguage(const std::string_view value) {
  constexpr size_t count = static_cast<size_t>(Language::_COUNT);
  const char* codes[count];
  const char* names[count];
  for (size_t i = 0; i < count; ++i) {
    codes[i] = I18N.getLanguageCode(static_cast<Language>(i));
    names[i] = I18N.getLanguageName(static_cast<Language>(i));
  }
  size_t index = 0;
  if (!SerialCommandGrammar::parseLanguage(value, codes, names, count, index)) {
    reply("SET_ERROR:bad value ", value);
    return;
  }
  UiLanguage::apply(static_cast<Language>(index));
  activityManager.requestUpdate();
  std::string line = "SET_OK:";
  appendLanguage(line);
  reply(line);
}

void cmdSet(const std::string_view arg) {
  std::string_view key;
  SetRequest request{{}, "SET_ERROR:unknown key ", false};
  if (!SerialCommandGrammar::splitKeyValue(arg, key, request.value)) {
    reply("SET_ERROR:expected <key>=<value>");
    return;
  }
  if (key == LANGUAGE_KEY) {
    setLanguage(request.value);
    return;
  }
  SettingsByKey::visitKeyed(key, &writeSetRow, &request);
  if (request.error) {
    reply(request.error, request.errorNamesValue ? request.value : key);
    return;
  }
  // As the web settings page does: settle the settings that depend on others, save, and read the
  // value back so the reply shows what was stored (a dependent setting may have moved it).
  if (!SettingsByKey::commit()) {
    reply("SET_ERROR:not saved ", key);
    return;
  }
  SettingsByKey::visitKeyed(key, &replySetRow, nullptr);
}

// ---- LOG ------------------------------------------------------------------------------------

void cmdLog(const std::string_view arg) {
  static constexpr const char* kLevelTags[] = {"ERR", "INF", "DBG", "TRC"};
  uint8_t level = 0;
  if (!arg.empty()) {
    if (!SerialCommandGrammar::parseLogLevel(arg, level)) {
      reply("LOG_ERROR:unknown level ", arg);
      return;
    }
    setLogWireLevel(level);
  }
  constexpr int kCompiled = LOG_LEVEL < 3 ? LOG_LEVEL : 3;
  replyf("LOG_OK:level=%s compiled=%s", kLevelTags[std::min<int>(logWireLevel(), 3)], kLevelTags[kCompiled]);
}

// ---- TOUCH / SWIPE --------------------------------------------------------------------------

// CMD:TOUCH:<x>,<y>[:LONG] (2 numbers) or CMD:SWIPE:<x1>,<y1>,<x2>,<y2> (4): logical screen pixels
// as drawn now, mapped to the touch HAL's normalized panel coordinates and queued like a contact.
// Builds for boards without a digitiser (CP_TOUCH_UI 0: X3, X4) only say so.
void injectContact(const std::string_view arg, const size_t count, const char* ok, const bool longPress = false) {
#if CP_TOUCH_UI
  if (!gpio.hasTouch() || !testRenderer) {
    reply("TOUCH_ERROR:no touch panel");
    return;
  }
  int xy[4];
  float n[4];
  bool onScreen = SerialCommandGrammar::parseIntList(arg, xy, count);
  // The reader's frame, as the touch HAL maps it: a vertical page holds the turned frame as the live one while it
  // draws, and a contact sent then is still a point of the reader's.
  const GfxRenderer::Orientation held = testRenderer->getHeldOrientation();
  for (size_t i = 0; onScreen && i < count; i += 2) {
    onScreen = xy[i] >= 0 && xy[i + 1] >= 0 && xy[i] < testRenderer->getScreenWidth(held) &&
               xy[i + 1] < testRenderer->getScreenHeight(held);
    if (onScreen) testRenderer->logicalToTap(xy[i], xy[i + 1], n[i], n[i + 1]);
  }
  if (!onScreen) {
    reply("TOUCH_ERROR:not on screen ", arg);
    return;
  }
  if (count == 4) {
    gpio.injectSwipe(n[0], n[1], n[2], n[3]);
  } else if (longPress) {
    gpio.injectLongTouch(n[0], n[1]);
  } else {
    gpio.injectTouch(n[0], n[1]);
  }
  noteEffect(Effect::Input);
  reply(ok);
#else
  (void)arg;
  (void)count;
  (void)ok;
  (void)longPress;
  reply("TOUCH_ERROR:no touch panel");
#endif
}

void cmdTouch(const std::string_view arg) {
  // BTN's suffix grammar, with the point where the button's name goes.
  std::string_view point;
  SerialCommandGrammar::PressKind kind = SerialCommandGrammar::PressKind::Short;
  if (!SerialCommandGrammar::parseButtonArg(arg, point, kind) || kind == SerialCommandGrammar::PressKind::Double) {
    reply("TOUCH_ERROR:not on screen ", arg);
    return;
  }
  injectContact(point, 2, "TOUCH_OK", kind == SerialCommandGrammar::PressKind::Long);
}
void cmdSwipe(const std::string_view arg) { injectContact(arg, 4, "SWIPE_OK"); }

// ---- HEAP -----------------------------------------------------------------------------------

// Test ballast: blocks held on the heap until CMD:HEAP:FREE, to put the device where a short or broken-up heap takes
// it (the reader's memory ladder, ImageMemoryLadder) on demand.
constexpr int kBallastBlocks = 16;
void* ballast[kBallastBlocks] = {};
size_t ballastBytes = 0;
// CMD:HEAP:REFUSE: set on the loop task, used up on whichever task climbs the ladder.
std::atomic<uint8_t> decodeRefusals{0};
std::atomic<uint8_t> reloadRefusals{0};

bool takeRefusal(std::atomic<uint8_t>& refusals) {
  uint8_t left = refusals.load();
  while (left > 0 && !refusals.compare_exchange_weak(left, static_cast<uint8_t>(left - 1))) {
  }
  return left > 0;
}

void replyHeap(const char* head) {
  replyf("%s:free=%lu largest=%lu held=%lu", head, static_cast<unsigned long>(esp_get_free_heap_size()),
         static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)),
         static_cast<unsigned long>(ballastBytes));
}

// CMD:HEAP (what is free), CMD:HEAP:HOLD=<bytes>[,<blocks>] (hold that much more, in equal blocks), CMD:HEAP:FREE,
// CMD:HEAP:REFUSE=<decodes>[,<reloads>].
void cmdHeap(const std::string_view arg) {
  int decodes = 0;
  int reloads = 0;
  if (SerialCommandGrammar::parseHeapRefuse(arg, decodes, reloads)) {
    decodeRefusals = static_cast<uint8_t>(decodes);
    reloadRefusals = static_cast<uint8_t>(reloads);
    replyf("HEAP_OK:refuse decodes=%d reloads=%d", decodes, reloads);
    return;
  }
  if (arg == "FREE") {
    for (void*& block : ballast) {
      free(block);
      block = nullptr;
    }
    ballastBytes = 0;
  } else if (!arg.empty()) {
    int bytes = 0;
    int blocks = 0;
    if (!SerialCommandGrammar::parseHeapHold(arg, bytes, blocks, kBallastBlocks)) {
      reply("HEAP_ERROR:expected HOLD=<bytes>[,<blocks>], REFUSE=<decodes>[,<reloads>] or FREE");
      return;
    }
    const size_t each = static_cast<size_t>(bytes / blocks);
    for (void*& block : ballast) {
      if (blocks == 0) break;
      if (block) continue;
      if (!(block = malloc(each))) break;
      ballastBytes += each;
      --blocks;
    }
    if (blocks > 0) {
      replyHeap("HEAP_ERROR:not all held");
      return;
    }
  }
  replyHeap("HEAP_OK");
}

// ---- TTF ------------------------------------------------------------------------------------

// CMD:TTF's fields for a loaded set. Under RenderLock.
void appendStats(std::string& line, const TtfFontSet& set) {
  const TtfFontSet::Stats s = set.stats();
  std::string names;
  for (uint8_t f = 0; f < sdFontSystem.ttfFamilyCount(); ++f) {
    if (f > 0) names += ',';
    names += sdFontSystem.ttfFamilyName(f);
  }
  line += "families=" + std::to_string(s.families);
  appendField(line, "names", names);
  appendField(line, "sd", s.readsCard ? 1L : 0L);
  appendField(line, "log_used", static_cast<long>(s.logUsed));
  appendField(line, "log_size", static_cast<long>(s.logSize));
  appendField(line, "slots", static_cast<long>(s.usedSlots));
  appendField(line, "ring", s.ringMode ? 1L : 0L);
  appendField(line, "pending", s.wipePending ? 1L : 0L);
  appendField(line, "renders", static_cast<long>(s.renders));
  appendField(line, "hits", static_cast<long>(s.hits));
  appendField(line, "segments", static_cast<long>(s.segments));
  appendField(line, "free", static_cast<long>(s.freeSegments));
  appendField(line, "live_slots", static_cast<long>(s.liveSlots));
  appendField(line, "draws", static_cast<long>(s.drawsCounted));
}

// What CMD:TTF:SHAPE measured for one word.
struct ShapeReport {
  const char* error = nullptr;
  const char* script = "";
  uint32_t cpac = 0;
  uint32_t initUs = 0;
  long repointUs = -1;
  uint32_t planUs = 0;
  uint32_t planBytes = 0;
  uint32_t shapeUs = 0;
  unsigned glyphs = 0;
  int width = 0;
  unsigned stored = 0;
};

constexpr unsigned kShapeRunCodepoints = 48;  // the run: the word from its first Indic or Arabic letter
constexpr unsigned kShapeSlots = 112;         // the shaper's buffer: room to grow past the run

// Rounds a 26.6 value to whole pixels, halves away from zero.
int roundPixels(const int32_t v) { return v >= 0 ? (v + 32) / 64 : -((-v + 32) / 64); }

// Shapes the first Indic or Arabic run of `text` with the body family's regular style at the body size, as a section
// build would, and times each step. Every block it makes is freed on return, the plan before the caller lets go of the
// shaping faces. Under RenderLock.
void shapeWord(TtfFontSet& set, TtfFont& font, const std::string_view language, const std::string_view text,
               ShapeReport& r) {
  const auto utf8 = makeUniqueNoThrow<char[]>(text.size() + 1);         // <= 193 B: the text, NUL-ended
  const auto cps = makeUniqueNoThrow<uint32_t[]>(kShapeRunCodepoints);  // 192 B: the run's code points
  const auto glyphs = makeUniqueNoThrow<uint16_t[]>(kShapeSlots);       // 224 B: the run's glyph IDs
  const auto records = makeUniqueNoThrow<uint8_t[]>(6 * kShapeSlots);   // 672 B: six bytes a record, at most
  auto plan = makeUniqueNoThrow<ot::Plan>();                            // ~1-4 KB: its lookups and their digests
  auto buffer = makeUniqueNoThrow<ot::Buffer>();                        // 1,272 B: the shaper's state
  const auto storage = makeUniqueNoThrow<uint8_t[]>(ot::Buffer::bytesFor(kShapeSlots));  // 6,947 B: its arrays
  if (!utf8 || !cps || !glyphs || !records || !plan || !buffer || !storage) {
    r.error = "no memory";
    return;
  }
  memcpy(utf8.get(), text.data(), text.size());

  unsigned count = 0;
  const indic::ScriptInfo* script = nullptr;
  bool inArabic = false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8.get());
       *p && count < kShapeRunCodepoints;) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!script && !inArabic) {
      script = indic::scriptOf(cp);
      inArabic = !script && arabic::isRunMember(cp) && !arabic::isMark(cp);  // a run starts at a base
    }
    if (inArabic && !arabic::isRunMember(cp) && cp != arabic::ZWNJ && cp != arabic::ZWJ) break;  // the run ends here
    if (script || inArabic) cps[count++] = cp;
  }
  if (!script && !inArabic) {
    r.error = "no Indic or Arabic letter";
    return;
  }
  r.script = inArabic ? "Arabic" : script->name;

  ShapingFace* const face = font.shapingFace(0);
  const EpdFont* const regular = font.font(0);
  if (!face || !regular) {
    r.error = "no shaping face";
    return;
  }
  uint32_t start = micros();
  const ot::Face* otFace = face->current();
  r.initUs = micros() - start;
  if (!otFace) {
    r.error = "no shaping face";
    return;
  }

  r.cpac = face->cpacBytes();

  // Mapping another flash-mode style's block unmaps this one's: the next current() re-points the face. The move is
  // forced untimed, then timed; -1 when no other style moved it.
  if (!set.readsCard()) {
    for (uint8_t style = 1; style < 4; ++style) {
      ShapingFace* const other = font.shapingFace(style);
      if (!other || other == face || !other->current()) continue;
      start = micros();
      face->current();
      const uint32_t elapsed = micros() - start;
      if (face->repointed()) r.repointUs = static_cast<long>(elapsed);
      break;
    }
  }

  char tag[16] = {};  // the language, at most 15 characters
  memcpy(tag, language.data(), language.size());
  otFace = face->current();
  start = micros();
  const bool planned =
      otFace && plan->build(*otFace, inArabic ? ot::Script::Arabic : static_cast<ot::Script>(indic::indexOf(*script)),
                            ot::languageTagsFor(tag));
  r.planUs = micros() - start;
  if (!planned) {
    r.error = "plan failed";
    return;
  }
  r.planBytes = static_cast<uint32_t>(plan->memoryBytes());

  buffer->setStorage(storage.get(), ot::Buffer::bytesFor(kShapeSlots));
  const uint32_t ppem = font.bodyPpem();
  ot::Scale scale;
  scale.set(static_cast<int32_t>(ppem), (ppem + 32) >> 6, otFace->upem());
  otFace = face->current();
  start = micros();
  const bool shaped = otFace && ot::shape(*otFace, scale, *plan, cps.get(), count, *buffer);
  r.shapeUs = micros() - start;
  if (!shaped) {
    r.error = "shape failed";
    return;
  }

  // The run as a section stores it, and the glyphs it needs, each once.
  size_t bytes = 0;
  unsigned distinct = 0;
  for (unsigned i = 0; i < buffer->len(); ++i) {
    const ot::GlyphInfo& info = buffer->info[i];
    const ot::GlyphPosition& pos = buffer->pos[i];
    GlyphRecord record;
    record.glyph = static_cast<uint16_t>(info.codepoint);
    record.advance = static_cast<int16_t>(std::clamp<int32_t>((pos.xAdvance + 2) >> 2, -16384, 16383));
    record.dx = static_cast<int8_t>(std::clamp<int32_t>(roundPixels(pos.xOffset), -128, 127));
    record.dy = static_cast<int8_t>(std::clamp<int32_t>(-roundPixels(pos.yOffset), -128, 127));
    size_t written = 0;
    if (info.codepoint > 0xFFFF ||
        !glyph_run::encode(record, records.get() + bytes, 6 * kShapeSlots - bytes, &written)) {
      r.error = "shape failed";
      return;
    }
    bytes += written;
    if (std::find(glyphs.get(), glyphs.get() + distinct, record.glyph) == glyphs.get() + distinct) {
      glyphs[distinct++] = record.glyph;
    }
  }
  r.glyphs = buffer->len();
  r.width = glyphRunAdvance(GlyphRun{records.get(), static_cast<uint16_t>(buffer->len()), static_cast<uint16_t>(bytes)},
                            false);

  // The store holds a glyph when asking for it is a hit: the lookups after the prefetch count their hits.
  const EpdFontFamily family(regular);
  family.prefetchGlyphs(glyphs.get(), distinct, EpdFontFamily::REGULAR);
  const uint32_t hitsBefore = font.storeHits();
  for (unsigned i = 0; i < distinct; ++i) {
    family.findGlyphKey(glyph_key::of(glyphs[i]), EpdFontFamily::REGULAR);
  }
  // The lookups above count their hits in the same store, so the two reads differ.
  // cppcheck-suppress duplicateExpression
  r.stored = font.storeHits() - hitsBefore;
}

// CMD:TTF:SHAPE:<bcp47>:<text>: one word shaped on the device, to time the steps a section build repeats.
void cmdTtfShape(const std::string_view arg) {
  std::string_view language;
  std::string_view text;
  if (!SerialCommandGrammar::parseTtfShape(arg, language, text)) {
    reply("TTF_ERROR:expected SHAPE:<language>:<text of 1-192 bytes>");
    return;
  }
  ShapeReport report;
  {
    RenderLock lock;  // the glyph store and the shaping faces are the render task's; the reply goes out after
    TtfFontSet* set = sdFontSystem.ttfSet();
    TtfFont* font = set ? set->font(0) : nullptr;
    if (!font) {
      report.error = "no TrueType family loaded";
    } else {
      shapeWord(*set, *font, language, text, report);
      set->releaseShapingFaces();  // after the plan, which read the face's tables
    }
  }
  if (report.error) {
    reply("TTF_ERROR:", report.error);
    return;
  }
  std::string line = std::string("TTF_SHAPE:script=") + report.script;
  appendField(line, "cpac", static_cast<long>(report.cpac));
  appendField(line, "init_us", static_cast<long>(report.initUs));
  appendField(line, "repoint_us", report.repointUs);
  appendField(line, "plan_us", static_cast<long>(report.planUs));
  appendField(line, "plan_bytes", static_cast<long>(report.planBytes));
  appendField(line, "shape_us", static_cast<long>(report.shapeUs));
  appendField(line, "glyphs", static_cast<long>(report.glyphs));
  appendField(line, "width", static_cast<long>(report.width));
  appendField(line, "stored", static_cast<long>(report.stored));
  reply(line);
}

// CMD:TTF: what the loaded TrueType set and its glyph store hold. CMD:TTF:FULL: the store acts as if it had just filled
// up (TtfFontSet::markFull): the next safe point reclaims the oldest segment, so that can be watched at any size.
// CMD:TTF:SHAPE: see cmdTtfShape.
void cmdTtf(const std::string_view arg) {
  if (arg.substr(0, 6) == "SHAPE:") {
    cmdTtfShape(arg);
    return;
  }
  if (!arg.empty() && arg != "FULL") {
    reply("TTF_ERROR:expected FULL or SHAPE:<language>:<text>, got ", arg);
    return;
  }
  const TtfFontSet::StoreEvents events = TtfFontSet::storeEvents();
  std::string line = "TTF:";
  {
    RenderLock lock;  // the set is the render task's too; the reply goes out once it is released
    TtfFontSet* set = sdFontSystem.ttfSet();
    if (arg == "FULL") {
      line = set && set->markFull() ? "TTF_OK:full" : "TTF_ERROR:no TrueType family loaded";
    } else if (!set) {
      line += "none";
    } else {
      appendStats(line, *set);
    }
  }
  if (arg != "FULL") {
    appendField(line, "fills", static_cast<long>(events.fills));
    appendField(line, "wipes", static_cast<long>(events.wipes));
    appendField(line, "last_wipe_ms", static_cast<long>(events.lastWipeMs));
    appendField(line, "rooms", static_cast<long>(events.rooms));
    appendField(line, "rebuilds", static_cast<long>(events.rebuilds));
    appendField(line, "dropped", static_cast<long>(events.dropped));
    appendField(line, "last_room_ms", static_cast<long>(events.lastRoomMs));
    appendField(line, "last_install_ms", static_cast<long>(events.lastInstallMs));
  }
  reply(line);
}

// ---- Bulk output and long commands: PAGE, FS:PUT, GET, HASH -----------------------------------

// The bulk commands' piece: a chunk of FS:PUT, GET and HASH, or a line of CMD:PAGE. Half the HWCDC receive queue
// (4096, SerialBuffers.h), so a stray byte or two that comes with an upload's piece before its acknowledgement (a
// host's CRLF, a retry) still fits: the receive interrupt drops what the queue cannot hold, without a word, and a
// piece as large as the queue would leave only a CRC mismatch to show for it.
constexpr size_t kWireChunk = 2048;

// The scratch a bulk command works in, from the heap for the length of that one command (the four run on the loop
// task, never two at once) and given back on every way out of it. Not a static, which would lower every heap figure
// of a development build by its size whether a bulk command ever ran or not; not the stack, the loop task's is 8 KB.
class WireScratch {
 public:
  WireScratch() : block_(makeUniqueNoThrow<uint8_t[]>(kWireChunk)) {
    if (!block_) LOG_ERR("SER", "No %u-byte block for the command", static_cast<unsigned>(kWireChunk));
  }
  uint8_t* get() const { return block_.get(); }
  explicit operator bool() const { return static_cast<bool>(block_); }

 private:
  std::unique_ptr<uint8_t[]> block_;
};

// A reply line put together in a scratch block and written in one go, so that a line another task prints (the
// display driver writes its own) can only land between two lines, never inside one. A line longer than the block, a
// word of thousands of bytes, goes out in pieces.
class WireLine {
 public:
  explicit WireLine(uint8_t* buffer) : buffer_(buffer) {}
  void add(std::string_view text) {
    while (!text.empty()) {
      if (length_ == kWireChunk) flush();
      const size_t n = std::min(text.size(), kWireChunk - length_);
      memcpy(buffer_ + length_, text.data(), n);
      length_ += n;
      text.remove_prefix(n);
    }
  }
  // A few short fields.
  __attribute__((format(printf, 2, 3))) void addf(const char* format, ...) {
    char fields[96];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(fields, sizeof(fields), format, args);
    va_end(args);
    if (n > 0) add(std::string_view(fields, std::min(static_cast<size_t>(n), sizeof(fields) - 1)));
  }
  // `text` as CMD:PAGE writes a word (SerialCommandGrammar::escapeTextUnit), without the quotes.
  void addEscaped(const std::string_view text) {
    size_t pos = 0;
    while (pos < text.size()) {
      if (kWireChunk - length_ < SerialCommandGrammar::kMaxEscapedUnit) flush();
      length_ += SerialCommandGrammar::escapeTextUnit(text, pos, reinterpret_cast<char*>(buffer_) + length_);
    }
  }
  // Ends the line and writes it. False when the host stopped reading.
  bool end() {
    add("\n");
    flush();
    return ok_;
  }

 private:
  void flush() {
    if (ok_ && length_ > 0) ok_ = writeAll(buffer_, length_, kReplyStallMs);
    length_ = 0;
  }
  uint8_t* buffer_;
  size_t length_ = 0;
  bool ok_ = true;
};

// Holds LOG_* lines back from the wire while it lives, as the USB Transfer screen does (Logging's mute: they still
// reach the crash ring), so none cuts into a binary payload or a dump; then puts back what was, since the transfer
// screen keeps the mute on for its whole session.
class LogHold {
 public:
  LogHold() : wasMuted_(isSerialWireMuted()) { setSerialWireMuted(true); }
  ~LogHold() { setSerialWireMuted(wasMuted_); }
  LogHold(const LogHold&) = delete;
  LogHold& operator=(const LogHold&) = delete;

 private:
  bool wasMuted_;
};

// A file command holds the loop task for as long as the file takes. Every 100 ms it feeds the task watchdog and lets
// the other tasks run, the idle task the watchdog also watches among them, as SDCardManager's long reads do.
class LongWork {
 public:
  void breathe() {
    if (millis() - lastMs_ < 100) return;
    HalSystem::feedWatchdog();
    delay(1);
    lastMs_ = millis();
  }

 private:
  unsigned long lastMs_ = millis();
};

// A stream to the host waits as long for it as SCREENSHOT's frame does (SerialTransferDevice's 15 s).
constexpr unsigned long kStreamStallMs = 15000;

// A short protocol line (FS_PUT_READY, FS_PUT_ACK, FS_GET_START) in one write, as WireLine writes its lines, from a
// buffer of its own: the command's scratch may hold data.
__attribute__((format(printf, 1, 2))) void replyWhole(const char* format, ...) {
  char line[64];
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(line, sizeof(line) - 1, format, args);
  va_end(args);
  if (n <= 0) return;
  const size_t length = std::min(static_cast<size_t>(n), sizeof(line) - 2);
  line[length] = '\n';
  writeAll(reinterpret_cast<const uint8_t*>(line), length + 1, kReplyStallMs);
}

// ---- PAGE -----------------------------------------------------------------------------------

const char* alignName(const CssTextAlign align) {
  switch (align) {
    case CssTextAlign::Justify:
      return "justify";
    case CssTextAlign::Left:
      return "left";
    case CssTextAlign::Center:
      return "center";
    case CssTextAlign::Right:
      return "right";
    case CssTextAlign::None:
      return "none";
    case CssTextAlign::Inherit:
      return "inherit";
    case CssTextAlign::Start:
      return "start";
    case CssTextAlign::End:
      return "end";
  }
  return "unknown";
}

// One PAGE_W line: word `j` of a line, as stored and where it is drawn. The one place a word line is written: the
// right-to-left work adds the word's display form and logical position here (disp="..." lidx=<n>) once a TextBlock
// carries them.
bool sendPageWord(uint8_t* scratch, const uint16_t j, const TextBlock::WordBox& box, const unsigned style,
                  const bool glue, const std::string_view text) {
  WireLine line(scratch);
  line.addf("PAGE_W:%u x=%d w=%d style=%u glue=%d text=\"", static_cast<unsigned>(j), box.x, box.width, style,
            glue ? 1 : 0);
  line.addEscaped(text);
  line.add("\"");
  return line.end();
}

// PAGE_LINE and its words. False when the host stopped reading.
bool sendPageLine(uint8_t* scratch, const size_t index, const TextBlock* block, const int x, const int y,
                  const int fontId, LongWork& work) {
  const uint16_t words = block && block->valid() ? block->wordCount() : 0;
  WireLine line(scratch);
  line.addf("PAGE_LINE:%u x=%d y=%d words=%u align=%s vertical=%d", static_cast<unsigned>(index), x, y,
            static_cast<unsigned>(words), block ? alignName(block->getRenderStyle().alignment) : "left",
            block && block->isVertical() ? 1 : 0);
  if (!line.end()) return false;
  for (uint16_t j = 0; j < words; ++j) {
    // Measured as TextBlock::render draws it: the same font, scale and position. A glyph the font does not hold yet
    // is loaded for it, so a page whose glyphs all have to come from the card takes seconds.
    const TextBlock::WordBox box = block->wordBox(*testRenderer, j, fontId, x, y);
    work.breathe();
    if (!sendPageWord(scratch, j, box, static_cast<unsigned>(block->wordStyle(j)), block->wordContinues(j),
                      std::string_view(block->wordText(j), block->wordTextLen(j)))) {
      return false;
    }
  }
  return true;
}

void cmdPage(std::string_view /*arg*/) {
  if (!testRenderer) {
    reply("PAGE_ERROR:no renderer");
    return;
  }
  const char* error = nullptr;
  {
    // No render pass while the page is read and measured: the reader's section file and font caches are the render
    // task's, and a pass's console line (the display driver's own) must not land inside the dump.
    RenderLock lock{RenderLock::ExclusiveActivityAccess{}};
    const LogHold hold;
    error = activityManager.sendPageOnScreen();
  }
  if (error) reply("PAGE_ERROR:", error);
}

// ---- FS -------------------------------------------------------------------------------------

// The card path a file command names, as its replies show it: normalisePath drops the leading slash (the root becomes
// ""); the replies and the root check want it back.
std::string cardPath(const std::string_view spelled) { return "/" + FsHelpers::normalisePath(std::string(spelled)); }

// What FS:RM refuses, for every command that reads, writes, makes or moves an item: the root, the credential stores,
// a dot-named item itself, a path spelled with an 8.3 alias (ProtectedPaths). Hidden folders can be walked through.
bool refusedItem(const std::string& path) {
  const auto refuseAlias = [](std::string_view, std::string_view) { return true; };
  return path == "/" || ProtectedPaths::isProtectedPath(path, ProtectedPaths::Target::Item, true, refuseAlias);
}

// FS:RMF's one refusal besides the root: a segment spelled like a generated 8.3 alias, which may open another entry
// than the one it names (ProtectedPaths::looksLikeShortAlias). Not resolved on the card: a forced removal takes no
// chance.
bool spelledLikeAlias(const std::string& path) {
  size_t pos = 0;
  while (pos < path.size()) {
    size_t end = path.find('/', pos);
    if (end == std::string::npos) end = path.size();
    const std::string_view name = ProtectedPaths::effectiveName(std::string_view(path).substr(pos, end - pos));
    if (ProtectedPaths::looksLikeShortAlias(name)) return true;
    pos = end + 1;
  }
  return false;
}

bool isFolder(const std::string& path) {
  const HalFile item = Storage.open(path.c_str());
  return item && item.isDirectory();
}

// "/a/b" -> "/a", "/a" -> "/".
std::string parentOf(const std::string& path) { return path.substr(0, std::max<size_t>(path.rfind('/'), 1)); }

// The zlib CRC32 of `size` bytes of `file`, read from where it stands into `scratch` (kWireChunk bytes). False on a
// short read.
bool crcOfFile(HalFile& file, const uint32_t size, uint8_t* scratch, uint32_t& crc) {
  LongWork work;
  crc = 0;
  for (uint32_t done = 0; done < size;) {
    const size_t want = std::min<uint32_t>(size - done, kWireChunk);
    if (file.read(scratch, want) != static_cast<int>(want)) return false;
    crc = serialtransfer::crc32Update(crc, scratch, want);
    done += want;
    work.breathe();
  }
  return true;
}

// The CRLF of a host that ends its lines so, before an upload's data or a download's acknowledgements: the line
// reader stops at the CR, and the LF is neither.
void dropLineEnds() {
  while (logSerial.available() > 0 && (logSerial.peek() == '\r' || logSerial.peek() == '\n')) logSerial.read();
}

// The upload's idle limit: no byte for this long ends it.
constexpr unsigned long kPutIdleMs = 5000;

// Reads `len` bytes of an upload into `scratch`. Waits with delay(1) while none have arrived: a busy wait starves the
// C3's USB-CDC receive. False when none came for kPutIdleMs.
bool receiveChunk(uint8_t* scratch, const size_t len, LongWork& work) {
  size_t got = 0;
  unsigned long lastByteMs = millis();
  while (got < len) {
    const int ready = logSerial.available();
    if (ready > 0) {
      got += logSerial.read(scratch + got, std::min(len - got, static_cast<size_t>(ready)));
      lastByteMs = millis();
    } else if (millis() - lastByteMs >= kPutIdleMs) {
      return false;
    } else {
      delay(1);
    }
    work.breathe();
  }
  return true;
}

// Why an upload's write came up short: a full card, or the card failing. Asked only then, since counting the free
// clusters reads the whole FAT; the watchdog is fed first, as the upload's own waits do.
const char* writeFailure(const uint32_t stillToWrite) {
  HalSystem::feedWatchdog();
  return Storage.sdFreeBytes() < stillToWrite ? "FS_ERROR:no space " : "FS_ERROR:write failed ";
}

// CMD:FS:PUT:<path>:<size>:<crc32hex>. The path is checked before the host sends a byte; then the data comes in
// kWireChunk pieces, each written to <path>.part and acknowledged before the next is sent, and at <size> bytes the
// CRC decides: the .part takes the target's place (FileReplace, which never loses the old file), or goes.
void fsPut(const std::string_view arg) {
  std::string_view spelled;
  uint32_t size = 0;
  uint32_t wantCrc = 0;
  if (!SerialCommandGrammar::parseFsPut(arg, spelled, size, wantCrc)) {
    reply("FS_ERROR:expected PUT:/<path>:<size>:<crc32hex>");
    return;
  }
  const std::string path = cardPath(spelled);
  if (refusedItem(path)) {
    reply("FS_ERROR:protected ", path);
    return;
  }
  if (!isFolder(parentOf(path))) {
    reply("FS_ERROR:no parent folder ", path);
    return;
  }
  if (isFolder(path)) {
    reply("FS_ERROR:is a folder ", path);
    return;
  }
  // Taken before the .part is made, so a refusal leaves nothing behind.
  const WireScratch scratch;
  if (!scratch) {
    reply("FS_ERROR:no memory ", path);
    return;
  }
  const std::string part = path + ".part";
  HalFile file;
  if (!Storage.openFileForWrite("SER", part, file)) {
    reply("FS_ERROR:could not create ", path);
    return;
  }

  const HalPowerManager::Lock fullSpeed;
  LongWork work;
  const char* error = nullptr;
  {
    const LogHold hold;
    dropLineEnds();
    replyWhole("FS_PUT_READY:%u", static_cast<unsigned>(kWireChunk));
    uint32_t received = 0;
    uint32_t crc = 0;
    while (received < size) {
      const size_t want = std::min<uint32_t>(size - received, kWireChunk);
      if (!receiveChunk(scratch.get(), want, work)) {
        error = "FS_ERROR:timeout ";
        break;
      }
      crc = serialtransfer::crc32Update(crc, scratch.get(), want);
      if (file.write(scratch.get(), want) != want) {
        error = writeFailure(size - received);
        break;
      }
      received += want;
      replyWhole("FS_PUT_ACK:%lu", static_cast<unsigned long>(received));
    }
    if (!error && crc != wantCrc) error = "FS_ERROR:crc mismatch ";
  }
  // Closed (its last sector and the directory entry written) before the .part is renamed or removed.
  file.flush();
  if (!file.close() && !error) error = "FS_ERROR:write failed ";
  if (!error && !FileReplace::replace(part.c_str(), path.c_str())) error = "FS_ERROR:could not replace ";
  if (error) {
    Storage.remove(part.c_str());
    reply(error, path);
    return;
  }
  std::string line = "FS_OK:put " + path;
  line += ' ';
  line += std::to_string(size);
  reply(line);
}

// The host's go-ahead for FS:GET's next piece: the byte the transfer protocol acknowledges a piece with. Any other
// byte cancels the download.
constexpr uint8_t kGetAck = 0x06;

enum class HostAnswer : uint8_t { Ack, Cancel, None };

// Waits for the host to take a piece: its ACK, another byte (a cancel), or nothing for kStreamStallMs. Waits with
// delay(1), as an upload does, so the USB-CDC receive gets to run.
HostAnswer awaitHostAck(LongWork& work) {
  const unsigned long start = millis();
  while (millis() - start < kStreamStallMs) {
    if (logSerial.available() > 0) return logSerial.read() == kGetAck ? HostAnswer::Ack : HostAnswer::Cancel;
    delay(1);
    work.breathe();
  }
  return HostAnswer::None;
}

// CMD:FS:GET:<path>: FS_GET_START:<size>:<crc32hex>:<piece>, then the file in pieces of <piece> bytes (the last one
// shorter), each sent only once the host has acknowledged the one before with kGetAck, then FS_GET_END. The CRC
// takes a read of its own first, so the host can check what it got.
//
// Paced because an unpaced stream loses data the host never sees go: on the X4, a host that stopped reading for half a
// second lost whole 64-byte USB packets of the stream, every time, and a normal one 2 GETs in 14. HWCDC had accepted
// every byte (the writes returned their full count and FS_GET_END followed), so nothing on this side could tell; it
// happened on the USB Transfer screen too, with its 1 s TX timeout. The transfer protocol's download paces itself the
// same way, for the same reason (SerialTransferProtocol::handleDownload). With one piece in flight, the TX ring never
// fills and the host is never behind by more than a piece.
void fsGet(const std::string_view arg) {
  std::string_view spelled;
  if (!SerialCommandGrammar::parseFsPath(arg, spelled)) {
    reply("FS_ERROR:expected GET:/<path>");
    return;
  }
  const std::string path = cardPath(spelled);
  if (refusedItem(path)) {
    reply("FS_ERROR:protected ", path);
    return;
  }
  HalFile file = Storage.open(path.c_str());
  if (!file) {
    reply("FS_ERROR:not found ", path);
    return;
  }
  if (file.isDirectory()) {
    reply("FS_ERROR:not a file ", path);
    return;
  }
  const WireScratch scratch;
  if (!scratch) {
    reply("FS_ERROR:no memory ", path);
    return;
  }
  const HalPowerManager::Lock fullSpeed;
  const auto size = static_cast<uint32_t>(file.fileSize());
  uint32_t crc = 0;
  if (!crcOfFile(file, size, scratch.get(), crc) || !file.seek(0)) {
    reply("FS_ERROR:read failed ", path);
    return;
  }
  bool readOk = true;
  {
    // No render pass while the bytes go out: the display driver prints a line of its own after a refresh, and in
    // the payload that would be data.
    RenderLock lock{RenderLock::ExclusiveActivityAccess{}};
    const LogHold hold;
    dropLineEnds();
    replyWhole("FS_GET_START:%lu:%08lx:%u", static_cast<unsigned long>(size), static_cast<unsigned long>(crc),
               static_cast<unsigned>(kWireChunk));
    LongWork work;
    for (uint32_t sent = 0; sent < size;) {
      const size_t want = std::min<uint32_t>(size - sent, kWireChunk);
      // A read that fails still sends its share, zeros, so the host's count of bytes holds; the line after says so.
      if (readOk && file.read(scratch.get(), want) != static_cast<int>(want)) readOk = false;
      if (!readOk) memset(scratch.get(), 0, want);
      // Up to 15 s for a slow host: longer than the task watchdog allows unfed.
      if (!writeAll(scratch.get(), want, kStreamStallMs, &HalSystem::feedWatchdog)) {
        reply("FS_ERROR:transfer stalled ", path);
        return;
      }
      sent += want;
      switch (awaitHostAck(work)) {
        case HostAnswer::Ack:
          break;
        case HostAnswer::Cancel:
          reply("FS_ERROR:cancelled ", path);
          return;
        case HostAnswer::None:
          reply("FS_ERROR:transfer stalled ", path);
          return;
      }
    }
  }
  if (readOk) {
    reply("FS_GET_END");
  } else {
    reply("FS_ERROR:read failed ", path);
  }
}

// CMD:FS:HASH:<path>: FS_HASH:<crc32hex> <size> <path>, to check a file against a copy on the host without reading
// it back.
void fsHash(const std::string_view arg) {
  std::string_view spelled;
  if (!SerialCommandGrammar::parseFsPath(arg, spelled)) {
    reply("FS_ERROR:expected HASH:/<path>");
    return;
  }
  const std::string path = cardPath(spelled);
  if (refusedItem(path)) {
    reply("FS_ERROR:protected ", path);
    return;
  }
  HalFile file = Storage.open(path.c_str());
  if (!file) {
    reply("FS_ERROR:not found ", path);
    return;
  }
  if (file.isDirectory()) {
    reply("FS_ERROR:not a file ", path);
    return;
  }
  const WireScratch scratch;
  if (!scratch) {
    reply("FS_ERROR:no memory ", path);
    return;
  }
  const HalPowerManager::Lock fullSpeed;
  const auto size = static_cast<uint32_t>(file.fileSize());
  uint32_t crc = 0;
  if (!crcOfFile(file, size, scratch.get(), crc)) {
    reply("FS_ERROR:read failed ", path);
    return;
  }
  char head[40];
  snprintf(head, sizeof(head), "FS_HASH:%08lx %lu ", static_cast<unsigned long>(crc), static_cast<unsigned long>(size));
  reply(head, path);
}

// CMD:FS:MKDIR:<folder>: makes it and the folders above it; FS_OK as well when it is there already.
void fsMkdir(const std::string_view arg) {
  std::string_view spelled;
  if (!SerialCommandGrammar::parseFsPath(arg, spelled)) {
    reply("FS_ERROR:expected MKDIR:/<folder>");
    return;
  }
  const std::string path = cardPath(spelled);
  if (path != "/" && refusedItem(path)) {
    reply("FS_ERROR:protected ", path);
    return;
  }
  {
    const HalFile item = Storage.open(path.c_str());
    if (item && !item.isDirectory()) {
      reply("FS_ERROR:not a folder ", path);
      return;
    }
    if (item) {
      reply("FS_OK:mkdir ", path);
      return;
    }
  }
  if (!Storage.mkdir(path.c_str(), true)) {
    reply("FS_ERROR:could not create ", path);
    return;
  }
  reply("FS_OK:mkdir ", path);
}

// CMD:FS:MV:<from>:<to>: renames or moves a file or a folder; never onto something that is there.
void fsMove(const std::string_view arg) {
  std::string_view fromSpelled;
  std::string_view toSpelled;
  if (!SerialCommandGrammar::parseFsMove(arg, fromSpelled, toSpelled)) {
    reply("FS_ERROR:expected MV:/<from>:/<to>");
    return;
  }
  const std::string from = cardPath(fromSpelled);
  const std::string to = cardPath(toSpelled);
  if (refusedItem(from)) {
    reply("FS_ERROR:protected ", from);
    return;
  }
  if (refusedItem(to)) {
    reply("FS_ERROR:protected ", to);
    return;
  }
  bool folder = false;
  {
    const HalFile item = Storage.open(from.c_str());
    if (!item) {
      reply("FS_ERROR:not found ", from);
      return;
    }
    folder = item.isDirectory();
  }
  // A folder moved into itself would hang off its own subtree, out of reach; onto itself it is there already, which
  // says more than "exists". Checked first for that. FAT compares names without case.
  if (folder && (ProtectedPaths::equalsIgnoreCase(to, from) || ProtectedPaths::startsWithIgnoreCase(to, from + "/"))) {
    reply("FS_ERROR:into itself ", to);
    return;
  }
  if (Storage.exists(to.c_str())) {
    reply("FS_ERROR:exists ", to);
    return;
  }
  if (!isFolder(parentOf(to))) {
    reply("FS_ERROR:no parent folder ", to);
    return;
  }
  if (!Storage.rename(from.c_str(), to.c_str())) {
    reply("FS_ERROR:could not move ", from);
    return;
  }
  std::string line = "FS_OK:moved " + from;
  line += ' ';
  line += to;
  reply(line);
}

// CMD:FS:LS:<folder> lists a folder, one "FS:<d|f> <bytes> <name>" line an entry and FS_END:<count>; CMD:FS:RM:<path>
// removes a file, or a folder with everything in it. Hidden folders can be reached (the caches under /.crosspoint);
// the credential stores, a dot-named item itself and any path spelled by an 8.3 alias cannot. PUT, GET, HASH, MKDIR
// and MV are above.
void cmdFs(const std::string_view arg) {
  std::string_view verb;
  std::string_view rest;
  splitHead(arg, verb, rest);
  struct Verb {
    const char* name;
    void (*run)(std::string_view rest);
  };
  static constexpr Verb kVerbs[] = {
      {"PUT", &fsPut}, {"GET", &fsGet}, {"HASH", &fsHash}, {"MKDIR", &fsMkdir}, {"MV", &fsMove}};
  for (const Verb& entry : kVerbs) {
    if (verb == entry.name) {
      entry.run(rest);
      return;
    }
  }
  // RMF: RM without the protection, for a test's own leftovers (a dot folder it made, a credential store it planted).
  const bool forced = verb == "RMF";
  if ((verb != "LS" && verb != "RM" && !forced) || rest.empty() || rest.front() != '/') {
    reply("FS_ERROR:expected LS, RM, RMF, PUT, GET, HASH, MKDIR or MV, then :/<path>");
    return;
  }
  // normalisePath drops the leading slash (the root becomes ""); the replies and the root check want it back.
  const std::string path = "/" + FsHelpers::normalisePath(std::string(rest));
  const auto refuseAlias = [](std::string_view, std::string_view) { return true; };
  const ProtectedPaths::Target target = verb == "LS" ? ProtectedPaths::Target::Directory : ProtectedPaths::Target::Item;
  const bool refused = path == "/" ? verb != "LS"
                       : forced    ? spelledLikeAlias(path)
                                   : ProtectedPaths::isProtectedPath(path, target, true, refuseAlias);
  if (refused) {
    reply("FS_ERROR:protected ", path);
    return;
  }
  HalFile item = Storage.open(path.c_str());
  if (!item) {
    reply("FS_ERROR:not found ", path);
    return;
  }
  if (verb == "RM" || forced) {
    const bool folder = item.isDirectory();
    item = HalFile();  // closed before it goes
    if (!(folder ? Storage.removeDir(path.c_str()) : Storage.remove(path.c_str()))) {
      reply("FS_ERROR:could not remove ", path);
      return;
    }
    reply("FS_OK:removed ", path);
    return;
  }
  if (!item.isDirectory()) {
    reply("FS_ERROR:not a folder ", path);
    return;
  }
  long count = 0;
  char name[128];
  for (HalFile entry = item.openNextFile(); entry; entry = item.openNextFile()) {
    entry.getName(name, sizeof(name));
    const bool folder = entry.isDirectory();
    char head[32];
    snprintf(head, sizeof(head), "FS:%c %lu ", folder ? 'd' : 'f',
             folder ? 0UL : static_cast<unsigned long>(entry.fileSize()));
    reply(head, name);
    ++count;
  }
  replyf("FS_END:%ld", count);
}

// ---- Power ----------------------------------------------------------------------------------

// CMD:LAB, development builds only: frames for the gray lab's X4 tests (tools/murphy-gray-lab/x4lab.py).
//   LAB:LUT:<hex>   the external LUT that LAB:DRAW:LUT sends, in the panel driver's own layout
//   LAB:DRAW:<FULL|LUT>:<bg>:<x>,<y>,<w>,<h>,<c>;...   both planes from rectangles in panel RAM
//                   coordinates, each code c (and the background bg) being RED * 2 + BW. FULL draws the BW
//                   bit and runs a normal full refresh; LUT sends both planes with the LUT. LAB_OK:<ms>.
namespace lab {
uint8_t lut[128];
size_t lutLength = 0;

int hexDigit(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// The next number up to `separator` (or the end), consumed from `text`.
bool takeNumber(std::string_view& text, const char separator, long& out) {
  const size_t end = std::min(text.find(separator), text.size());
  if (end == 0) return false;
  long value = 0;
  for (size_t i = 0; i < end; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + (text[i] - '0');
  }
  out = value;
  text.remove_prefix(std::min(end + 1, text.size()));
  return true;
}

void fill(uint8_t* bw, uint8_t* red, const uint16_t width, const uint16_t height, const uint16_t widthBytes,
          const long x0, const long y0, const long w, const long h, const long code) {
  const long x1 = std::min<long>(x0 + w, width), y1 = std::min<long>(y0 + h, height);
  for (long y = std::max<long>(y0, 0); y < y1; ++y) {
    uint8_t* bwRow = bw + y * widthBytes;
    uint8_t* redRow = red ? red + y * widthBytes : nullptr;
    for (long x = std::max<long>(x0, 0); x < x1; ++x) {
      const uint8_t mask = 0x80 >> (x & 7);
      bwRow[x >> 3] = (code & 1) ? (bwRow[x >> 3] | mask) : (bwRow[x >> 3] & ~mask);
      if (redRow) redRow[x >> 3] = (code & 2) ? (redRow[x >> 3] | mask) : (redRow[x >> 3] & ~mask);
    }
  }
}
}  // namespace lab

void cmdLab(std::string_view arg) {
  if (arg.substr(0, 4) == "LUT:") {
    const std::string_view hex = arg.substr(4);
    if (hex.empty() || hex.size() % 2 != 0 || hex.size() / 2 > sizeof(lab::lut)) return reply("LAB_ERROR:bad lut");
    for (size_t i = 0; i < hex.size() / 2; ++i) {
      const int high = lab::hexDigit(hex[2 * i]), low = lab::hexDigit(hex[2 * i + 1]);
      if (high < 0 || low < 0) return reply("LAB_ERROR:bad lut");
      lab::lut[i] = static_cast<uint8_t>(high << 4 | low);
    }
    lab::lutLength = hex.size() / 2;
    return replyf("LAB_OK:lut %u", static_cast<unsigned>(lab::lutLength));
  }
  if (arg.substr(0, 5) != "DRAW:") return reply("LAB_ERROR:LUT or DRAW");
  arg.remove_prefix(5);
  const bool full = arg.substr(0, 5) == "FULL:";
  if (!full && arg.substr(0, 4) != "LUT:") return reply("LAB_ERROR:FULL or LUT");
  arg.remove_prefix(full ? 5 : 4);
  if (!full && lab::lutLength == 0) return reply("LAB_ERROR:no lut");
  long background = 0;
  if (!lab::takeNumber(arg, ':', background) || background > 3) return reply("LAB_ERROR:bad background");

  const uint16_t width = display.getDisplayWidth(), height = display.getDisplayHeight();
  const uint16_t widthBytes = display.getDisplayWidthBytes();
  RenderLock lock;  // the frame buffer is the render task's
  uint8_t* bw = renderer.getFrameBuffer();
  uint8_t* red = nullptr;
  if (!bw) return reply("LAB_ERROR:no frame buffer");
  if (!full) {
    // The display's second frame buffer: a third 48 KB block is more than a C3 has free.
    size_t lent = 0;
    red = renderer.borrowSecondaryBuffer(&lent);
    if (!red || lent < display.getBufferSize()) {
      if (red) renderer.returnSecondaryBuffer();
      return reply("LAB_ERROR:no second buffer");
    }
  }
  lab::fill(bw, red, width, height, widthBytes, 0, 0, width, height, background);
  while (!arg.empty()) {
    long x = 0, y = 0, w = 0, h = 0, code = 0;
    if (!lab::takeNumber(arg, ',', x) || !lab::takeNumber(arg, ',', y) || !lab::takeNumber(arg, ',', w) ||
        !lab::takeNumber(arg, ',', h) || !lab::takeNumber(arg, ';', code) || code > 3) {
      if (red) renderer.returnSecondaryBuffer();
      return reply("LAB_ERROR:bad rectangle");
    }
    lab::fill(bw, red, width, height, widthBytes, x, y, w, h, code);
  }
  const unsigned long start = millis();
  bool shown = true;
  if (full) {
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  } else {
    shown = display.displayLabFrame(bw, red, lab::lut);
  }
  if (red) renderer.returnSecondaryBuffer();
  if (!shown) return reply("LAB_ERROR:no lab path on this panel");
  replyf("LAB_OK:%lu", millis() - start);
}

void cmdSleep(std::string_view /*arg*/) {
  reply("SLEEP_OK");
  powerRequest = PowerRequest::Sleep;
}

void cmdReboot(std::string_view /*arg*/) {
  reply("REBOOT_OK");
  powerRequest = PowerRequest::Reboot;
}
#endif  // SERIAL_TEST_API

void cmdScreenshot(std::string_view /*arg*/) { ScreenshotUtil::request(ScreenshotUtil::Target::Host); }

// On-demand replay of this session's boot summary, for when the monitor was attached too late to
// catch it and the automatic repeats are used up. The summary is log lines; the reply after them
// tells a script it is complete.
void cmdBootLog(std::string_view /*arg*/) {
  BootDiag::logSummary();
  reply("BOOTLOG_OK");
}

void cmdHelp(std::string_view arg);

using Handler = void (*)(std::string_view arg);
struct Command {
  const char* name;
  Handler run;
};
constexpr Command kCommands[] = {
    {"BTN", &cmdButton},      {"ACT", &cmdActivity},  {"STATE", &cmdState}, {"SCREENSHOT", &cmdScreenshot},
    {"BOOTLOG", &cmdBootLog}, {"HELP", &cmdHelp},
#if SERIAL_TEST_API
    {"IDLE", &cmdIdle},       {"EVENTS", &cmdEvents}, {"GET", &cmdGet},     {"SET", &cmdSet},
    {"LOG", &cmdLog},         {"TOUCH", &cmdTouch},   {"SWIPE", &cmdSwipe}, {"SLEEP", &cmdSleep},
    {"REBOOT", &cmdReboot},   {"HEAP", &cmdHeap},     {"TTF", &cmdTtf},     {"FS", &cmdFs},
    {"PAGE", &cmdPage},       {"LAB", &cmdLab},
#endif
};

// The commands this build has: with SERIAL_TEST_API or without.
void cmdHelp(std::string_view /*arg*/) {
  writeText("HELP:", kReplyStallMs);
  for (const Command& command : kCommands) {
    writeText(" ", kReplyStallMs);
    writeText(command.name, kReplyStallMs);
  }
  writeText("\n", kReplyStallMs);
}

#if SERIAL_TEST_API
// ---- service() helpers ----------------------------------------------------------------------

void serviceEvents() {
  const uint32_t activity = activityManager.activityChangeCount();
  const TextProbe popup = readProbe(popupProbe);
  BuildDone builds[kBuildRing];
  int buildCount = 0;
  FontInstalled fonts[kFontRing];
  int fontCount = 0;
  portENTER_CRITICAL(&probeMux);
  while (buildHead != buildTail) {
    builds[buildCount++] = buildRing[buildHead];
    buildHead = static_cast<uint8_t>((buildHead + 1) % kBuildRing);
  }
  while (fontHead != fontTail) {
    fonts[fontCount++] = fontRing[fontHead];
    fontHead = static_cast<uint8_t>((fontHead + 1) % kFontRing);
  }
  portEXIT_CRITICAL(&probeMux);
  const TtfFontSet::StoreEvents store = TtfFontSet::storeEvents();
  // Nothing is written, and nothing piles up, while no host asked for events.
  if (!eventsOn) {
    reportedActivity = activity;
    reportedPopup = popup.sequence;
    reportedFills = store.fills;
    reportedWipes = store.wipes;
    reportedRooms = store.rooms;
    return;
  }
  if (activity != reportedActivity) {
    reportedActivity = activity;
    reply("EVT:ACT ", activityManager.currentActivityName(), kEventStallMs);
  }
  if (popup.sequence != reportedPopup) {
    reportedPopup = popup.sequence;
    reply("EVT:POPUP ", popup.text, kEventStallMs);
  }
  for (int i = 0; i < buildCount; ++i) {
    char line[48];
    snprintf(line, sizeof(line), "EVT:BUILD_DONE spine=%d pages=%u", builds[i].spine + 1,
             static_cast<unsigned>(builds[i].pages));
    reply(line, {}, kEventStallMs);
  }
  if (store.fills != reportedFills) {
    reportedFills = store.fills;
    char line[40];
    snprintf(line, sizeof(line), "EVT:TTF_FULL fills=%lu", static_cast<unsigned long>(store.fills));
    reply(line, {}, kEventStallMs);
  }
  if (store.wipes != reportedWipes) {
    reportedWipes = store.wipes;
    char line[56];
    snprintf(line, sizeof(line), "EVT:TTF_WIPE ms=%lu wipes=%lu", static_cast<unsigned long>(store.lastWipeMs),
             static_cast<unsigned long>(store.wipes));
    reply(line, {}, kEventStallMs);
  }
  if (store.rooms != reportedRooms) {
    reportedRooms = store.rooms;
    char line[72];
    snprintf(line, sizeof(line), "EVT:TTF_ROOM ms=%lu rooms=%lu dropped=%lu",
             static_cast<unsigned long>(store.lastRoomMs), static_cast<unsigned long>(store.rooms),
             static_cast<unsigned long>(store.dropped));
    reply(line, {}, kEventStallMs);
  }
  for (int i = 0; i < fontCount; ++i) {
    std::string line = "EVT:FONT_INSTALLED";
    appendField(line, "family", fonts[i].family);
    appendField(line, "ok", fonts[i].ok ? 1L : 0L);
    reply(line, {}, kEventStallMs);
  }
}

void serviceIdle() {
  // Nothing to follow most of the time: then the device is not sampled at all, which keeps the
  // reader's lock attempt in hasPendingBackgroundWork() off the ordinary loop pass.
  if (!idleGate.active()) return;
  IdleGate::Sample sample;
  sample.nowMs = millis();
  sample.busy = activityManager.busyReason(/*includeBackground=*/idleGate.waiting() && !idleScreenOnly);
  sample.effectHandled = effectHandled();
  sample.renderPasses = activityManager.renderPassCount();
  switch (idleGate.step(sample)) {
    case IdleGate::Verdict::Waiting:
      break;
    case IdleGate::Verdict::Idle:
      replyf("IDLE:%lu", static_cast<unsigned long>(idleGate.elapsedMs(sample.nowMs)));
      break;
    case IdleGate::Verdict::TimedOut:
      reply("IDLE_TIMEOUT:", idleGate.reason());
      break;
  }
}
#endif  // SERIAL_TEST_API

}  // namespace

void handleLine(const std::string_view line) {
  constexpr std::string_view kPrefix = "CMD:";
  if (line.substr(0, kPrefix.size()) != kPrefix) return;
  const std::string_view command = trim(line.substr(kPrefix.size()));
  std::string_view name;
  std::string_view arg;
  splitHead(command, name, arg);
  for (const Command& entry : kCommands) {
    if (name == entry.name) {
      entry.run(arg);
      return;
    }
  }
  reply("CMD_ERROR:unknown command ", command);
}

void sendScreenshot(const uint8_t* frame, const uint32_t size, const int width, const int height, const bool busy) {
  if (busy) {
    reply("SCREENSHOT_ERROR:busy");
    return;
  }
  if (!frame) {
    // Framebuffers are released during the web server session — nothing to send.
    reply("SCREENSHOT_ERROR:framebuffer released");
    return;
  }
  replyf("SCREENSHOT_START:%d:%d:%lu", width, height, static_cast<unsigned long>(size));
  // ~48 KB in one go is exactly the sustained write that trips the short-write behaviour, so the
  // END marker must not be sent if the payload was cut short: a truncated frame with a valid
  // trailer looks complete to the host. 15 s without progress matches SerialTransferDevice.
  if (writeAll(frame, size, 15000)) {
    reply("SCREENSHOT_END");
  } else {
    reply("SCREENSHOT_ERROR:transfer stalled");
  }
}

#if SERIAL_TEST_API
void begin(GfxRenderer& renderer) { testRenderer = &renderer; }

void service() {
  serviceEvents();
  serviceIdle();
}

void notePopup(const char* text) { noteText(popupProbe, -1, text); }

void noteSelection(const int index, const char* label) { noteText(selectionProbe, index, label); }

void noteBuildDone(const int spineIndex, const int pageCount) {
  portENTER_CRITICAL(&probeMux);
  const uint8_t next = static_cast<uint8_t>((buildTail + 1) % kBuildRing);
  if (next != buildHead) {
    buildRing[buildTail] = BuildDone{static_cast<int16_t>(spineIndex), static_cast<uint16_t>(pageCount)};
    buildTail = next;
  }
  portEXIT_CRITICAL(&probeMux);
}

void noteFontInstalled(const char* family, const bool ok) {
  portENTER_CRITICAL(&probeMux);
  const uint8_t next = static_cast<uint8_t>((fontTail + 1) % kFontRing);
  if (next != fontHead) {
    FontInstalled& entry = fontRing[fontTail];
    snprintf(entry.family, sizeof(entry.family), "%s", family ? family : "");
    entry.ok = ok;
    fontTail = next;
  }
  portEXIT_CRITICAL(&probeMux);
}

bool takeDecodeRefusal() { return takeRefusal(decodeRefusals); }

bool takeReloadRefusal() { return takeRefusal(reloadRefusals); }

bool isTestBook(const std::string_view path) {
  // In any case: FAT folds it, and the file browser opens the folder a test names /test/ as /TEST/.
  constexpr std::string_view kFolder = "/test/";
  return path.size() >= kFolder.size() &&
         std::equal(kFolder.begin(), kFolder.end(), path.begin(), [](const char want, const char got) {
           return want == std::tolower(static_cast<unsigned char>(got));
         });
}

PowerRequest takePowerRequest() {
  const PowerRequest request = powerRequest;
  powerRequest = PowerRequest::None;
  return request;
}

bool sendPage(const PageOnScreen& onScreen) {
  const WireScratch scratch;
  if (!scratch) return false;
  const Page& page = *onScreen.page;
  const size_t count = page.elements.size();
  // A glyph the heap had no room for is measured as missing: the dump's widths are then not the panel's.
  const uint32_t starvedBefore = glyph_miss::starvedCount();
  WireLine start(scratch.get());
  start.addf("PAGE_START:spine=%d/%d page=%d/%d flow=%s elements=%u", onScreen.spine, onScreen.spineCount,
             onScreen.pageNumber, onScreen.pageCount, onScreen.verticalFlow ? "vertical-rl" : "horizontal",
             static_cast<unsigned>(count));
  // A host that stopped reading is no error to report: nobody would read it.
  if (!start.end()) return true;
  // One element at a time, straight from the page: nothing here grows with it.
  LongWork work;
  for (size_t i = 0; i < count; ++i) {
    const PageElement& element = *page.elements[i];
    const int x = element.xPos + onScreen.originX;
    const int y = element.yPos + onScreen.originY;
    WireLine line(scratch.get());
    switch (element.getTag()) {
      case TAG_PageLine:
        if (!sendPageLine(scratch.get(), i, static_cast<const PageLine&>(element).getBlock(), x, y, onScreen.fontId,
                          work)) {
          return true;
        }
        continue;
      case TAG_PageImage: {
        const ImageBlock& image = static_cast<const PageImage&>(element).getImageBlock();
        line.addf("PAGE_IMAGE:%u x=%d y=%d w=%d h=%d", static_cast<unsigned>(i), x, y, image.getWidth(),
                  image.getRenderedHeight());
        break;
      }
      case TAG_PageTable:
        line.addf("PAGE_EL:%u table", static_cast<unsigned>(i));
        break;
      case TAG_PageHR:
        line.addf("PAGE_EL:%u hr", static_cast<unsigned>(i));
        break;
      default:
        line.addf("PAGE_EL:%u tag%u", static_cast<unsigned>(i), static_cast<unsigned>(element.getTag()));
        break;
    }
    if (!line.end()) return true;
  }
  WireLine end(scratch.get());
  end.addf("PAGE_END:%u starved=%lu", static_cast<unsigned>(count),
           static_cast<unsigned long>(glyph_miss::starvedCount() - starvedBefore));
  end.end();
  return true;
}
#endif  // SERIAL_TEST_API

}  // namespace SerialTestApi
