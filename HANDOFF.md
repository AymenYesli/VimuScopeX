# VimuScope X — handoff

Qt 6 / C++17 desktop app replacing the vendor software for the **Vimu MSO21AL Plus**
USB 3.0 oscilloscope (2 analog channels, 12-bit ADC, up to 250 MS/s block capture,
62.5 MS/s streaming). Targets Linux (main dev box: Arch) and Windows (MSYS2 UCRT64).
UI modelled on PicoScope; strong automotive-diagnostics feature set. Version 1.0.0.

Full development history: `docs/HANDOFF-archive.md` (large — search it, don't load it).

## Build & package
- Build: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVIMU_SDK_DIR=/path/to/VmMsoSDK-master && cmake --build build`
- Deps: Qt 6 (Core, Gui, Widgets, Network; optional Multimedia → startup video/sounds,
  optional Svg → Hugeicons icons), fftw3, libsigrokdecode (optional → protocol decoding), Python (for sigrok decoders).
- Vendor SDK header: `include/VmMultMsoLib.h`; library `libvmmultmso.so` / `vmmultmso.dll` (+ `vmsignal`), found under `VIMU_SDK_DIR`.
- Packaging: `packaging/README.md`. Windows `packaging/windows/build-windows.sh` (folder, zip, NSIS installer);
  Linux `packaging/linux/build-appimage.sh` (linuxdeploy AppImage, custom AppRun sets PYTHONHOME);
  Arch `packaging/arch/PKGBUILD` (→ /opt/vimuscopex); udev rule `60-vimuscope.rules` (USB d4a2:5663).
- No Qt/toolchain exists in the AI sandbox: code is never compiled there. The user builds and pastes errors.
  Check edits with a brace/paren balance sweep (string literals and comments excluded) and simulate logic standalone.

## Code map (src/)
| File | Role |
|---|---|
| `mainwindow.*` (≈12k lines) | Everything UI-level: top cards, left rail, sliding panel pages, mode switching, settings, decoding orchestration, automotive tests, file I/O |
| `scopeview.*` | The plot: traces, gutters/scales, rulers + flags + Rulers panel, bus rows (decoded blocks), info/test panels, annotations, zoom buttons, filters |
| `navigatorview.*` | Overview strip above the plot (shared by all modes) |
| `acquisitionmanager.*` | Block capture (Capture mode) via SDK; ADC-window readback; trigger; averaging |
| `streamengine.*` + `ringbuffer.h` | Streaming (Live / Record); per-channel rings; stream ranges & coupling |
| `recordwriter.*` | Disk recordings: folder = `recording.vsxrec` (raw int16, channels concatenated) + `meta.ini` |
| `liveworker.*` | Background thread preparing long Live windows (min/max reduction, measurements, overview) |
| `decoderengine.*` | libsigrokdecode wrapper (CAN, CAN FD, LIN, UART, K-Line…) + native SENT + OBD-II/UDS layer; async jobs |
| `cantools.h`, `serialbus.h`, `sent.h`, `diag.h` | Qt-free protocol logic: CAN health & CRC checks; LIN/K-Line/UART; SENT (fast, slow, SPC); ISO-TP/OBD-II/UDS |
| `batterytools.h`, `enginetools.h` | Cranking/charging/ripple/parasitic-drain analysis; engine cycle & compression |
| `mathops.h` | Math channel operations, units, labels |
| `adcwindow.h` | What the ADC actually covers (from SDK zoom/bias readback) |
| `panelcontrols.*` | PicoScope-style restyling pass over the sliding panel + numeric keypad popup |
| `vsxdialog.*` | Frameless themed dialog base, `VsxDialogs::question/warning/pickColour` |
| `cardbutton.*` | The top/left "cards" (title, note, value, detail, −/+ side bars, compact mode) |
| `theme.h`, `fonts.*`, `icons.*` | Colours (dark/light), global style sheet, fonts, SVG/Font-Awesome icons |
| `probes.*`, `probedialog.*` | Probe library (unit, gradient, offset) and its dialogs |
| `remoteserver.*` | HTTP + WebSocket phone remote (QR code: `qrcode.h`, page: `resources/remote/`) |
| `spectrumview.*`, `xyview.*` | FFT and XY views |
| `perftrace.h` | Opt-in timing (`VSX_TRACE=1`) |
Other: `decoders/vsx_kline/` (our sigrok decoder), `resources/` (icons, fonts, sounds, logo video, app.qrc).

## Modes & data flow
- **Capture** (block): `AcquisitionManager` → frames → `ScopeView` (whole buffer, indices = buffer samples). Trigger, pre-trigger %, depth, averaging (2–128, trigger-aligned).
- **Live** (stream): `StreamEngine` rings; history sized from 35 % of RAM; long windows reduced on `LiveWorker` to 8192 min/max pairs; newest sample at the right edge.
- **Record** (stream → disk): `RecordWriter`; view pages read decimated from the file (`m_recordViewStart/Length`); tabs per recording; Temporary recordings deleted on exit, Permanent kept; Save → `.vsxrec` copy (`saveRecordingAs`, background thread).
- Per-mode state: timebases (Capture/Live separately), Record rate/size/keep, zoom history, annotations (per mode / per recording), snapshots restored on mode switch. **Channel settings are shared by all modes on purpose** (physical setup).
- Opening a `.vsxwave` is a temporary *view*: settings snapshot (`takeFileViewSnapshot`) restored by `leaveFileView()` on mode switch / Run / Single; file colours never applied; `saveSettings()` writes the snapshot while a file is viewed.

## Hardware / SDK facts (learned on the real scope)
- 15 symmetric hardware ranges ±145 mV … ±29.9 V (~√2 steps). The window is always symmetric around 0 V; input offset only selects a larger range. Readback of the real window: `adcwindow.h`.
- Zero calibration is keyed by the **hardware range actually used** (read back), looked up as the smallest calibrated range ≥ request. Full calibration sweeps pages + geometric midpoints, skipping covered ranges.
- Entering/leaving stream mode resets coupling and ranges → the app re-sends them every start/stop. Relays click at stream start/stop even with no mode switch: it's inside `StreamCapture`/`StreamStopCapture`. Accepted (attempts reverted).
- `libvmmultmso` crashes ("pure virtual method called", in its own thread) recording **two channels at 62.5 MS/s** → Record with ≥2 channels is capped at 31.25 MS/s. One channel keeps 62.5.
- In stream mode block-range calls are inert; stream ranges via `SetStreamChannelRangemV`.
- A "stuck range" self-heal toggles coupling once if the read-back window is >2.5× the request (after 1.5 s settle; posts a status message).

## UI system & rules (Qt pitfalls we hit — respect these)
- Theme: `theme.h`. Dark: plot `#0C0C0C`, top/left sections `#191919`, cards and sliding panel `#282828`, accent `#73FF00` (light `#62D900`), secondary accent `accentSoft`. Neutral greys only (old blue-greys removed). `onAccentBar()` = ink on accent bars (dark in dark mode, white in light).
- **Fonts**: the global sheet has `* { font-family }`, so Qt rebuilds widget fonts from the *application* font — `setFont()` on a parent does NOT reach children. Size text with style-sheet `font-size` (the panel pages get `* { font-size: 90 % }` in `PanelControls::adopt`).
- **Style-sheet `.arg()` placeholders**: multi-arg `arg()` fills the lowest placeholders present — removing one (e.g. `%2`) silently shifts every colour. Keep numbering contiguous (`panelHostStyle()` keeps `%2` at a 0 px border for this reason).
- A parent's style sheet overrides the global one regardless of specificity → when overriding a property in a parent sheet, restate every state (hover/checked/disabled).
- `PanelControls::adopt()` runs once after the panel is built: exclusive button groups in a row → joined segments (end caps among *visible* buttons, `SegmentRoles`); in a grid → flat choices; single check box → title + Off|On; grid check boxes → toggle buttons; combos/spins → "compact card" fields (−/+ bars, fixed 150×24); combos with property `asSegments` → segments; clicking a number opens the keypad (units select, ✓ confirms, slider). Originals stay (hidden/0×0) as the real settings.
- The app updates many controls with **signals blocked** (≈37 places): stand-ins are re-synced by a 120 ms timer while the panel is visible. `Mirror` copies visibility to the wrapper and enabled-state to sibling controls — never disable the wrapper (a child in a disabled parent gets no EnabledChange).
- `drawRulers()` leaves the clip on the plot; anything drawn later outside the plot must `setClipping(false)` (zoom buttons).
- Panel title bar and record tabs share `kTabStripHeight` (30); `NoTabShiftStyle` removes Qt's 2 px unselected-tab shift.
- Dialogs: always `VsxDialog` / `VsxDialogs` (never QMessageBox/QColorDialog directly, except the system picker behind "Custom colour…"). File open/save stay native.
- Icons: Hugeicons SVGs in `resources/icons` recoloured at runtime (`currentColor`), mapped from Font Awesome names in `icons.cpp`; Font Awesome fallback.

## Feature inventory (all implemented)
- Channels: Off/Auto/Manual (user range change in Auto → Manual), scale, DC/AC, probes (custom units relabel the page: "bar per page", offsets in unit), invert, input/DC offset, filters (LP/HP/BP, High-res ×4…×256).
- Math (16 ops, own page/scale in V/mV), references (unit from source, solid), measurements (per channel/Math), rulers (time/voltage, track mode, slope; flags in the axis unit, panel in seconds), annotations, zoom history, spectrum, XY, persistence.
- Decoding (2 slots): CAN / CAN FD (CRC verified independently), LIN (auto baud from sync), UART, K-Line (own decoder), SENT (fast / slow serial messages / SPC), OBD-II/UDS over ISO-TP (PIDs, DTCs with descriptions, VIN, NRCs, response times). Blocks re-projected on pan (no lag).
- Automotive page: engine cycle overlay, relative compression, cranking / charging (ripple verdict = every-3rd/6th-hump pattern + robust MAD spreads; ignition dips ignored), parasitic drain, battery limits, bus health panels (CAN, LIN/K-Line/SENT, OBD-II/UDS).
- Phone remote (QR, live view, controls), Range report & ADC info (hidden diagnostics), startup video & plug sounds.

## Publishing
Vimu SDK = github.com/vimu-tech/VmMsoSDK, MIT: CMake fetches it (FetchContent → build/_deps/vimusdk-src) when no `VIMU_SDK_DIR`/`VIMU_SDK_LIB` is given; scripts take the SDK path optionally; every package ships `licenses/VmMsoSDK-LICENSE.txt`. `.github/workflows/release.yml` builds on tag `v*` (Windows via MSYS2, AppImage on Ubuntu 22.04 + Qt 6.8), no secrets. AUR: `packaging/aur/PKGBUILD` (vimuscopex-bin, unpacks the AppImage to /opt). winget: `packaging/winget/` (nullsoft). Open: our own LICENSE (GPL-3.0-or-later + Vimu linking permission suggested); Windows WinUSB binding on a clean PC unverified. Steps: `packaging/README.md`.

## Files & settings
- `.vsxwave` (Capture/Live snapshot, format v3: samples + full settings), `.vsxrec` recording folder (see Record), reference files (magic "VSXR", v2 adds unit; v1 = volts; newer refused).
- Settings: `QSettings("VimuScopeX","VimuScopeX")` via `saveSettingsTo/restoreSettingsFrom(QSettings&, session)`.

## Open items / ideas
- Packaging scripts (NSIS, AppImage, PKGBUILD) written but never run — expect first-run fixes.
- Not implemented: FlexRay, J1939, J1850; SENT trigger-width → sensor-ID mapping; enhanced SENT CRC bit order unconfirmed (shown "not verified").
- Averaging / High-res on real hardware and two-channel recording at 31.25 MS/s still to confirm.

## Working conventions
- Edits atomic (all anchors asserted before writing); balance sweep after each change; HANDOFF updated briefly.
- New algorithms (decoders, CRCs, ripple analysis…) tested standalone with synthetic signals before integration.
- Keep comments explaining *why* (many encode hardware or Qt behaviour learned the hard way).
