# Platforms: Linux, macOS, Windows (binding)

Vyrn builds natively on each system: compile it on the machine that will run it (no cross-compiling).
Linux (the Raspberry Pi 5 with labwc/Wayland) stays the reference and must keep passing every test.

| | Linux (Wayland, labwc) | macOS 13+ (Apple Silicon) | Windows 10/11 (x64) |
|---|---|---|---|
| Compiler | GCC 12+ / Clang | Apple Clang (Xcode CLT) | MSVC 2022 (`/utf-8 /permissive- /Zc:__cplusplus /EHsc`) |
| Deps | apt (install.sh) | Homebrew (install-mac.sh) | vcpkg or Qt installer + libtorch zip (install-windows.ps1) |
| libtorch | Debian libtorch-dev | pytorch.org arm64 zip or `brew install pytorch` | pytorch.org CPU zip (Release) |
| Python | /usr/bin/python3 | python3 ≥ 3.10 on PATH (Homebrew) | `py -3` / python.exe ≥ 3.10 |
| Worker transport | socketpair, fd 3 (`VYRN_FD=3`) | same as Linux | TCP 127.0.0.1 + token (`VYRN_ADDR`, `VYRN_TOKEN`) |
| Interrupt stuck user code | SIGINT to the worker pid | SIGINT | control message `I` |
| Freeze (keys up, block input) | SIGUSR1 | SIGUSR1 | control message `F` |
| Kill the worker tree | SIGKILL to the process group | same | `TerminateJobObject` (job with KILL_ON_JOB_CLOSE) |
| Worker notices the app died | POLLRDHUP on fd 3 | kqueue `EVFILT_PROC NOTE_EXIT` on the parent pid + socket EOF | EOF on the control connection |
| Keys (real games) | uinput via python-evdev | Quartz `CGEventPost` via ctypes (needs Accessibility permission for Vyrn) | `SendInput` via ctypes, scan codes |
| Keys released if the worker is killed | kernel (uinput fd closes) | **the app** sends key-ups for every allowed key (`platform::release_all_keys`) | **the app**, same |
| Screen capture | grim | Pillow `ImageGrab` (needs Screen Recording permission) | Pillow `ImageGrab` |
| Focus guard | `vyrn-focus` helper (wlr-foreign-toplevel) | ctypes: `CGWindowListCopyWindowInfo` (on screen, no desktop elements): the first layer-0 window's owner pid + name, polled at 20 Hz; locked on pid + name; anything unreadable = not focused. NSWorkspace/NSRunningApplication only add the bundle id for display | ctypes: `GetForegroundWindow` (hwnd, title, exe name), 20 Hz |
| Panic keys | labwc keybinds → `vyrn-panic` | the app's global hotkey (Carbon `RegisterEventHotKey`) | the app's global hotkey (`RegisterHotKey`) |
| Pick area / Snapshot | slurp + grim | Qt overlay (`QScreen::grabWindow`) | Qt overlay |
| Runtime dir (pid files, panic marker) | `$XDG_RUNTIME_DIR/vyrn` | `$TMPDIR/vyrn` | `%LOCALAPPDATA%\Vyrn\run` |
| Data dir (after `$VYRN_DATA`) | `<exe>/../share/vyrn` | `<exe>/../Resources/vyrn` (inside Vyrn.app), then `<exe>/../share/vyrn` | `<exe dir>/share/vyrn`, then `<exe>/../share/vyrn` |
| Projects | `~/Vyrn` | `~/Vyrn` | `%USERPROFILE%\Vyrn` |
| Install | `~/.local` + menu entry + MIME | `~/Applications/Vyrn.app` (macdeployqt) | `%LOCALAPPDATA%\Programs\Vyrn` (windeployqt) + Start menu shortcut |

Unchanged everywhere: the frame format (protocol.md §2), every message, the sharing formats (package.md), the MCP
server, Vyrn Link v1 (docs/user/vyrn-link.md). CPU only (no CUDA/MPS) in this version.

## 1. Windows worker transport and control channel

1. The app listens on `127.0.0.1:0` (one ephemeral port) and makes a token: 32 lowercase hex characters from a
   cryptographic RNG (`BCryptGenRandom`).
2. It starts the worker with `VYRN_ADDR=127.0.0.1:<port>` and `VYRN_TOKEN=<token>` (no `VYRN_FD`), inside a job object
   with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, stdout/stderr as pipes, `CREATE_NO_WINDOW`.
