# DILL Library - Architecture & Function Call Flow

## Overview
DILL (DirectInput Listener Library) is a Windows-only C++17 library that wraps
DirectInput 8 joystick enumeration/polling behind a C ABI, exposing a
callback-based interface for joystick/gamepad input handling. It runs a
**single internal thread**, driven by `MsgWaitForMultipleObjectsEx`, that owns
DirectInput, the hidden notification window, and all device state.

This document was rewritten after converting DILL from an unsynchronized
two-thread design (a 4ms busy-poll thread plus a separate `GetMessage`
message thread) to the current single-thread, event-driven design. The
"Tricky Aspects" section at the end records bugs that are easy to
reintroduce if this code is touched again without this context — read it
before changing `event_loop_main`, `enumerate_devices`, or `initialize_device`.

## Architecture Components

### 1. Core Data Structures

#### Global Storage (`g_data_store`, `src/dill.h`)
```cpp
struct DeviceDataStore {
    std::unordered_map<GUID, LPDIRECTINPUTDEVICE8> device_map;
    std::unordered_map<GUID, bool> is_buffered;
    std::unordered_map<GUID, DeviceSummary> cache;
    std::unordered_map<GUID, DeviceState> state;
    std::unordered_map<GUID, bool> is_ready;
    std::vector<GUID> active_guids;
    std::unordered_map<GUID, HANDLE> event_handles;   // per-device notification event
};
```
Guarded by a single `static std::mutex g_data_store_mutex;` (`dill.cpp`).
Every read/write takes a short-lived `std::lock_guard`; the lock is never
held across a DirectInput/COM call or a user callback invocation.

