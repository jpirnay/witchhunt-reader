#include "LongTaskProgress.h"

namespace LongTaskProgress {
namespace {
Handler gHandler = nullptr;
}  // namespace

void setHandler(const Handler handler) { gHandler = handler; }

void step(const char* stage) {
  if (gHandler != nullptr && stage != nullptr) {
    gHandler(stage);
  }
}

}  // namespace LongTaskProgress
