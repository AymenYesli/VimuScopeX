# VimuScope X — Linux (Arch) build

A Qt 6 frontend for Vimu MSO USB scopes, built on the vendor's public
`vmmultmso` SDK. No part of `VmMso.exe` is used or modified.

## The library you want

```
VmMsoSDK-master/SharedLibrary/Ubuntu22.04/X64/Release/libvmmultmso.so
```

The SDK ships aarch64, armhf, armel and Raspberry Pi builds under the same
file name. On x86-64 they are all wrong, and CMake filters them out.
`Ubuntu20.04/X64/Release` also works; it is just built against an older glibc.

Two things live beside it and matter:

- `libvmsignal.so` — `libvmmultmso.so` depends on it.
- `DllTest`, `DllTest-Polling`, `DLLMultDev-Callback`, `DLLMultDev-Stream` —
  prebuilt x86-64 demo binaries. Run one before building anything (step 3).

## 1. Dependencies

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base libusb fftw
```

## 2. Check the library before trusting it

```bash
cd /path/to/VmMsoSDK-master/SharedLibrary/Ubuntu22.04/X64/Release
ls -l libvmmultmso.so libvmsignal.so
file libvmmultmso.so.1.24
readelf -d libvmmultmso.so.1.24 | grep -E 'SONAME|NEEDED|RUNPATH|RPATH'
```

- `ls -l` should show `libvmmultmso.so -> libvmmultmso.so.1.24`. Some zip
  extractions turn symlinks into small text files. If `file` reports "ASCII
  text" instead of "ELF 64-bit LSB shared object", rebuild the links:
  ```bash
  ln -sf libvmmultmso.so.1.24 libvmmultmso.so
  ln -sf libvmsignal.so.1.0   libvmsignal.so
  ```
- Note the `SONAME`. If it is `libvmmultmso.so.1` and no such file exists,
  add that link too:
  `ln -sf libvmmultmso.so.1.24 libvmmultmso.so.1`. Same for `libvmsignal`.

Then:

```bash
LD_LIBRARY_PATH=. ldd libvmmultmso.so.1.24
```

Anything `not found` has to be installed. `libusb-1.0.so.0` comes from
`libusb`; if it asks for `libusb-0.1.so.4`, install `libusb-compat`.

## 3. USB permissions, then prove the stack works

Do not run anything with `sudo`. Install the udev rule:

```bash
lsusb                              # note the scope's ID xxxx:yyyy
$EDITOR scripts/99-vimu.rules      # replace VVVV and PPPP
sudo install -m 644 scripts/99-vimu.rules /etc/udev/rules.d/99-vimu.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Unplug and replug the scope, then run the vendor's own demo:

```bash
cd /path/to/VmMsoSDK-master/SharedLibrary/Ubuntu22.04/X64/Release
LD_LIBRARY_PATH=. ./DLLMultDev-Callback
```

If that finds the device, the driver, permissions and USB path are all good,
and anything left is our code. If it does not, fix that first. Building a Qt
app on top of a broken device path only adds variables.

The Python scripts in the same folder (`vimu_scope.py`, `vimu_capture.py`)
are a second reference implementation, and `vimu_driver.py` holds the ctypes
bindings, including the capture-channel mask constants.

## 4. Build

```bash
cmake -S . -B build -G Ninja -DVIMU_SDK_DIR=/path/to/VmMsoSDK-master
cmake --build build
./build/VimuScopeX
```

CMake prints the library it chose. Check that the line says `Ubuntu22.04/X64`.
To force a specific file:

```bash
cmake -S . -B build -G Ninja \
      -DVIMU_SDK_LIB=/path/to/SharedLibrary/Ubuntu22.04/X64/Release/libvmmultmso.so
```

The library directory is baked in as `DT_RPATH` rather than `DT_RUNPATH`. A
RUNPATH is not consulted when the loader resolves `libvmsignal` on behalf of
`libvmmultmso`, which is why `-Wl,--disable-new-dtags` is in the build. No
`LD_LIBRARY_PATH` is needed at runtime. Verify with:

