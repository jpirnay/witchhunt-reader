#pragma once

// A firmware version as the release tags and CROSSPOINT_VERSION spell it: "1.31", "1.34.2", "1.35.0-rc.1",
// "1.36.0-rc.2.5", or a dev build's "2.41-dev+branch". The numbers are read as the sscanf format
// "%d.%d.%d-rc.%d.%d" read them: from the start, up to the first text that does not match, and the fields it does
// not reach stay 0. Versions in the old "rc.<hash>" form are not handled; reaching this code took a manual install
// of a later release anyway.
struct ReleaseVersion {
  int major = 0;
  int minor = 0;
  int patch = 0;
  int rcRelease = 0;
  int rcBuild = 0;
  bool isRc = false;  // "-rc." appears anywhere in the text
};

ReleaseVersion parseReleaseVersion(const char* text);

// Whether `latest` is a newer release than `current`: by major, minor and patch, then a stable release over a release
// candidate of the same version, then between two candidates by their RC release and build numbers.
bool isNewerRelease(const ReleaseVersion& latest, const ReleaseVersion& current);
