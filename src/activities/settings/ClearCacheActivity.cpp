#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ClearCacheActivity::onEnter() {
  Activity::onEnter();

  state = WARNING;
  ConfirmationActivity::Question question;
  // Warning 1 is a sentence, not a caption, so it may wrap.
  question.title = tr(STR_CLEAR_CACHE_WARNING_1);
  question.titleMaxLines = 3;
  question.headline = tr(STR_CLEAR_CACHE_WARNING_2);
  // Warnings 3 and 4 are one sentence split across two keys.
  question.message = std::string(tr(STR_CLEAR_CACHE_WARNING_3)) + " " + tr(STR_CLEAR_CACHE_WARNING_4);
  question.acceptLabel = StrId::STR_CLEAR_BUTTON;
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, std::move(question)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             LOG_DBG("CLEAR_CACHE", "User cancelled");
                             goBack();
                             return;
                           }
                           startClearing();
                         });
}

void ClearCacheActivity::startClearing() {
  LOG_DBG("CLEAR_CACHE", "User confirmed, starting cache clear");
  {
    RenderLock lock(*this);
    state = CLEARING;
  }
  requestUpdateAndWait();
  clearCache();
}

void ClearCacheActivity::render(RenderLock&&) {
  // Nothing of our own while asking: the question is the ConfirmationActivity above.
  if (state == WARNING) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_CLEAR_READING_CACHE));

  const int midY = contentRect.y + contentRect.height / 2;
  if (state == CLEARING) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_CLEARING_CACHE));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 20, tr(STR_CACHE_CLEARED), true, EpdFontFamily::BOLD);
    std::string resultText = std::to_string(clearedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 20, tr(STR_CLEAR_CACHE_FAILED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearCacheActivity::clearCache() {
  LOG_DBG("CLEAR_CACHE", "Clearing cache...");

  // Open .crosspoint directory
  auto root = Storage.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    LOG_DBG("CLEAR_CACHE", "Failed to open cache directory");
    if (root) root.close();
    state = FAILED;
    requestUpdate();
    return;
  }

  clearedCount = 0;
  failedCount = 0;
  char name[128];

  // Iterate through all entries in the directory
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    String itemName(name);

    if (file.isDirectory() && (itemName.startsWith("epub_") || itemName.startsWith("xtc_") ||
                               itemName.startsWith("txt_") || itemName == "fontprev")) {
      String fullPath = "/.crosspoint/" + itemName;
      LOG_DBG("CLEAR_CACHE", "Removing cache: %s", fullPath.c_str());

      file.close();  // Close before attempting to delete

      if (Storage.removeDir(fullPath.c_str())) {
        clearedCount++;
      } else {
        LOG_ERR("CLEAR_CACHE", "Failed to remove: %s", fullPath.c_str());
        failedCount++;
      }
    } else {
      file.close();
    }
  }
  root.close();

  LOG_DBG("CLEAR_CACHE", "Cache cleared: %d removed, %d failed", clearedCount, failedCount);

  state = SUCCESS;
  requestUpdate();
}

void ClearCacheActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
