# Activities and the ActivityManager

An activity is one screen. `ActivityManager` (`src/activities/ActivityManager.h`) owns the activity stack and the one render task, and makes sure only one activity is active at a time. This page is the reference for writing an activity: the base class, navigation, results, the task and locking model, and the pitfalls.

Related headers: `src/activities/Activity.h` (base class), `ActivityResult.h` (result types), `RenderLock.h`.

## The model

```text
┌──────────────────────────────────────────────────────────┐
│ Main Task (Arduino loop)                                 │
│                                                          │
│  activityManager.loop()                                  │
│    ├── currentActivity->loop()                           │
│    │     ├── handle input                                │
│    │     ├── update state (under RenderLock)             │
│    │     └── requestUpdate()                             │
│    ├── process pending action (Push / Pop / Replace)     │
│    └── if requestedUpdate: ──notify──► Render Task       │
│                                         (single, shared) │
│                                         10 KB stack      │
│                                         global mutex     │
│                                                          │
│  Activity Stack:                                         │
│  ┌──────────┬──────────┬──────────┐    ┌──────────┐     │
│  │ Home     │ Settings │ Wifi     │    │ Keyboard │     │
│  │ (stack)  │ (stack)  │ (stack)  │    │ (current)│     │
│  └──────────┴──────────┴──────────┘    └──────────┘     │
│   stackActivities[]                    currentActivity   │
└──────────────────────────────────────────────────────────┘
```

- There is one render task and one render mutex for the whole firmware, not one per activity.
- Navigation requests (`replaceActivity`, `pushActivity`, `popActivity`) do not act at once. They set a pending action that `ActivityManager::loop()` runs after the current `loop()` returns. An activity is never destroyed while its own code is on the stack.
- There is no `onPause` / `onResume`. A buried activity is kept alive but gets no `loop()` and no `render()`.
- Results come back through a callback passed to `startActivityForResult()`, not through a separate method.

## Writing an activity

Derive from `Activity` and pass a name, the renderer and the input manager:

```cpp
class MyActivity final : public Activity {
 public:
  MyActivity(GfxRenderer& r, MappedInputManager& m) : Activity("MyActivity", r, m) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
```

Do not take navigation callbacks (`goBack`, `goHome`) in the constructor. Use `finish()`, `onGoHome()` or the `activityManager` navigation functions described below.

### onEnter() and onExit()

Call the base class first in `onEnter()` and last in `onExit()`. The base versions reset the touch and hint-strip state and log.

```cpp
void MyActivity::onEnter() {
  Activity::onEnter();
  // allocate resources, load state
  requestUpdate();
}
void MyActivity::onExit() {
  // free resources
  Activity::onExit();
}
```

- `onEnter()` runs on the main task, after the manager has made the activity current. Request the first render here.
- `onExit()` runs just before the activity is deleted. Free what you allocated. Keep it short and do not block.
- Neither function creates or destroys a render task. The manager creates it once in `ActivityManager::begin()`.

### loop()

`loop()` runs on the main task every iteration while the activity is current. Read input, change state, call `requestUpdate()`. Any state that `render()` also reads must be changed under a `RenderLock` (see below). Do not draw from `loop()`.

### render()

`render(RenderLock&&)` runs on the render task with the render mutex held. It must:

- only read state; never navigate or `finish()` from it,
- draw through `renderer` (and `GUI`) and finish with `renderer.displayBuffer()`,
- not call `requestUpdateAndWait()` (it asserts).

Other virtual hooks (`skipLoopDelay`, `preventAutoSleep`, `keepAwake`, `usesWifi`, `prepareFramebufferForCapture`, `handleForcedRefresh`, `selectListRow`, `pageList` and so on) are documented in `Activity.h`. Override only what the screen needs.

## Navigation

### Push vs replace

On an ESP32-C3 heap fragmentation is a real constraint, especially when the next activity is heavy (EPUB reader, TLS sync). The choice of call decides whether the caller's memory is freed first.

| Call                                               | Current activity                | Stack            | When to use                                                 |
|----------------------------------------------------|---------------------------------|------------------|-------------------------------------------------------------|
| `replaceActivity()` / `goTo*()` / `replaceWith*()` | **destroyed** (onExit + delete) | cleared          | Forward flow: the caller has no reason to stay resident     |
| `pushActivity()` / `startActivityForResult()`      | kept alive on stack             | parent preserved | Modal result flow: the caller needs to resume with a result |

