# PlainView

A minimal native Windows image viewer: Win32 window, WIC decoding on a worker thread,
Direct2D rendering. One `.exe`, no bundled runtime and no third-party libraries.

The point of the design is latency: the image is opened and decoded immediately, the
window is otherwise empty, and nothing (folder listings, caches, thumbnails) is done
until the first picture is already on screen.

## Features

| Area | Behaviour |
| --- | --- |
| Opening | Command-line path, `File → Open…` (`Ctrl+O`), drag & drop |
| Decoding | WIC on a worker thread; the UI thread never touches a file |
| Rendering | Direct2D HWND render target (GPU, with the platform's software fallback) |
| Fit | Fits the window on open, keeps aspect ratio, re-fits on resize until you zoom |
| Zoom | Mouse wheel around the cursor, `1.15×` per notch (configurable) |
| Pan | Left-button drag |
| Navigation | `←` / `→` within the current folder, natural filename order, wraps around |
| EXIF | All eight orientation values applied during decode |
| Colour | Embedded ICC profiles transformed to sRGB |
| Huge images | Images beyond the GPU texture limit use an overview plus on-demand tiles |
| Background | Black by default, configurable |
| Settings | `%LOCALAPPDATA%\PlainView\settings.json` |
| Menu | `Alt` reveals `File → Open / Options / Exit` |

Deliberately absent, per the plan: thumbnail database, persistent image cache,
prefetching, recents, overlays, toolbar, telemetry, auto-update, logs.

No app-written file exists outside `%LOCALAPPDATA%\PlainView\`.

## Build

Visual Studio 2026 with the *Desktop development with C++* workload and a Windows 10/11 SDK.

```bat
build.bat            :: release, /O2, output build\PlainView.exe
build.bat debug      :: debug build in build\debug
```

Or with CMake (Visual Studio generator, or any recent MSVC):

```bat
cmake -B build-cmake -S . -G "Visual Studio 18 2026"
cmake --build build-cmake --config Release
```

From WSL, `tools/msvc.sh` runs a Windows command inside the MSVC environment with the
current directory mapped to a temporary drive letter:

```bash
./tools/msvc.sh -C . 'build.bat'
```

The build is warning-clean at `/W4 /WX /permissive-`.

## Usage

```bat
PlainView.exe                     :: empty black window, opens via Ctrl+O or drag & drop
PlainView.exe "C:\photos\a.jpg"   :: open immediately
```

Development aids (not needed for normal use):

```bat
:: render the real pipeline offscreen to a PNG (software Direct2D target)
PlainView.exe --capture out.png image.jpg --size 1600x1000 [--zoom 2.0] [--tile-threshold 1024]

:: measure cold open: decode phases, first frame, full detail, peak working set
PlainView.exe --bench report.txt image1.jpg image2.png
```

## Settings

`%LOCALAPPDATA%\PlainView\settings.json`, written only when you press OK in Options:

```json
{
  "zoomStep": 1.15,
  "backgroundColor": "#000000",
  "initialSizing": "fit"
}
```

- `zoomStep` — wheel multiplier, 1.01–3.00.
- `backgroundColor` — `#RRGGBB` or `#AARRGGBB`.
- `initialSizing` — `fit` or `actual` (100 %).

Missing or malformed members keep their defaults; an unreadable file is ignored.

## Layout

| File | Responsibility |
| --- | --- |
| `src/main.cpp` | entry point, window procedure, menus, bench and capture modes |
| `src/viewer.cpp` | view state (zoom/pan), Direct2D resources, tiled drawing |
| `src/decode.cpp` | WIC session, EXIF orientation, colour management, worker thread |
| `src/folder.cpp` | background directory listing, natural sort |
| `src/settings.cpp` | flat `settings.json` reader/writer |
| `src/options.cpp` | Options dialog (DPI-scaled by hand) |
| `src/app.rc` | menu, accelerators, dialog, manifest |
| `tests/` | orientation fixtures, classification and browser-benchmark helpers |

Threading model: one decode thread with a latest-wins queue, one folder-scan thread,
and a generation counter. Every result carries its generation and is dropped by the UI
if a newer file has been requested; the worker notices the same counter between decode
stages and abandons obsolete work.

## Performance

Measured on this machine, 7008 × 4672 JPEG (27.5 MB, 32.7 MP), window 1200 × 800.
Full details and methodology in [docs/measurements.md](docs/measurements.md).

| Measurement | PlainView | Chromium 1228 (same file) |
| --- | --- | --- |
| First visible image | **698 ms** after process start¹ | 656 ms median LCP |
| Full-resolution detail | 1343 ms | — |
| Decode worker (open + decode) | 25 ms + 624 ms | — |
| Peak working set | 507 MB | — |

¹ measured on the build before the preview threshold was raised to 64 MP (last
executable build, see below).

Two changes came out of measuring rather than guessing:

1. Starting the decode **before** the Direct2D device is created overlaps ~300 ms of
   GPU initialisation with decoding (1182 ms → ~700 ms to first frame).
2. A preview pass is *not* worthwhile for ordinary photos: WIC's scaled JPEG decode cost
   the same as the full decode (622 ms for half size vs 624 ms full), so the extra pass
   only delayed full detail. It now happens only above 64 MP, where a full decode would
   not meet the one-second target on its own.

## Verification status

Verified by running earlier builds of this tree:

- 7008 × 4672 JPEG renders correctly at fit, and the tiled path (forced with
  `--tile-threshold 1024`) produces a sharp 1:1 crop.
- All eight EXIF orientations produce the correct quadrant layout, checked by sampling
  corner colours (`tests/data`, `tests/sample-corners.ps1`). This caught a real bug:
  orientations 5 and 7 were swapped because WIC applies the flip before the rotation.
- `--bench` numbers above; `--capture` renders through the same draw code as the window.

**Not verified at runtime:** the final commit. Windows *Smart App Control* on this
machine (enforcement mode) began blocking newly built unsigned executables, so later
builds could not be launched. The tree compiles cleanly, but the GUI paths that were
never exercised — menu, Options dialog, file picker, drag & drop, arrow navigation,
`settings.json` round-trip, per-monitor DPI transitions — should be smoke-tested on
first run. Disable Smart App Control (one-way) or sign the binary to run local builds.

## Known limitations

- Single image at a time by design; animated formats show frame 1.
- Decoding uses Windows codecs only, so WebP/HEIC/AVIF depend on the codecs installed.
- Downscaling uses Direct2D linear filtering; a mip-mapped pipeline would look slightly
  better at strong reductions.
- Tiles are re-decoded as you pan away and back (no persistent cache, by design).
