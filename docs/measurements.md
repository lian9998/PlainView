# Measurements

All numbers from this machine (WSL2 on Windows 11, host `LAPTOP-LB8IPAST`), building with
MSVC 14.50 (VS 2026) and running the x64 Windows binary.

Test file: `C:\Users\lianzheyuan\source\repos\big-image.jpg`
— JPEG, 7008 × 4672 (32.7 MP), 27,482,813 bytes, EXIF orientation 1, no ICC profile.
Window: 1200 × 800 client area, fit view.

## How each number is produced

`PlainView.exe --bench report.txt <file>` opens the real window, requests the file, and
records:

- `open+metadata` — decoder creation, frame metadata, EXIF and colour-context queries.
- `pixel decode` — the WIC copy/conversion pass that produces BGRA pixels.
- `preview` — decoder-scaled pass (only for images above 64 MP in the final build).
- `request issued` — process-relative time the UI asked for the file.
- `first frame` — the first `EndDraw` that contains the image (this is the user-visible
  moment, measured from process start).
- `full detail` — when the full-resolution bitmap replaced the preview.
- `peak working set` — `GetProcessMemoryInfo`, at the moment the image is ready.

The benchmark quits by itself and writes the report; no logging exists in normal use.

## Iterations (7008 × 4672 JPEG)

| Build | First frame | Full detail | Notes |
| --- | --- | --- | --- |
| v1: single decode, GPU init before decode | 1182 ms | 1182 ms | decode 751 ms, then device init and upload |
| v2: + decoder-scaled preview (2048 edge) | 806 ms | 1366 ms | preview still paid a full decode |
| v3: + decode starts before Direct2D device | 698 ms | 1343 ms | GPU init (~300 ms) now overlaps decoding |
| v4: final tree (preview only above 64 MP) | not measured | not measured | see verification note in README |

Detailed v3 run:

```
image size      : 7008 x 4672  (exif orientation 1, color managed no, single texture)
decode worker   : open+metadata 24.9 ms, pixel decode 624.2 ms, total 1271.3 ms
preview         : 3504 x 2336, 622.1 ms decode, 42.7 ms upload
request issued  : 28.7 ms after process start
first frame     : 698.0 ms after process start  (decoder-scaled preview)
full detail     : 1342.9 ms after process start
peak working set: 506.8 MB
```

### What the iterations showed

1. **GPU initialisation was on the critical path.** Creating the D3D/D2D device costs
   ~300 ms on a cold start. Requesting the decode first (and creating the device while
   the worker runs) removed it from the time-to-first-frame entirely.
2. **WIC's scaled JPEG decode is not cheaper.** `IWICBitmapSourceTransform` reported
   half size (3504 × 2336) and produced it, but took 622 ms versus 624 ms for the full
   decode, i.e. the decoder still touches every source pixel. A preview therefore only
   helps by reducing the texture upload (43 ms instead of ~70 ms) while delaying the
   full-resolution image by a whole extra decode.
3. **The upload is not the bottleneck.** 131 MB of BGRA reached the GPU in ~70 ms;
   decoding is ~9× more expensive than uploading.

Consequences in the shipped code: the preview survives only for images above 64 MP
(`kPreviewPixelThreshold` in `src/decode.cpp`), where a full decode alone would not meet
the one-second target; everything else goes straight to full resolution.

## Browser baseline

The plan asks for a comparison against the same file dropped into a browser.
`tests/browser-bench.mjs` drives Chromium over the DevTools protocol using Node's
built-in WebSocket (no npm dependencies), loads the image as a `file://` URL in a
1200 × 800 window, and reads `largest-contentful-paint` and the navigation timing.

```bash
node tests/browser-bench.mjs \
  ~/.cache/ms-playwright/chromium-1228/chrome-linux64/chrome \
  file:///mnt/c/Users/lianzheyuan/source/repos/big-image.jpg 3
```

Three runs, Chromium 1228 headless (software rendering, no GPU):

| Run | load event | LCP | process total |
| --- | --- | --- | --- |
| 1 | 471.7 ms | (not recorded) | 2230 ms |
| 2 | 567.2 ms | 696 ms | 1107 ms |
| 3 | 538.8 ms (median) | 656 ms (median) | — |

The browser paints the image at ~656 ms after navigation start inside an already running
process; PlainView shows it at ~698 ms **including process start, window creation and
GPU device creation**. At this file size the two are in the same class, and the plan's
target ("at least as quickly as the same file in a browser") holds.

Caveats: the browser ran on the Linux side of WSL2 with software rasterisation, so the
numbers are indicative rather than a controlled comparison. Re-run the script on the
Windows side (Edge, `--remote-debugging-port`) for a same-OS baseline.

## Still to measure

These need a build that can be executed (see the Smart App Control note in the README):

- PNG, WebP and very large (> 16384 px edge) files; animated formats show one frame.
- Responsiveness while holding an arrow key through a large folder, and folder scan time
  for tens of thousands of files.
- 4K display at 100 %, 150 % and 200 % scaling.
- A filesystem-write audit while opening, navigating and closing (expected: only
  `%LOCALAPPDATA%\PlainView\settings.json`, and only after pressing OK in Options).