`is_buffered`/`cache`/`state`/`is_ready` entries are erased together with
`device_map`/`event_handles`/`active_guids` when a device disconnects (all
under the same lock, in `enumerate_devices()`'s stale-removal loop). The
`Disconnected` callback still gets the device's last-known `DeviceSummary`
because it's read out of `cache` *before* the erase, into a local `di`. This
wasn't always true — see "Tricky Aspect #5" below.

#### Per-Device State (`DeviceState`, `src/dill.h`)
```cpp
struct DeviceState {
    std::vector<LONG> axis;    // size 9  - indices 1-8 valid, 0 unused
    std::vector<bool> button;  // size 129 - indices 1-128 valid, 0 unused
    std::vector<LONG> hat;     // size 5  - indices 1-4 valid, 0 unused
};
```
All three are **1-based**, matching `axis_map`'s `axis_index` convention and
what the Python side expects: physical input N is stored/read at index N,
index 0 is permanently unused. `get_axis`/`get_button`/`get_hat` all reject
index `0` and validate against the same upper bound the vector was sized
for (8/128/4). See "Tricky Aspect #5" below for why the sizes are `count+1`,
not `count`.

#### Callback Function Pointers
```cpp
std::atomic<JoystickInputEventCallback> g_event_callback{nullptr};
std::atomic<DeviceChangeCallback> g_device_change_callback{nullptr};
```
Atomics, not guarded by the mutex — plain scalar function pointers, written
from any client thread via `set_input_event_callback`/
`set_device_change_callback`, read via `.load()` on the event loop thread
before invoking.

### 2. Thread Architecture

DILL owns **exactly one internal thread** (`event_loop_main`, started by
`init()`, joined by `shutdown()`). It is a `std::thread` member of a
`LoopThreadGuard` whose destructor `detach()`s (never `join()`s) if the
thread is still joinable at static-teardown time — a bare `std::thread`
left joinable there would call `std::terminate()`; this is a defensive
last resort, not a substitute for calling `shutdown()`.

That one thread does everything: owns the hidden message-only window,
`RegisterDeviceNotification`, DirectInput enumeration/device objects, input
draining, and polled-fallback device servicing. There is no separate
polling thread and no separate message thread.

## Function Call Flow

### Phase 1: `init()`

```
User Application
    │
    ├─ set_input_event_callback(fn)   → g_event_callback.store(fn)
    ├─ set_device_change_callback(fn) → g_device_change_callback.store(fn)
    │
    └─ init()
        ├─ g_running.compare_exchange_strong(false, true)   [idempotency gate]
        ├─ CreateEvent × 4: g_quit_event, g_rebuild_event,
        │                   g_hotplug_event, g_startup_done_event
        ├─ g_loop.thread = std::thread(event_loop_main)
        └─ WaitForSingleObject(g_startup_done_event, INFINITE)
             (blocks until the loop thread's bootstrap enumeration
             completes or fails — preserves the historical contract that
             init() doesn't return until devices are enumerated)
```

### Phase 2: `event_loop_main()` (the one internal thread)

```
event_loop_main()
    ├─ CoInitializeEx(COINIT_APARTMENTTHREADED)   [RAII: CoUninitialize on exit]
    ├─ create_window()
    │    ├─ RegisterClassEx / CreateWindow(HWND_MESSAGE)
    │    └─ WM_CREATE → on_create_window() → RegisterDeviceNotification()
    │                                         → g_device_notify
    ├─ enumerate_devices()          [bootstrap - see Phase 3]
    ├─ SetEvent(g_startup_done_event)
    ├─ handles[0..2] = quit, rebuild, hotplug
    ├─ rebuild_wait_handles(...)    → handles[3..], handle_count
    │
    └─ for(;;)
         wait_result = MsgWaitForMultipleObjectsEx(
             handle_count, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE)

         WAIT_OBJECT_0        → quit: break, thread returns
         WAIT_OBJECT_0+1      → rebuild: ResetEvent, rebuild_wait_handles()
         WAIT_OBJECT_0+2      → hotplug: ResetEvent, enumerate_devices(),
                                 then rebuild_wait_handles() SYNCHRONOUSLY
                                 (see Tricky Aspect #3 below)
         (+3 .. +handle_count-1) → one buffered device's notification event
                                    signaled → process_buffered_events()
                                    for just that device
         WAIT_OBJECT_0+handle_count → window has a message (rare in
                                    practice - see Tricky Aspect #1;
                                    WM_DEVICECHANGE does NOT normally
                                    arrive here) → PeekMessage/
                                    DispatchMessage loop
         WAIT_TIMEOUT         → service polled-fallback devices
                                 (1ms cadence, only when any exist)
```

`timeout` is `1` (ms) when any polled-fallback device exists, else
`INFINITE`. A buffered device beyond the wait-slot cap (see Performance
Characteristics below) does not fall back to polling - it gets no input
events, and `rebuild_wait_handles()` logs an error identifying it.

### Phase 3: `enumerate_devices()` — bootstrap AND every hotplug event

```
enumerate_devices()
    ├─ DirectInput8Create() → g_direct_input   [lazy, once per process/init cycle]
    ├─ EnumDevices(DI8DEVCLASS_GAMECTRL, handle_device_cb, ..., DIEDFL_ATTACHEDONLY)
    │    └─ handle_device_cb(instance) [per currently-attached device]
    │         ├─ current_devices[guid] = true        [always]
    │         ├─ if guid already in device_map: return DIENUM_CONTINUE
    │         │    (see Tricky Aspect #2 - do NOT remove this check)
    │         └─ else: initialize_device(guid, name)  [genuinely new device]
    │              ├─ CreateDevice [FAILED → log + return, do NOT fall
    │              │    through into SetCooperativeLevel on a null device]
    │              ├─ SetCooperativeLevel → SetDataFormat
    │              ├─ SetProperty(DIPROP_BUFFERSIZE)   [assume buffered;
    │              │    demote to polled on DI_POLLEDDEVICE]
    │              ├─ if buffered: CreateEvent + SetEventNotification
    │              │    (BEFORE Acquire - DIERR_ACQUIRED otherwise)
    │              ├─ Acquire → GetCapabilities → EnumObjects(axis) →
    │              │    build_axis_map()
    │              ├─ [lock] write device_map/is_buffered/event_handles/
    │              │    active_guids/cache
    │              ├─ SetEvent(g_rebuild_event) if a new notification
    │              │    event was created
    │              └─ fire g_device_change_callback(info, Connected)
    │                    [unlocked]
    │              └─ [lock] is_ready[guid] = true
    │
    └─ stale removal: for guid in device_map NOT in current_devices:
         ├─ [lock] read di from cache (last-known DeviceSummary, for the
         │    Disconnected callback below)
         ├─ [lock] pop device_map/event_handles entries, remove from
         │    active_guids
         ├─ [lock] erase cache/state/is_ready/is_buffered entries too
         │    (see Tricky Aspect #5 - these used to be left behind)
         ├─ [unlocked] SetEventNotification(nullptr) → Unacquire → Release
         ├─ [unlocked] CloseHandle(event) if it had one
         └─ fire g_device_change_callback(di, Disconnected)   [unlocked]
    └─ SetEvent(g_rebuild_event) if any buffered device was removed
```

Note what does **not** happen here anymore: devices that are already in
`device_map` and still attached are **never** touched — no
Unacquire/Release/CreateDevice/Acquire churn on them. Only genuinely new
arrivals and genuinely gone departures cause any COM object lifetime
changes. See Tricky Aspect #2.

### Phase 4: Input draining

- **Buffered devices**: `process_buffered_events()` — `Poll()` →
  `GetDeviceData()` loop → `emit_joystick_input_event()` per report; on
  `DIERR_NOTBUFFERED` demotes the device to polled (clears its
  `event_handles` entry, closes the event, signals `g_rebuild_event`).
- **Polled-fallback devices**: `poll_device()` — `Poll()` →
  `GetDeviceState()` → diff against `g_data_store.state[guid]` → gather
  changed values under the lock → release the lock → fire
  `g_event_callback` for each change.
- Both index `state[guid].axis/button/hat` with a 1-based physical input
  number (e.g. button 1 lives at `button[1]`, not `button[0]`) — see
  "Per-Device State" above and Tricky Aspect #5. This was *not* true of the
  original threading-refactor pass; it was corrected in a follow-up
  post-refactor audit.

### Phase 5: `shutdown()`

```
shutdown()
    ├─ g_running.compare_exchange_strong(true, false)   [idempotency gate]
    ├─ SetEvent(g_quit_event)
    ├─ PostMessage(g_hwnd, WM_NULL, 0, 0)   [belt-and-suspenders wake]
    ├─ g_loop.thread.join()
    ├─ [lock] for every device: Unacquire → SetEventNotification(nullptr)
    │    → Release; CloseHandle every event_handles entry; clear all
    │    DeviceDataStore maps
    ├─ UnregisterDeviceNotification(g_device_notify)
    ├─ DestroyWindow(g_hwnd)
    ├─ UnregisterClass(CLS_NAME, ...)   [required - see note below]
    ├─ g_direct_input->Release()
    └─ CloseHandle all 4 control events
```

`UnregisterClass` is not optional: `create_window()` calls
`RegisterClassEx` unconditionally, which fails with
`ERROR_CLASS_ALREADY_EXISTS` on a second `init()` if the class from the
first `init()` was never unregistered — this would silently break
`init()` → `shutdown()` → `init()` cycles.

## Callback Mechanisms

### 1. Input Event Callback
```cpp
typedef void (*JoystickInputEventCallback)(JoystickInputData);
```
Fires from `emit_joystick_input_event()` (buffered path) or `poll_device()`
(polled path), always on the event loop thread, always with
`g_data_store_mutex` released.

### 2. Device Change Callback
```cpp
typedef void (*DeviceChangeCallback)(DeviceSummary, DeviceActionType);
```
Fires from `initialize_device()` (Connected) or `enumerate_devices()`'s
stale-removal loop (Disconnected), always on the event loop thread, always
unlocked.

**Threading contract**: both callbacks run on DILL's one internal thread.
Do not call back into any DILL export from within a callback — `shutdown()`
would deadlock joining the very thread that's calling it, and other
exports would deadlock or corrupt state re-entering the mutex. A slow
callback stalls the entire loop (no input draining, no hotplug processing)
until it returns.

### 3. DirectInput's own callbacks
- `handle_device_cb` (`DIENUM_Callback`, via `EnumDevices`) — see Phase 3.
- `enumerate_axis_objects` (`DIENUM_ObjectCallback`, via `EnumObjects`) —
  detects present axes and sets each one's range; untouched by the
  threading refactor, lives in `axis_mapping.cpp`.

## Tricky Aspects (read before touching threading/hotplug code)

These were each responsible for a real, reproducible bug found only
through live hardware testing — none of them showed up in the hardware-free
test suite, and each was individually plausible-looking code that only
failed under specific real-world conditions.

### 1. `WM_DEVICECHANGE` arrives via `SendMessage`, not `PostMessage`

Windows delivers device-arrival/removal notifications via `SendMessage`.
When the target window belongs to the *same thread* that's currently
blocked inside `MsgWaitForMultipleObjectsEx`, the OS invokes the WndProc
**directly and reentrantly from inside that wait call** — bypassing
`PeekMessage`/`DispatchMessage` entirely. This was confirmed empirically:
diagnostic logging placed around `TranslateMessage`/`DispatchMessage`
never fired even once across dozens of real `WM_DEVICECHANGE` deliveries.

**Consequence if you do real work in the WndProc**: `on_device_change()`
used to call `enumerate_devices()` directly — meaning mutex locks and
blocking DirectInput/COM calls ran *nested inside* the wait function on a
thread that's also supposed to just be waiting. This is a classic Win32
reentrancy hazard.

**The fix in place**: `on_device_change()` does nothing but
`SetEvent(g_hotplug_event)`. The actual `enumerate_devices()` call only
ever happens from the loop's own top-level `for(;;)` iteration
(`WAIT_OBJECT_0+2` branch), never nested inside the reentrant callback.
Keep it that way — do not move real work back into `on_device_change`,
`on_create_window`, or `window_proc`.

### 2. Never mutate other devices from inside `EnumDevices`' own callback

The original design called `initialize_device()` (full
Unacquire/Release/CreateDevice/Acquire teardown-and-rebuild) for **every**
currently-attached device on **every** hotplug notification, not just the
device that changed — `handle_device_cb` had no "already known" check.

This corrupted DirectInput's internal enumeration state: recreating other
devices' COM objects from inside `EnumDevices`' own enumeration callback,
repeatedly, produced crashes at inconsistent points (different devices,
different stages) across repeated tests — the signature of accumulating
memory corruption, not a fixed bad line. It only manifested on
hotplug-triggered scans, never on the bootstrap scan, because bootstrap
processes every device via the "new" path with no existing device to tear
down mid-enumeration.

**The fix in place**: `handle_device_cb` checks `device_map` under lock
first and returns immediately (`DIENUM_CONTINUE`) for any GUID already
present — only genuinely new devices reach `initialize_device()`. As a
consequence, `initialize_device()` no longer has (or needs) an
existing-instance/re-init branch; it can assume the GUID is not already in
`device_map`. Do not reintroduce unconditional re-initialization of
already-known devices without re-validating this very carefully against
real hardware, ideally with multiple devices attached.

### 3. The wait-handle array must never contain a closed handle

`rebuild_wait_handles()` refreshes `handles[]` from the current
`is_buffered`/`event_handles` state, but it used to only be invoked
*asynchronously* — a device topology change would `SetEvent(g_rebuild_event)`
and rely on a **separate, later** loop iteration to notice and rebuild.

That left a window: after a device's notification event was `CloseHandle`'d
(e.g. on disconnect), the very next `MsgWaitForMultipleObjectsEx` call could
still have that closed handle sitting in its array. Windows recycles handle
values, so a closed handle in a wait array isn't guaranteed to fail fast
with `ERROR_INVALID_HANDLE` — it can silently reference some unrelated
kernel object that never gets signaled, producing an indefinite hang inside
the wait call. This was confirmed via a live debugger session: the thread
was found blocked at the `MsgWaitForMultipleObjectsEx` line itself, with a
fully-clean, non-crashing `enumerate_devices()` call having just completed
right before it.

**The fix in place**: the `WAIT_OBJECT_0+2` (hotplug) branch calls
`rebuild_wait_handles()` **synchronously**, immediately after
`enumerate_devices()` returns, before falling through to the next wait
call — never relying on a later iteration to catch up. If you add another
code path that can close a handle that's part of `handles[]`, it must
rebuild the array before the next wait call too, not just signal
`g_rebuild_event` and hope.

### 4. Diagnosing a live-hardware-only bug

None of the three bugs above were reproducible without real, physically
attached DirectInput hardware, and none showed up in the automated test
suite (which is deliberately hardware-free). The approach that worked:

1. Add `logger->info(...); logger->flush();` checkpoints bracketing every
   suspect call, temporarily. `logger->flush()` matters — without it, the
   last few lines before a hang/crash may never hit disk.
2. Have the user reproduce with a debugger attached (VS Code's Call Stack
   panel lists every thread as a top-level expandable node — there is no
   separate "Threads window" the way full Visual Studio has one). DirectInput
   itself spawns background threads (`dinput8.dll thread` was observed in
   the call stack) — don't confuse those with DILL's own thread, which
   shows the module that created it (observed as `ucrtbased.dll thread`)
   with `dill.dll!event_loop_main` inside its stack.
3. Cross-reference the log's last line against the paused call stack's
   line number — a *complete* last log line with the thread parked in a
   kernel wait means a real hang; a *truncated* mid-write last log line
   means the process was killed by a crash (SEH exceptions like access
   violations are not caught by `catch(...)` under default `/EHsc`, so the
   thread can die silently with no "terminated by exception" log line at
   all).
4. Remove the temporary logging once root-caused — it is not meant to
   survive in the shipped code.

### 5. Button/hat state used a 0-based array size with 1-based writes — a real heap overflow

Found by a static, non-live-hardware code audit done *after* the threading
refactor above, prompted by the same "check every array index against real
hardware limits" instinct that caught Tricky Aspects #1-3. Unlike those
three, this bug predates the refactor and isn't threading-related at all —
plain out-of-bounds writes.

`DeviceState::button`/`hat` used to be sized to exactly the DirectInput
maximum (`button(128, false)`, `hat(4, -1)` — i.e. `count`, not `count+1`),
but every writer (`poll_device()`, `emit_joystick_input_event()`) stored at
`[i+1]` for `i` in `[0, count)` — a 1-based convention copied from how axis
already worked (`axis(9, 0)` sized for indices `1..8`, one slot wasted at
`0`). For button/hat the vector was never widened to match, so a device
reporting the maximum count wrote one element past the end of the
allocation: `button[128]` into a 128-bit `vector<bool>` (zero slack, two
full 64-bit words, no headroom), `hat[4]` into a 4-element `vector<LONG>`.
This is genuine heap corruption, not just a wrong value, and was
**concretely reachable** with hardware already in the user's own
`dill_debug.log` — a connected vJoy device reporting `Buttons=128 Hats=4`,
exactly the boundary that overflows.

Compounding it: `get_button()`/`get_hat()` read with a *plain 0-based*
index (`button[index]`, no `+1`), so even where the overflow didn't fire,
every caller of `get_button`/`get_hat` was reading one slot away from what
the writers had just written.

**The fix in place**: widened `button`/`hat` to `count+1` (129/5), matching
axis's existing pattern, and re-based `get_button`/`get_hat`'s validated
range from `[0, count)` to `[1, count]` so the readers agree with the
writers' `[i+1]` convention (see "Per-Device State" above). If you ever
touch button/hat storage again: the vector size must always be
`max_physical_count + 1` when index `0` is reserved as unused, the same
rule axis already followed correctly. Don't "fix" this by removing the
`+1` from the writers instead — the calling Python code is written
expecting 1-based button/hat indices.

### 6. `initialize_device()` didn't bail out on `CreateDevice` failure

Also found in the same post-refactor audit. `CreateDevice` failing was only
logged, not treated as fatal — execution fell through into
`device->SetCooperativeLevel(...)` and every subsequent call on a `nullptr`
device pointer, an immediate access violation. Low probability in normal
operation (`CreateDevice` failing at all is rare) but real: it's the kind
of thing that can happen if a device disappears in the gap between
`EnumDevices` reporting it and `CreateDevice` being called for it, which is
exactly the kind of race hotplug testing tends to surface. **The fix in
place**: `initialize_device()` now `return`s immediately after logging a
`CreateDevice` failure.

## API Functions (C ABI, `extern "C"` in `dill.h`)

1. **Lifecycle**
   - `init()`: idempotent; starts the event loop thread, blocks until
     bootstrap enumeration completes.
   - `shutdown()`: idempotent; joins the event loop thread and releases
     every device/window/DirectInput resource.
2. **Callback registration**
   - `set_input_event_callback(JoystickInputEventCallback cb)`
   - `set_device_change_callback(DeviceChangeCallback cb)`
3. **Device query** (safe from any thread, any time, including
   before-`init()`/after-`shutdown()` — each takes the mutex briefly and
   returns a copy)
   - `get_device_count()`, `get_device_information_by_index(size_t)`,
     `get_device_information_by_guid(GUID)`, `device_exists(GUID)`
4. **State query** (same thread-safety guarantee)
   - `get_axis(GUID, DWORD)`, `get_button(GUID, DWORD)`, `get_hat(GUID, DWORD)`
     — all three take a **1-based** index (axis 1-8, button 1-128, hat 1-4)

## Performance Characteristics

- **Buffered devices**: event-driven, woken immediately on each DirectInput
  report via `SetEventNotification` — no fixed polling interval.
- **Polled-fallback devices**: 1ms cadence, only while at least one such
  device exists (`INFINITE` wait otherwise).
- **Thread count**: 1 internal thread + caller's thread(s).
- **Wait-slot cap**: `MsgWaitForMultipleObjectsEx` requires
  `nCount < MAXIMUM_WAIT_OBJECTS` (64) — 3 control handles (quit, rebuild,
  hotplug) leaves 60 buffered-device slots. There is no polled fallback for
  devices beyond that cap: `rebuild_wait_handles()` logs an error identifying
  the device and it simply receives no input events. Given this library's
  actual usage (a handful of physical joysticks/HOTAS/pedals), hitting this
  cap is not expected in practice.

## Files Reference

- **[dill.h](src/dill.h)**: API declarations, data structures, callback
  typedefs, threading contract documentation.
- **[dill.cpp](src/dill.cpp)**: implementation of `event_loop_main` and all
  DirectInput/threading/callback logic.
- **[axis_mapping.h/.cpp](src/axis_mapping.h)**: axis detection/mapping,
  untouched by the threading refactor.
- **[example.cpp](src/example.cpp)**: input-event callback usage.
- **[example2.cpp](src/example2.cpp)**: device-change callback + state
  polling usage.
- **[tests/test_lifecycle.cpp](tests/test_lifecycle.cpp)**: hardware-free
  `init()`/`shutdown()` idempotency and handle-leak smoke tests.
