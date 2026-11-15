// FirmwareFiles: what the firmware keeps at the card root for itself, which the Library index,
// Recent books and the next-book suggestion leave out (crosspoint-reader PRs #4373 and #4471,
// matcha-reader PRs #783 and #802).

#include <gtest/gtest.h>

#include <string>

#include "FirmwareFiles.h"

namespace {

TEST(FirmwareFiles, TheCrashReportAtTheRootIsTheFirmwares) {
  EXPECT_TRUE(FirmwareFiles::isFirmwareFile(FirmwareFiles::CRASH_REPORT_PATH));
  EXPECT_TRUE(FirmwareFiles::isFirmwareFile("/CRASH_REPORT.TXT")) << "FAT names ignore case";
  EXPECT_TRUE(FirmwareFiles::isEntry("/", "crash_report.txt", false));
  EXPECT_TRUE(FirmwareFiles::isRootEntry("crash_report.txt", false));
}

TEST(FirmwareFiles, ABookNamedLikeTheCrashReportIsABook) {
  EXPECT_FALSE(FirmwareFiles::isFirmwareFile("/Books/crash_report.txt")) << "only the root's";
  EXPECT_FALSE(FirmwareFiles::isFirmwareFile("/crash_report_novel.txt"));
  EXPECT_FALSE(FirmwareFiles::isFirmwareFile("/crash_report.txt.md"));
  EXPECT_FALSE(FirmwareFiles::isRootEntry("crash_report.txt", /*isDir=*/true)) << "a folder of that name";
}

TEST(FirmwareFiles, EverythingInTheResourceFoldersIsTheFirmwares) {
  for (const char* folder : FirmwareFiles::ROOT_FOLDERS) {
    const std::string root = std::string("/") + folder;
    EXPECT_TRUE(FirmwareFiles::isRootEntry(folder, /*isDir=*/true)) << root;
    EXPECT_TRUE(FirmwareFiles::isEntry("/", folder, true)) << root;
    EXPECT_TRUE(FirmwareFiles::isFirmwareFile(root + "/OFL.txt")) << root;
    EXPECT_TRUE(FirmwareFiles::isFirmwareFile(root + "/Inter/README.md")) << root;
    EXPECT_TRUE(FirmwareFiles::isEntry(root, "OFL.txt", false)) << root;
    EXPECT_TRUE(FirmwareFiles::isEntry(root + "/", "OFL.txt", false)) << root;
    EXPECT_TRUE(FirmwareFiles::isEntry(root + "/Inter", "Sub", true)) << root;
  }
  EXPECT_TRUE(FirmwareFiles::isFirmwareFile("/FoNtS/OFL.TXT"));
  EXPECT_TRUE(FirmwareFiles::isFirmwareFile("/Dictionaries/en/README.txt"));
}

TEST(FirmwareFiles, BooksOutsideTheResourceFoldersAreBooks) {
  for (const char* folder : FirmwareFiles::ROOT_FOLDERS) {
    const std::string name(folder);
    EXPECT_FALSE(FirmwareFiles::isFirmwareFile("/Books/" + name + "/Mort.epub")) << "a folder of that name below";
    EXPECT_FALSE(FirmwareFiles::isFirmwareFile("/" + name + "-books/Mort.epub")) << "a longer name";
    EXPECT_FALSE(FirmwareFiles::isFirmwareFile("/my" + name + "/Mort.epub")) << "a longer name";
    EXPECT_FALSE(FirmwareFiles::isFirmwareFile("/" + name + ".txt")) << "a root file of that name";
    EXPECT_FALSE(FirmwareFiles::isRootEntry(folder, /*isDir=*/false)) << "a file, not the folder";
    EXPECT_FALSE(FirmwareFiles::isEntry("/Books", folder, true)) << "below the root";
  }
  EXPECT_FALSE(FirmwareFiles::isFirmwareFile("/Mort.epub"));
  EXPECT_FALSE(FirmwareFiles::isFirmwareFile("/COMPLETED/Mort.epub")) << "finished books are still books";
  EXPECT_FALSE(FirmwareFiles::isEntry("/", "Books", true));
  EXPECT_FALSE(FirmwareFiles::isFirmwareFile("Mort.epub")) << "not a card path";
}

}  // namespace
