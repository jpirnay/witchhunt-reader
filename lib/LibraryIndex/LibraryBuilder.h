#pragma once

#include <BuildArena.h>
#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

#include "LibraryFormat.h"

// Builds the book index a step at a time (docs/design/library-index.md, "Building"): walks the card,
// joins what it found with the previous index, publishes, then resolves the books whose author is not
// known yet, publishing again every so often so the Authors tab fills in as it goes.
//
// A step is one bounded piece of work, so the screen hosting it stays responsive: a few dozen
// directory entries, the join, a publish, or one book's author. Every phase but the walk needs the
// lent framebuffer as `arena`, and resets it; without one it waits. The walk holds only the folders
// it is in and their metadata sidecars, 8 bytes each and at most MAX_SIDECARS per folder.
class LibraryBuilder {
 public:
  struct Author {
    std::string name;    // the primary author; "" for a book without one
    std::string fileAs;  // its opf:file-as; "" when the book gives none
  };
  // Looks up a book's author. False when that cannot be done just now (for want of memory, most
  // likely): the book stays pending and the next build asks again. `scratch` is the arena, reset.
  using Resolver = bool (*)(void* user, const std::string& path, uint32_t size, Author& out, BuildArena* scratch);

  struct Config {
    std::string root = "/";
    std::string workDir = library::DIR;
    std::string indexPath = library::INDEX_PATH;
    bool showHidden = false;  // what Browse Files lists; recorded in the index
    bool resolveAll = false;  // Refresh library
    uint16_t maxBooks = library::MAX_BOOKS;
    uint16_t republishEvery = 100;
    bool (*isBook)(const char* name) = nullptr;
    Resolver resolve = nullptr;
    void* resolveUser = nullptr;
  };

  enum class Phase : uint8_t { Walk, Join, Publish, Resolve, Done, Failed };

  explicit LibraryBuilder(Config config);

  Phase step(BuildArena* arena);
  Phase phase() const { return phase_; }
  bool finished() const { return phase_ == Phase::Done || phase_ == Phase::Failed; }
  bool needsArena() const { return phase_ == Phase::Join || phase_ == Phase::Publish || phase_ == Phase::Resolve; }
  // The next step replaces the index file: a reader holding it open must close it first.
  bool nextStepPublishes() const { return phase_ == Phase::Publish; }
  // True once after each publish, for the screen to reopen the index.
  bool takePublished();
  uint16_t booksFound() const { return found_; }
  uint16_t pending() const { return pending_; }
  uint16_t resolved() const { return resolved_; }
  // The books this build set out to resolve, fixed at the join: what resolved() counts towards.
  // (pending() is what is left, and goes down as resolved() goes up.)
  uint16_t toResolve() const { return toResolve_; }

 private:
  static constexpr size_t MAX_DEPTH = 8;
  static constexpr size_t MAX_SIDECARS = 128;
  static constexpr int WALK_BUDGET = 32;

  struct Sidecar {
    uint32_t stemHash;
    uint32_t sig;
  };
  struct Level {
    HalFile dir;
    std::string path;
    bool booksPass = false;  // false: noting the folder's sidecars; true: staging books, going down
    bool overflow = false;   // the folder had more sidecars than MAX_SIDECARS
    std::vector<Sidecar> sidecars{};
  };

  std::string work(const char* name) const;
  bool startWalk();
  void walkStep();
  bool listable(const char* name, bool atRoot) const;
  void noteSidecar(Level& level, HalFile& entry);
  uint32_t sidecarFor(const Level& level) const;
  bool stageBook(const Level& level, HalFile& entry);
  void join(BuildArena& arena);
  void publish(BuildArena& arena);
  void resolveStep(BuildArena& arena);
  bool openResolveFiles();
  void closeResolveFiles();
  void removeWorkingFiles() const;
  void fail(const char* what);

  Config config_;
  Phase phase_ = Phase::Walk;
  bool started_ = false;
  bool partial_ = false;
  bool published_ = false;
  bool resolveOpen_ = false;
  std::vector<Level> levels_;
  HalFile stage_;
  HalFile paths_;
  HalFile records_;
  HalFile names_;
  uint32_t pathsBytes_ = 0;
  uint32_t buildGen_ = 0;
  uint16_t found_ = 0;
  uint16_t pending_ = 0;
  uint16_t resolved_ = 0;
  uint32_t newestFolderDate_ = 0;  // the newest folder the walk listed, for the index's newestDate
  uint16_t toResolve_ = 0;
  uint16_t cursor_ = 0;
  uint16_t sincePublish_ = 0;
  char name_[500] = {};
};
