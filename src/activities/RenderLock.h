#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  // Tag type selecting the constructor below. Named rather than a bool so call sites
  // read as a statement of intent instead of `RenderLock lock(true)`.
  struct ExclusiveActivityAccess {};

  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility

  // Acquire the rendering mutex AND guarantee the render task is not inside
  // currentActivity->render(). Use this — never the plain constructor — before
  // destroying or replacing the current activity.
  //
  // Holding the mutex alone is NOT enough: the render task deliberately drops it in the
  // middle of a pass (renderContents() releases it before the waveform wait so the loop
  // task can service input and schedule a pre-render) and then re-acquires it and keeps
  // dereferencing the activity. A transition that took the plain lock in that window
  // could run the activity's destructor while the render task was still using it.
  //
  // Blocks until both conditions hold. See ActivityManager::renderPassActive.
  explicit RenderLock(ExclusiveActivityAccess);

  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  static bool peek();
};