3. The worker opens **two** connections, each starting with one ASCII line:
   - `<token> main\n`: then the normal frames (protocol.md), exactly like fd 3.
   - `<token> ctl\n`: the control connection. The app sends single bytes: `I` interrupt (the worker's watchdog thread
     calls `_thread.interrupt_main()`, so stuck user code raises KeyboardInterrupt at its line, like SIGINT), `F`
     freeze (release all keys and block input until `resume`, like SIGUSR1). EOF on it = the app died: release keys,
     exit at once.
4. The app accepts exactly these two connections (wrong token or a third connection → closed), within the hello
   timeout, then closes the listener. A worker that can't connect exits with status 2.

POSIX keeps fd 3 + signals. Code must select the transport by platform, not by guessing.

## 2. Keys on macOS and Windows

- Same allow-list and effect grammar (protocol.md §4.3). Each backend has its own code table for every allowed key
  (macOS `kVK_*` virtual key codes; Windows set-1 scan codes with `KEYEVENTF_SCANCODE`, extended flag for arrows etc.,
  so games that read DirectInput see them).
- Synthetic keys don't come back up when the process dies, so: the worker releases on every path it can (as today),
  and the **app** calls `platform::release_all_keys()` (C++: CoreGraphics on macOS, `SendInput` on Windows) after a
  worker that used keys is killed, frozen without answer, or on panic. On Linux that function does nothing.
- macOS needs Vyrn in System Settings → Privacy & Security → Accessibility (keys) and Screen Recording (screen). The
  SetupDialog checks both (`AXIsProcessTrusted`, `CGPreflightScreenCaptureAccess`) and opens the right pane.

## 3. Rules for portable code

- Platform code lives in `core/src/platform/` (C++, one header per concern, `*_posix.cpp` / `*_linux.cpp` /
  `*_mac.cpp` / `*_win.cpp`) and `python/vyrn/_platform/` (Python backends). Nothing else uses `#ifdef _WIN32` /
  `__APPLE__` / `__linux__` except trivial includes; Python checks `sys.platform` only in the backend chooser.
- Use `std::filesystem` with UTF-8 (`u8path`/`path::u8string`) for paths; on Windows, wide APIs (`...W`).
- Atomic replace: POSIX `rename`; Windows `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.
  fsync: POSIX `fsync`; Windows `FlushFileBuffers`. Exclusive create: `O_CREAT|O_EXCL` / `CREATE_NEW`.
- Process liveness + identity for locks and pid files: Linux `/proc/<pid>/stat` start time + boot id; macOS
  `proc_pidinfo(PROC_PIDTBSDINFO)` start time + `sysctl kern.boottime`; Windows `OpenProcess` + `GetProcessTimes` +
  boot time from `GetTickCount64`.
- The OMP passive re-exec (`ensure_passive_openmp`) stays on Linux and macOS (`_NSGetExecutablePath`); on Windows set
  `OMP_WAIT_POLICY`/`KMP_BLOCKTIME` only for child processes and accept the default in-process.
- Ctrl+C: POSIX `sigaction` (as today); Windows `SetConsoleCtrlHandler` (CTRL_C/CTRL_BREAK/CTRL_CLOSE → the same
  graceful stop, second → exit 130).
- Sockets (Vyrn Link): a tiny shim (`platform/net.h`) over BSD sockets / Winsock (`WSAStartup`, `closesocket`,
  `WSAPoll`, non-blocking via `ioctlsocket`).
- Build: Linux-only targets (`vyrn-focus`, the panic script, MIME/desktop files) are skipped elsewhere; warnings
  flags per compiler; `FindQScintilla` also searches Homebrew (`/opt/homebrew`, `/usr/local`) and vcpkg layouts;
  `Torch_DIR` comes from the user (install scripts set it).
- Tests: POSIX-only tests are skipped on Windows (`GTEST_SKIP()` with a reason), Linux-only ones elsewhere; nothing
  may silently pass. Fake backends (record keys, fake grim) work everywhere.

## 4. What can't be verified here

The Pi can only build and test Linux. macOS/Windows code is written against the documented APIs and checked by
`.github/workflows/ci.yml` (Linux, macOS 14 arm64, Windows 2022: build + unit tests) once the repo is on GitHub, or by
the person compiling on their machine. Expect a first round of compile fixes there.