Default to replace. Push only when a still-living parent must receive a result (keyboard entry, confirmation dialog, chapter picker). For one-way transitions such as opening a book, going to settings or switching tabs, replace, so the parent is freed before the next activity runs.

### Navigating

From any activity method:

```cpp
activityManager.goToSettings();
activityManager.goToReader(path);
activityManager.replaceActivity(std::make_unique<MyActivity>(renderer, mappedInput));
```

`replaceActivity()` destroys the current activity and clears the stack. The `goTo*()` helpers are thin wrappers around it. The full list is in `ActivityManager.h`. `goHome()` is the hard reset to the home screen.

### Starting a child and receiving its result

Use `startActivityForResult()` when the parent needs an answer:

```cpp
void MyActivity::launchWifi() {
  startActivityForResult(
      std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto& wifi = std::get<WifiResult>(result.data);
        onWifiDone(wifi.connected);
      });
}
```

The child reports back with `setResult()` and `finish()`:

```cpp
setResult(WifiResult{.connected = true, .ssid = ssid});
finish();

// to cancel:
ActivityResult res;
res.isCancelled = true;
setResult(std::move(res));
finish();
```

- `startActivityForResult()` moves the parent onto the stack and makes the child current.
- `finish()` queues a pop. On the next loop iteration the manager deletes the child, makes the parent current again, calls the parent's result handler with the child's result, and requests a re-render of the parent.
- Result types are the structs in `ActivityResult.h`, held in the `ResultVariant`. A new result type goes into that variant. A child that calls `finish()` without `setResult()` delivers an empty result (`std::monostate`) with `isCancelled` false, so check the variant before `std::get`.
- The handler is moved out before it runs, so it may start another child with `startActivityForResult()`.
- Capturing `this` in the handler is safe: the parent stays alive on the stack while the child runs.

### Leaving a flow: finish(), onGoHome(), goHome()

- `finish()`: with a non-empty stack, pops back to the parent. With an empty stack (the activity was launched by a replace), it falls through to `returnFromChild()`.
- `onGoHome()`: "up and out". It calls `returnFromChild()`, so a long press of Back in a reader returns to the view that opened the book, not always Home.
- `activityManager.goHome()`: hard reset to Home. Clears the return hint.

### The ReturnHint pattern

Replace destroys the parent, but Back should still return to where the user came from. `ActivityManager` keeps one `ReturnHint` for that:

```cpp
enum class ReturnTo : uint8_t { Home, Library, AllFiles, GlobalBookmarks };

struct ReturnHint {
  ReturnTo target = ReturnTo::Home;
  std::string path;              // FileBrowser directory to restore
  std::string selectName;        // item to re-focus (file name, book title)
  int selectIndex = -1;          // e.g. the Home carousel's index
  std::string selectionContext;  // optional activity-specific restore key
  int selectBookmarkIndex = -1;  // optional bookmark index for GlobalBookmarks
};
```

A parent records a hint before a forward flow. When the launched activity, or anything it chains to, exits with an empty stack, `returnFromChild()` consumes the hint and routes to the right parent, restoring its selection. With no hint it calls `goHome()`.

`ReturnTo::Library` routes through `goToLibrary()`: `selectIndex` carries the Library tab, `path` the Books tab's folder, and `selectionContext` the hash of the author open on the Authors tab, if any. `ReturnTo::AllFiles` reopens the file browser in its All Files mode.

There are two ways to set a hint.

1. The dedicated wrappers `replaceWithReader(path, hint)` and `replaceWithFileBrowser(path, hint, focusName)`, which record the hint and replace in one call:

   ```cpp
   // HomeActivity, opening a recent book
   ReturnHint hint;
   hint.target     = ReturnTo::Home;
   hint.selectName = path;   // book to re-focus in the recents strip
   activityManager.replaceWithReader(path, std::move(hint));
   ```

   `FileBrowserActivity` builds its hint in `returnHint()`, so closing a book comes back to the Library tab, folder or author, and row it was opened from.

2. `setReturnHint()` followed by any plain `goTo*()`:

   ```cpp
   // HomeActivity, opening Settings
   ReturnHint hint;
   hint.target      = ReturnTo::Home;
   hint.selectIndex = selectorIndex;   // restore focus on the same menu entry
   activityManager.setReturnHint(std::move(hint));
   activityManager.goToSettings();     // parent destroyed; hint survives
   ```