```bash
readelf -d build/VimuScopeX | grep -E 'RPATH|RUNPATH'
ldd build/VimuScopeX | grep vm
```

## Interface

Modelled on PicoScope 7: a card strip along the top (Run, Scope, Trigger), a
rail of channel cards down the left, a 10-button tool grid below the channel
cards (Math, Measure, Rulers, Decoding, Annotations, Settings, Instruments,
Default, Zoom, References), and a detail panel that opens between the rail
and the plot when a card or grid button is clicked. Clicking the same one again closes the panel.
The -/+ steppers on each card adjust that card's primary value without
opening anything: time/div on the scope card, threshold on the trigger card,
volts/div on a channel card.

Three acquisition modes, selected by a segmented control on the Scope card:
**Capture** (triggered block snapshots), **Live** (continuous, no trigger,
rolling display — this app's equivalent of a bench scope's roll mode, since
the hardware's own roll mode is unsupported, see below), and **Record**
(stream to disk at a fixed size, then navigate/zoom offline — the overview
strip below the plot shows the whole recording compressed, same as Capture
mode's own, not just whatever page is currently zoomed into, with the
highlighted rectangle marking where that page sits within it; dragging
that rectangle all the way to either edge pans to the true edge of the
recording without the visible span narrowing along the way). Switching
between the three keeps each one's own state fully separate: each has its
own timebase — including whatever a drag-a-box zoom has narrowed it to,
not just what the dropdown was set to before zooming — (Capture and Live
have separate, non-overlapping ranges, so a
shared value would mean one mode's setting silently getting
nearest-match-substituted into the other's range and never coming back)
and its own sample rate (Live has its own selector now, matching Record's;
it no longer derives a rate from Capture's own timebase), Live also
remembers its own pan position through history, and each mode redisplays
exactly its own last-shown waveform when switched back into — or is
genuinely empty if it has never run yet — rather than inheriting whatever
another mode most recently left on screen. A channel turned off on its
own card is respected everywhere this restoration happens: it never
reappears showing old data from while it was on, in any mode. Trigger modes
are **None** (free run), **Normal** (wait for an edge, software-enforced —
see the LIANXU section below), plus **Single** and **Force** buttons.
Zoom (drag a box to zoom in, double-click to reset — also clears the
back/forward history below) and Freeze (Live mode only — pauses the
display while acquisition continues) are top-bar toggles. Two small
arrows appear in the strip just above the plot once there is a zoom to
go back to — one independent back/forward history per acquisition mode,
so Capture's, Live's, and Record's own zoom trails never interfere with
each other. Once paused (Freeze) or stopped (Run/Stop) — either one
leaves the buffered history static — dragging
the plot, or dragging the green viewport rectangle on the overview strip,
pans back through whatever of
that history is still held; dragging left reveals newer content (matching
a "drag a piece of paper" feel), dragging right reveals older; neither
does anything while actively running and unfrozen, since the very next
update would immediately overwrite whatever a drag revealed. Zooming
(drag a box) on a Live-mode view updates the overview strip's own
viewport rectangle immediately to match, and panning afterward continues
from exactly that zoomed position rather than snapping back to the
newest data — Live's own buffer is refetched fresh from the ring on
every refresh, sized to match whatever the current zoom is, so zooming
there works by moving the same history-pan position (`m_liveViewOffset`)
panning itself uses, not by a separate, buffer-relative mechanism the
next refresh would immediately invalidate.
Panning — on any of the three acquisition modes, and on either the plot
itself or the overview strip's own drag — moves the whole time grid along
with the waveform: gridlines and their labels together, glued to the
signal rather than staying at fixed screen positions with only the label
text changing underneath them, matching the vendor's own app. The dashed
"time zero" reference line follows along too, appearing wherever time
zero actually falls and simply not drawing at all once it has panned out
of view — it only looked fixed to the screen's centre before because the
view was never panned relative to it.

Detail panel pages: **Scope** (time/div, acquisition mode, and — Record only
— sample rate, record size, and whether the recording is kept or deleted on
exit), **Trigger**, **Channel** (one page per
channel: volts/page, coupling, probe, invert, DC/input/percentage offset,
colour), **Instruments** (mode shells — see Known rough edges), **Math**
(enable, operation, source A/B, volts/page including Auto, trace colour —
settings persist), **Rulers** (time cursors on/off, one voltage-cursor
checkbox per enabled channel plus Math, each tinted in that channel's own
colour — see `HANDOFF.md` for the full cursor architecture), and
**Settings** (where recordings are saved — see below).

The whole UI runs Fusion with a hand-built dark palette and a global
stylesheet (`theme.h`) replacing Fusion's own chrome for standard controls;
see `HANDOFF.md` for the QSS gotchas this ran into.

## Features

- **Trigger**: edge trigger, rising or falling, selectable source channel,
  threshold in displayed volts, adjustable pre-trigger percentage.
  Hysteresis is derived from the source channel's volts/div so a noisy signal
  does not retrigger on every wobble. Pre-trigger is 50%.
- **Timebase**: time/div is the primary horizontal control and picks the
  sample rate, as on a bench scope. Capture and Record share one range (up
  to 100 ms/div); Live has its own, separate range for slow, rolling display
  — 100 ms/div through 5 s/div — since that is what a real scope's roll mode
  actually covers. The combo's contents swap when the mode changes
  (`refreshTimebaseOptions()`), picking the closest match from the other
  range rather than jumping to an arbitrary value.
- **Per channel**: volts/div, DC/AC coupling, probe attenuation (built-in
  x1/x10/x20/x100 plus a custom probe library with linear scaling and units),
  vertical position in divisions with a ground marker, DC offset, input
  offset (hardware window), percentage offset, per-channel colour.
- **Math channel**: a computed CH A +/-/x CH B trace, its own volts/page
  (Auto or a fixed value — Auto refits every frame to the signal's own
  amplitude, which looks unstable, so a fixed page is the default) and
  draggable position, its own colour. All Math settings persist.
- **Rulers**: two time cursors and, independently, a voltage-cursor pair per
  channel (including Math) — any combination can be on at once, each in that
  channel's own colour, positioned on the same side as that channel's own
  voltage scale. A draggable, minimizable readout box shows every active
  cursor's values and deltas in a table. See `HANDOFF.md` for the full
  architecture — this was the single largest feature built after the initial
  core, and has several non-obvious design decisions worth understanding
  before extending it.
- **Annotations**: the grid button drops a floating text note that grows to
  fit as you type (no scrollbars). Selecting it (click into the text) shows
  a trash button, a move handle, and 8 resize handles, plus a default arrow
  hanging straight down; drag that arrow onto a trace to make it a real
  pointer that tracks a specific sample and channel from then on, the same
  waveform-relative math a Ruler voltage cursor uses. Click away and the
  chrome — and the arrow, if it was never aimed at anything — disappears.
  Session-only, not persisted.
- **Record**: each recording gets its own uniquely-named subdirectory of the
  record directory (`yyyyMMdd_HHmmss`, so recordings sort chronologically by
  name and never overwrite each other), holding one `recording.vsxrec` file
  for the whole recording regardless of channel count (raw int16 codes plus
  a volts-per-code scale, not full doubles — a fixed-size region per channel
  that was actually on, concatenated together; a disabled channel gets no
  region at all) plus a small `meta.ini` with the sample rate, channel mask,
  each channel's scale, and each channel's actual written length (a
  recording stopped early does not fill its full reserved region), so the
  recording can be read back correctly in a later session with no in-memory
  state left over from when it was made. The
  record directory itself is Settings page's own section: `Browse...` to
  change it or `Use default` to go back to `~/.vimuscopex/records` (or the
  platform's own app-data location). A tab bar above the navigator strip in
  Record mode lists every past recording in that directory (label + a
  tooltip with the full timestamp), styled to match the rest of the app
  rather than plain Fusion chrome, each showing exactly the channels it was
  actually recorded with regardless of the live channel rail's current
  on/off state — click a tab to browse it, or its Font Awesome close button
  (matching the one on the detail panel, dark on the selected tab's own
  bright background and given room from the tab's edge rather than sitting
  flush against it) to remove that tab from view; this
  never deletes the recording itself, only hides its tab, and stays hidden
  across restarts too until explicitly reopened via the top bar's **Open**
  button. Closing every tab empties the plot entirely. The tab bar
  is disabled while a recording is actively being written, and re-enabled
  with a fresh tab once it stops. Quitting the app while still recording
  finishes it properly first (flush, close the files) before anything else
  happens, whether it is about to be deleted or not. **Keep** on the Scope
  page's Record-only section chooses Temporary or Permanent for the
  recording about to start, defaulting to Permanent (deletion is opt-in,
  not opt-out) — Temporary deletes that one recording's whole folder when
  the app closes, not when the recording itself stops, so it is still
  there to review until then; its tab is simply gone on the next launch,
  while a Permanent one's tab is there with its data intact. What actually
  happens at exit is fixed the moment a recording starts, not read fresh
  from the toggle when the app closes, so flipping Keep afterward without
  recording again never changes what happens to files already on disk from
  an earlier choice. The top bar's **Open** button picks any of this app's
  own recordings by its `.vsxrec` file (any recording's own, not only ones
  in the current record directory) and loads it, switching to Record mode
  first if needed. Each tab remembers its own zoom (which slice of the
  recording is on screen), each channel's own vertical scale/position, and
  each channel's own visibility — switch away and back and it is exactly
  as it was left, rather than resetting to a fresh full-recording view
  every time, and turning a channel off while viewing one recording never
  affects any other tab. This does not
  extend to a channel's own acquisition setup (coupling, probe, colour),
  which stays global by design; only what is specific to viewing a
  particular recording is per-tab.
- **Measure**: the grid button opens a page with a channel selector (CH1-4,
  and Math) and a 3-per-row grid of eleven measurements: Vpp, Vrms, Mean,
  Max, Min, AC RMS (Vrms with the DC/mean component removed), Frequency,
  Period, Duty Cycle, and positive/negative pulse width. Each source keeps
  its own independent set, even while off — the selector only changes which
  one the checkboxes are currently showing. A source's own line on the plot
  shows exactly the measurements checked for it and nothing else; with none
  checked, it is just the plain "CHn ±volts" line (or, for Math, just the
  operation label). This replaced the previous fixed, always-on
  Vpp/Vrms/Mean/frequency summary — nothing is shown by default now until
  picked here. Every source's set persists across restarts, one bitmask per
  source (`channelN/measurements`, `math/measurements` in QSettings).
- **Spectrum** (Instruments -> Spectrum; Capture mode only — see below): splits
  the plot into a resizable
  time-domain pane on top and an FFT (FFTW3) pane below, matching PicoScope's
  own layout — drag the divider past 60% and it snaps the rest of the way
  to fully maximized; drag it away to nothing and Spectrum mode turns off
  entirely, back to Scope. Pick up to 2 sources at once (CH1-4 and/or Math, each
  drawn in that source's own colour), a window (Hann, Hamming, Blackman, or
  Flat Top for amplitude accuracy), and a unit (dBV or dBm, 50 ohm). Every
  window is normalized by its own coherent gain, so a 1 V peak sine reads
  0 dBV no matter which one is picked. The dB floor is -100. FFT size is a
  fixed 2048 for now, no selector yet. Its graticule mirrors ScopeView's own
  exactly — same division counts, same centre-axis crosshair and tick
  marks, same border/grid colours, dB scale on the right the way a single
  channel's own volts scale is — dropping to 8 vertical divisions once the
  pane gets short enough that 10 would be cramped. Frequency range is
  whatever the current capture's sample rate implies (Nyquist = sample
  rate / 2) — seeing further out means a higher sample rate, which on this
  hardware means a shorter time window or a smaller capture depth (see
  `HANDOFF.md` for why that coupling is inherent, not a bug). Rulers' page gained an
  "FFT" checkbox (shown only while Spectrum mode is on) — a separate
  frequency/level cursor pair, each with a coloured value flag right on the
  plot exactly like ScopeView's own time/voltage cursors, plus a small
  delta readout — independent of ScopeView's own time/volts cursors.
  Display (Normal/Average) and Scale (Logarithmic/Linear) round out the Spectrum controls: Average is a
  smoothing exponential moving average across successive spectra that
  never discards the latest data, so switching back to Normal is instant;
  Linear shows plain peak volts (0-2 V) instead of dB, which disables the
  Unit combo while active. Available in Capture mode only, same as XY and
  Persistence: all three analyze a single, settled capture (an FFT of one
  snapshot, here), not a continuously streaming or recording signal, so
  Instruments' Spectrum/XY/Persistence segments are disabled — not
  hidden, so the choice stays visible — the moment Live or Record starts,
  and forced back to Scope if one of the three happened to be active when
  that happens.
- **XY** (Instruments -> XY; Capture mode only, same reason as Spectrum
  above): shares Spectrum's own second pane (a
  `QStackedWidget` holds both — only one is ever visible, so the splitter
  itself never has to know which) — pick one real channel for X and
  another for Y (CH1-4 each, exclusive, defaulting to CH1/CH2), and
  consecutive samples are connected in order into a classic Lissajous /
  hysteresis-style trace, drawn in the Y channel's own colour. Each axis
  is fixed to its own channel's own configured range (the same +-X V its
  card shows) rather than fitting to whatever the signal currently peaks
  at — the same stable-scale convention the main plot and Spectrum both
  already use — and updates immediately if that channel's volts/div
  changes or a different channel is assigned to the axis. Its Y axis is a
  direct visual match for the time-domain plot's own voltage scale — right
  side, same mV/V switching, same number rounding
  (`ScopeView::axisNumber()`, shared rather than duplicated), unit sitting
  above the topmost tick, all in the Y channel's own colour, exactly like
  a single channel's scale looks on the main plot. Same splitter
  behaviour as Spectrum throughout: past 60% snaps to maximized, dragged
  to nothing turns Instruments back to Scope mode, and reactivating after
  that (or for the first time) restores a visible 40% rather than staying
  at zero.
- **Display**: dotted 10 x 8 graticule with 1/5-division ticks on the centre
  axes, numeric voltage scale on the left (and, with two channels, the right)
  in the colour of the channel it follows, time scale along the bottom with
  zero at the trigger instant, trigger level marker on the right edge,
  trigger instant marked at the top, per-channel Vpp / Vrms / mean /
  frequency readouts, and a navigator strip below the plot with drag-to-pan
  and drag-rectangle zoom.
- **Why the trace does not walk**: the display is anchored to the trigger
  point returned by `ReadVoltageDatasTriggerPoint()` rather than to the start
  of the buffer — free running (`TRIGGER_STYLE_NONE`) would otherwise start
  every capture at an arbitrary phase. Cursors (Rulers) are stored as an
  *offset from that same anchor*, not an absolute sample index, for the same
  reason: the anchor itself moves a little between captures (real hardware
  trigger jitter), and a cursor stored as a fixed index would visibly drift
  relative to the now-correctly-repositioned trace.
- **Appearance**: Dark (default) or Light, toggled from Settings, persisted
  across restarts. Takes effect immediately for the app chrome, the plot
  itself, the cards, and every dynamically-styled `QToolButton` — the small
  number of section-header labels built by `sectionLabel()` (used 20+ times
  as anonymous locals) need a restart to pick up a change made mid-session,
  though they are always correct at the next launch either way. See
  `HANDOFF.md` for `theme.h`'s architecture and which colours deliberately
  stay fixed across both modes.

## What changed from the first draft

- `AcquisitionManager` no longer includes `windows.h` and no longer stubs
  itself out with `#ifdef _WIN32`. It includes the SDK header, which supplies
  the empty `CALLBACK`/`WINAPI` macros on Linux.
- **Fixed a real bug:** the data-ready callback used
  `QMetaObject::invokeMethod(obj, "poll", ...)` on a private, non-slot,
  non-`Q_INVOKABLE` method. That lookup fails at runtime, so acquisition was
  running entirely off the 10 ms timer. It now uses the Qt 6 functor overload.
- SDK callbacks arrive on the SDK's own thread. They previously emitted Qt
  signals and called SDK setters directly from that thread. They now only post
  a queued call onto the owning thread.
- Devices already connected at startup are enumerated explicitly via
  `GetAvailableDevCount()`/`GetOnlyId()` instead of relying on the add-callback.
- Capture length defaults to 32 KB instead of 1 MB. A 1 MB block at 1 MS/s is
  one second of data per frame, allocated and copied per channel per
  acquisition.
- The waveform widget draws one min/max bar per pixel column when there are
  more samples than pixels. The previous version built a `QPainterPath` with
  one `lineTo` per sample.
- Callbacks are detached before `FinishDll()`, and `FinishDll()` is no longer
  called when `InitDll()` failed.
- Added Single, Force trigger, per-channel enable, input range and capture
  length controls.

## Environment switches

| Variable | Effect |
|---|---|
| `VIMU_CAPTURE_MASK=0x0100` | Force one literal `Capture()` mask (kept for logic-channel work) |
| `VIMU_WATCHDOG=1` | Re-enable the SDK hardware watchdog (off by default) |
| `VIMU_LOG=1` | Turn on the SDK's own logging |

## Device facts, measured

From `tools/maskprobe` on an MSO21AL PLUS with SDK 1.24:

```
channels    : 2, 12-bit ADC, 65536 KB memory
hard trigger: 1,  pre-trigger: 1,  roll: 0
rates (18)  : 1k 2k 5k 10k 20k 50k 100k 200k 500k 1M 2.5M 5M 12.5M 25M
              31.25M 62.5M 125M 250M
```

### The capture mask

`Capture()`'s channel mask is documented two incompatible ways in the same
comment block of the vendor header:

- prose: `ch1=0x0001 ch2=0x0020 ch3=0x0040 ch4=0x0080`
- examples: `ch1+ch2 = 0x03`

**The examples are right and the prose is wrong.** Measured:

```
mask 0x0001 ->  CH1: 8192 pts   CH2: no data
mask 0x0002 ->  CH1: no data    CH2: 8192 pts
mask 0x0003 ->  CH1: 8192 pts   CH2: 8192 pts
```

One bit per channel from bit 0, and simultaneous multi-channel capture works.
`ReadVoltageDatas` returns zero samples for a channel that was not in the mask,
rather than returning flat data. Logic is `0x0100`, confirmed independently by
`vimu_driver.py`.

### Capture depth, measured

`vimu_driver.py` caps triggered captures at 8 KB. That file is **not part of the
vendor SDK** — the guide's directory listing names the Python examples as
`test_callback.py`, `test_polling.py` and `test_multdev_stream_callback.py` —
and its author was working around the LIANXU bug below, which wedges the device
regardless of depth. Measured with `tools/maskprobe --style 0x01`:

```
depth    8 KB : ready in  300 ms
depth   64 KB : ready in  400 ms
depth  256 KB : ready in 1060 ms
depth 1024 KB : timeout at 3000 ms
```

Triggered captures work far beyond 8 KB. The real constraint is latency, not
correctness: roughly 290 ms fixed plus 3 ms per KB, so 256 KB is about one frame
per second. 64 KB is the default — it keeps the display live while allowing
250 MS/s at fast timebases.

### The trigger mode that does not work

`TRIGGER_MODE_LIANXU` (normal) never completes a capture on an MSO21AL Plus.
The vendor's own application fails the same way, and their `DllTest-Callback`
demo ships with `SetTriggerMode(0x00)` active and the LIANXU line commented out.
The device is therefore always left in `TRIGGER_MODE_AUTO`, and Normal mode is
reconstructed in software: `verifyEdge()` confirms the source channel really
crosses the threshold in the right direction near the reported trigger point,
and frames that fail are held back.

### Stream mode, measured

`StreamCapture()` **returns 0 on success**, not 1. The header documents
`1 success / -1 sample rate error / -2 device id error`; the vendor's own
`DLLMultDev-Stream` demo ignores the return value, which is how the discrepancy
went unnoticed. Judge success by whether data callbacks arrive.

Measured on an MSO21AL PLUS at 1 MS/s, two channels, `count_every_1s = 3`:

```
buffer_length  340992 samples per channel per callback   (333 x 1024)
menoryuse      1363968, flat - no backlog
failed         0
need_total     length_kb x 1024 samples PER CHANNEL
```

Blocks quantise to whole KB, so samples per callback is
`round(rate / count_every_1s)` rounded to a KB boundary.

Other stream facts:

- Range is +/-24 V, twice the block-mode range.
- 17 stream sample rates, 1 kS/s to 125 MS/s (62.5 MS/s is the two-channel
  figure per Vimu's site; the device is USB 3.0).
- `SetOscCaptureMode(dev, 1)` switches the device into stream mode, and the
  stream path has its own setters: `SetStreamChannelRangemV`,
  `GetStreamRangeMinmV/MaxmV`, `GetStreamSupportSampleRates`,
  `GetStreamVoltageResolution`. The block-mode equivalents do nothing here.
- `StreamCapture` is finite: it stops once `total_sample` reaches
  `need_total_sample`. Continuous streaming means requesting a very large
  length and ending it with `StreamStopCapture()`.
- Configure and start from the device-added callback, with both callbacks
  registered before `ScanDevice()`, and call `ScanDevice()` exactly once —
  rescanning re-claims the interface and returns `LIBUSB_ERROR_BUSY`.
- Roll mode is genuinely unsupported: `IsSupportRollMode` returns 0 and
  `SetRollMode(1)` fails. Stream is the only rolling-display path.

### Two vendor-library hazards

**A mask bit for a channel the device does not have raises SIGFPE.** Not an
error return — an integer divide by zero inside `libvmmultmso.so`, which takes
the whole process down. `arm()` therefore masks off every bit beyond
`GetOscChannelNum()` before calling `Capture()`.

**A stalled capture wedges the device.** From `vimu_driver.py`: "A wedged
device fails EVERY capture, free run included, and only a full
FinishDll/InitDll restart clears it." `AcquisitionManager` detects a capture
that produces no data within its expected window and rebuilds the SDK session
automatically, so one bad capture does not poison everything after it.

The same driver caps capture depth at 8 KB while a trigger is armed. This code
runs `TRIGGER_STYLE_NONE` (free run), so that cap does not apply yet, but it
will once real triggering is added.

## Diagnostic tool

```bash
cmake --build build --target maskprobe
./build/maskprobe                                # sweep every plausible mask
./build/maskprobe --masks 0x01,0x02,0x03 --kb 8  # specific ones
```

Each attempt runs in a forked child with a fresh `InitDll`/`FinishDll` session,
so a mask that crashes the library only kills the child, and no attempt
inherits a wedged device from the one before it.

## Windows (portable build)

VimuScopeX builds on Windows with MSYS2 and packages itself as a folder
that runs on any 64-bit Windows 10/11 PC with nothing to install: Qt,
all runtime DLLs, the Vimu SDK DLLs, the protocol decoders and a trimmed
Python for them are inside.

One-time build (on any Windows PC, e.g. the laptop itself):

1. Install MSYS2 from https://www.msys2.org and open **MSYS2 UCRT64**.
2. Copy this source folder and the `VmMsoSDK-master` folder somewhere on
   the PC, then run:

       cd /c/path/to/VimuScopeX
       ./packaging/windows/build-windows.sh /c/path/to/VmMsoSDK-master

The first run downloads the toolchain and Qt and builds libsigrokdecode
from source (allow 20-40 minutes); later runs take a few minutes. The
result is `dist/VimuScopeX/` (run `VimuScopeX.exe`) and
`dist/VimuScopeX-windows.zip` to copy to other PCs.

The scope itself still needs Windows' USB driver for it, which the
official Vimu PC software installs.
