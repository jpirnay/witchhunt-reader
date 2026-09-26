#pragma once

#include <HalStorage.h>

#include <cstdint>

class CrossPointSettings;
class CrossPointState;
class WifiCredentialStore;
class KOReaderCredentialStore;
class RecentBooksStore;
class OpdsServerStore;
class ReadingStatsStore;

namespace JsonSettingsIO {

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
bool loadSettings(CrossPointSettings& s, const char* json, bool* needsResave = nullptr);

// CrossPointState
bool saveState(const CrossPointState& s, const char* path);
bool loadState(CrossPointState& s, const char* json);

// WifiCredentialStore
bool saveWifi(const WifiCredentialStore& store, const char* path);
bool loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave = nullptr);

// KOReaderCredentialStore
bool saveKOReader(const KOReaderCredentialStore& store, const char* path);
bool loadKOReader(KOReaderCredentialStore& store, const char* json, bool* needsResave = nullptr);

// RecentBooksStore
bool saveRecentBooks(const RecentBooksStore& store, const char* path);
bool loadRecentBooks(RecentBooksStore& store, const char* json);

// OpdsServerStore
bool saveOpds(const OpdsServerStore& store, const char* path);
bool loadOpds(OpdsServerStore& store, const char* json, bool* needsResave = nullptr);

// ReadingStatsStore
// Serialises the store straight into `out` (an open file), no in-RAM copy of the JSON.
bool saveReadingStats(const ReadingStatsStore& store, HalFile& out);
// Streams `in` into the store. NoMemory is a transient failure (the caller keeps the file and
// retries later); Corrupt is permanent (the caller sets the file aside).
enum class ReadingStatsLoad : uint8_t { Ok, NoMemory, Corrupt };
ReadingStatsLoad loadReadingStats(ReadingStatsStore& store, HalFile& in);

}  // namespace JsonSettingsIO