Plain `goTo*()` helpers leave the hint alone, so it survives chains such as Home, Reader, KOReaderSync, Reader, back to Home. These clear or overwrite it: `goHome()`, the `replaceWith*()` helpers (they set their own), `goToGlobalBookmarks(hint)` (it takes its own), and `returnFromChild()` (it consumes it). `clearReturnHint()` drops it explicitly.

## Task model

The firmware runs on two kinds of chip. The Xteink X3 and X4 use an ESP32-C3, a single-core RISC-V part. The X4 Pro and the LilyGo T5 S3 use a dual-core ESP32-S3. `ActivityManager::begin()` creates the render task with `xTaskCreatePinnedToCore()`, priority 1, 10 KB stack, on CPU 1 when the part has two cores and on CPU 0 otherwise. On the S3 the pin keeps multi-second page builds and cover decodes off CPU 0, where the system and Wi-Fi tasks and the idle-task watchdog live. The render stack is 10 KB because the render task runs the deepest call chains (page build, CSS resolve, image decode) and its stack abuts the heap top, so an overflow shows up as heap corruption elsewhere.

```text
┌──────────────────────┐     ┌──────────────────────────┐
│ Main Task            │     │ Render Task              │
│ (Arduino loop)       │     │ (ActivityManager-owned)  │
│ Priority: 1          │     │ Priority: 1              │
│                      │     │                          │
│ Runs:                │     │ Runs:                    │
│ - gpio.update()      │     │ - ulTaskNotifyTake()     │
│ - activity->loop()   │     │   (blocks until notified)│
│ - pending actions    │     │ - RenderLock (mutex)     │
│ - sleep/power mgmt   │     │ - activity->render()     │
│ - requestUpdate →────┼─────┼─► xTaskNotify()          │
│   (end of loop)      │     │                          │
└──────────────────────┘     └──────────────────────────┘
```

On the C3 the two tasks alternate: the main task runs `loop()` and, at the end of the iteration, notifies the render task if an update was requested. The render task wakes, takes the mutex, calls `render()`, releases the mutex and blocks again.

Do not call `xTaskCreate` inside an activity. If a screen seems to need a background task, propose a lifecycle-aware worker abstraction first.

## The render mutex and RenderLock

One FreeRTOS mutex (`renderingMutex`, private to `ActivityManager`) protects state shared between `loop()` and `render()`. They run on different tasks, so anything `render()` reads and `loop()` writes must be guarded. `RenderLock` (`RenderLock.h`) is the RAII wrapper:

```cpp
class RenderLock {
 public:
  enum class Mode { Blocking, Try };
  explicit RenderLock(Mode mode = Mode::Blocking);  // acquire the global mutex
  explicit RenderLock(Activity&);                   // same; parameter unused
  explicit RenderLock(ExclusiveActivityAccess);     // mutex AND no render pass in flight
  ~RenderLock();                                    // releases if still held
  bool ownsLock() const;                            // false after a failed Try
  void unlock();                                    // early release
  static bool peek();
};
```

Activities use the plain constructor. `ExclusiveActivityAccess` is for the manager's own transitions: the render task drops the mutex mid-pass and keeps using the activity, so code that destroys the current activity must wait for the pass to end, not just for the mutex.

`Mode::Try` takes the mutex only if it is free at that instant and never waits. Check `ownsLock()` before touching guarded state. It is for loop-task code that reads render-owned state but would rather skip a tick than stall input behind a render pass, such as the reader's `skipLoopDelay()`, `preventAutoSleep()` and `shouldSkipPeriodicUpdate()` hooks. It answers "is the mutex free", not "is a pass in flight": a Try can succeed while the render task has dropped the mutex mid-pass.

```cpp
// In loop(): guard state that render() reads
void MyActivity::loop() {
  if (somethingChanged) {
    RenderLock lock;
    state = newState;        // render() cannot run while the lock is held
  }
  requestUpdate();           // after the lock is released
}

// In render(): the lock is passed in and held for the whole call
void MyActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderer.drawText(/* ... */);
  renderer.displayBuffer();
}
```

Keep critical sections short. Acquire, change state, release, then do blocking work:

```cpp
// WRONG: render is blocked for the whole network call
RenderLock lock;
state = LOADING;
auto result = http.get(url);
state = DONE;

// CORRECT
{ RenderLock lock; state = LOADING; }
requestUpdate(true);          // show "Loading..." before we block
auto result = http.get(url);  // lock not held
{ RenderLock lock; state = DONE; }
requestUpdate();
```

