#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <variant>

#include "GlobalBookmarkIndex.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ClearCacheActivity::askToKeepReadingState(ConfirmationActivity::Question& question) {
  question.headline = tr(STR_KEEP_PROGRESS_QUESTION);
  // Keep is where every other question has its yes, so going ahead as usual loses nothing of the
  // reader's own. Reset is the deliberate answer, between it and Cancel.
  question.alternateLabel = StrId::STR_RESET_BUTTON;
  question.acceptLabel = StrId::STR_KEEP_BUTTON;
}

BookCache::Keep ClearCacheActivity::keepFor(const ActivityResult& answer) {
  const auto* confirm = std::get_if<ConfirmResult>(&answer.data);
  return confirm && confirm->alternate ? BookCache::Keep::Nothing : BookCache::Keep::ReadingState;
}

void ClearCacheActivity::onEnter() {
  Activity::onEnter();

  state = WARNING;
  ConfirmationActivity::Question question;
  // Warning 1 is a sentence, not a caption, so it may wrap.
  question.title = tr(STR_CLEAR_CACHE_WARNING_1);
  question.titleMaxLines = 3;
  askToKeepReadingState(question);
  // Warnings 3 and 4 are one sentence split across two keys.
  question.message = std::string(tr(STR_CLEAR_CACHE_WARNING_3)) + " " + tr(STR_CLEAR_CACHE_WARNING_4);
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, std::move(question)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             LOG_DBG("CLEAR_CACHE", "User cancelled");
                             goBack();
                             return;
                           }
                           startClearing(keepFor(result));
                         });
}

void ClearCacheActivity::startClearing(const BookCache::Keep keep) {
  LOG_DBG("CLEAR_CACHE", "User confirmed, starting cache clear (%s)",
          keep == BookCache::Keep::ReadingState ? "keeping reading state" : "resetting reading state");
  {
    RenderLock lock(*this);
    state = CLEARING;
  }
  requestUpdateAndWait();
  clearCache(keep);
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

void ClearCacheActivity::clearCache(const BookCache::Keep keep) {
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

    const bool bookCache = itemName.startsWith("epub_") || itemName.startsWith("xtc_") || itemName.startsWith("txt_");
    if (file.isDirectory() && (bookCache || itemName == "fontprev")) {
      String fullPath = "/.crosspoint/" + itemName;
      LOG_DBG("CLEAR_CACHE", "Removing cache: %s", fullPath.c_str());

      file.close();  // Close before attempting to delete

      // The font previews are no book's: they go whatever the answer.
      if (bookCache ? BookCache::clear(fullPath.c_str(), keep) : Storage.removeDir(fullPath.c_str())) {
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

  // The bookmarks went with the folders: the Bookmarks list forgets them now, not at its next open.
  if (keep == BookCache::Keep::Nothing) GLOBAL_BOOKMARKS.reconcile();

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
