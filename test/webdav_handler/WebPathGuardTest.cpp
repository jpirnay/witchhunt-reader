// isProtectedWebPath() (src/network/WebPathGuard.cpp), the guard the web file manager, its uploads and WebDAV all ask,
// on the in-memory card of stubs/HalStorage.h, which opens an 8.3 alias as SdFat does: the handle gives the alias back
// as its name, not the long name behind it.

#include <gtest/gtest.h>

#include "HalStorage.h"
#include "WebPathGuard.h"

using ProtectedPaths::Target;

namespace {

class WebPathGuardTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    Storage.putFolder("/.crosspoint");
    Storage.put("/.crosspoint/wifi.json", "{}");
    Storage.alias("/CROSSP~1", "/.crosspoint");
    Storage.putFolder("/Books");
    Storage.put("/Books/AB~C0DE.txt", "plain");
  }

  static bool refused(const char* path, const Target target, const bool allowHidden) {
    return isProtectedWebPath(String(path), target, allowHidden);
  }
};

// The guard read the name back from the handle the alias opened, got "CROSSP~1" and took it for the entry's real
// name, so /CROSSP~1/wifi.json passed: the file manager served the credential store to anyone on the network.
TEST_F(WebPathGuardTest, CredentialStoreByAliasIsRefused) {
  for (const bool allowHidden : {false, true}) {
    EXPECT_TRUE(refused("/CROSSP~1/wifi.json", Target::Item, allowHidden)) << allowHidden;
    EXPECT_TRUE(refused("/crossp~1/wifi.json", Target::Item, allowHidden)) << allowHidden;
    EXPECT_TRUE(refused("/CROSSP~1", Target::Directory, allowHidden)) << allowHidden;  // its listing
  }
}

TEST_F(WebPathGuardTest, RealAliasShapedNamesStayReachable) {
  EXPECT_FALSE(refused("/Books/AB~C0DE.txt", Target::Item, false));
  EXPECT_FALSE(refused("/Books/ab~c0de.TXT", Target::Item, false));  // FAT matches it without case
  EXPECT_FALSE(refused("/Books/NEW~1.TXT", Target::Item, false));    // nothing there yet: it gets that name
}

// When the long name cannot be found, the segment counts as an alias.
TEST_F(WebPathGuardTest, AnUnfinishedLookupFailsClosed) {
  Storage.failOpen("/Books");
  EXPECT_TRUE(refused("/Books/AB~C0DE.txt", Target::Item, false));
  Storage.reset();
  SetUp();
  Storage.failListing("/Books");
  EXPECT_TRUE(refused("/Books/AB~C0DE.txt", Target::Item, false));
}

}  // namespace
