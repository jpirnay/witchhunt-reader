#pragma once

#include <ArduinoJson.h>
#include <WString.h>

// A JsonDocument written to the card whole, or not at all.
//
// Two allocations can fail on the way to the file without an error: the document's own, while it is
// filled (doc.overflowed() reports it), and the String's, while serializeJson() appends to it.
// ArduinoJson's String writer drops what the String cannot take and carries on, so on a heap with no
// block big enough the String ends short. Storage.writeFile() then swaps that in as a complete file:
// a truncated settings.json or wifi.json reads back as a parse error, and every setting or saved
// network is gone. The String's length is therefore checked against measureJson().
//
// From matcha-reader PR #315 and PR #508 by Eszter Schuffert: the check settings.json had, given to
// every JSON file the firmware writes.
namespace JsonFile {

// Serialises doc into out. False, logged under tag with `path` as the file it was for, when the
// document ran out of memory as it was built or out could not hold all of it.
bool serializeWhole(const char* tag, const char* path, const JsonDocument& doc, String& out);

// serializeWhole(), then Storage.writeFile(path). False when either fails; a refused save leaves the
// file on the card as it was.
bool save(const char* tag, const char* path, const JsonDocument& doc);

}  // namespace JsonFile
