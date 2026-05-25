# tasdll

A Windows DLL that gives any process (including most Unity games) a TAS-style
**speedhack** plus a **screen recorder** that compensates for the speed
multiplier — so a clip captured at 0.5x is sped back up to look 1x, and a clip
captured at 2x is slowed back down. The DLL is designed to be injected with any
standard injector (Extreme Injector, Process Hacker, Cheat Engine's auto-inject,
xenos, etc.).

## What's in the box

- **Speedhack** — inline hooks on the Windows time APIs Unity (and most native
  games) read every frame:
  - `QueryPerformanceCounter`
  - `GetTickCount` / `GetTickCount64`
  - `timeGetTime`
  - `GetSystemTimeAsFileTime`
  - `GetSystemTimePreciseAsFileTime`

  Each hook re-anchors a fake clock on every speed change, so changing the
  multiplier does not produce time discontinuities. MinHook is used for the
  trampolines.

- **Speed-compensated recorder** — launches `ffmpeg` as a child process using
  `gdigrab` for capture. The current speedhack multiplier is sampled at
  start-of-recording and passed in as the `setpts=PTS*<speed>` filter, so the
  resulting video shows game-time at 1x regardless of how slow/fast the game
  was actually running while recording. Optional `dshow` audio capture with a
  matching `atempo` chain.

- **ImGui GUI** — a separate DX11 + Win32 window owned by the DLL (no rendering
  hook into the game, so it works the same on D3D9/11/12, OpenGL, and Vulkan
  titles). Sliders for speed, presets, capture settings, start/stop.

- **Console CLI fallback** — `AllocConsole`-spawned console with text commands
  (`speed`, `rec start`, `rec stop`, `status`, ...). Useful when the GUI window
  is in the way or when scripting.

- **Hotkeys** (global):
  - `F8` — show/hide GUI window
  - `F9` — start/stop recording
  - `Ctrl+[` — halve speed
  - `Ctrl+]` — double speed
  - `Ctrl+\` — reset to 1.0x

## Build (Windows)

Requires CMake 3.20+, Visual Studio 2022 (or any MSVC v143 toolchain), Git, and
`ffmpeg` somewhere on `PATH` (only at run-time; not needed for the build).

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

The DLL ends up at `build/Release/tasdll.dll`. Build the x86 version
(`-A Win32`) if your target game is 32-bit — the DLL bitness must match the
target process.

The CMake config uses `FetchContent` to pull MinHook (`v1.3.3`) and Dear ImGui
(`v1.91.5`). No manual dependency setup.

## Use

1. Start the game.
2. Inject `tasdll.dll` with your injector of choice (Extreme Injector works;
   any standard `LoadLibrary`-based injector works).
3. A `tasdll` window appears, plus a console. Pick a speed multiplier; hit
   "Start Recording" or `F9`.
4. Recordings land in `%USERPROFILE%\Videos\tasdll\` by default.

### Unity-specific notes

Unity's `Time.deltaTime` derives from `QueryPerformanceCounter` (on Windows
desktop builds), which is hooked. Most Unity games therefore respond to the
multiplier with no per-game tuning. A handful of titles use
`Time.realtimeSinceStartup` for animations that intentionally ignore game-time;
those will not slow down — that's expected behavior, not a tasdll bug.

### Recording caveats

- `gdigrab` cannot capture content drawn by an exclusive-fullscreen DX swap
  chain. Use **windowed** or **borderless windowed** mode. Almost all Unity
  games default to borderless these days; for the rest, set windowed in the
  graphics options.
- The recorder snapshots the speed at start. If you change speed mid-recording
  the video won't be perfectly 1x — stop and restart to re-snapshot.
- Audio capture uses the optional DirectShow `virtual-audio-capturer` device
  (from screen-capture-recorder). If it isn't installed, leave audio off.

## CLI reference

```
speed <x>        set speed multiplier (e.g. 0.5, 2.0)
reset            speed back to 1.0
rec start        start recording (snapshots current speed)
rec stop         stop recording
title <name>     set capture window title
title fg         use current foreground window title
desktop on|off   capture whole desktop instead of a window
audio on|off     toggle dshow audio capture
fps <n>          set capture framerate
out <dir>        set output directory
show|hide        toggle GUI window
status           show current settings
help             this list
quit             exit CLI loop
```

## Layout

```
CMakeLists.txt
src/
  dllmain.cpp      DllMain + hotkeys + lifecycle
  speedhack.{h,cpp}  MinHook + fake clocks
  recorder.{h,cpp}   ffmpeg subprocess driver
  gui.{h,cpp}        Win32 + D3D11 + ImGui window
  console_cli.{h,cpp}  AllocConsole + command parser
  state.{h,cpp}      shared atomic config
  log.{h,cpp}        thread-safe file logger
```

## Logs

A log file is written next to the DLL (`tasdll.log`). All log lines are also
emitted via `OutputDebugString` and visible in DebugView/WinDbg.

## Disclaimer

For offline / single-player use. Don't ship this near a multiplayer anticheat
— hooking `QueryPerformanceCounter` is exactly the kind of thing EAC / BE /
Vanguard will flag, and you will get banned. Treat this as a TAS/dev tool.
