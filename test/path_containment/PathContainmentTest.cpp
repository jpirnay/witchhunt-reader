// FsHelpers::normalisePath is what keeps every web-server path inside the card.
//
// The web server's protected-item rules (dotfiles, "System Volume Information",
// "XTCache") test only the LAST component of a requested path. That is sound only
// if ".." has already been resolved: "/books/../.private/notes.txt" ends in
// "notes.txt", which passes every one of those rules, while the filesystem reads
// it inside the dot-folder the rules exist to protect. CrossPointWebServer routes
// user input through normalizeWebPath -> normalisePath before those checks for
// exactly that reason, so the property below is load-bearing rather than cosmetic.
// crosspoint-reader PR #3353.
//
// Contract note: normalisePath emits components joined by "/" with NO leading
// slash -- "/books/x" comes back as "books/x". normalizeWebPath (a static in
// CrossPointWebServer.cpp, hence not reachable from here) re-adds the leading
// slash and strips a trailing one. The tests below are written in normalisePath's
// own terms; what matters for containment is which components survive, not the
// slash the caller puts back.

#include <gtest/gtest.h>

#include <string>

#include "FsHelpers.h"

namespace {
std::string norm(const std::string& in) { return FsHelpers::normalisePath(in); }
}  // namespace

// Ordinary paths keep their components; redundant separators and "." collapse.
TEST(PathContainment, LeavesPlainPathsAlone) {
  EXPECT_EQ(norm("/books/novel.epub"), "books/novel.epub");
  EXPECT_EQ(norm("/books//novel.epub"), "books/novel.epub");
  EXPECT_EQ(norm("/books/./novel.epub"), "books/novel.epub");
}

// A ".." cancels the component before it.
TEST(PathContainment, ResolvesParentReferences) {
  EXPECT_EQ(norm("/books/../notes.txt"), "notes.txt");
  EXPECT_EQ(norm("/a/b/c/../../d"), "a/d");
}

// The case the protected-item rules depend on. Before normalisation the last
// component is an innocuous "notes.txt" while the path reaches into a dot-folder;
// after it, the dot-folder is visible in the result the caller will open.
TEST(PathContainment, TraversalCannotHideADotFolder) {
  const std::string raw = "/books/../.private/notes.txt";
  const std::string resolved = norm(raw);
  EXPECT_EQ(resolved, ".private/notes.txt");
  // The guard reads the last component, and it looks harmless either way ...
  EXPECT_EQ(resolved.substr(resolved.rfind('/') + 1), "notes.txt");
  // ... so containment has to come from the resolved prefix, which is now present.
  EXPECT_EQ(resolved.rfind(".private/", 0), 0u);
}

// Climbing past the root clamps there rather than escaping or underflowing.
TEST(PathContainment, ClampsAtRoot) {
  EXPECT_EQ(norm("/../../../etc/passwd"), "etc/passwd");
  EXPECT_EQ(norm("/.."), "");
  EXPECT_EQ(norm("/../.."), "");
  EXPECT_EQ(norm("/a/../../../b"), "b");
}

// Degenerate inputs must not crash or produce something that reads as a path.
TEST(PathContainment, HandlesDegenerateInput) {
  EXPECT_EQ(norm(""), "");
  EXPECT_EQ(norm("/"), "");
  EXPECT_EQ(norm("//"), "");
  EXPECT_EQ(norm("/./"), "");
}

// ".." only counts as a parent reference when it is the WHOLE component. These
// are ordinary names and must survive, which is why the web server rejects only
// the exact "." and ".." components rather than any name containing "..".
TEST(PathContainment, DoubleDotInsideANameIsNotTraversal) {
  EXPECT_EQ(norm("/books/volume..2.epub"), "books/volume..2.epub");
  EXPECT_EQ(norm("/books/notes...txt"), "books/notes...txt");
  EXPECT_EQ(norm("/books/..hidden"), "books/..hidden");
}
