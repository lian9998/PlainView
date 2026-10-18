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
| Background | Black by default; RGBA picker (native colour dialog + alpha slider) |
| Non-images | The file's Explorer icon at 1/10 of the window width instead of a blank window |
| Settings | `%LOCALAPPDATA%\PlainView\settings.json` |
| Menu | Hidden by default; `Alt` reveals it (native menu activation), `Alt`/`Esc` hides it again |
| Caching | Session-only, screen-resolution, unlimited by default, neighbours prefetched while idle |
| Default app | Per-user registration; pick it in Settings → Default apps |

Deliberately absent, per the plan: thumbnail database, persistent image cache,
prefetching, recents, overlays, toolbar, telemetry, auto-update, logs.

No app-written file exists outside `%LOCALAPPDATA%\PlainView\`, and the preview cache
lives only in RAM (see [Caching](#caching) below).

## Becoming the default viewer

### Opening files that are not images

If a file cannot be decoded, the window shows the same icon Explorer uses for that file,
scaled to a tenth of the window width and centred on the background colour. Navigation
with the arrow keys still works, so pressing `→` moves on to the next file normally.

Windows 11 does not let an application make itself the default for a file type — the
user picks it in Settings. What the app *can* do is register itself as a proper handler
so it appears in the list:

- **From the app:** `Alt` → `File → Register as Image Viewer…`, then confirm the prompt
  to open the Default apps page.
- **From the command line:**

  ```bat
  PlainView.exe --register      :: HKCU only, no administrator rights
  PlainView.exe --unregister    :: removes every entry it created
  ```

Registration writes only under `HKEY_CURRENT_USER`:

```
HKCU\Software\Classes\PlainView.Image\...                 ProgID, icon, open command
HKCU\Software\Classes\Applications\PlainView.exe\...     "Open with" entry + SupportedTypes
HKCU\Software\PlainView\Capabilities\FileAssociations     for Settings → Default apps
HKCU\Software\RegisteredApplications                       PlainView = the above
HKCU\Software\Classes\.jpg\OpenWithProgids                (one per supported extension)
```

Covered extensions: `.jpg .jpeg .jpe .jfif .png .bmp .dib .gif .tif .tiff .ico .webp
.jxr .wdp`. HEIC/AVIF are deliberately not claimed — they need paid Store codecs, and
offering PlainView for files it usually cannot decode would be misleading.

To make it the default: **Settings → Apps → Default apps → Choose defaults by file type
→ `.jpg` → PlainView**, or **Set defaults for applications → PlainView**. Explorer's
right-click `Open with` will list it immediately after registering (the app also signals
`SHChangeNotify`, so no logoff is needed).

A plain exe that is merely *browsed to* from "Choose another app" is not a registered
handler; Windows will not accept it as a default for `.jpg`. That is why registration
exists.

## Caching

The plan originally forbade caches; the requirement changed to "right arrow then left
must be immediate, without disk use and without unbounded RAM". What is implemented:

- **In-memory only.** Nothing is ever written to disk.
- **Screen resolution.** Entries are downscaled to the window's longest edge (capped at
  4096 px), not full resolution. A 4K screen therefore costs about
  3840 × 2160 × 4 B ≈ 33 MB per cached image.
- **Budget.** Unlimited by default: browsing is allowed to fill RAM and Windows pages
  out what it needs. Set `"cacheBudgetMB"` to cap it (then one entry may never take more
  than a third of the budget, and least-recently-used entries are evicted).
- **Keyed by path + size + last-write time**, so an edited file is never served stale.
- **Idle prefetch.** Once an image is on screen and the folder list is ready, the app
  waits ~0.7 s. If you are still on that image, the previous and next files are decoded
  in the background at screen size. Any navigation cancels the pending timer and drops
  queued prefetches, so fast arrow-key scrubbing never wastes work.
- **Prefetch cannot delay navigation.** It runs on its own decode thread; the thread that
  serves the file you just asked for is never behind a background decode.
- **Full resolution is lazy.** Opening a cached file is instant; the full-resolution
  decode is only requested when you zoom past what the cached copy can resolve. That
  keeps both RAM and time-to-pixels low while browsing.

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

:: measure cold open: decode phases, first frame, peak working set
PlainView.exe --bench report.txt image1.jpg image2.png

:: file-handler registration
PlainView.exe --register
PlainView.exe --unregister
```

## Settings

`%LOCALAPPDATA%\PlainView\settings.json`, written only when you press OK in Options:

```json
{
  "zoomStep": 1.15,
  "cacheBudgetMB": 0,
  "backgroundColor": "#000000",
  "initialSizing": "fit"
}
```

- `zoomStep` — wheel multiplier, 1.01–3.00.
- `cacheBudgetMB` — session cache limit in MB; `0` (default) means unlimited.
- `backgroundColor` — `#RRGGBB` or `#AARRGGBB`. The alpha byte is composited over
  black (the window has no backdrop of its own), so it acts as a dimmer and the picker's
  preview shows the result. `File → Background Color…` opens the native colour dialog
  for RGB followed by a small popup with the alpha slider.
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
| `src/cache.cpp` | session-only preview cache (LRU, byte budget) |
| `src/register.cpp` | per-user file-handler registration |
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