## requestUpdate() and requestUpdateAndWait()

```text
requestUpdate()                 requestUpdate(true)           requestUpdateAndWait()
───────────────                 ───────────────────           ──────────────────────
Sets a flag. Render starts      Notifies the render task      Notifies the render task and
after loop() returns and        at once. Does not wait for    blocks the caller until the
the manager sees the flag.      the render to finish.         render is done.
```

- Default `requestUpdate()` is deferred and batched. Several state changes in one `loop()` give one render. Use it almost always.
- `requestUpdate(true)` only when the render must start before the current function returns, for example before a blocking network call.
- `requestUpdateAndWait()` when the screen must show the new state before you go on (for example "Checking for update..." before an API call). Use sparingly.

`requestUpdateAndWait()` asserts that the caller is not the render task, does not hold a `RenderLock`, and that no other task is already waiting (only one waiter is supported). Holding the lock while waiting would deadlock: the render task needs the mutex to run `render()`, and the caller holds it while waiting for `render()` to finish.

An activity can call `isUpdateSuperseded()` from `render()` to learn that another render is already queued, and skip a purely cosmetic tail of the current one. The answer is advisory.

## Lifecycle

Replace:

```text
activityManager.replaceActivity(make_unique<MyActivity>(...))
  → pendingAction = Replace, pendingActivity = MyActivity
  (next loop iteration, ActivityManager::loop())
  ├── currentActivity->loop()        // old activity's last loop
  ├── RenderLock (exclusive)
  ├── oldActivity->onExit(); delete oldActivity; clear stack
  ├── currentActivity = MyActivity
  ├── unlock
  └── MyActivity->onEnter()
```

Push and pop:

```text
Parent: startActivityForResult(make_unique<Child>(...), handler)
  → handler stored on parent, pendingAction = Push
  (next iteration)
  ├── parent moved to stackActivities
  ├── currentActivity = Child
  └── Child->onEnter()

        ... child runs ...

Child: setResult(MyResult{...}); finish();
  → pendingAction = Pop
  (next iteration)
  ├── take child->result
  ├── Child->onExit(); delete Child
  ├── currentActivity = parent (popped from stack)
  ├── parent's handler(result)
  └── requestUpdate()                // automatic re-render for the parent
```

Every transition also arms an input drain: button events are discarded until all buttons are released, so the press that left one screen cannot act on the next. Queued transitions also paint a busy indicator first, unless the activity returns true from `suppressesBusyIndicator()`.

## Common pitfalls

**Continuing after `finish()`.** `finish()` only queues a pop. The activity is destroyed on the next `ActivityManager::loop()` iteration. Touching members later in the same function is safe, but do not rely on the activity surviving past the current `loop()`.

**Shared state without `RenderLock`.** If `render()` reads a value and `loop()` writes it, the write needs a `RenderLock`. Otherwise `render()` can see a half-written string or struct.

**Background tasks that outlive the activity.** The manager does not track tasks. A task created in `onEnter()` must be deleted in `onExit()` before the activity is destroyed. Better, do not create one.

**Pushing for forward navigation.** A pushed parent stays resident. For a heavy child (EPUB reader, TLS sync) on a fragmented heap, that can decide between a clean launch and OOM. Push only to receive a result. For "back to where I came from", record a `ReturnHint` and replace.

**Stale `ReturnHint`.** A hint lives until `returnFromChild()` or `goHome()` clears it or a `replaceWith*()` overwrites it. If a flow aborts down an unusual path (error screen, boot transition), the next unrelated `finish()` could consume it. Set the hint immediately before the transition, and call `activityManager.clearReturnHint()` if you abort without launching the target.

**Holding `RenderLock` across blocking calls.** See the example in the render mutex section.

**Re-syncing the write buffer on a path that already prepared it.** `dispatchLightPanelGesture()` calls `syncWriteBufferFromDisplayed()` and then `prepareFramebufferForCapture()`, in that order. In the reader the secondary buffer holds Background-A's pre-rendered next page, so the sync alone restores the wrong page and only `prepareFramebufferForCapture()` puts the visible one back. Any later code in the same `loop()` tick that syncs again undoes the second step and composites the drawer onto the wrong page. A step added between the gesture dispatchers and the pending-action loop must not sync unconditionally. `ActivityManager::framebufferPreparedThisTick` records that the frame is already prepared.
