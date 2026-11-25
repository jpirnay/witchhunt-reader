#include "ReleaseVersion.h"

#include <cstring>

ReleaseVersion parseReleaseVersion(const char* text) {
  ReleaseVersion version;
  version.isRc = strstr(text, "-rc.") != nullptr;
  int* const fields[] = {&version.major, &version.minor, &version.patch, &version.rcRelease, &version.rcBuild};
  // What comes before each field; the first has nothing before it.
  const char* const separators[] = {"", ".", ".", "-rc.", "."};
  const char* p = text;
  for (int i = 0; i < 5; i++) {
    const size_t separatorLength = strlen(separators[i]);
    if (strncmp(p, separators[i], separatorLength) != 0) break;
    p += separatorLength;
    if (*p < '0' || *p > '9') break;
    int value = 0;
    // Digits past the cap are read but leave the value as is: a field that large is not a version.
    for (; *p >= '0' && *p <= '9'; p++) {
      if (value < 100000000) value = value * 10 + (*p - '0');
    }
    *fields[i] = value;
  }
  return version;
}

bool isNewerRelease(const ReleaseVersion& latest, const ReleaseVersion& current) {
  if (latest.major != current.major) return latest.major > current.major;
  if (latest.minor != current.minor) return latest.minor > current.minor;
  if (latest.patch != current.patch) return latest.patch > current.patch;
  // The stable segments are equal: a stable release is newer than a candidate of the same version.
  if (latest.isRc != current.isRc) return !latest.isRc;
  if (!latest.isRc) return false;
  if (latest.rcRelease != current.rcRelease) return latest.rcRelease > current.rcRelease;
  return latest.rcBuild > current.rcBuild;
}
