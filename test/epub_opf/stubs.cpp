#include <cstddef>
#include <new>
#include <string>
#include <unordered_set>
#include <vector>

#include "../../lib/Epub/Epub/BookMetadataCache.h"

namespace opf_test_hooks {
std::vector<std::string>* g_spineHrefSink = nullptr;
// Nonzero: nothrow array new refuses any request larger than this many bytes and counts the
// refusal, so a test can make the heap turn down ContentOpfParser's index growth.
size_t g_refuseNothrowArraysAbove = 0;
size_t g_refusedNothrowArrays = 0;
// Nonzero: nothrow scalar new refuses any request larger than this many bytes, so a test can make
// SaxParser::init() fail for want of its ~10 KB state, as a device short of heap would.
size_t g_refuseNothrowScalarsAbove = 0;

// The nothrow arrays handed out and not yet freed, so a test can tell whether the parser still
// holds one. Never destroyed: delete[] may run during static destruction.
std::unordered_set<void*>& liveNothrowArrays() {
  static auto* live = new std::unordered_set<void*>();
  return *live;
}
}  // namespace opf_test_hooks

void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (opf_test_hooks::g_spineHrefSink != nullptr) {
    opf_test_hooks::g_spineHrefSink->push_back(href);
  }
}

// Replaces the global nothrow array new for this test binary: makeUniqueNoThrow<T[]>, which grows
// the manifest index, lands here. Unarmed it defers to the throwing operator new[], exactly as the
// default nothrow form does, so operator delete[] below still pairs with it.
void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
  if (opf_test_hooks::g_refuseNothrowArraysAbove != 0 && size > opf_test_hooks::g_refuseNothrowArraysAbove) {
    ++opf_test_hooks::g_refusedNothrowArrays;
    return nullptr;
  }
  try {
    void* block = ::operator new[](size);
    opf_test_hooks::liveNothrowArrays().insert(block);
    return block;
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

// What the default operator delete[] does, plus forgetting a tracked nothrow array.
void operator delete[](void* block) noexcept {
  if (block != nullptr) opf_test_hooks::liveNothrowArrays().erase(block);
  ::operator delete(block);
}
void operator delete[](void* block, std::size_t) noexcept { operator delete[](block); }

// Replaces the global nothrow scalar new: SaxParser allocates its parser state through it.
void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
  if (opf_test_hooks::g_refuseNothrowScalarsAbove != 0 && size > opf_test_hooks::g_refuseNothrowScalarsAbove) {
    return nullptr;
  }
  try {
    return ::operator new(size);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}
