// IdfPrintfShim (src/platform/IdfPrintfShim.h): the prebuilt IDF libraries' printf, fprintf and vprintf calls,
// formatted with vsnprintf instead of newlib's stream formatter. These read back what it writes to a stream.
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "IdfPrintfShim.h"

extern "C" int __wrap_fprintf(FILE* stream, const char* format, ...);

namespace {

// What one __wrap_fprintf call writes, and what it returns.
template <typename... Args>
std::string printed(int* returned, const char* format, Args... args) {
  FILE* file = tmpfile();
  EXPECT_NE(file, nullptr);
  *returned = __wrap_fprintf(file, format, args...);
  fflush(file);
  rewind(file);
  std::string text;
  for (int c = fgetc(file); c != EOF; c = fgetc(file)) text.push_back(static_cast<char>(c));
  fclose(file);
  return text;
}

}  // namespace

TEST(IdfPrintfShim, WritesWhatPrintfWould) {
  int returned = 0;
  const std::string text =
      printed(&returned, "E (%lu) %s: %s(%d): %.2f %x\n", 12345UL, "gpio", "remove", 576, 1.5, 255);
  EXPECT_EQ(text, "E (12345) gpio: remove(576): 1.50 ff\n");
  EXPECT_EQ(returned, static_cast<int>(text.size()));
}

TEST(IdfPrintfShim, APieceWithoutALineEndStaysOne) {
  int returned = 0;
  EXPECT_EQ(printed(&returned, "%s", "free: "), "free: ");
  EXPECT_EQ(printed(&returned, "%s", ""), "");
  EXPECT_EQ(returned, 0);
}

TEST(IdfPrintfShim, ALongLineIsCutAndKeepsItsLineEnd) {
  const std::string longText(300, 'x');
  int returned = 0;
  const std::string text = printed(&returned, "E (1) tag: %s\n", longText.c_str());
  ASSERT_EQ(text.size(), idf_printf_shim::kLineBytes - 1);
  EXPECT_EQ(text.back(), '\n');
  EXPECT_EQ(text.substr(0, 11), "E (1) tag: ");
  EXPECT_EQ(text[text.size() - 2], 'x');
}

TEST(IdfPrintfShim, ALongPieceWithoutALineEndGetsNone) {
  const std::string longText(300, 'y');
  int returned = 0;
  const std::string text = printed(&returned, "%s", longText.c_str());
  ASSERT_EQ(text.size(), idf_printf_shim::kLineBytes - 1);
  EXPECT_EQ(text.back(), 'y');
}

TEST(IdfPrintfShim, ALineThatJustFitsIsWhole) {
  const std::string fits(idf_printf_shim::kLineBytes - 2, 'z');
  int returned = 0;
  const std::string text = printed(&returned, "%s\n", fits.c_str());
  EXPECT_EQ(text, fits + "\n");
}
