# Measurements

Windows 11 x64, MSVC 14.50 (Visual Studio 2026), Release build (`/O2 /MT`),
1200 × 800 window. Test images were stored on a local NTFS disk, not the WSL
mount: the same bytes read through `\\wsl.localhost\Ubuntu\...` (9P) decode
several times more slowly and make the numbers meaningless.

| File | Size | Notes |
| --- | --- | --- |
| `big-image.jpg` | 7008 × 4672 (32.7 MP), 27.5 MB | EXIF orientation 1, no ICC profile |
| `demo-photo.jpg` | 4000 × 2628 (10.5 MP), 5.4 MB | embedded sRGB ICC profile |

## Method

`tools/bench.ps1` copies the executable and the inputs onto the local disk, runs
`PlainView.exe --bench report.txt <file>` five times per image, and reports the
minimum, median and maximum. A bench run opens the real window, requests the
file, and records:

- `open+metadata` — decoder creation, frame metadata, EXIF and colour queries.
- `pixel decode` — the WIC copy/conversion pass that produces BGRA pixels.
- `request issued` — process-relative time the UI asked for the file.
- `first frame` — the first `EndDraw` that contains the image, measured from
  process start (the user-visible moment).
- `texture upload` — the Direct2D bitmap upload, done off the UI thread.
- `peak working set` — `GetProcessMemoryInfo` when the image became ready.

The benchmark quits by itself and writes the report. There is no logging in
normal use.

## Results (five runs each)

| Measurement | 33 MP JPEG | 10.5 MP JPEG, sRGB profile |
| --- | --- | --- |
| First frame, median | **711.4 ms** | **217.1 ms** |
| First frame, min–max | 676.0–919.1 ms | 212.1–243.4 ms |
| Pixel decode, median | 566.2 ms | 125.5 ms |
| Texture upload, median | 61.1 ms | 11.9 ms |
| Peak working set | 449 MB | 195 MB |

The spread on the 33 MP file is dominated by the 131 MB texture upload
(44.5–190.3 ms), which competes with the decode for memory bandwidth.

## What each change bought

1. **GPU initialisation was on the critical path.** Creating the D3D/D2D device
   costs roughly 300 ms cold. Requesting the decode first, then creating the
   device while the worker runs, removed it from the time-to-first-frame
   entirely.
2. **There is no preview pass.** `IWICBitmapSourceTransform` reports and produces
   a half-size image, but it cost 622 ms versus 624 ms for the full decode: the
   decoder still touches every source pixel. A preview would only delay the
   full-resolution image, so the first frame is the full image.
3. **The upload is not the bottleneck.** 131 MB of BGRA reached the GPU in
   44–190 ms while the decode took ~566 ms.
4. **An sRGB ICC transform is pure cost.** A 10.5 MP photo with the usual
   embedded "sRGB" profile decoded in 783 ms with the WIC transform and 482 ms
   with the profile stripped. `src/decode.cpp` now reads the ICC description tag
   and skips the transform when the profile is sRGB; the same file measures
   125 ms decode and 217 ms first frame on the local disk, matching a copy with
   the profile removed (134 ms / 218 ms). Adobe RGB, Display P3 and
   camera-specific profiles still go through the transform.

## Browser baseline

`tests/browser-bench.mjs` drives Chromium over the DevTools protocol using Node's
built-in WebSocket (no npm dependencies), loads the image as a `file://` URL in a
1200 × 800 window, and reads `largest-contentful-paint` and navigation timing.

```bash
node tests/browser-bench.mjs \
  ~/.cache/ms-playwright/chromium-1228/chrome-linux64/chrome \
  file:///mnt/c/Users/lianzheyuan/source/repos/big-image.jpg 3
```

Three runs, Chromium 1228 headless (software rendering, no GPU): median LCP
656 ms, median load event 538.8 ms.

The browser paints at ~656 ms after navigation inside an already running
process; PlainView shows the 33 MP image at ~711 ms **including process start,
window creation and GPU device creation**. At this file size the two are in the
same class; for ordinary photos the viewer is far ahead (217 ms for 10.5 MP).

Caveat: Chromium ran on the Linux side of WSL2 with software rasterisation and
read the file through `/mnt/c`, so this is indicative rather than a controlled
same-OS comparison.

## Session cache and prefetch

- The cache is RAM-only and bounded at 256 MB by default. Entries are
  screen-sized: the window's longest edge, capped at 4096 px, so a 4K display
  costs about 33 MB per entry. A budget of 0 means "no limit".
- The first open of a file: one full decode (566 ms for the 33 MP test image)
  plus a box-filter downscale to fill the cache.
- Returning to a cached file: no decode, just the upload (a few milliseconds for
  a screen-sized entry).
- Full resolution is deferred when the cached copy was shown first: it is
  requested once the zoom level exceeds what the cached copy can resolve.
- Prefetch runs on two dedicated threads (one per direction) after ~0.7 s without
  navigation, `prefetchDepth` images each way (0–20, default 3).

## DPI

Dialogs are created by the dialog manager at the system DPI. The Options dialog
is then rescaled by `monitorDpi / systemDpi`, not by `dpi / 96`: on a 200% system
the old code produced a 1768 × 1662 dialog instead of 884 × 831.

## Not measured here

- 4K display at 100%, 150% and 200% scaling (this machine runs a 200% system).
- Holding an arrow key through a folder of tens of thousands of images, and the
  folder scan time for such a folder.
- Images above the 16384 px texture limit, and PNG/WebP/HEIC decode times.
- A filesystem-write audit while opening, navigating and closing (expected: only
  `%LOCALAPPDATA%\PlainView\settings.json`, and only after OK in Options).
