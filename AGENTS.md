# AGENTS.md — `dill/`

Read this file fully before touching anything in this repo.

## What This Repo Is

DILL (DirectInput Listener Library) is a Windows-only C++17 library that
wraps DirectInput 8 joystick enumeration/polling behind a C ABI, meant to be
consumed from other languages (e.g. Python via ctypes) as well as C++.

**Owns:** joystick/HID device enumeration, axis/button/hat state polling,
and the C-ABI callback API (`DeviceSummary`, `JoystickInputData`, etc.) in
`src/dill.h` / `src/dill.cpp`.
**Does not own:** anything cross-platform — this is a Win32/DirectInput-only
project, not an abstraction over multiple input backends.

## Toolchain

- **CMake 3.20** (hard floor — do not use newer CMake features)
- **MSVC**, C++17 standard (`CMAKE_CXX_STANDARD 17` in `CMakeLists.txt`)
- `cmake` may not be on `PATH` in this environment. Use the full path to the
  VS-bundled copy if it exists:
  ```
  C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
  ```

## Build

From the repo's root:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Debug
```

This reconfigures automatically if `CMakeLists.txt` changed, then builds all
targets. Output binaries land in `bin\Debug\`:

| Binary | Target | Purpose |
|---|---|---|
| `dill.dll` | `dill` | The library itself (shared lib, links `dinput8` + `dxguid`) |
| `example.exe` | `example` | Minimal usage example |
| `example2.exe` | `example2` | Usage example that also prints device/axis info |
| `dill_tests.exe` | `dill_tests` | Catch2 unit tests for `src/axis_mapping.*` and the `init()`/`shutdown()` lifecycle (hardware-free) |

To build a single target, append `--target <name>`, e.g.:
```powershell
& "...\cmake.exe" --build build --config Debug --target dill_tests
```

## Running

```powershell
.\bin\Debug\dill_tests.exe
```
Runs the Catch2 unit tests against the pure, hardware-independent
axis-mapping logic in `src/axis_mapping.cpp` (`tests/test_axis_mapping.cpp`)
and the `init()`/`shutdown()` lifecycle (`tests/test_lifecycle.cpp`). **No
joystick hardware required** — this is the fast, deterministic way to verify
changes to axis detection/mapping logic or to the event loop/shutdown
lifecycle.

```powershell
.\bin\Debug\example.exe
.\bin\Debug\example2.exe
```
Both require a real (or virtual, e.g. vJoy) joystick device attached to
produce meaningful output, and both run an infinite polling loop — run them
with a timeout (e.g. `timeout 4 ./example2.exe` from a bash-like shell) when
using them for a quick manual smoke test rather than interactive use.
`example2.exe` has `set_device_change_callback` commented out in `main()` by
default; uncomment it (and rebuild the `example2` target) to print each
connected device's `axis_count`/`axis_map`, which is the quickest way to
manually verify axis detection against real hardware. Revert that edit
afterward — it's a debugging aid, not the intended default behavior.

## Verification

Before considering a change to axis detection/mapping, the event loop, or
the `init()`/`shutdown()` lifecycle complete:

```powershell
& "...\cmake.exe" --build build --config Debug
.\bin\Debug\dill_tests.exe
```

Both must succeed with zero errors and all assertions passing. If the change
affects real device behavior (not just the pure `axis_mapping.cpp` logic or
lifecycle bookkeeping), also do a manual spot-check with `example2.exe` as
described above.

## Maintainer Notes

- DILL runs a single internal thread (`event_loop_main`, started by
  `init()`), driven by `MsgWaitForMultipleObjectsEx` waiting on per-device
  DirectInput notification events, a quit/rebuild control event pair, and
  the hidden message window's queue (`WM_DEVICECHANGE` hotplug). Devices
  that can't use buffered notification fall back to being polled on a
  timeout. `g_data_store` is guarded by a single `std::mutex`; callbacks are
  always fired with the lock released. See the threading contract in
  `src/dill.h` before touching any of `event_loop_main`, `initialize_device`,
  `enumerate_devices`, `init()`, or `shutdown()`.
