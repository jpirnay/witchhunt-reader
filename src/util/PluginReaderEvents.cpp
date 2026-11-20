#include "PluginReaderEvents.h"

#include <Arduino.h>
#include <BssMutex.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>

#include "PluginEvents.h"
#include "activities/reader/ReaderSession.h"

namespace plugin_events::reader {

namespace {

// shown() runs on the render task for TXT, Markdown and XTC, the rest on the loop task: one lock for the state.
BssMutex stateMutex;
std::string book;  // the open book, "" between books
ReaderSession session;
uint32_t lastPosition = 0;
bool hasPosition = false;
int percent = 0;
int endPercent = 0;
std::atomic<bool> pendingTurn{false};

bool readerEventsWanted() {
  return anySubscriber(Event::ReaderOpen) || anySubscriber(Event::ReaderExit) || anySubscriber(Event::ReaderSession) ||
         anySubscriber(Event::SleepEnter);
}

// reader.session from the summary, then a fresh one. Called with the lock held.
void flushLocked() {
  if (!session.isEmitWorthy() || !anySubscriber(Event::ReaderSession) || book.empty()) {
    session.reset();
    return;
  }
  // KOSync's binary partial MD5, which sync servers key a book by.
  const std::string document = KOReaderDocumentId::calculate(book);
  bool valid = document.size() == 32;
  for (const char c : document) valid = valid && (isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f'));
  if (valid) {
    char start[24], end[24], duration[16], startBp[8], endBp[8];
    snprintf(start, sizeof(start), "%lld", static_cast<long long>(session.startTime()));
    snprintf(end, sizeof(end), "%lld", static_cast<long long>(session.endTime()));
    snprintf(duration, sizeof(duration), "%lu", static_cast<unsigned long>(session.durationSeconds()));
    snprintf(startBp, sizeof(startBp), "%u", session.startProgressBp());
    snprintf(endBp, sizeof(endBp), "%u", session.endProgressBp());
    const Var vars[] = {{"book", book.c_str()},     {"document", document.c_str()}, {"start_time", start},
                        {"end_time", end},          {"duration_seconds", duration}, {"start_progress_bp", startBp},
                        {"end_progress_bp", endBp}, {"progress_scale", "10000"}};
    emit(Event::ReaderSession, vars, sizeof(vars) / sizeof(vars[0]));
  } else {
    LOG_ERR("PEVT", "reader.session: no document hash for %s", book.c_str());
  }
  session.reset();
}

}  // namespace

void opened(const std::string& bookPath) {
  std::lock_guard<BssMutex> guard(stateMutex);
  book = readerEventsWanted() ? bookPath : std::string();
  session.reset();
  hasPosition = false;
  percent = endPercent = 0;
  pendingTurn.store(false);
  if (book.empty()) return;
  const Var vars[] = {{"book", book.c_str()}};
  emit(Event::ReaderOpen, vars, 1);
}

bool tracking() {
  std::lock_guard<BssMutex> guard(stateMutex);
  return !book.empty();
}

void turned() { pendingTurn.store(true); }

void shown(const uint32_t position, const int progressBp, const int pageEndPercent) {
  std::lock_guard<BssMutex> guard(stateMutex);
  if (book.empty()) return;
  const bool turn = pendingTurn.exchange(false);
  // The same page saved again (a setting change, the exit's save): nothing new was read, and its time keeps running.
  if (hasPosition && position == lastPosition && !turn) return;
  const bool forward = turn && hasPosition && position > lastPosition;
  session.noteTurn(forward, true);
  session.onRenderComplete(millis(), trustedEpoch(), progressBp);
  lastPosition = position;
  hasPosition = true;
  percent = progressBp / 100;
  endPercent = pageEndPercent;
}

void closed() {
  std::lock_guard<BssMutex> guard(stateMutex);
  if (book.empty()) return;
  flushLocked();
  if (anySubscriber(Event::ReaderExit)) {
    char pct[8], endPct[8];
    snprintf(pct, sizeof(pct), "%d", percent);
    snprintf(endPct, sizeof(endPct), "%d", endPercent);
    const Var vars[] = {{"book", book.c_str()}, {"percent", pct}, {"end_percent", endPct}};
    emit(Event::ReaderExit, vars, 3);
  }
  book.clear();
  std::string().swap(book);
}

void flushSession() {
  std::lock_guard<BssMutex> guard(stateMutex);
  flushLocked();
}

bool current(std::string& bookPath, int& pct, int& endPct) {
  std::lock_guard<BssMutex> guard(stateMutex);
  if (book.empty()) return false;
  bookPath = book;
  pct = percent;
  endPct = endPercent;
  return true;
}

}  // namespace plugin_events::reader
