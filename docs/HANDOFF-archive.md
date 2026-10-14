# ARCHIVE — full development log (2026-09/10). Do NOT load by default: read HANDOFF.md instead; search this file only for the history of a specific decision.

# VimuScope X — handoff

A Qt 6 oscilloscope frontend for a Vimu MSO21AL PLUS on Arch Linux, built on
Vimu's public `vmmultmso` SDK. **Read `README.md` first** — it holds the
build instructions and the hard-won SDK facts (capture mask encoding, the
broken LIANXU trigger mode, stream mode measurements, the two vendor-library
crash hazards). This file is the second thing to read: what state the project
is actually in, the non-obvious bugs already found and fixed, and the risks
still open. It assumes README's facts as background.

The person you're working with runs Arch Linux, user `Scorpion`, SDK at
`/home/Scorpion/VmMsoSDK-master`. They rebuild and run the app themselves —
you cannot compile, run, or test any of this directly. Every change has to be
reasoned through statically, packaged, and handed off with exact rebuild
commands. Two real crashes shipped this way before being caught by an actual
backtrace (see below) — this is not a hypothetical risk, it is what already
happened twice.

## Working environment (for you, the next Claude)

The project's canonical working copy is `/home/claude/VimuScopeX` inside your
own container — edit there directly. Package for delivery with:

```bash
cd /home/claude && zip -qr /mnt/user-data/outputs/VimuScopeX-linux.zip VimuScopeX
```

Give the person the md5 and this exact extraction command — **`-DD` is not
optional**: without it, the archive's file timestamps can end up older than
objects already in an existing `build/` directory, and Ninja's incremental
build silently skips recompiling files that actually changed. This cost
several round trips before it was understood, in an earlier session.

```bash
unzip -oDD "<md5>" -d ~/src
cd ~/src/VimuScopeX && cmake --build build && ./build/VimuScopeX
```

If a *fresh* configure is ever genuinely needed (e.g. to rule out stale
object files as a hypothesis), the SDK path is a CMake cache variable that
gets wiped along with `build/` — it must be re-supplied, or configure fails
outright:

```bash
cmake -S . -B build -DVIMU_SDK_DIR=/home/Scorpion/VmMsoSDK-master
```

**Before packaging any change**, run a consistency sweep — this has caught
real mistakes (though not the two structural crashes below, which a
name-existence sweep cannot catch):

```python
# every declared method has a Class::method( definition; every m_\w+ used
# in the .cpp is declared in the .h. Adapt the regex to whichever classes
# you touched.
import re, pathlib
h = pathlib.Path("src/X.h").read_text(); c = pathlib.Path("src/X.cpp").read_text()
# ...
```
Also grep for stale references to any field/member you renamed or removed,
across every file you touched, not just the one you were editing.

### Debugging protocol — use this, do not guess from source alone

Two real crashes and one hang happened this session. Both times, blind
static-review guessing (even careful, extensive guessing) either missed the
cause entirely or found a plausible-but-wrong theory. Both times, an actual
backtrace found the real cause in one shot. **The instant something crashes
or hangs, ask for a backtrace before proposing a fix.**

For a crash:
```bash
cd ~/src/VimuScopeX && gdb -batch -ex run -ex "thread apply all bt" --args ./build/VimuScopeX
```

For a hang that does *not* crash on its own, attaching after the fact
(`gdb -p $(pgrep ...)`) is a race — by the time the person notices, waits,
and runs the command, the process may have already died on its own (this
exact race happened once — the attach failed with "program terminated with
signal SIGABRT" because gdb wasn't there to catch it). Always launch under
gdb from the start instead, as above, even for a suspected hang.

Get **all threads**, not just the one that received the signal — this app
runs many SDK-internal and Qt-internal threads (libusb polling, the SDK's own
device thread, Qt's thread pool, Wayland/glib plumbing), and the actual root
cause has shown up in a *different* thread's stack than the one that
crashed, every time.

## Current feature state

Three acquisition modes (Capture / Live / Record), full per-channel controls,
a Math channel, a Rulers cursor system, Annotations, Measure, a Spectrum
(FFT) view, an XY view, Zoom, Freeze (Live only, and now pannable while
frozen), a probe library, and a from-scratch dark theme with a global
stylesheet. All of this works and has been iterated on directly with the
person, including real bug
reports from actual hardware use — this is well past the "just built, not
tested" state an earlier version of this file described.

Still shells with nothing behind them: Instruments' Persistence
mode, Decoding, Settings (the grid button, not the Rulers/Math
settings pages, which are real), Open/Save reference waveforms, the
References grid button (label shortened from "Reference Waveforms" to fit
the ~87 px grid cell; full name in its tooltip; not checkable, no panel page),
Full-screen. Freeze is Live-mode only; extending it to Record (pause the
display while a recording continues in the background) was discussed and
explicitly deferred, not built.

## Rulers — the largest feature built this session, full architecture

This is complex enough, and was iterated on enough times, that extending it
without understanding the design will likely reintroduce bugs already fixed.

**Data model** (`ScopeView::RulerState`, private to `ScopeView`):
- `t1Offset`, `t2Offset` — time cursors, stored as **offsets from the same
  anchor `viewAnchor()` uses** (trigger instant, or buffer centre free
  running), never as absolute sample indices. This is the fix for a real,
  reported bug ("the cursors keep wobbling"): the anchor itself moves a
  little between captures (genuine hardware trigger jitter), and the window
  re-centres on it each time so the *trace* stays visually still. A cursor
  stored as a fixed absolute index does not get that same correction and
  visibly drifts relative to the now-correctly-repositioned trace by exactly
  the jitter, every frame. Any future per-sample position stored on
  `ScopeView` should default to this pattern, not an absolute index.
- `channels[MaxChannels]` (one `ChannelCursor{enabled, v1, v2}` per real
  channel) plus a separate `math` `ChannelCursor` — **every channel and Math
  can have independent voltage cursors on at the same time**, not a single
  shared selection. This was a deliberate redesign partway through the
  session, matching the reference vendor app's own per-channel rulers, after
  an earlier single-selector design was replaced.
- `v1`/`v2` are stored in **raw, pre-probe volts** — the same space
  `valueToY()`/`yToValue()` operate in for a real channel, or the same space
  `mathValueToY()`/`mathYToValue()` operate in for Math.
- `boxPos`, `boxMinimized` — the draggable readout box's own state.

**Drag identification** (`m_rulerDrag`, file-scope sentinels in
`scopeview.cpp`): `kRulerT1=0`, `kRulerT2=1`, `kRulerBox=2`,
`kRulerMinimize=3`; voltage cursors encode *which* channel via
`kRulerVoltsBase(10) + channel*2 + (isV2?1:0)`, where `channel` ranges
`0..MaxChannels` (`MaxChannels` itself is the Math sentinel,
`ScopeView::RulersMathChannel`). Helper functions `rulerVoltsDragId()`,
`isRulerVoltsDrag()`, `rulerVoltsChannelOf()`, `rulerVoltsIsV2()` encode and
decode this. This scheme exists because multiple channels' cursors can be
dragged independently — a single flat enum could not distinguish "CH2's V1"
from "Math's V2".

**Shared layout computation** — `ScopeView::computeRulerLayout()` returns a
`RulerLayout` (with a `RulerChannelLayout` per channel index, including
Math) that both `drawRulers()` and `hitTestRulers()` consume. This is
deliberate and should be the template for any future draw+interact feature
in this app: computing geometry once and having both drawing and hit-testing
read from the *same* computed rectangles is what fixed "the flag has a tiny
clickable area" — before this, hit-testing used a thin-line distance check
while drawing used a full flag rectangle, so they disagreed.

**Flag positioning** — CH1 (and CH3/CH4, which never get a drawn scale of
their own on this two-channel device) render on the left; CH2 renders on the
*right*, matching where CH2's own voltage scale is actually drawn; Math
renders in its own narrow column between CH1's gutter and the plot, matching
where Math's scale is drawn. Flag width is computed once from a worst-case
template string (`"-000.000"`), not from the actual rendered text, so a flag
never resizes as its value's digit count changes while dragging. CH1 and Math
share the *narrower* of their two column widths (Math's column is narrower
than CH1's gutter) so their flags visually match rather than CH1's being
wider.

**Voltage precision** — a dedicated `rulerVoltsText()` function, *not*
`axisNumber()` (which is used for the main axis and deliberately caps at 2
decimals). Rulers show plain volts, never millivolts (an earlier
millivolt-conversion design was explicitly reversed), with decimals scaled
to magnitude: `>=100 -> 0, >=10 -> 1, >=1 -> 2, <1 -> 3` decimals — e.g.
`0.879`, not `879.3` or `0.88`.

**Settings panel** (`PageRulers`, `MainWindow::buildRulersPage()`) — a "Time
cursors" checkbox plus one checkbox per channel (CH1–4) and one for Math,
each showing/hiding based on whether that channel/Math is actually enabled
right now (`refreshRulersChannelList()`, called from `applyChannel()`,
`applyMathChannel()`, and on opening the page). **Checkbox label colours are
set via `setStyleSheet()`, never `setPalette()`** — see the QSS gotcha below;
this is not optional, `setPalette()` silently does nothing here once the
global stylesheet is active.

**The box** — draggable, minimizable (title bar only when minimized, 20%
narrower), accent-coloured title strip, a real table (swatch / "1" / "2" /
"Δ" columns) sized dynamically to however many rows are actually active.
Double-clicking *on the box* does not reset zoom — `mouseDoubleClickEvent()`
explicitly excludes the box's own rect, since it visually sits on top of the
plot.

**Not done**: Rulers state is session-only, not persisted to QSettings (Math
is; Rulers currently is not — worth doing the same way if asked). No lock
icons on individual cursors (present in the reference app, not requested).

## Math channel

Defaults to a **fixed ±1V scale**, not Auto — Auto refits every frame to the
signal's own amplitude and looked visually unstable to the person using it;
Auto is still available as an explicit choice in the volts/page combo (index
0). Position is drag-adjustable (`mathOffsetDragged` signal ->
`setMathPositionVolts`). **All Math settings now persist** — enabled,
operation, both sources, volts/page index, and colour — saved/restored the
same way channel settings are, applied through `applyMathChannel()` in one
pass on restore so it does not fight itself.

`NavigatorView`'s own Math trace was, for a while, always auto-fit
independent of `ScopeView`'s actual Math scale — this made Math look like it
was "dominating" the navigator regardless of how it was scaled in the main
plot. Fixed via `NavigatorView::setMathScale(voltsPerDiv, positionVolts)`,
mirroring `ScopeView`'s own manual-mode mapping exactly, falling back to
auto-fit only when Math itself is in Auto mode. Kept in sync from two call
sites in `mainwindow.cpp`: `applyMathChannel()` and the `mathOffsetDragged`
handler — if Math's scale/position ever gains a third way to change, sync
the navigator there too.

## Live panning — why it lives in MainWindow, not ScopeView

Capture and Record both pan by keeping a buffer bigger than one screen and
letting `ScopeView`'s own `windowStart()`/`m_viewOffsetSamples`/
`m_viewFollowing` machinery slice a sub-window out of it. Live deliberately
does **not** reuse that path, even though it looks like the obvious fit —
the reason is purely about how much data that would mean copying. Live's
rate selection (`applyTimebase()`'s stream branch) is chosen to fill the
*screen*, independent of how long History is; at a high rate and a long
History, the ring can hold many tens of millions of samples per channel.
Pushing the whole ring into `ScopeView::setSamples()` every 33 ms tick so it
could slice a window out of it — the Capture-shaped approach — would mean
copying tens of MB of doubles per channel per tick, every tick, forever.
Not viable.

So Live panning is built the same shape as Record's, not Capture's: the pan
offset (`MainWindow::m_liveViewOffset`, samples back from the newest one)
lives in `MainWindow`, and `refreshLiveView()` (the direct counterpart to
`refreshRecordView()`) fetches only ever exactly one screen's worth,
starting that many samples before the live edge, via a new
`RingBuffer::rangeFromNewest()` — a generalization of the ring's existing
`newest()` to an arbitrary offset rather than always ending at the newest
sample. Because the fetched buffer is always exactly the visible window with
no slack, `ScopeView` has nothing to pan *within* for Live — it just draws
whatever it was handed, same as always. `m_viewOffsetSamples`/
`m_viewFollowing` stay completely unused for Live; do not try to make
`ScopeView` "aware" of Live panning, it does not need to be.

**Panning is gated on Freeze, by explicit choice, not derived from it.**
Dragging while not frozen does nothing (the answer chosen when this was
built, over the alternative of auto-engaging Freeze on the first drag) — the
reasoning being that Freeze is a manual, explicit control, and a drag
gesture silently flipping it would be surprising. This also happens to make
the whole feature correctness-simple: `StreamEngine::ingest()` stops
appending to the ring entirely once frozen, so the ring is provably static
for as long as Freeze stays on, and `m_liveViewOffset` can never drift out
from under a held drag or a repeated pan the way it would if the ring kept
advancing underneath a "stay this far behind" offset.

`m_liveViewOffset` must be reset to 0 at every place that could otherwise
leave it stale: un-freezing (the toggle handler), a fresh Live run starting
(`startAcquisition()`), any acquisition-mode switch (`setAcquisitionMode()`,
defensively, since the freeze checkbox's own `toggled()` only fires on an
actual state change), and double-clicking the plot while frozen
(`resetZoom()`'s Stream branch — this snaps back to the newest frozen slice
but deliberately does **not** touch Freeze itself, for the same
manual-control reasoning above). If a future change adds another way to
start or stop a Live run, reset it there too, or a later freeze/unfreeze
cycle will silently start from an old offset instead of the live edge.

The Navigator strip is synced directly inside `refreshLiveView()`, the same
way `refreshRecordView()` syncs it directly rather than through the generic
`syncNavigator()` — for the same reason: the pan offset is MainWindow-side
state that `ScopeView` cannot see, so `viewportCentreFraction()` /
`isFollowingTrigger()` have nothing meaningful to report for it.

## Streaming's channel range — two separate bugs behind one noise report, only the second one actually explained it

`applyChannel()` had `if (m_acquisitionMode == ModeStream) m_stream->setChannelRange(...); else m_acq->setRangeMv(...);`
— meaning Record (`ModeRecord`) fell into the `else` and called
`AcquisitionManager`'s block/Capture range setter, which configures a
session Record never actually reads from. This is the same bug an earlier
session already found and fixed once before ("Record mode never sets the
stream input range"), just narrowed back from `!= ModeCapture` (covers
both Live and Record) to `== ModeStream` (Live only) at some point — this
file has no record of when, only that it had regressed. Fixed back to
`!= ModeCapture`. If this narrows to `== ModeStream` again in a future
edit, the same bug returns with it: search for `m_acquisitionMode`
conditions inside `applyChannel()`'s range-setting block specifically, not
just anywhere in the file, since several *other* `== ModeStream` checks
elsewhere in `MainWindow` are correctly Live-only (Live's own ring-buffer
panning path, the "History" capacity check, Live's own rate table) or
already have their own explicit `ModeRecord` branch sitting right next to
them (the status-bar text, the view-reset handler) — this was the one
place where Record's case was silently absent rather than handled.

**That fix was real, but it was not the actual explanation — and taking it
as one would have been a mistake worth naming.** A person reported
Record's trace looking visibly noisier than Live's, which this fix
addressed by giving Record the same range-setting call Live already had.
But the person then corrected the comparison: Live was noisy too, exactly
as much as Record, next to a clean Capture. That single correction
invalidated the whole diagnosis — a fix that only touches Record cannot
explain a symptom that Live has equally, and the honest response to that
was to say so plainly and re-open the investigation, not to look for a
second, smaller reason the first fix might still be relevant.

**The real cause, found once the vendor's own SDK manual was available to
check against: streaming's channel range was being set before the device
was actually switched into streaming mode, not after.** The manual's own
worked example for entering stream mode (section 9.2) calls
`SetOscCaptureMode(dev_id, 1)` *first*, then `SetStreamChannelRangemV()`
per channel, then `StreamCapture()` — in that order. This codebase's own
order was backwards: `MainWindow::startAcquisition()` calls
`applyChannel()` for every channel (which calls
`StreamEngine::setChannelRange()`, i.e. `SetStreamChannelRangemV()`)
*before* calling `m_stream->start()`/`startRecord()` — and
`SetOscCaptureMode(m_deviceId, 1)` is the very first thing `start()` itself
does. So every single range-setting call landed while the device was still
in block/Capture mode from whatever it was doing before, and
`SetStreamChannelRangemV()` never actually reconfigured anything for the
streaming session that was about to begin. Left that way, the hardware's
programmable gain amplifier — the manual is explicit that the whole point
of setting a range is choosing how this amplifier is configured, using
more of the ADC's bits for a smaller signal — stayed wherever it already
was, typically wider and less sensitive than the channel's actual
configured range. A wider range spreads the same physical signal over more
volts per ADC step: more quantization noise, same underlying signal,
exactly the symptom reported, and exactly as present in Live as in Record
since both share this one `start()`.

This also explains why Capture alone was clean, without Capture doing
anything special: `AcquisitionManager` sets its own range
(`SetOscChannelRangemV()`) whenever `applyChannel()` runs for a Capture
channel, and by the time the user is ever in Capture mode the device is
already in block mode — either from app startup, or because leaving
streaming always calls `SetOscCaptureMode(0)` on the way out (see
`StreamEngine::stop()`). Capture's range-setting call was never racing a
mode switch the way streaming's was; it only ever runs after the device is
already in the mode it configures. The asymmetry was in *when* each path's
range call happened to land relative to its own mode switch, not in
anything about the range values, the SDK function used, or which mode was
"correct" — which is exactly why the first fix (Record's condition) missed
it: that fix was about *which* range-setting call gets made, and this bug
was about *when* the call actually takes effect once made.

Fixed in `StreamEngine`, not `MainWindow`: `setChannelRange()` now also
caches the requested range per channel (`m_rangeMinMv`/`m_rangeMaxMv`/
`m_rangeSet`), and `start()` re-applies every channel's cached range
immediately after `SetOscCaptureMode(1)` succeeds, before `StreamCapture()`
is called — reproducing the manual's own order exactly, regardless of when
the caller happened to have requested each range. This keeps the ordering
requirement encapsulated inside `StreamEngine` itself rather than relying
on every future caller of `setChannelRange()` to remember to call it at
the right moment relative to `start()`.

## Record: Temporary/Permanent, and a real Settings page

A first real page behind the "Settings" grid button, which had been a pure
shell until now (`buildToolGrid()`'s generic "not implemented yet" fallback,
same as Decoding/References/Default). `PageSettings` was added at the end
of the `Page` enum (7, after `PageMeasure`) specifically so no existing
page's numeric value shifts — `m_panel`'s `QStackedWidget` index has to
match the enum value exactly (`m_panel->setCurrentIndex(int(page))` in
`openPage()`), so inserting anywhere else would have silently pointed every
later page at the wrong stacked widget.

**Where recordings are saved is now a real setting, not a hardcoded
path.** `recordDirectory()` used to always return
`AppLocalDataLocation/records` (or `~/.vimuscopex/records` as a fallback).
It now reads a `QSettings` key (`settings/recordDirectory`) fresh on every
call, falling back to that same original path when the key is empty —
deliberately not cached anywhere, so a directory changed on the Settings
page takes effect on the very next recording without anything else needing
to know it happened. The Settings page itself is just a label showing
whatever `recordDirectory()` currently resolves to, a `Browse...` button
(`QFileDialog::getExistingDirectory`) that writes the new path and calls
`refreshSettingsPage()`, and a `Use default` button that removes the key
entirely rather than writing the default path back literally — so the
"default" stays whatever `recordDirectory()`'s own fallback computes, even
if that changes later, rather than freezing today's value into the user's
settings file.

**Temporary vs Permanent is a per-recording choice, captured at the moment
a recording starts — not read fresh from the toggle when the app exits.**
The Keep segment buttons (Scope page, Record-only section, right after
Record size) are `m_recordPersistGroup`; what the app actually does at exit
is a separate bool, `m_lastRecordWasTemporary`, set only inside
`startAcquisition()`'s `ModeRecord` branch, and only after
`m_stream->startRecord()` has actually succeeded (a failed start touches no
files, so it must not overwrite whatever choice a previous successful
recording was made under). This was written when every recording still
wrote to the same fixed filenames and only one could exist on disk at a
time, which is no longer true (see the per-recording-folder section below)
— but the reasoning for capturing the choice at start time, not exit time,
holds regardless of how many recordings coexist: reading the toggle's live
state at exit would have meant flipping Keep afterward — just to look at
the page, without recording again — could silently change what a
different, already-finished recording's own choice was, which the person
would have no way to notice until it was already gone. `m_lastRecordDirectory`
is the companion piece this needed once multiple recordings could coexist:
it names exactly *which* recording's folder `m_lastRecordWasTemporary`
describes, since "the last one" is no longer implicit in there only ever
being one on disk.

`RecordWriter::deleteRecording(directory)` is a static helper, called from
`MainWindow::closeEvent()` with `m_lastRecordDirectory` when
`m_lastRecordWasTemporary` is true, right alongside the existing
`saveSettings()` call. It is a plain `QDir::removeRecursively()` on that one
recording's own folder — deleting the whole directory rather than naming
individual files inside it is what let this survive the later move to
per-recording folders (see below) without needing to change at all: whether
a recording is one file or several, "delete this recording" is always
"delete its folder."

The Keep choice itself (which segment is checked, not what any past
recording was made under) persists across launches the same way Record
Size and Sample Rate already do — `scope/recordPersist` in
`saveSettings()`/`restoreSettings()`, defaulting to Permanent (id 1) both
when the key is missing and via the segment's own construction-time
`setChecked(true)`, so a first-ever launch and a launch that predates this
feature both land on the same safe default rather than on whichever
segment id happens to be 0.

## Records: one folder per recording, .vsxrec, mask-aware files, and browsing tabs

A follow-up pass on Record, prompted by three things at once: the `.raw`
extension being generic rather than this app's own, a disabled channel
still getting a file written for it, and every recording overwriting the
last (both true at the time — see the section above, written when that was
still the design). All three turned out to need the same underlying
change: recordings needed real, independent identities on disk, not just
better file names within a shared one.

**Each recording is now its own uniquely-named subdirectory of the record
directory**, from the new `RecordWriter::newRecordingDirectory()` —
`yyyyMMdd_HHmmss`, with a numeric suffix appended on the rare collision (two
recordings started within the same second) rather than ever reusing a name.
Alphabetical sort on that name is also chronological, which is what lets
`listRecordings()` return oldest-first with a plain `QDir::entryList(...,
QDir::Name)` rather than parsing timestamps back out of names to sort by.
`MainWindow::startAcquisition()` calls this once per recording and passes
the result to `StreamEngine::startRecord()` as `directory` — that parameter
already existed and already flowed straight through to `RecordWriter::open()`,
so nothing about the interface between those two changed, only what
`MainWindow` passes for it.

**A channel's file exists only if the channel does.** `RecordWriter::open()`
changed its second parameter from `int channels` (a count, always 0..N-1)
to `unsigned short channelMask` (the actual mask, same bit-per-channel
convention every other mask in this codebase already uses) — the per-channel
loop that used to run `for (ch = 0; ch < channels; ++ch)` now checks
`channelMask & (1u << ch)` and skips the channel entirely, no file created,
when it is not set. `append()`, `read()`, `readDecimated()`, and
`samplesWritten()` needed no changes at all: they already null-check
`m_write[channel]`/`m_reader[channel]` before touching them, so a channel
that never got a file already behaved as "no data here" through the exact
same path a channel that simply had not started writing yet did. The one
small addition was to `append()` itself — an explicit `!m_write[channel]`
check alongside its existing bounds check, so a sample for a disabled
channel is dropped before the wasted work of converting it to a code and
queuing a block that `writerLoop()` would have discarded anyway.

**Metadata a recording could not previously survive without.** `m_scale`
(volts per code, per channel) was always in-memory only — fine when the
same `RecordWriter` instance that wrote a file was the only thing that ever
read it back, which stopped being true the moment browsing an *older*
recording, possibly from a *previous session*, became a feature. `open()`
now writes a `meta.ini` (via `QSettings(path, QSettings::IniFormat)`,
matching the app's own settings-file convention rather than introducing a
new format) into the recording's folder: `record/channelMask`,
`record/sampleRate`, `record/label` (a human-readable time, generated once
at recording time — not reconstructed from the folder name later, so it can
read more naturally than the raw timestamp does), and one
`scale/channelN` entry per channel actually present. `readInfo()` — a
static private helper, not an instance method, specifically so both the
static `listRecordings()` and the instance method `openForReading()` can
call the same one — reads it back. A recording with no `meta.ini` (or one
whose `channelMask` comes back 0) is treated as unreadable rather than
guessed at with a fallback scale, since a wrong guess here would silently
mis-scale an entire recording rather than visibly failing.

**Browsing an old recording reuses `StreamEngine`'s own `RecordWriter`
instance and every accessor that already reads from it, rather than
introducing a second, parallel read path.** `StreamEngine::openRecordForReading()`
calls `m_writer.openForReading()`, then recomputes `m_recordPrimaryChannel`
(the lowest channel actually in the mask — the exact rule `startRecord()`
already uses, extracted to nowhere new, just applied a second time here)
and `m_recordChannels`, and sets `m_recordTarget` to the recording's own
full length so `recorded()` immediately reports it as complete. Every
`readDecimated()`/`recorded()`/`recordMegabytes()` call `MainWindow` already
had for showing the *current* recording keeps working completely unchanged
for an *old* one, because as far as `StreamEngine` is concerned there is no
difference — a `RecordWriter` opened for reading and one that just finished
writing end up in the same state (`m_open == false`, readers populated,
`m_flushed` equal to the full length).

`RecordWriter::openForReading()` guards against calling it on a recording
currently being written by simply not being reachable that way: MainWindow
never calls it on `m_lastRecordDirectory` while `m_stream->recording()` is
true, and the tab bar itself is disabled for the whole time a recording
runs (`startAcquisition()`'s `ModeRecord` branch disables it immediately;
`stopAcquisition()` re-enables it once `wasRecording` is confirmed true).
The tab-changed handler has a second, independent guard on top of that —
`dir == m_lastRecordDirectory` skips reloading — which exists less for
safety (the tab bar being disabled already prevents the dangerous case) and
more so that `refreshRecordTabs()` can freely re-select the active
recording's own tab (to keep it visibly current as its label/position in
the list is re-read) without that re-selection itself triggering a reload
of a file that is still being written to.

**The tab bar never loads anything itself — every caller that wants a
selection to actually load data does so explicitly.** `refreshRecordTabs()`
rebuilds the tab list with `blockSignals(true)` held for the entire
operation, including the `setCurrentIndex()` that restores whichever
recording was previously selected (or falls back to the most recent one).
This was a deliberate choice over letting `currentChanged` fire during the
rebuild: a rebuild happens both when a new recording just started (where
selecting its tab must NOT trigger `openRecordForReading()` on a file still
being written) and when Record mode is simply entered fresh (where the
caller — `setAcquisitionMode()` — DOES want the selected tab's recording
loaded, but wants to do so itself, explicitly, rather than depending on a
signal that a different call site needs suppressed). One function, two
different desired outcomes for the same rebuild, resolved by having neither
outcome be automatic.

## Records, continued: a real exit-safety gap, tab styling, delete buttons, and Open

**A genuine bug, not yet reported as one, found by reasoning through what
"Permanent tabs should actually show their data next launch" requires.**
`closeEvent()` deleted a Temporary recording's folder, and saved settings,
but never stopped an actively-running recording first. If the window closes
while `m_stream` is still mid-write, `RecordWriter::deleteRecording()` could
run `QDir::removeRecursively()` against files the writer thread still has
open (a delete racing an open handle), and — the more likely case in
practice — a *Permanent* recording left mid-write at exit would have its
writer thread torn down by `RecordWriter`'s own destructor during the
ordinary C++ destruction cascade after `closeEvent()` returns, rather than
via `finishWriting()`'s own explicit flush. `close()` (which the destructor
calls) does still flush and close each file, so this was not silently
corrupting data — but relying on destruction order to do the same job
`stopAcquisition()` already does deliberately, in the same way pressing
Stop does, was the wrong thing to depend on. Fixed with one guard at the
top of `closeEvent()`: `if (m_running && m_acquisitionMode == ModeRecord)
stopAcquisition();`, before `saveSettings()` and before the Temporary
delete — so by the time either of those run, whatever was recording has
already been finished exactly the way stopping it by hand finishes it.

**The tab bar had no styling of its own at all — a gap, not a stylistic
choice.** `QTabBar` is not one of the widget types `Theme::globalStyleSheet()`
touches, so it was rendering in plain, unstyled Fusion chrome next to a
UI where every other control has been deliberately restyled. `kRecordTabStyle`
(defined alongside `kGridButtonStyle`/`kToolButtonStyle`, the same
`.arg(Theme::accentHex())` templating convention) matches its colours
exactly to `QComboBox`/`QPushButton`'s own — `#20262e` background, `#333c46`
border, `#4a5560` hover, `#1c2127`/`#262d35` disabled — not just similar
ones, so it reads as the same design rather than a different one that
happens to sit nearby. Applied as a direct `setStyleSheet()` on
`m_recordTabs` itself, not folded into the global stylesheet: `QTabBar` is
not used anywhere else in this app, so there was no reason to risk the
"touching a widget type suppresses native fallback for the rest of it"
gotcha (see the Theme section below) for a change with only one possible
target.

**Close buttons are `QTabBar`'s own built-in `setTabsClosable(true)` /
`tabCloseRequested` mechanism, not a custom per-tab icon.** A manually-built
close button (reusing `Icons::icon("xmark", ...)`, the same glyph the
detail panel's own close button already uses) was the first instinct, but
`setTabsClosable` needs no image resource, no per-tab widget to recreate on
every `refreshRecordTabs()` rebuild, and Qt's own Fusion-rendered close
glyph is legible enough against the dark theme on its own — the extra
control a custom icon would have bought was not worth the extra state to
keep in sync. Confirming before deleting
(`QMessageBox::question`) exists because this is the one action in the
whole Record feature that is not reversible by any other means already
built — Temporary's own deletion at exit at least only removes what the
person explicitly marked disposable at record time; a tab's own close
button can remove a Permanent recording just as easily, with no such
signal that it was expected.

Deleting the *currently displayed* recording's own tab needed its own
small resolution, not just removing the tab: the handler checks whether
the closed directory was `m_lastRecordDirectory` (clearing it if so, so
exit does not later try to delete a folder that is already gone), then
calls `refreshRecordTabs()` and reloads whatever it selected instead —
falling back to clearing the view entirely (`m_recordViewLength = 0`) if no
recordings remain at all. One known, accepted rough edge: deleting the
recording currently on screen unlinks its files while `m_writer`'s own
reader handles may still be open on them; on Linux this leaves the space
unreclaimed until something next calls `close()`/`openForReading()` (which
happens on the very next recording action of any kind) rather than an
immediate reclaim — a real, minor imperfection, not a correctness bug, and
not worth chasing further for how rarely someone deletes the exact
recording they are currently looking at.

**Open (top bar) browses to and loads any of this app's own recordings —
not only ones in the current record directory.** `QFileDialog::getExistingDirectory()`
starting at `recordDirectory()`, then straight into the same
`m_stream->openRecordForReading()` the tab bar itself uses — no new reading
path, same as browsing a tab does not introduce one either. If the app is
not already in Record mode, it switches there first: `m_acqMode->button(ModeRecord)->setChecked(true)`
followed by an explicit `setAcquisitionMode(ModeRecord)` call, since
`setChecked()` alone does not emit `idClicked` (only an actual click does)
— the same two-step every other programmatic mode switch in this file
already needs. `Save` and `Full` are still plain shells; only `Open` was
asked for here.

**Whether a Live/Capture "Save" should use a different file extension than
Record's `.vsxrec`, if it is ever built:** yes, and for the same reason
`.vsxrec` replaced `.raw` in the first place — a saved single-shot snapshot
of whatever is currently on screen (one moment, whichever channels are
visible right then) is a structurally different thing from a Record-mode
recording (however many channels, streamed continuously, its own
`meta.ini`, its own directory) even though both would end up as files this
app wrote. Reusing `.vsxrec` for both would mean either format having to
guess, from a file's contents alone, which kind of thing it actually is
before it could even try to read it — a distinct extension (e.g. `.vsxwave`)
sidesteps that by making the two kinds of file tell each other apart
before either is ever opened. Nothing about Save/Open-for-Capture-and-Live
exists yet; this is a design note for when it does, not a currently
implemented format.

## Records, continued again: the actual bug behind "browsed recordings show nothing", Open's file dialog, and a tab redesign

**The real cause of a browsed recording appearing to show nothing —
"black tabs", stuck on whatever the first tab happened to load, correct
right after recording but broken again next launch — was one line in
`refreshRecordView()`: it decided which channels to read and draw using
`m_channel[ch].mode->checkedId() == ChannelOff`, the LIVE channel rail's own
current on/off state, not the loaded recording's own channel mask.** This
had always been there, and it happened to look correct for as long as
"the recording on screen" and "whatever the rail is currently set to" were
the same thing — true by construction for a recording just made in this
session (the rail's state is what decided its mask in the first place, and
nothing had touched the rail since). It stopped being true the moment
browsing became possible: an *older* recording, or one made with a
different channel selection, or simply a fresh app launch's own default
rail state, has no reason to match. Whenever the two masks did not
overlap at all, every `readDecimated()` call the loop reached was for a
channel the rail called off, `any` stayed `false`, and the function
returned having drawn nothing — while whatever the last *successful* load
had put on screen stayed exactly as it was, since nothing here clears it
on the way out. That combination is "black tabs" (nothing new drawn) and
"stuck on the first tab" (nothing after it ever draws either, so the first
one's success is the last thing visible) at once, from a single cause.

The fix needed a mask actually tied to the loaded recording, not the
rail — `StreamEngine::recordChannelMask()` (a thin new getter, `m_writer.channelMask()`)
gives it. `refreshRecordView()`'s loop now checks that instead, and —
importantly — calls `m_view->clearChannel(ch)` (an existing method,
previously unused for this) for every channel `recordMask` does NOT
include, rather than merely skipping them. Skipping and leaving the OLD
draw in place have the same "no new data" symptom until you look for the
difference: a mask check alone (with no explicit clear) would still fix a
completely-blank recording — nothing overlapping means nothing draws
regardless — but it would not stop a *partial* overlap from showing a
stale channel that belonged to whichever recording was browsed
immediately before, overlaid on the correct ones from the current
recording. Explicit clearing closes both failure modes with the same
change, not just the one that was actually reported.

**Open's file dialog was asking for the wrong kind of thing.** A
recording is a directory internally, so the first version used
`QFileDialog::getExistingDirectory()` — which cannot select a *file* at
all, meaning every `.vsxrec` a person could see inside one showed up
greyed out and unselectable, matching exactly what "I can't open
previously recorded waveforms, it's greyed out" describes, and matching
neither what a directory-picker dialog looks like doing that nor what
"open a .vsxrec file" suggests the interaction should be. Switched to
`QFileDialog::getOpenFileName()` filtered to `*.vsxrec`, letting the
person pick the file the way the extension itself invites, then taking
`QFileInfo(file).absolutePath()` as the recording's directory — the
directory-based loading underneath (`openRecordForReading()`) did not
need to change at all, only what the person actually clicks on to get
there.

**The tab bar's close button was redesigned from "delete this recording"
to "just remove this tab" — a real behaviour change, not only a visual
one, made because closing a tab and deleting the underlying recording are
different actions that deserve different affordances.** The previous
version used `QTabBar`'s own `setTabsClosable`/`tabCloseRequested`, backed
by `RecordWriter::deleteRecording()` and a confirmation dialog. That
mechanism is gone: each tab now gets its own manually-created close
button — a real Font Awesome glyph (`Icons::icon("xmark", kIconColour, 12)`,
the exact same icon and colour the detail panel's own close button
already uses, not a similar one) set via `setTabButton(index,
QTabBar::RightSide, button)` — whose click handler does nothing but
`m_recordTabs->removeTab(...)`. The recording's files are never touched by
it. `refreshRecordTabs()`'s own rescan of the record directory (which runs
on every mode entry and every new recording) will bring a "closed" tab
back the next time it runs, since nothing about closing it changed what is
actually on disk — closing is a per-session, undo-by-rescanning UI action,
deleting (still only via a Temporary recording's own exit-time cleanup) is
the only thing that is actually permanent. Each button captures its
recording's directory by value rather than its tab index, and re-finds its
own current index by searching `tabData()` at click time, since indices
shift under every other tab whenever one is removed or added — capturing
an index directly would silently start closing the wrong tab after the
first removal.

Closing the very last tab needed its own explicit handling in
`currentChanged`: `QTabBar` reports `index == -1` once no tabs remain, and
the handler now clears all four channels (`m_view->clearChannel()`) and
`m_nav->clearAll()` directly rather than calling `refreshRecordView()` —
that function's own `if (m_recordViewLength == 0) m_recordViewLength =
total;` rule means passing it a deliberate "show nothing" of 0 gets
reinterpreted as "show everything `m_stream` still has loaded", the
opposite of what closing every tab is asking for.

**`m_recordTabs->setDrawBase(false)`** removes a stray light line that
appeared between the tabs and the navigator strip directly below them —
`QTabBar` draws that line by default to visually connect itself to a
`QTabWidget`'s content page; with nothing here playing that role, it was
just a line to nothing.

## Records, continued a third time: the real bugs behind "still shows nothing" — the previous fix was necessary but not sufficient

**The previous pass's channel-mask fix was correct as far as it went, but
it was not the whole bug — a person testing it reported every symptom
persisting unchanged, which was the right signal to stop trusting the
earlier diagnosis and go looking for what else could independently cause
the exact same "nothing drawn" outcome.** Two more causes were found,
both structural, both meaning the mask fix alone could never have worked:

**Bug 1 — `refreshRecordView()`'s channel loop was bounded by
`m_stream->channels()`, which is only ever set by `StreamEngine::attach()`,
which only runs when a device is actually connected and `m_acq->ready()`
is true.** Browsing a recording from disk has nothing to do with whether
a scope is plugged in — the whole point of `.vsxrec` + `meta.ini` is that
a recording is self-describing and needs nothing else to read back
correctly. But the loop itself — `for (int ch = 0; ch < m_stream->channels(); ++ch)`
— would never execute at all if `attach()` had not yet run in this
session (a fresh launch, before ever entering a streaming mode with
hardware attached) or would use a stale/zero count otherwise. A correct
mask check inside a loop that never iterates changes nothing. Fixed by
bounding the loop at `RecordWriter::MaxChannels` (a fixed constant, 4)
instead — the mask check already correctly gates which of those iterations
actually draw anything; the bound only needs to cover every possible
channel index, not however many a possibly-unattached device claims to
have.

**Bug 2 — `StreamEngine::openRecordForReading()` never set `m_sampleRate`
at all.** `refreshRecordView()` has its own early return,
`if (rate <= 0.0) return;`, reading `m_stream->sampleRate()` — which
returns `StreamEngine`'s own `m_sampleRate` member, set only by `start()`/
`startRecord()` (an actual acquisition beginning). Loading an old
recording never went through either of those, so `m_sampleRate` stayed at
whatever it was before — 0 on a session where no streaming had happened
yet, which is exactly the "just launched, going straight to Record mode to
browse" case the person was testing. This alone would have blanked the
view regardless of the channel mask being perfectly correct. Fixed at the
source: `RecordWriter` gained its own `m_sampleRate` member, set in both
`open()` (from the rate passed in while writing) and `openForReading()`
(from `meta.ini`'s own `record/sampleRate`, via the same `RecordInfo` the
mask already came from), exposed as `RecordWriter::sampleRate()`.
`StreamEngine::openRecordForReading()` now copies it into its own
`m_sampleRate` right after a successful `openForReading()`, the same way
`recordChannelMask()`'s value already came from `m_writer` rather than
any device-attached state.

Both bugs share one shape, worth naming for whatever comes after this:
**any code path that browsing a recording exercises must trace every
value it reads back to either the recording's own files/metadata or a
fixed constant — never to `StreamEngine`'s own `attach()`-derived state,
which genuinely does describe the currently-attached device and nothing
else.** The first pass's mask fix respected this for the mask itself
(`recordChannelMask()` correctly reads `m_writer`, not the rail); it did
not yet apply the same discipline to the loop bound or the sample rate,
which is exactly why fixing the mask alone changed nothing observable —
two other paths straight past it were each independently sufficient to
blank the view on their own.

**Closed tabs staying closed across restarts, not just for the rest of
the session, needed a small persisted list — closing a tab was not
enough to actually stop it from reappearing.** `refreshRecordTabs()`
rescans the record directory on every mode entry and every new
recording, by design, so that recordings really do turn back up after
being closed *unless something says otherwise*. `records/hiddenTabs` in
`QSettings` (read and written directly, no cached member to keep in sync
— matching how `recordDirectory()` already handles its own setting) is
that "otherwise": `refreshRecordTabs()` reads it once per rebuild and
skips any recording whose directory is in it; the tab's own close button
adds its directory to the list (`setRecordTabHidden(dir, true)`); Open
removes it (`setRecordTabHidden(dir, false)`) on the reasoning that
deliberately opening a recording is exactly the kind of explicit action
that should undo an earlier close. A closed-then-deleted (Temporary,
on exit) recording leaves a harmless orphaned entry in this list — it
simply never matches anything again, since `listRecordings()` will not
find that directory either — not worth cleaning up given how rarely it
would actually accumulate.

## Records, continued a fourth time: a genuine architectural flaw in "what's currently shown", and per-tab view state

**The odd "switching works between 2 tabs only once a 3rd exists" report
could not be reproduced by tracing the code by hand — every trace of the
2-tab case appeared to behave correctly. Rather than keep guessing at a
mechanism, the actual flaw this pass found and fixed is a real
correctness problem regardless of whether it was the exact cause of that
specific symptom: `m_lastRecordDirectory` was being used for two different
questions that only sometimes have the same answer.** Its one real job is
"which recording did this session most recently start, for the Temporary
delete-on-exit decision" — it is deliberately never touched by browsing.
But the tab-switch guard, and `refreshRecordTabs()`'s own "which tab should
be selected" logic, were both also reading it to mean "which recording is
currently being displayed" — a question with a different answer the
moment browsing (a tab click, Open, or simply entering Record mode with
past recordings and nothing recorded yet this session) is involved at all.
Those two questions coincide for as long as "the recording on screen" and
"the recording this session just made" are the same thing, and silently
diverge the moment they are not — exactly the same shape of bug as the
channel-rail mask issue two passes back, just with a different pair of
values quietly standing in for each other.

Fixed by giving the second question its own answer: `m_currentRecordTab`,
updated in exactly one place a load actually succeeds (the new
`loadRecordTab()`, or directly in `startAcquisition()` for a fresh
recording, which is "current" the moment it starts even though it is being
written rather than browsed) and read everywhere something needs to know
what is actually on screen right now — the tab-switch guard, and
`refreshRecordTabs()`'s own tab-selection logic (previously the one
remaining reader of `m_lastRecordDirectory` for this purpose, which caused
a visible tab/data mismatch specifically after Open loaded a recording
that was not made this session: the correct data would load, then
`refreshRecordTabs()` would silently re-select whichever tab matched
`m_lastRecordDirectory` instead, blocked from firing `currentChanged` by
its own `blockSignals`, leaving the highlighted tab and the displayed
recording disagreeing with each other). `m_lastRecordDirectory` keeps its
original, narrower job and nothing else reads it anymore.

**`loadRecordTab()` also consolidates what were three separate,
hand-written copies of "open a recording, reset the view, refresh" —
entering Record mode with past recordings on hand, a tab click, and
Open — into one function.** Three independent copies of the same sequence
is exactly the condition under which one of them quietly drifts from the
other two, which is a plausible contributor to a bug that resisted being
reproduced by tracing any single one of them in isolation.

**Per-tab independence — zoom, per-channel vertical scale/position — is
new, not a bug fix, and scoped deliberately: it covers what is specific to
*viewing* a recording, not the channel's own physical acquisition
configuration (coupling, probe, colour), which stays global by design —
those describe the hardware/display setup, not something that makes sense
to vary per past recording.** `MainWindow::RecordTabViewState` holds one
recording's own view window (`viewStart`/`viewLength`, matching
`m_recordViewStart`/`m_recordViewLength` exactly) and each channel's own
`voltsPerDiv`/`positionVolts` (via two small new `ScopeView` getters
added specifically for this — `voltsPerDiv(int)`/`positionVolts(int)`,
mirroring the existing setters exactly, since nothing previously needed to
read these back rather than only ever set them). `loadRecordTab()` saves
the outgoing recording's current state into `m_recordTabStates` (keyed by
directory) before switching, and restores the incoming one's saved state
if it has visited before this session, or leaves the view at its fresh
default (whole recording, whatever scale is already set) on a first
visit. `voltsPerDiv` of exactly `0.0` in a saved state means "never
actually saved" (a channel that had no data, or a state predating this
save) and is left alone rather than forced to zero, which the plot could
not otherwise use anyway (`setVoltsPerDiv()` itself already rejects
`<= 0.0`). This state is deliberately in-memory only, for this session —
it says nothing about a recording that a later session, or a person who
never revisits it, would need.

## Records, continued a fifth time: per-tab channel visibility, one file per recording, and tab polish

**Channel visibility joins the per-tab state that was already there for
zoom and vertical scale — the same mechanism, one more field.**
`ScopeView` gained `channelVisible(int) const`, matching the existing
`setChannelVisible()` exactly, since nothing had previously needed to read
this back rather than only ever set it. `RecordTabViewState::ChannelState`
gained a `visible` bool alongside `voltsPerDiv`/`positionVolts`, saved and
restored in the same two places (`loadRecordTab()`, and
`startAcquisition()`'s matching save-before-a-new-recording-starts logic)
that already handle the other two fields — there was no reason for
visibility to need its own separate code path when the "save the outgoing
tab, restore the incoming one" shape was already exactly right for it.
Turning CH2 off while viewing one recording now only ever changes that
recording's own saved state; a different tab keeps whatever visibility it
already had, the same way it already kept its own zoom and scale.

**Recordings now write to one file, not one per channel — a real,
structural change to `RecordWriter`, not a bug fix.** Every channel used
to get its own `channelN.vsxrec`; now there is exactly one
`recording.vsxrec` per recording, holding a fixed-size byte region per
active channel, concatenated in ascending channel order. `RecordWriter::open()`
gained a new parameter, `samplesPerChannel` — the reserved capacity per
channel, already available at the call site
(`StreamEngine::startRecord()` already had it, just was not passing it
through) — used to pre-size the whole file
(`activeChannels * samplesPerChannel * 2` bytes) up front, so every
channel's own region is valid to seek into from the very first write
regardless of which channel's data happens to arrive first. This mattered
because blocks for different channels arrive interleaved in time on one
shared queue, not one channel's worth completely before the next begins —
a channel cannot simply be "appended to" the way a per-channel file could,
since its own region sits in the middle of a file another channel is also
writing into.

A new private helper, `channelByteOffset(int channel) const`, computes a
channel's own base offset as the sum of every lower-numbered active
channel's reserved region size ahead of it — the exact same computation
used on both the write side (`writerLoop()`, seeking before every single
write, since the file's shared position moves around under different
channels' blocks) and the read side (`read()`/`readDecimated()`, added to
whatever sample-position offset they already compute), so the two can
never disagree about where a channel's data actually lives. `append()`'s
bounds check changed from `!m_write[channel]` (an array, one slot per
channel) to a single `!m_write` plus an explicit mask check, since there
is now one write handle for the whole file rather than one per channel.
`m_reader` stays an array — each channel keeps its own read handle, all
pointing at the same underlying path, so one channel's own seek position
never disturbs another's; reading is inherently a seek-then-read pattern
already, so per-channel handles cost nothing extra there the way a shared
write position would have.

Flushing changed shape too: a flush covers everything written to the file
so far regardless of channel, so `writerLoop()` now advances every
channel's own `m_flushed` mark together after a flush, not just the one
belonging to whatever block was just written — the previous per-channel-file
version only ever needed to flush the one file a given block belonged to,
since each channel's data lived somewhere else entirely.

**A recording that stops early needed a real length recorded somewhere
other than the file itself, since the file is always pre-sized to the
full reserved capacity regardless of how much of it is genuine data.**
`finishWriting()` now writes each active channel's actual `m_written`
count into `meta.ini` under `written/channelN`, and `openForReading()`
reads it back (via `readInfo()`, which gained two more out-parameters,
`samplesPerChannel` and a per-channel `written` array, alongside the scale
array it already returned) to know where real data actually ends —
treating that written length as the flushed length outright, since a
finished recording has no writer thread left to confirm anything further.
Before this change, "how much did this channel actually write" was
implicit in a per-channel file's own size on disk; with one shared,
pre-sized file, it has to be recorded explicitly instead.

One thing this pass caught in its own draft before it became a build
failure: `__builtin_popcount` (a GCC/Clang builtin already used elsewhere
in this file, no header needed) was briefly written as `std::popcount`
instead — a real C++20 standard library function, but this project targets
C++17 (`CMakeLists.txt`'s own `CXX_STANDARD`), where it does not exist.
Worth remembering for any future channel-mask counting anywhere else in
this codebase: `__builtin_popcount`, not `std::popcount`, until the
project's C++ standard actually changes.

**Tab polish: a dark close icon on the selected tab, and more room from
the tab's own edge.** The selected tab's background is the same bright
accent green every other checked control in this app already uses, and
the close button's default light icon read very poorly against it — the
same reason the Instruments mode segments already swap their own icon to
black when checked (`#10180a`, the identical colour, reused here rather
than a similar one). `updateRecordTabCloseIcons()` walks every tab's own
close button (retrieved via `tabButton(i, QTabBar::RightSide)`, cast back
to the `QToolButton` it always is) and sets whichever one matches
`currentIndex()` to the dark icon, everything else to the normal light
one — called after a full rebuild, after the selection itself changes,
and after a tab is removed, since any of the three can change which tab
needs which colour. `kRecordTabStyle`'s own tab padding grew from 6 px to
12 px on the right specifically, giving the close button real breathing
room from the tab's visual edge rather than sitting flush against it,
without changing anything about the left/top/bottom padding that was
already correct. That still was not enough room, by report — grown again
to 20 px in a later pass; if this needs another look, the tab's own
`padding` in `kRecordTabStyle` is the one place to change it, not the
close button's own size or position.

## Instruments: Spectrum, XY, and Persistence restricted to Capture mode

Live and Record both continuously produce new samples — a stream that
never settles into one fixed thing to analyze — while Spectrum, XY, and
(whenever it gets built) Persistence all analyze a single, static capture:
an FFT of one snapshot, a Lissajous figure from one pairing, an overlay of
repeated triggered captures. Available now only in Capture mode; disabled,
not hidden, on the other two of the Instruments segment buttons
(`m_instrumentMode->button(1..3)`, Spectrum/XY/Persistence — button 0,
Scope, is always available), so the choice is still visibly there rather
than vanishing, just not clickable from Live or Record. A tooltip on each
of the three explains why, set once at construction rather than swapped in
and out with the enabled state, since the reason itself never changes.

Switching away from Capture while one of the three was active forces the
Instruments selection back to Scope — `setChecked(true)` on the Scope
button plus an explicit `setInstrumentMode(InstrumentMode::Scope)` call
right alongside it, the same two-step every other programmatic mode
switch in this file already needs, since `setChecked()` alone never emits
`idClicked` the way an actual click does. Switching back to Capture simply
re-enables all three again; nothing forces a selection in that direction,
since Scope being current is never a problem to leave alone.

This intentionally narrows something that used to work in Live and Record
too — `feedSpectrum()`/`feedXY()` were, and still are, wired into all
three acquisition modes' own sample-arrival call sites, so the two
features have not been removed at any lower level, only made unreachable
through the Instruments selector outside Capture. If a future request asks
for FFT-of-a-recording specifically (analyzing a finished recording's own
frequency content, as distinct from a live or currently-recording signal),
that plumbing is still intact underneath and only the Instruments-selector
restriction above would need to make an exception for it.

## Record's overview strip, a drag-to-the-edge zoom bug, Live's axis not following a pan, and the close button's actual position

**The overview strip in Record mode was showing the current page, not the
recording — a real, confirmed bug, not a rendering quirk.**
`NavigatorView`'s own header comment says exactly what it is supposed to
be: "the entire capture drawn compressed, with the part currently on
screen highlighted." `refreshRecordView()`'s own comment on the line
feeding it — `m_nav->setSamples(ch, samples); // overview of the same
slice` — admitted the actual bug in so many words: `samples` here was the
same windowed, decimated read already made for the main plot
(`m_recordViewStart` through `span`), not a separate read of the whole
recording. Capture mode never had this problem because there `samples`
already *is* the whole capture — a capture is one fixed-size block
acquired in one shot, so there is no separate "current page" to
distinguish it from. Fixed with a second, separate `readDecimated()` call
per channel spanning `0..total` (the whole recording) at a small,
fixed `kOverviewPoints` (1000) — cheap regardless of how large the
recording actually is, since `readDecimated()`'s own window-sampling
fallback already bounds the I/O for a span too large to read wholesale —
fed to `m_nav->setSamples()` instead of the page-local `samples`.
`setViewport()` right below was already correct and untouched: it was
already being given the true `(start, span)/total` fractions, so once the
strip itself is drawn from the whole recording rather than the current
page, the highlighted rectangle lands exactly where it always should have.

**Dragging that rectangle to the right edge produced an unclamped
`m_recordViewStart` that let the visible span shrink — which reads as an
unexpected zoom, not a plain pan, and let the rectangle spill past the
strip's own edge.** `NavigatorView::centreRequested`'s handler computed
`m_recordViewStart` from the dragged centre fraction using only
`std::min(..., total - 1)` as a bound — clamping the *start point* to stay
inside the recording, but not clamping it so the *window* (`m_recordViewStart`
+ `m_recordViewLength`) also stays inside. Near the very end of a
recording this let `total - m_recordViewStart` — the actual span
`refreshRecordView()` can show, since it can never show more than exists
from the start point onward — narrow well below `m_recordViewLength`, so
the visible time range on screen shrank the closer the drag got to the
right edge. Fixed by clamping the start point itself: whenever the
recording is longer than the configured view length, `start` is bounded
by `total - m_recordViewLength`, so the window keeps its full configured
width and simply stops panning further right once that width would reach
exactly to the end — never shrinks, and the rectangle it produces via
`setViewport()` never extends past the strip's own bounds either, since
both symptoms shared this one root cause.

**Live mode's time axis never actually knew a pan had happened — it
always showed the same fixed "-N to +N" range regardless of how far back
in history the pan had gone, unlike the vendor's own app, where the grid
moves with the waveform during a drag.** `ScopeView` has no real concept
of a trigger to centre on in Live mode (`setSamples(ch, samples, rate,
-1.0)` — the `-1.0` is "no trigger"), so `drawAxes()`'s trigger-centred
branch (`if (... m_triggerPoint >= 0.0)`) never actually applied there at
all — `offsetDivs` stayed at its default of 0 no matter what
`m_liveViewOffset` (the live pan state, tracked entirely in `MainWindow`,
never previously communicated to `ScopeView` at all) was set to.
`refreshLiveView()` now sets `m_view->setAbsoluteTimeOrigin()` — Record's
own mechanism for a plot with nothing to centre on — to a negative time
whenever panned (`m_liveViewOffset > 0`, which per the existing invariant
is only ever true while frozen): the window's left edge sits `wanted +
m_liveViewOffset` samples before "now," expressed as negative time since
everything on screen already happened. Cleared back to the ordinary
trigger-centred axis (`clearAbsoluteTimeOrigin()`) once `m_liveViewOffset`
returns to 0, i.e. showing the newest data again, so the normal
(meaningless-in-Live-anyway, but unchanged) axis behaviour is untouched
whenever nothing is actually panned. Capture mode was checked and needs no
equivalent fix: its own pan (`ScopeView::panBySamples()`) already updates
`m_viewOffsetSamples` directly, and `windowStart()` — which `drawAxes()`'s
trigger-centred branch already reads from — already incorporates that
offset, with a genuine, valid `m_triggerPoint` (unlike Live's `-1.0`)
making that branch apply correctly there already. Record mode was also
checked: `refreshRecordView()` already sets `setAbsoluteTimeOrigin()` from
`m_recordViewStart` on every call, waveform and axis together, from both
of its own drag paths (the plot's own `panRequested` and the navigator
strip's `centreRequested`) — so nothing there needed changing either; the
bug was specific to Live.

**The tab's close button needed an actual repositioning mechanism, not
more padding on the tab.** The previous two attempts increased
`kRecordTabStyle`'s own `QTabBar::tab` right padding (6px, then 12px, then
20px) on the theory that a tab's QSS padding governs where everything
inside it sits — true for the text label, but `QTabBar` positions a
`setTabButton()` widget flush against the tab's own edge independent of
that padding, which is exactly why none of those three attempts changed
where the button actually was. The close button is now wrapped in a small
plain `QWidget` container (its own `QHBoxLayout`, a 4px right margin, the
button as its only child) passed to `setTabButton()` in place of the
button itself — a margin on a container `QTabBar` positions, rather than
on the button `QTabBar` positions, actually moves it. `updateRecordTabCloseIcons()`
had to change to match: `tabButton()` now returns the container, not the
button, so it looks for the button as a child (`findChild<QToolButton*>()`)
when a direct cast comes back null, rather than assuming the returned
widget always is the button.

## The time grid redesigned to actually move with the waveform, and an unresolved pan bug

**The previous pass's axis fix only changed which time value each label
showed — the grid lines themselves, and the label positions, stayed at
fixed screen locations. That was a real, useful improvement on its own,
but it fell short of what was actually asked: the whole time grid moving
together with the waveform during a pan, "glued to it" as the person put
it, matching the vendor's own app rather than a fixed set of gridlines
with only the numbers underneath them changing.** This pass redesigns
both `drawGraticule()`'s vertical lines and `drawAxes()`'s label
positions to actually slide.

A new shared helper, `ScopeView::timeAxisOffsetDivs()`, extracts exactly
the offset computation `drawAxes()` used to do inline (the trigger-centred
case, and the record's-own-absolute-origin case) with no change to its
logic — moved out only so `drawGraticule()` can use the identical
computation and the two can never disagree with each other about where a
given time value actually falls.

Both functions now walk integer "how many whole divisions from the
reference point" values (`k`) rather than fixed division indices (`i` from
0 to `HorizontalDivs`), and compute each one's actual screen position as
`i = k - offsetDivs + HorizontalDivs/2` — a division-fraction that shifts
continuously as `offsetDivs` changes during a pan, rather than the fixed
integers 0..10 the old code walked. `drawGraticule()`'s minor (dotted)
vertical lines use this for their own x-positions; `drawAxes()`'s labels
use the identical `i` for theirs, and `k * step` for the label text
itself (`step` being one division's worth of time in whatever display
unit is active) — so a label is always directly under its own gridline,
both computed from the same `k`, never independently.

The dashed "time zero" reference line (previously always drawn at the
screen's physical centre, `plot.center().x()`) now draws at wherever time
zero actually falls (`zeroI = -offsetDivs + HorizontalDivs/2`), and simply
does not draw at all when that point has panned out of view — it only
happened to coincide with the physical centre before because the view was
never panned relative to its own reference point. Its own 1/5-division
tick marks (perpendicular ticks along both centre references) now
originate from wherever this line actually is, rather than from the fixed
centre, for the same reason. The horizontal (voltage) centre reference and
its own ticks are untouched — voltage has nothing to pan against, and
stays exactly as before.

Checked and confirmed backward-compatible for the ordinary, unpanned case:
with `offsetDivs == 0`, walking `k` from `-HorizontalDivs/2` to
`+HorizontalDivs/2` produces exactly the same 11 fixed division positions
(0 through `HorizontalDivs`) the old fixed-index loop did, by construction
— this redesign changes nothing about the unpanned display, only what
happens once `offsetDivs` becomes nonzero.

This lives entirely inside `ScopeView`'s own paint logic, so it applies to
all three acquisition modes uniformly without any of them needing their
own separate change — Capture and Record already fed `timeAxisOffsetDivs()`
(via `windowStart()`/`m_triggerPoint`, or `m_absoluteTimeOrigin`) correctly
during their own pans before this pass even started; the earlier pass's
Live-mode fix (setting `m_absoluteTimeOrigin` from `m_liveViewOffset`)
automatically benefits from this redesign too, with no further changes
needed there.

**The reported inability to pan a previously-recorded signal directly on
the plot (only the overview strip working) was investigated at length and
not conclusively resolved this pass — worth recording exactly what was
checked and ruled out, so a future pass does not repeat the same dead
ends.** Confirmed NOT the cause: `ScopeView::mousePressEvent()`'s panning
branch (`m_panning = true`) has no mode-specific gating and is reached
whenever the click is inside `plotRect()`, which is purely geometric (no
dependency on recording/hardware state); `plotRect()`, `windowStart()`
and `StreamEngine::recorded()`/`m_recordPrimaryChannel` all read correctly
for a browsed (not actively-written) recording, traced end to end;
`m_view` is never disabled anywhere in `mainwindow.cpp`; `m_zoomMode`
defaults false and is only ever toggled by the dedicated Zoom button, with
no automatic code path that could leave it stuck on; and the pan
debounce's own timeout handler (`m_recordPanDebounce`) is mode-agnostic
and re-arms correctly. One remaining, unconfirmed hypothesis worth
checking first: whether the person had the top bar's **Zoom** toggle
switched on at the time — `mousePressEvent()` routes every drag on the
plot into a zoom-box selection instead of a pan whenever it is, and if
this genuinely is what happened, it would explain the exact symptom
(direct drag not panning, the navigator strip's own separate drag path
unaffected since it is a different widget with its own mouse handling)
without being a bug in the pan logic itself.

## The time-grid redesign was broken on arrival — two real bugs, not a design disagreement

**The previous pass's redesign was reported as not working in any of the
three modes it was supposed to cover, and that report was correct — this
was not the person wanting something different from what got built, it
was the thing that got built not actually working.** Two separate,
concrete bugs, found by working through the arithmetic by hand rather than
continuing to guess:

**Bug 1 — the k-range formula had the sign of `offsetDivs` backwards.**
`drawGraticule()` and `drawAxes()` both walk integer "how many whole
divisions from the reference point" values (`k`), converting each to a
screen position via `i = k - offsetDivs + HorizontalDivs/2.0`. To find
which `k` values land inside the visible plot (`i` between 0 and
`HorizontalDivs`), solving that equation for `k` at the two edges gives
`k = offsetDivs ± HorizontalDivs/2.0` — but the range these two functions
actually computed was `firstK = floor(-HorizontalDivs/2.0 - offsetDivs) - 1`,
`lastK = ceil(HorizontalDivs/2.0 - offsetDivs) + 1`: `offsetDivs` carries
the wrong sign in both. The bug is invisible exactly when `offsetDivs == 0`
— `-offsetDivs` and `offsetDivs` are the same number there — which is
exactly the ordinary unpanned case this was checked against before
shipping it, and exactly why that check passed while the real thing was
still broken. It stops being invisible the moment `offsetDivs` is
anything else: Record mode's own convention (`m_absoluteTimeOrigin >= 0`,
time counts up from the plot's own left edge) puts `offsetDivs` at
`HorizontalDivs/2.0` — 5.0, for a 10-division plot — as its *resting*
value from the very first frame of a fresh recording, before any pan at
all, which is exactly "no grid in Record mode in the first place." Capture
mode zoomed or panned away from the trigger, and Live panned back through
history, both push `offsetDivs` away from 0 the same way, just less
severely and less immediately — "the grid disappears when zooming in
Capture mode" is the identical bug, arrived at by a smaller but still
nonzero `offsetDivs`. Fixed by swapping the sign: `firstK = floor(offsetDivs
- HorizontalDivs/2.0) - 1`, `lastK = ceil(offsetDivs + HorizontalDivs/2.0) + 1`
— re-derived from the same equation rather than patched by trial and
error, and checked by hand against `offsetDivs` at 0 (recovers exactly the
previous fixed 11-position layout), 5 (Record mode's own resting value,
now correctly producing a full 9-line grid), and 3 (an arbitrary
Capture-mode pan, also a full grid) before considering it settled.

**Bug 2 — the earlier pass's own Live-mode fix, from two passes back, never
actually took effect, on top of the above.** `m_absoluteTimeOrigin`'s
established convention was "negative means not set" (`m_absoluteTimeOrigin
= -1.0` as the sentinel). That fix computed a genuinely negative origin for
Live mode panned back before "now" — correctly, since everything shown
while panned back already happened, so its time really is negative — and
handed it to `setAbsoluteTimeOrigin()`, which stored it verbatim. Every
reader of that value, including the newly-added `timeAxisOffsetDivs()`,
checked `m_absoluteTimeOrigin >= 0.0` to decide whether an origin was even
set — so a real, negative origin was silently indistinguishable from "no
origin was ever set," and Live mode fell through to the trigger-centred
branch every time, which (Live has no real trigger — `setSamples(...,
-1.0)`) always returns an offset of exactly 0. The fix from two passes
back never worked at all; only the previous pass's own testing gap (an
unpanned check, same as Bug 1's) let it ship unnoticed. Fixed by replacing
the sign-as-sentinel convention with an explicit `m_hasAbsoluteTimeOrigin`
bool, set by `setAbsoluteTimeOrigin()` and cleared by
`clearAbsoluteTimeOrigin()`, checked instead of the value's sign
everywhere `m_absoluteTimeOrigin` is read (`timeAxisOffsetDivs()`, and
`sampleIndexToTime()`, a second, previously-unnoticed reader with the
same sign check and the same latent bug against a negative origin, caught
only by searching for every remaining use of the old convention rather
than assuming the two sites already fixed were the only ones).

**Take-away for whoever touches this code next: "works when unpanned" is
not evidence this class of change works, since every one of these bugs is
specifically invisible in that case** (`offsetDivs == 0` for Bug 1;
`m_absoluteTimeOrigin` not yet set at all, so its sign never gets
exercised, for a fresh reproduction of Bug 2's shape) — check a nonzero
pan, a fresh Record-mode recording, and a Capture-mode zoom away from the
trigger before considering any change to this offset/origin machinery
settled, not only the resting state.

## A third Live-mode bug: a discontinuity right at the pan boundary — still not working after the first two fixes

**Bugs 1 and 2 above were real, but Live mode was reported still broken
after fixing both, and it was: a third, separate bug, in the same area but
a different mechanism — conditionally choosing between two different
reference points for "where is time zero" depending on whether the view
happened to be panned.** `refreshLiveView()` called
`setAbsoluteTimeOrigin()` only when `m_liveViewOffset > 0`, and
`clearAbsoluteTimeOrigin()` otherwise (falling back to the ordinary
trigger-centred axis). Those two branches do not describe the same
reference point at all: the trigger-centred fallback (now correctly fixed
by Bug 2, but still a *different formula*) places "time zero" at the
screen's centre — an arbitrary position with no physical meaning for a
signal with no real trigger, which is all Live mode has (`setSamples(...,
-1.0)`, unconditionally) — while the absolute-origin formula places "now"
at wherever the newest sample actually is, which (worked out on paper)
lands at the plot's right edge specifically. These are a full half-screen
apart. The instant a pan began — `m_liveViewOffset` going from 0 to
anything else — the axis's reference point silently jumped from one to
the other, discontinuously, by exactly `HorizontalDivs/2` divisions. That
jump, not a residual sign or flag bug, is what "still not working" was
describing: from the person's side this looks exactly like "the grid
isn't following," just triggered at the start of a pan rather than
throughout it.

Fixed by removing the conditional entirely: `setAbsoluteTimeOrigin()` is
now called unconditionally, every time `refreshLiveView()` runs, at
`m_liveViewOffset == 0` included. Worked out on paper once more before
calling it settled: at `m_liveViewOffset == 0`, this formula places "now"
exactly at the plot's right edge (`zeroI == HorizontalDivs`) — correct,
since the rightmost sample shown genuinely is the newest one — and moving
`m_liveViewOffset` up from there pushes "now" *past* the right edge
(`zeroI > HorizontalDivs`), correctly off-screen, since panning back means
the newest sample is no longer in view at all. No discontinuity anywhere
in between: the reference point moves continuously with `m_liveViewOffset`
across the entire range, not just once it crosses zero.

This is a different failure mode from Bugs 1 and 2 above, and worth
keeping distinct in mind for next time: those two were "the formula itself
is wrong" (a sign error; a value silently discarded). This one was "two
different formulas, individually correct in isolation, disagree about
where zero is" — a seam between two code paths rather than an error
within either one. Checking a nonzero pan (per the take-away two sections
up) would have caught Bugs 1 and 2, but would not by itself have caught
this one, since a single fixed pan amount never crosses the seam at
`m_liveViewOffset == 0` — the boundary itself, not just any nonzero point
past it, needed checking directly.

## A change that assumed the wrong thing and was reverted: the overview strip's Live-mode drag was already working

**The previous pass diagnosed the overview strip's Live-mode drag as
"never actually driving the pan mechanism" and rewrote it to require
Freeze and update `m_liveViewOffset` directly, on the theory that the
existing `m_view->setViewCentreFraction(f)` call was meaningless for Live
mode — reasoning that Live's buffer is always exactly the size of the
window shown, leaving nothing for that kind of in-place panning to move
within. That reasoning was never actually verified against how the
overview strip was really being used, and it was wrong: the person
reported that the overview strip's drag was genuinely working correctly
before this change, and panning by dragging the plot directly was the one
that was not — the previous pass's fix broke the one that worked without
fixing the one that did not.**

Reverted outright, back to the plain `m_view->setViewCentreFraction(f);
syncNavigator();` fallthrough for every mode outside Record, exactly as it
was before that pass touched it. The most likely explanation for why it
was genuinely working — not confirmed, since nothing here should be
asserted without checking it directly against the running app first — is
that `ScopeView`'s own buffer is not always exactly window-sized the way
the previous pass assumed: zooming in (the ordinary drag-a-box zoom,
independent of Live's own history panning) shrinks `windowLength()` below
`m_lastLength` while `refreshLiveView()` still passes the full `wanted`
samples, leaving real room for `setViewCentreFraction()`'s own in-place
panning to work within. If that is right, the overview strip's drag was
never meant to reach further back into history via `m_liveViewOffset` at
all — it was panning within whatever is currently zoomed into, a
genuinely different and already-correct feature, which the previous
pass's fix replaced rather than left alone.

**The original report — panning by dragging the plot itself does not
work in Live mode — is still open, and is not resolved by this revert.**
It was never actually diagnosed on its own terms; the previous pass
jumped to changing the overview strip's mechanism instead. Worth checking
directly rather than guessed at further: whether the person testing it is
frozen at the time (the plot's own `panRequested` handler for
`ModeStream` requires it, by existing, correct, unchanged design — an
unfrozen ring buffer would overwrite whatever a drag revealed on its very
next tick), and whether zooming in first changes anything, given the
hypothesis above about what makes the overview strip's own mechanism work
at all.

## The actual Live-mode pan bug, found from screenshot evidence: two functions read the wrong source for the current zoom level

**Screenshots settled what code reading alone could not. The Scope card's
own top-bar readout said "1 s/div"; the status strip under the plot,
drawn straight from `ScopeView`'s own `m_secondsPerDiv`, said "25.12
ms/div" — a ~40x mismatch, on the very same view at the very same
moment.** Confirmed with the person: Zoom was off, dragging still did
nothing, and the axis labels' own spacing (10 divisions spanning ~250 ms,
not 10 seconds) matched the smaller, "hidden" value — `ScopeView` really
was displaying a narrowed-down zoom, silently, with the dropdown never
updated to reflect it. `zoomToRect()` (the drag-a-box zoom, confirmed to
apply to Live mode same as Capture) narrows `ScopeView`'s own
`m_secondsPerDiv` directly and has no reason to touch the timebase
dropdown at all — the two are independent controls by design, one for
"the timebase setting chosen," the other for "how far zoomed in right
now," and nothing before this was reading the second one where it
actually mattered.

Both `refreshLiveView()` and `panRequested`'s `ModeStream` branch computed
`windowSamples` (the size of the currently-displayed window, in samples)
from `m_timebase->currentData().toDouble()` — the dropdown's own nominal
value — rather than `m_view->secondsPerDiv()`, `ScopeView`'s actual
current one. Ordinarily these agree (`applyTimebase()` sets both to the
same value whenever the dropdown changes), which is exactly why this went
unnoticed through every previous pass at this: none of them zoomed in via
`zoomToRect()` first. The moment they diverge, `windowSamples` comes out
far larger than what is actually on screen — in the reported case, using
"1 s" worth of samples (500,000 at 50 kS/s) where the real window was
"25.12 ms" worth (~12,560) — and `maxOffset = max(0, available -
windowSamples)` can then collapse to exactly 0 whenever the ring buffer's
own history (sized off the same, correct "History" setting, unrelated to
zoom) is smaller than this inflated `windowSamples`. Once `maxOffset` is
0, `m_liveViewOffset` clamps to 0 regardless of drag distance or
direction — a pan that silently does nothing at all, which reads exactly
as "fixed in place, dragging it left and right has no effect," reported
with no error and no visual feedback of anything having gone wrong.

Fixed by reading `m_view->secondsPerDiv()` in both places instead. Both
call sites' comments now point at each other and at this section, so a
zoomed-Live-mode pan bug does not need rediscovering from scratch if
either one regresses. Left deliberately untouched: the Scope card's own
top-bar display, and the settings persisted on exit, both still read the
dropdown's nominal value on purpose — those describe "the timebase the
person chose," not "what is currently zoomed to," and conflating the two
would be a different, unrequested change to what those displays mean, not
a fix.

This took two prior passes (the reverted overview-strip rewrite, and
the still-open "just check whether Zoom is on" question) to actually
reach, and both of those were reasonable hypotheses to rule out — but
neither was checked against the running app before being acted on. What
finally cracked it was concrete evidence (two numbers on screen that
should have matched and did not), not a further theory about the
mechanism. Worth carrying forward: when a report keeps surviving each
theory in turn, asking for what is actually on screen is worth more than
another round of code reading.

## Four refinements: Stop as well as Freeze, a reversed drag, the overview strip's Live drag rewired for real this time, and per-mode state

**Panning now also works when Stopped, not only Frozen.** Both
`panRequested`'s `ModeStream` branch and the new `centreRequested` branch
below changed their gate from `!m_stream->frozen()` to `!m_stream->frozen()
&& m_running` — blocking a drag only while actually running and unfrozen,
since that is the one state where the very next tick's "show the newest"
pull would overwrite whatever the drag revealed. Both Freeze and Stop
leave the ring genuinely static, for different reasons (Freeze: `ingest()`
stops appending; Stop: nothing is running to append at all), so both now
correctly allow it.

**The plot's own drag direction was reversed.** `panRequested`'s
`ModeStream` branch computed `shift = -dxFraction * windowSamples` — the
negation was backwards. Checked with a "dragging a piece of paper"
intuition: dragging left should reveal whatever was to the right of the
current view, and "right" on this timeline is newer (`m_liveViewOffset`
closer to 0) — so a left drag should *decrease* the offset, not increase
it. Removed the negation (`shift = dxFraction * windowSamples`); verified
both directions against the same intuition before considering it settled,
not just the one that was reported.

**The overview strip's Live-mode drag is rewired again — this time with
the actual root cause (the previous entry above) already fixed, and
confirmed necessary by the person rather than assumed.** The generic
`m_view->setViewCentreFraction(f)` fallthrough was reinstated last pass
after breaking a case that happened to work while zoomed; this pass the
person confirmed directly that, unzoomed, it does not work at all — "gets
maximized, drags nothing" — matching the original diagnosis exactly once
the zoom-mismatch bug was out of the picture. `centreRequested` now has a
real `ModeStream` branch, structurally identical to `panRequested`'s (same
Frozen-or-Stopped gate, same `m_view->secondsPerDiv()` — not the dropdown
— for `windowSamples`), computing `m_liveViewOffset` directly from the
absolute target fraction by inverting `refreshLiveView()`'s own forward
formula (`centre = (available - offset - span/2) / available`, solved for
`offset`) rather than accumulating a delta the way the relative-drag
handler does. The previous revert's caution was correct given what was
known then; this time the fix rests on a confirmed bug fix underneath it
rather than an unverified assumption.

## Per-mode state: each of Capture, Live, and Record now remembers its own timebase, pan position, and (Capture) its own last waveform

**The timebase-reverting report — set 1 s/div in Live, switch to Capture
and back, land on 100 ms/div — traced to actual, named constants, not
inferred.** `kTimebases` (Capture and Record's list) runs from 1 µs to
`1e-1` (100 ms) — it does not go up to 1 second at all.
`kRollTimebases` (Live's own, separate list) runs from 0.1 s to 5 s.
`refreshTimebaseOptions()` tried to preserve "whatever the combo currently
shows" by nearest-match across a mode switch: leaving Live at 1 s and
entering Capture found no exact match in `kTimebases` and substituted the
closest available value — 100 ms, `kTimebases`'s own maximum, which is
also the closest thing to 1 s that exists there. Switching back to Live,
100 ms *is* an exact match in `kRollTimebases` (it is the list's own first
entry), so the substitution was silently and permanently adopted as if it
had always been Live's own setting, with a genuine 1 s nowhere left to
recover.

Fixed by giving each mode its own remembered value instead of sharing one:
`m_captureTimebase` and `m_liveTimebase` (Record does not use this
mechanism at all — see `applyTimebase()`'s own early return).
`refreshTimebaseOptions()` now restores the mode being switched *into*
its own value; `applyTimebase()` stores whatever the combo changes to into
whichever member matches the *current* mode. Also fixed the same bug's
cross-restart form, which a pre-existing code comment had already
correctly diagnosed without actually fixing ("quitting at 1 s/div in Live
came back as 10 µs/div") — `saveSettings()`/`restoreSettings()` now
persist both values under their own keys (`scope/captureTimebase`,
`scope/liveTimebase`), with the old shared `scope/timebase` key read once,
only as Capture's own fallback default, for a settings file saved by a
version before this split.

**`m_liveViewOffset` (Live's own pan position) no longer resets on a mode
switch** — only when Freeze comes off or a fresh Live run actually starts
(both mean the ring it was measured against is gone or moving again),
which are the cases where resetting it is actually correct; a plain mode
switch is not one of them anymore.

**Capture mode now remembers its own last waveform.** A new
`m_captureSnapshot` (per-channel samples, sample rate, trigger point,
populated continuously inside the `samplesReady` handler regardless of
which mode happens to be active when a capture actually finishes) is
restored into `ScopeView` — `setSamples()`, the navigator, and (since
Instruments' Spectrum/XY are Capture-only anyway) `feedSpectrum()`/
`feedXY()` — every time Capture mode is switched back into. Before this,
`ScopeView`'s own buffer is shared across all three modes, so whichever
mode ran most recently simply overwrote it; switching to Live or Record
and back to Capture showed whatever Live or Record had last put there,
not the actual last capture. A channel with no data yet
(`m_captureSnapshot.valid[ch]` false) is explicitly cleared rather than
left showing a stale trace from before this mechanism existed.

**Deliberately left unaddressed this pass, worth flagging explicitly
rather than leaving silently incomplete: Capture's own zoom/pan state
(`ScopeView`'s internal `m_viewOffsetSamples`) is not part of what
persists across a mode switch, only the waveform data and the timebase
are.** `ScopeView` does not currently expose a getter for this internal
offset the way it does for `secondsPerDiv()`, `voltsPerDiv()`, and the
others added over the past several passes — extending the same pattern
(a getter, captured into `CaptureSnapshot` alongside the samples, restored
after `setSamples()`) would close this gap if it turns out to matter in
practice.

## Cross-mode display leakage, Live's own sample rate, and a full rebuild of Live-mode zoom

**A capture's waveform showing up in Live and Record after switching away
from Capture — with Live's own trigger-less axis drawn under it, which is
what looked like "the grid is centred on zero" — traced to a gap in the
previous pass's own per-mode restore work: it added a restore path for
whichever mode a snapshot existed for, but never an explicit path for the
case where the mode being switched into has no snapshot yet.**
`setAcquisitionMode()`'s Capture branch restored `m_captureSnapshot` when
it had data, but did nothing at all otherwise — leaving whatever `ScopeView`
already held on screen, since `ScopeView`'s own buffer is shared across
all three modes and nothing clears it on a plain mode switch. Live had no
branch of its own at all. Record's "no recordings in this directory" case
fell through the same way — `loadRecordTab()` simply never got called,
and nothing else stepped in either. All three now explicitly own their
entry into a mode: restore that mode's own last state if it has one, or
clear every channel and the navigator to genuinely empty if it does not —
never falling through to leave whatever the previous mode displayed. Live
gained its own snapshot (`m_liveSnapshot`, the same shape as
`m_captureSnapshot`, populated continuously inside `refreshLiveView()`)
for exactly this purpose.

**Live's sample rate was never its own — it derived automatically from
whatever Capture's own timebase happened to produce, since both fed the
same shared `AcquisitionManager`.** `applyTimebase()`'s `ModeStream`
branch used to auto-pick a rate from Live's own supported-rate table,
sized to keep the screen legible for the current timebase window — a
reasonable idea in isolation, but it meant Live's actual rate tracked
whatever Live's *timebase* was set to, with no way to choose a rate
independently of that, and every mode switch back to Capture stamped the
shared `AcquisitionManager` with Capture's own derived rate regardless of
what Live had been using. A new `m_liveRate` combo — built exactly like
Record's own `m_recordRate` (the same `kSampleRates` list, the same
nearest-match-to-device-supported-rates snap) — replaces the
auto-derivation entirely: Live's rate is now chosen directly, the same way
Record's already was, persisted the same way, and — since every mode
re-applies its own rate to the shared `AcquisitionManager` on every entry
(`applyTimebase()` for Capture/Live, `applyRecordRate()` for Record) —
sharing that one underlying variable across modes causes no actual
cross-contamination in practice, as long as each entry keeps re-asserting
its own value, which they all now do.

**Live-mode zoom was rebuilt from the ground up — the previous mechanism
never actually worked for Live at all, and both remaining symptoms (the
overview strip not updating, and panning afterward snapping to the end of
the buffer) trace to the same root cause.** `zoomToRect()`'s existing
"Capture and Live" branch operates entirely on `ScopeView`'s own internal,
buffer-relative pan/zoom state (`setViewCentreFraction()`) — correct for
Capture, whose buffer is static once a capture finishes, but not for Live:
since an earlier pass fixed `refreshLiveView()` to fetch exactly
`secondsPerDiv() * HorizontalDivs * rate` samples (matching the zoom
exactly, with no slack), any offset computed relative to the *current*
buffer becomes meaningless the moment the *next* fetch grabs a
differently-sized one — which happens on the very next `refreshLiveView()`
call, natural tick or otherwise. Two consequences fell out of this same
gap: `zoomToRect()` never called `refreshLiveView()` at all afterward, so
the overview strip's own viewport (only ever updated inside
`refreshLiveView()`'s `setViewport()` call) never moved to reflect the new
zoom; and the next time anything *did* call `refreshLiveView()` (a
subsequent drag, or a natural tick), it read `m_liveViewOffset` — never
touched by the old zoom code, so still whatever it was *before* the zoom,
typically 0 — and fetched from there, which reads as the view snapping
straight back to "the newest" regardless of where the zoom had pointed.

Live now has its own branch in `zoomToRect()`, operating on
`m_liveViewOffset` directly instead of `ScopeView`'s internal state: it
computes where the box's own centre falls in "samples back from newest"
terms against the *current* window, narrows `secondsPerDiv()`, then
derives the *new* `m_liveViewOffset` from that same centre point against
the *new*, narrower window size — and calls `refreshLiveView()`
immediately, not deferred to the next tick, so both the plot and the
overview strip's viewport reflect the new zoom the instant the gesture
completes. Capture's own branch is untouched and still correct for
exactly the reason its own comment now states explicitly: its buffer
really is static once captured, which is the one precondition the
`ScopeView`-internal mechanism actually needs.

## Five confirmed bugs, all found from concrete screenshot evidence rather than guessed at

**Live's own sample rate selector, added last pass, never actually
applied.** The screenshot showed the combo reading "100 kS/s" while both
the Acquisition info panel and the top bar's own Scope card — reading
`m_acq->sampleRate()` directly — still showed "200 kS/s", proving the
underlying value itself was never being set, not merely a stale display.
`applyTimebase()`'s `ModeStream` branch guarded the entire
`setSampleRate()` call behind `if (!streamRates.isEmpty())` — copied from
the shape of the old auto-derivation code it replaced, but wrong for a
direct selector: whenever `m_acq->ready()` was false at the moment the
combo changed (idle/stopped, matching the screenshot exactly),
`streamRates` came back empty and the whole block, including the actual
`setSampleRate()` call, was skipped outright. `applyRecordRate()` — the
pattern this was supposed to copy — never had this bug, because it always
calls `setSampleRate()` unconditionally, falling back to the raw
requested value when device rates are unknown rather than skipping the
call entirely. Fixed to match that exactly.

**The status bar disappearing after Record mode traced to one connection
with no timeout at all**, unlike every other `showMessage()` call in this
file: `connect(m_acq, &AcquisitionManager::status, ...)` called
`statusBar()->showMessage(text)` with no second argument, which means an
indefinite message — until explicitly cleared or replaced by another
`showMessage()` call, it permanently hides the status bar's permanent
widgets (`m_statusLine`) underneath it, regardless of what
`updateStatusBar()`'s own 250 ms timer keeps setting that widget's text
to. `AcquisitionManager` emits this status signal on stop-related
transitions too, not only while actively capturing, so a plain mode
switch away from Capture — which calls `stopAcquisition()` regardless of
which mode is being switched into — was enough to trigger it. Fixed with
an explicit timeout (4000 ms), matching every other message in this file.

**The grid centred on zero until dragging, after returning from Capture,
traced to Live's own snapshot restore missing one call `refreshLiveView()`
always makes.** Live has no real trigger (`setSamples(..., -1.0)`
unconditionally), so its axis depends entirely on `setAbsoluteTimeOrigin()`
being set correctly — `refreshLiveView()` always does this, but the
snapshot-restore code added last pass (`setAcquisitionMode()`'s
`ModeStream` branch) called `setSamples()` directly for each channel and
never called `setAbsoluteTimeOrigin()` at all. With `m_hasAbsoluteTimeOrigin`
still false from `setAcquisitionMode()`'s own earlier
`clearAbsoluteTimeOrigin()` call, the axis fell through to the
trigger-centred branch — which, with no real trigger, always resolves to
an offset of exactly 0, drawing the grid at its own centred, unpanned
resting position regardless of what `m_liveViewOffset` or the snapshot
itself actually held. Dragging "fixed" it because `panRequested` calls
`refreshLiveView()`, which does set the origin correctly. Fixed by
computing the same formula `refreshLiveView()` uses (from the current
`secondsPerDiv()`, the snapshot's own sample rate, and `m_liveViewOffset`)
directly inside the restore path.

**A channel explicitly turned off could still show old data it was
captured or recorded with while it was on — the screenshot showed CH2's
own card reading "Off" while `CH2 ±2.00 V` was still drawn on the plot.**
None of the three per-mode restore paths (`m_captureSnapshot`,
`m_liveSnapshot`, and `refreshRecordView()`'s own use of a recording's
own channel mask) checked the channel rail's current on/off state at
all — each only asked "does this channel have data to show," never
"is this channel currently allowed to be shown at all." All three now
also require `m_channel[ch].mode->checkedId() != ChannelOff` before
displaying a channel's restored data, clearing it otherwise — an explicit
off is a deliberate, current choice, and none of these mechanisms should
be able to silently override it with data captured from before it was
turned off. Record's own case needed care not to regress the earlier,
separate fix that made a recording show its own actual channels
regardless of the rail's *default* state: the two conditions are ANDed
together now (has-data-in-this-recording AND not-currently-off), so a
recording still shows whatever it actually has unless the person has
explicitly turned that channel off since.

**Zoom level did not survive a mode switch, because `zoomToRect()` never
updated the per-mode timebase members `refreshTimebaseOptions()` actually
restores from.** `zoomToRect()`'s Capture and Live branches both call
`m_view->setSecondsPerDiv()` directly to narrow the view — correct for
the plot itself, but `m_captureTimebase`/`m_liveTimebase` (the values
`refreshTimebaseOptions()` reads back when re-entering a mode; see the
per-mode timebase entry two sections up) were never told about it, since
only `applyTimebase()` previously updated them and `zoomToRect()` never
calls it. Switching away and back silently reverted to whatever the
timebase was *before* the zoom. Fixed by setting the matching member
(`m_captureTimebase` or `m_liveTimebase`) right alongside each branch's
own `setSecondsPerDiv()` call. Explicitly not addressed this pass, and
worth flagging again since it is the same gap noted two sections up:
Capture's own pan/zoom position *within* a zoomed-in view (`ScopeView`'s
internal `m_viewOffsetSamples`, via `viewCentreFraction()`) still is not
part of what is restored — only the zoom *amount* is. Live does not have
this gap: its own pan position (`m_liveViewOffset`) already persists
correctly, and its zoom mechanism is now unified with panning through the
same variable (see the Live-mode zoom rebuild above).

## Timebase precision loss across a mode switch, a channel-off label that survived clearChannel(), and a window that would not shrink back

**"Zoomed in on Live, switched to Capture and back, and the grid changed"
— the screenshot showed the dropdown reading "100 ms/div", and the axis
labels' own spacing confirmed the plot really was at that coarser value,
not just displaying it wrong. The previous pass's fix (persisting
`m_liveTimebase`/`m_captureTimebase` through a zoom) was necessary but not
sufficient: it stored the exact zoomed value, but the *restore* path threw
that precision away again on every single mode round-trip.**
`refreshTimebaseOptions()` restores a mode's own remembered timebase by
finding the *nearest* entry in that mode's fixed dropdown list
(`kRollTimebases` for Live has only six entries: 0.1, 0.2, 0.5, 1.0, 2.0,
5.0 seconds) — a continuous value a drag-a-box zoom produced essentially
never lands exactly on one of those six. `applyTimebase()` then read the
dropdown's own now-snapped value back and stored *that* into
`m_liveTimebase`, permanently replacing the precise zoomed value with a
coarser, rounded one — every time, not just once. Split `applyTimebase()`
into two paths with a new optional parameter (`exactOverride`, default
-1.0 meaning "read the dropdown as before"): a person changing the
dropdown themselves still stores its own snapped value as before, but
`setAcquisitionMode()` now calls `applyTimebase(m_liveTimebase)` /
`applyTimebase(m_captureTimebase)` explicitly on mode entry, which applies
that exact value to the plot and leaves the remembered member alone,
letting the dropdown's own displayed label be the only thing that still
shows a rounded, nearest-match value.

Fixing this surfaced a second, real ordering bug in the same function:
the Capture/Live snapshot-restore block (added two passes back) ran
*before* `refreshTimebaseOptions()`/`applyTimebase()`, so Live's own
axis-origin calculation — which reads `m_view->secondsPerDiv()` — was
reading whatever the *previous* mode had left that at, not the value just
restored for the mode actually being entered. Moved the entire restore
block to run *after* the timebase application instead, so by the time it
computes anything from `secondsPerDiv()`, that value is already correct
for the mode on screen.

**The channel-off label persisting (`CH2 ±2.00 V` still drawn with the
channel's own card reading "Off") traced to `ScopeView::clearChannel()`
itself, not to any of the three restore paths that call it — all three
were already correctly gating on the channel's current on/off state, but
what they were calling to enforce that didn't fully do it.**
`clearChannel()` only ever cleared `samples`, never `visible` — a channel
some earlier restore had explicitly set `visible = true` on (Record's own
per-tab state, or an earlier session's Capture/Live snapshot) kept that
flag set even after `clearChannel()` ran on it, and `visible` alone is
enough to draw a channel's label even with no trace to actually plot,
which is exactly what a card reading "Off" while its label still showed
looked like. Fixed at the source: `clearChannel()` now resets `visible`
to false as well, so every one of its callers — the three restore paths,
and the "close every Record tab" handler — actually gets the "this
channel shows nothing at all" it was already asking for.

**The window not shrinking back after Record mode, requiring two clicks
on the resize control before the status bar reappeared, is addressed with
an explicit, defensive fix rather than a fully diagnosed one** — Qt's own
minimum-size-hint recalculation when a widget's visibility changes is not
something this pass traced all the way through, and the fix does not
depend on having done so. `m_recordTabs->setVisible()` toggling can grow
the window's own minimum size hint to fit the newly-visible tab bar, and
Qt does not automatically shrink a window back down once it has grown —
if that growth ever pushed the window taller than the screen itself, the
status bar at the very bottom would be exactly what fell off the visible
area, matching "changes its window size... until I click resize twice"
precisely. Fixed by capturing the window's own size immediately before
this specific visibility toggle and restoring it via `resize()` right
after, deferred one event-loop turn (`QTimer::singleShot(0, ...)`) since
some layouts do not finish recalculating their own minimum size hint
until the visibility change has actually been processed, which is not
guaranteed to have happened synchronously by the time `setVisible()`
itself returns.

## The window-height fix replaced with a deterministic one, and a genuine fresh-install bug found from "when I first launch the app"

**The previous pass's window-height fix ("measure the size, then resize
back afterward, deferred one event-loop turn") was proven insufficient by
two screenshots with the same window at the same zoom level, before and
after a Record round-trip — the status bar had moved down by roughly the
tab bar's own height in the second one, meaning the deferred check found
nothing to undo.** Whatever Qt layout recalculation actually causes the
growth evidently had not run yet even one event-loop turn later, so
`size() != sizeBeforeTabsToggle` came back false and the `resize()` never
fired. Replaced with a deterministic mechanism instead of a better-timed
guess: `setFixedHeight()` to the window's own current height immediately
before `m_recordTabs->setVisible()`, then releasing the constraint
(`setMinimumHeight(0)`, `setMaximumHeight(QWIDGETSIZE_MAX)`) right after.
With the window's own height held fixed for the duration of the toggle,
showing the tab bar has nowhere to grow the window into — the layout is
forced to fit it by giving the one flexible, stretchable element in that
column (the plot area) slightly less room instead, which is the ordinary,
already-correct way this layout absorbs a size change everywhere else.
This does not depend on guessing when Qt's own recalculation happens,
since it prevents the growth outright rather than reacting to it
afterward.

**"When I first launch the app and hit run with CH1 on, no trace appears,
though it does on the overview strip, unless I change it to Manual" is a
real, separate bug, distinct from every mode-switching issue in the
entries above it — it happens before any mode has even been switched.**
Traced to `ScopeView::Channel::visible` defaulting to `false`, and no
code path ever calling `setChannelVisible(true)` for a channel that
starts on by default (CH1's own initial Auto mode) on a genuinely fresh
install. The two places that do set it — `restoreSettings()`'s own loop,
and each channel's mode-combo `currentIndexChanged` handler — are both
conditional: `restoreSettings()` returns immediately, before ever reaching
that loop, whenever no settings file exists yet at all (a fresh install's
defining condition), and the mode combo's handler only runs once a person
actually touches that specific combo, which "switching it to Manual" is
exactly an instance of — explaining both halves of the report at once:
the overview strip shows data because `NavigatorView` tracks its own
visibility independently and was never affected by this gap, while the
main plot, gated on `ScopeView`'s own flag, stayed invisible until
something happened to set it.

Fixed at two points: `applyChannel()` itself now sets both
`m_view`/`m_nav`'s `setChannelVisible()` unconditionally, from the
channel's own current mode, rather than leaving visibility to whichever
of the two conditional paths happened to have run — visibility is clearly
part of what "applying a channel's settings" already means, and every
other call site (there are several, all through user interaction with a
different control) already indirectly relies on `applyChannel()` having
established it correctly first. And the constructor now calls
`applyChannel()` for all four channels unconditionally, immediately after
`restoreSettings()`, regardless of whether that actually restored
anything — redundant, not harmful, on an ordinary launch where
`restoreSettings()` already did this itself, and the only thing that
actually establishes it on a fresh install, where nothing else does.

## A real regression from the previous pass's own clearChannel() fix — restoring a snapshot never made a channel visible again

**Run a capture, switch to any other mode, switch back — no trace, in
every mode, confirmed by the overview strip still showing it correctly
while the main plot did not. This was a genuine regression introduced by
this session's own earlier fix for the channel-off label bug, not a
pre-existing issue surfacing differently.** That fix made
`ScopeView::clearChannel()` reset `visible` to `false` alongside clearing
`samples` — correct and necessary on its own, since a channel's label was
persisting after being cleared specifically because `visible` was not
part of what got reset. But every one of the three per-mode restore paths
switches through this exact sequence on an ordinary mode round-trip:
switching away from Capture to a mode with no data of its own yet (Live or
Record, freshly never run) hits that mode's own "else, clear" branch,
which calls `clearChannel()` on every channel — including the ones
Capture's own snapshot still holds perfectly good data for — setting
`visible = false` on all of them. Switching back to Capture then calls
`ScopeView::setSamples()` to restore that data — but `setSamples()` never
set `visible` at all, in either direction, so a channel `clearChannel()`
had just turned off stayed off, with samples silently sitting in
`ScopeView`'s own buffer the whole time and never drawn. The overview
strip was unaffected because `NavigatorView` tracks its own visibility
independently of `ScopeView`'s, which is exactly why it kept showing the
data correctly while the main plot did not — the same asymmetry that made
the original channel-off label bug possible to diagnose from a screenshot
in the first place.

The workarounds the person found — toggling the channel off and back to
Auto, or switching to Manual — both happen to call `applyChannel()`,
which (from the same earlier pass) now sets visibility itself; that is
what actually fixed it, not anything about Auto vs. Manual specifically.

Fixed at the one place that actually needed it: `ScopeView::setSamples()`
now sets `visible = true` unconditionally, the same way `clearChannel()`
already sets it to `false`. This is the more robust fix, not a
narrower one scoped to only the restore paths that happened to trigger
this: every caller of `setSamples()`, in this codebase or any future one,
only ever calls it when a channel is meant to be shown — the exact
condition every call site already gates on before choosing between
`setSamples()` and `clearChannel()` — so setting visibility as a direct
consequence of providing samples, rather than trusting each of the many
call sites to separately remember to do so, removes an entire class of
"forgot to also call setChannelVisible()" bugs at the source rather than
patching this one instance of it.

## A direct follow-on from the previous pass's own fix, and a genuinely separate bug in Temporary-recording cleanup

**CH2, off on its own card, drawing anyway in Capture mode specifically —
not Live, not Record — traced directly to `setSamples()` now always
setting `visible = true` (the previous pass's own fix, made minutes
earlier). That fix was correct in principle; this is the one call site it
did not account for.** `refreshLiveView()` and `refreshRecordView()` both
already skip a disabled channel entirely before ever calling
`setSamples()` for it — but the Capture-mode `samplesReady` handler never
had that check at all, on the assumption that the SDK simply would not
deliver data for a channel `m_acq->setChannelEnabled(ch, false)` had
already told it to stop sampling. That assumption held as long as nothing
made a channel visible just from being handed samples — once
`setSamples()` started doing exactly that, whatever the SDK still
delivers for a disabled channel (evidently something, based on the
screenshot) was enough to turn it back on. Fixed by adding the same check
Live and Record already have: `m_view->setSamples()`/`m_nav->setSamples()`
only run when the channel is actually on, `clearChannel()` otherwise. The
snapshot capture just above it stays unconditional — harmless, since the
restore path already re-checks the channel's current state before ever
showing it.

**Temporary recordings surviving a restart traced to the delete-on-exit
mechanism only ever tracking the single most recently started recording,
not every one made during the session.** `m_lastRecordWasTemporary`/
`m_lastRecordDirectory` were a single pair of members, overwritten every
time a new recording started — so making two recordings in one session,
Temporary then Temporary (or Temporary then Permanent), left only the
second one's own status remembered at exit; the first Temporary recording
was never tracked for deletion again the moment the second one started,
and its files persisted indefinitely across every subsequent restart with
nothing left in the app that still pointed at them. Replaced with
`m_temporaryRecordings`, a `QStringList` appended to (never overwritten)
every time a recording starts with Keep set to Temporary, with
`closeEvent()` now looping over the whole list and deleting every entry,
not just checking one boolean. `m_lastRecordDirectory` had no other real
use once this changed (a grep confirmed only comments referenced it
elsewhere, describing its distinctness from `m_currentRecordTab`, which
those comments have been updated to describe without it).

## Entering Record mode showed Capture/Live's leftover state, and dragging after closing every tab resurrected a recording no tab pointed at

**Entering Record mode with existing tabs showed a Capture/Live-style
waveform and axis — small time range, trigger-centred, the wrong sample
count on the status strip — with the recording's own content only
appearing after a drag. Traced to two separate, concrete bugs in
`setAcquisitionMode()`/`loadRecordTab()`, not one.**

First: `loadRecordTab()` calls `refreshRecordView()`, which — correctly —
sets the navigator's viewport, trigger fraction, and following-trigger
state at its own end, computed against Record's own convention (a
fraction of the whole recording). `loadRecordTab()` then called
`syncNavigator()` immediately afterward, which reads `ScopeView`'s own
internal `viewportCentreFraction()`/`viewportWidthFraction()`/
`triggerFraction()`/`isFollowingTrigger()` — values that describe whatever
Capture or Live left there, with nothing to do with the recording just
loaded — and overwrote everything `refreshRecordView()` had just set
correctly, moments earlier. Removed the redundant, harmful call; nothing
else in `loadRecordTab()` needs it, since `refreshRecordView()` already
owns the navigator's state entirely for Record mode.

Second, and unrelated: `m_recordViewStart = 0; m_recordViewLength = 0;`
ran unconditionally partway through `setAcquisitionMode()` — *after*
`loadRecordTab()` (called earlier in the same function, inside the
`m_recordTabs` block) had already carefully restored this specific
recording's own saved zoom/pan state, or set a deliberate first-view
default. This silently threw that away, leaving the two fields at "show
the whole recording" instead of whatever was actually just restored —
harmless on its own until the next refresh (a drag) actually used them,
at which point the view would snap to a completely different span than
what had just been established, matching the report's own "off until I
drag it" shape from a second, independent angle. Fixed by only running
this reset when the mode being entered is not Record — Capture and Live's
own restore paths still need it (a fresh mode entry there has no
equivalent "already correctly restored earlier in this function" step to
protect), so it was never wrong for them, only for Record specifically.

**Dragging after closing every tab brought back the trace of whatever
recording was last loaded, even with no tab selected at all, because
closing every tab clears the display but never resets `m_stream`'s own
loaded-recording state — and every drag-driven code path
(`panRequested`, `centreRequested`, `zoomToRect`, `resetZoom`) checked
only `m_stream->recorded() > 0` before reading straight from it.**
`m_stream`'s `RecordWriter` keeps whatever recording `openRecordForReading()`
last opened until something explicitly opens a different one or closes
it outright — closing the last tab was never one of those things, since
the "no tabs left" handler only clears `ScopeView`'s and `NavigatorView`'s
own displayed state, which is a different thing entirely from what
`m_stream` itself still holds. `m_stream->recorded()` therefore kept
reporting the old recording's own sample count as if it were still
current, and any of the four drag/zoom paths would happily read from it
and redraw it, with no tab anywhere suggesting it should still exist. All
four now also require `!m_currentRecordTab.isEmpty()` — the same variable
already used everywhere else in this codebase to mean "a recording is
actually being viewed right now" — alongside the existing check, so none
of them can act on `m_stream`'s stale state once nothing is meant to be
shown at all.

## Per-channel volts/div and vertical position leaked between Capture, Live, and Record

**Adjusting a channel's vertical scale or position in one mode carried
over into another — change the level in Capture, switch to Record, and
the same shift showed up there too, on an otherwise-correct recording,
until the level was readjusted from scratch in each mode separately.**
`ScopeView`'s own `voltsPerDiv`/`positionVolts` are per-channel but not
per-mode — a single, shared pair of values regardless of which
acquisition mode set them. Every other piece of per-mode state (the
waveform itself, the timebase, Record's own per-tab view) already had an
explicit save/restore snapshot for exactly this reason; vertical
scale/position never did, so it simply carried across unchanged whatever
mode was entered next — invisible so long as every mode's own waveform
happened to sit at a similar level, and immediately obvious the moment
they didn't.

Fixed the same way the rest of this per-mode state already works:
`CaptureSnapshot` (shared by `m_captureSnapshot` and `m_liveSnapshot`)
gained `voltsPerDiv`/`positionVolts` arrays alongside the samples it
already held. `setAcquisitionMode()` now saves the *outgoing* mode's
current per-channel values into its own snapshot right at the top,
before anything else runs, keyed off `previousMode` (captured before
`m_acquisitionMode` is overwritten) — Capture's own snapshot when
leaving Capture, Live's own when leaving Live; Record needs no entry
here since `loadRecordTab()` already saves its own outgoing tab's state
the same way, per tab rather than per mode. Each restore block then
applies its own snapshot's values per channel, the same loop that
already restores the waveform itself: `voltsPerDiv` only when it's
`> 0.0` (0 is never a real range, so that's "never saved yet" — a mode
never used this session leaves the scale at whatever the previous mode
had, same as it always did), `positionVolts` unconditionally (0 V is a
real, meaningful position, so there's no equivalent gate — matches
`RecordTabViewState`'s own existing convention for the same field). Both
are applied as raw volts via `setPositionVolts()`/`setVoltsPerDiv()`
directly, not through `setChannelPositionPercent()` — that helper
computes volts from whatever the position spinbox's percentage times the
*currently selected* volts/div page works out to, which at the moment of
restore is not yet guaranteed to match the volts/div just restored a
line above it, so going through it here would silently apply a
different offset than the one that was actually saved. This does leave
the position spinbox itself potentially showing a stale percentage after
a restore, exactly like `RecordTabViewState`'s existing restore already
does — a known, pre-existing limitation carried forward for consistency
rather than fixed here, since fixing it would mean understanding what
`voltsPage`'s combo actually represents, which is a separate question
from what was reported this time.

Record mode's own zoom/pan persistence across mode switches — the
second half of this report — was already fixed by the previous turn's
`syncNavigator()` removal and the `m_recordViewStart`/`m_recordViewLength`
reset reordering (see above): `RecordTabViewState` already keeps this
per recording, restored by `loadRecordTab()` whenever that specific tab
is revisited, and neither of this turn's changes touch that path at all.
Worth re-confirming after this build, since it was reported again in the
same message — zoom into a recording, switch to Capture or Live and
back, and the same span and position should already be exactly where
they were left, no drag needed.

## The Record-mode leak's actual root cause — the previous turn's fixes were real but did not touch it

**The previous turn's `syncNavigator()` removal and reset-reordering were
both genuine bugs, but neither was why entering Record mode with
existing tabs kept showing Capture/Live's leftover trace — that had a
separate, single cause never previously found: `loadRecordTab()`'s own
"already showing this one" guard.**

`loadRecordTab(directory)` returns immediately, doing nothing at all, if
`directory == m_currentRecordTab` — a deliberate optimization so clicking
the tab that's already selected doesn't do redundant work. But
`m_currentRecordTab` is never cleared just by leaving Record mode for
Capture or Live — it stays pointing at whichever recording was last
being viewed. So the exact reported sequence (capture something in
Record, switch to Capture or Live, adjust something there, switch back
to Record) calls `setAcquisitionMode()`'s Record-entry code, which asks
`refreshRecordTabs()` to reselect a tab — and it reselects the *same*
one, since nothing about which recording is "current" ever changed.
`loadRecordTab()` is then called with that same directory, sees it
matches `m_currentRecordTab`, and returns having done nothing whatsoever
— never calling `refreshRecordView()`, never touching `ScopeView` at
all. Whatever Capture or Live left on screen stays exactly as it was.
Dragging "fixes" it because `panRequested`'s handler calls
`refreshRecordView()` directly, entirely bypassing `loadRecordTab()` and
its guard.

The fix splits `loadRecordTab()` into two pieces. The existing function
keeps its guard and its "switching to a genuinely different tab" duties
(saving the outgoing tab's state, opening the new recording for reading,
updating `m_currentRecordTab`) exactly as before — that guard is correct
and still wanted for the ordinary in-mode tab click, where nothing else
could have touched the display in the meantime. The other half — apply
this tab's own saved scale/position/visibility (or fresh defaults) and
call `refreshRecordView()` — moved into a new function,
`applyRecordTabViewState(directory)`, which `loadRecordTab()` now calls
at its own end. `setAcquisitionMode()`'s Record-mode entry now checks
which case it's in itself: a genuinely different tab still goes through
`loadRecordTab()` as before, but the same tab as before calls
`applyRecordTabViewState()` directly, skipping `loadRecordTab()` and its
guard entirely — exactly the case that guard was never meant to cover in
the first place, since re-entering the mode is not the same thing as
clicking a tab that was already selected.

That still left one gap: `applyRecordTabViewState()`'s restore reads
from `m_recordTabStates`, which — before this turn — was only ever
written when *switching between* tabs inside Record mode
(`loadRecordTab()`'s own save-outgoing step) or when a new recording
starts. A recording viewed exactly once and never tab-switched away from
in-mode had no entry there yet, so the very first time this new
same-tab path ran, it would have found nothing to restore and fallen
through to fresh defaults — leaving `voltsPerDiv`/`positionVolts` exactly
as Capture or Live had left them, silently reintroducing the leak this
whole fix exists to close. `setAcquisitionMode()`'s outgoing-state save
(added earlier this turn for Capture and Live) now has a third branch for
`previousMode == ModeRecord`, saving the current tab's state the same
way, so leaving Record mode for any reason — not just switching to
another tab within it — always leaves something correct behind to
restore later.

## Vertical position leaked between modes even after the snapshot fix — the position spinbox was never kept in sync

**Adjusting the level in any mode, then switching to another mode and
trying to adjust it there too, made the trace instantly jump to
whatever level the previous mode had been left at — the per-mode
snapshot from earlier this turn was saving and restoring the right
values, but restoring them by calling `setPositionVolts()` directly
left the sidebar's own offset spinbox (`offsetPct`) still showing
whatever the previous mode's percentage had been.**
`ScopeView::offsetDragged`'s handler computes every drag as
`m_channel[ch].offsetPct->value() + deltaPct` — the *spinbox's current
value* plus the drag delta, not `ScopeView`'s own internal position. A
restore that updates `ScopeView` but not this spinbox leaves the two out
of sync: the plot shows the freshly-restored, correct level, but the
very next drag computes its result from the stale percentage still
sitting in the spinbox, snapping the trace to a value based on whatever
mode was active before — exactly the "instantly jumps to the previous
mode's level" symptom.

All three restore sites — Capture's and Live's snapshot restore in
`setAcquisitionMode()`, and Record's own per-tab restore in the new
`applyRecordTabViewState()` — now go through `setChannelPositionPercent()`
instead of calling `setPositionVolts()` directly. That function's own
signature takes a percentage, and it already handles keeping the spinbox
and `ScopeView` in sync in the one place it's called from originally, so
each restore site computes `pct = storedPositionVolts / page * 100.0`
(using the channel's currently-selected `voltsPage` combo value — this
recomputes the percentage fresh, at restore time, from the current page,
rather than trusting a percentage that might have been computed against
a different page at save time) and calls `setChannelPositionPercent(ch,
pct)` rather than touching `ScopeView` directly. The stored value itself
stays raw volts, not a percentage — only how it gets applied changed.

## Light mode, persisted (theme.h rewritten; every hardcoded colour in the app converted)

**Added a full light theme alongside the existing dark one, toggled from
a new "Appearance" section at the top of the Settings page, persisted via
QSettings, and applied live without a restart for the large majority of
the UI.** This touched every source file that painted or styled anything,
since dark mode had never had a second mode to share code with — every
colour in the app was a hardcoded literal before this.

**Architecture (`theme.h`, fully rewritten).** `Theme::Mode { Dark, Light }`,
persisted as `QSettings`' `"ui/theme"` key (`"dark"`/`"light"`). Read lazily:
`modeStorage()` is a function-local `static Mode`, initialised once on
first call from an immediately-invoked lambda that reads QSettings —
the standard "magic static" idiom, which the C++ standard guarantees
resolves to one single, shared instance across every translation unit
that includes this header, not a separate copy per `.cpp` file, because
`inline` functions with external linkage must be identical everywhere
and the linker merges them (this matters: nearly every `.cpp` in this
project now includes `theme.h`). `Theme::setMode()` updates the static and
writes QSettings in the same call. Every colour is its own `Theme::`
function (`windowBackground()`, `plotBackground()`, `channelColor(int)`,
accent(), etc.), each reading `Theme::currentMode()` fresh on every call —
deliberately not cached constants, because several call sites across this
codebase are file-scope `const` globals or struct default-member-
initialisers evaluated once, at static-init or widget-construction time,
and those are exactly the places that would otherwise bake in whatever
mode was default before `main()` had even loaded the persisted choice
from disk.

**What stays the same across both modes, and why.** `accent()` (the
bright green) and `onAccent()` (near-black text drawn on it) are
unchanged — both are self-contained pairings, always used together, so
neither one's contrast ever actually depends on the surrounding theme.
`triggerColor()` (warm amber) and a handful of "identity" colours in
mainwindow.cpp — the top cards' own accents (Run/Stop, Scope, Trigger,
Logic, DIO), the Stopped/Running status colours, the over-range warning's
dark-red-background-with-bright-red-text pairing, and the annotation
handle's near-black border — are left as their original hardcoded values
for the same reason: each already has adequate self-contained contrast
against either background, or is a self-contained pairing like accent
itself. What DOES flip: `accentSoft()` (icon colour and secondary-accent
text — dark mode's pale value is nearly invisible on a light background),
every channel's default trace colour and the math channel's default
colour (`channelColor(int)`, `mathColor()` — thin traces have far less
contrast margin than text does, so the same paleness problem hits harder
here), and of course every chrome surface (window/panel/card/field
backgrounds, borders, text) and the plot's own background/grid/axis-text.
Two new backdrop tokens exist purely because their old fixed values
stopped making sense once the surrounding colours flip:
`plotLabelBackdrop()` (the dimming patch behind a measurement label,
which needs to darken behind now-pale dark-mode text but lighten behind
now-dark light-mode text) and `annotationBackground()` (the sticky-note
box, dark green-tinted in dark mode, pale note-paper tint in light mode).

**Every file that had a hardcoded colour, converted:**
- `main.cpp` — `applyDarkPalette()` deleted; now calls `Theme::palette()`.
- `mainwindow.cpp` — `kChannelColors`, `kIconColour`, `kGridButtonStyle`,
  `kToolButtonStyle`, `kRecordTabStyle` were all file-scope `const`s
  computed once from `Theme::` colours (or, for `kIconColour`, used as a
  *default argument value* referencing such a const) — every one of these
  had to become a plain function (`gridButtonStyle()`, `toolButtonStyle()`,
  `recordTabStyle()`) or a direct call at each site
  (`Theme::channelColor(ch)`, `Theme::accentSoft()`), since a `const`
  evaluated once at static-init time can never reflect a mode loaded from
  disk moments later, let alone a live toggle afterwards. `sectionLabel()`,
  the detail panel frame, and a few muted-text labels converted too.
- `scopeview.cpp/h`, `spectrumview.cpp`, `xyview.cpp` — three files that
  had each independently duplicated the exact same `kBackground`/
  `kGridMinor`/`kGridMajor`/`kGridAxis`/`kText` constant set; all three
  now share `Theme::plotBackground()` etc., collapsing the duplication.
  Their own overlay/readout-box colours (elevated background, border,
  muted/disabled text) converted too.
- `navigatorview.cpp/h` — its own (slightly different, now unified) copy
  of the same plot-chrome constants converted the same way.
- `cardbutton.h/cpp` — `restyle()`'s three background states, border,
  title/value/note text all converted; added a public `refreshTheme()`
  (just calls the already-private `restyle()`) so `applyTheme()` has a
  way to re-run it on an already-built card.
- `probedialog.cpp` — `kPresetStyle` (same const-vs-function problem as
  mainwindow.cpp's trio, fixed the same way, as `presetStyle()`) and the
  header bar's/icon's/list item's hardcoded greys.
- `annotationbox.cpp` — its own `kBackground` (a distinct dark
  green-tinted note colour, unrelated to the plot's own background)
  converted to the new `annotationBackground()`.

**Settings page**: a new "Appearance" section, first in the page, with a
Dark/Light segmented control built from the same `segment()` helper the
existing Temporary/Permanent toggle uses. `QButtonGroup` ids are
`static_cast<int>(Theme::Mode::Dark/Light)` so the id and the enum can
never drift apart. Clicking either segment calls `Theme::setMode()` then
`MainWindow::applyTheme()`.

**`applyTheme()`** — called once at construction (after every widget it
touches exists) and again from the toggle's handler — re-applies, in
order: the app palette and global style sheet (`qApp->setPalette()`/
`setStyleSheet()`, which alone covers every standard Qt control anywhere
in the app — combo boxes, spin boxes, checkboxes, buttons, line edits,
scrollbars, tooltips, menus, group boxes); the handful of individually
-styled member widgets that build their own style-sheet string instead
of relying on the global one (`m_recordTabs`, `m_panelHost`,
`m_sampleInfo`, `m_recordDirLabel`); two new tracking lists,
`m_toolStyledButtons`/`m_gridStyledButtons`, appended to at every one of
the five call sites that set `toolButtonStyle()`/`gridButtonStyle()` on a
`QToolButton` (most of which are anonymous locals built in a loop — the
Open/Save/Full toolbar row and the sidebar's sixteen-odd instrument
buttons — with no member pointer to re-style through otherwise); every
`CardButton`'s new `refreshTheme()`; and finally a plain `update()` on
each of the four plot views (`ScopeView`, `NavigatorView`,
`SpectrumView`, `XYView`), which is all they need since their own
paint code already reads `Theme::` colours fresh on every repaint.

**Explicit, deliberate scope limit — worth knowing before assuming a
toggle updates literally everything:** `sectionLabel()` is a free
function called 20+ times, each call returning its own anonymous
`QLabel` with no member pointer anywhere to re-style later; the Record
page's one-off info note has the same shape. Both read `Theme::` fresh
at construction, so they are always exactly correct at every launch —
but a toggle made mid-session will not retroactively recolour ones
already on screen without a restart. Threading a tracking list through
20+ scattered call sites for this felt like the wrong trade for what it
would buy. `ProbeDialog` needed no such tracking at all: it is a fresh
dialog instance built from scratch every time it opens, so it is simply
never stale.

### Follow-up: `toolButtonStyle()` silently resolved to `QMainWindow::toolButtonStyle()`, not this file's own function

Failed to compile: every `single->setStyleSheet(toolButtonStyle())`-style
call inside a `MainWindow::` member function reported "cannot convert
'Qt::ToolButtonStyle' to 'const QString&'" — the call was resolving to
the wrong function entirely. `QMainWindow` (which `MainWindow` derives
from) has its own built-in `Qt::ToolButtonStyle toolButtonStyle() const`
(the getter paired with the well-known `setToolButtonStyle()`), and it
takes the exact same zero arguments this file's own free function does.
C++ unqualified name lookup from inside a member function checks the
class's own scope — including everything inherited from its base
classes — before it ever looks at file-scope names in an enclosing
namespace, and it stops looking entirely the moment it finds a match:
`QMainWindow::toolButtonStyle()` hid the free function in this file's
anonymous namespace for every single call made from anywhere inside
`MainWindow`, with no warning, since both are equally valid zero-argument
calls as far as overload resolution is concerned once the (wrong) scope
has already been settled on. Renamed the free function to
`toolButtonQss()` throughout — `gridButtonStyle()`, `recordTabStyle()`,
and `presetStyle()` (in probedialog.cpp) do not collide with anything on
`QMainWindow`/`QWidget`/`QDialog` and compiled correctly as they already
were, so they were left alone rather than renamed on spec. Worth
remembering for any future theme-style helper: a short, generic name
living inside a `QMainWindow` subclass carries a real, silent collision
risk against Qt's own inherited method names, and the compiler gives no
hint that a wrong scope was even considered.

## Five light-mode polish fixes: logo, accent, icon/text colour matching, disabled icons, cursor flag text

**Logo swap.** `resources/logo.png` (white text, for dark mode's near-black
bar) renamed to `logo-dark.png`; the supplied light-mode artwork (black
text) added as `logo-light.png`; both registered in `app.qrc` under
`/images`. The logo `QLabel`, previously a local in `buildTopBar()`, is
now the member `m_logoLabel`; a new `MainWindow::refreshLogo()` picks
`logo-dark.png`/`logo-light.png` by `Theme::isDark()`, called once at
construction and again from `applyTheme()` so a live toggle swaps it too.

**Accent darkened a touch in light mode.** `Theme::accent()` is now
`#73ff00` in dark mode (unchanged) and `#62d900` in light mode — the same
hue, modestly darker, since the original read a little too glaring
against white. `onAccent()` (the near-black text always paired with an
accent background) needed no change — a darker green only improves that
pairing's contrast, never hurts it.

**Icon colour now matches its own adjacent text, at every site where it
didn't before:**
- The Run/Stop card's play/stop icon had its own separate hardcoded red
  (`#e05c5c`) and green (`#4cc06a`), while the card's title/value text and
  side accent all use `runColour` (`Theme::accent()` running,
  `#ff3b30` stopped) — a different, slightly duller shade each. Now the
  icon is passed `runColour` directly, so it always matches exactly.
- The two "Trace colour" buttons (Math's and each channel's own) and the
  "Manage probes" button are plain `QPushButton`s — their text colour is
  whatever the palette's `ButtonText` resolves to (`Theme::text()`),
  since `globalStyleSheet()`'s own `QPushButton` rule sets no explicit
  `color`. Their eye/wrench icons were defaulting to `uiIcon()`'s own
  `accentSoft()` instead. All three now pass `Theme::text()` explicitly.
- The Instruments Mode segments (Scope/Spectrum/XY/Persistence) had the
  same `accentSoft()`-vs-`text()` mismatch when unchecked, and used a
  literal `Qt::black` for the checked-state icon instead of the actual
  `Theme::onAccent()` the checked text itself uses (`QPushButton:checked`
  in `globalStyleSheet()`). Both `normalIcon` and the renamed
  `checkedIcon` (was `blackIcon` — the name stopped describing its actual
  colour) now use `Theme::text()`/`Theme::onAccent()` respectively.

**Disabled icons darker in light mode.** Qt's own auto-generated
`QIcon::Disabled` pixmap lightens the icon toward the palette — correct
for a pale icon fading against a dark surface, but it made a disabled
icon nearly invisible against light mode's light one, since that
built-in effect assumes the opposite pairing. `uiIcon()` now explicitly
renders and attaches its own `QIcon::Disabled` pixmap in
`Theme::textDisabled()`'s own colour (already mode-aware), overriding
Qt's guess outright rather than fighting it — this reaches every icon
built through `uiIcon()`, which is nearly all of them.

**Time cursor flag text, white in light mode.** `ScopeView::drawFlag()`
(shared by both the time cursors and the per-channel voltage cursors) and
`SpectrumView`'s own copy (frequency and level cursors) always drew flag
text in `Theme::onAccent()` (fixed near-black), while the *time* cursor
flags — and *every* flag in `SpectrumView` — are filled with
`Theme::plotForegroundText()`, which is light in dark mode but dark in
light mode. Near-black text on that fill reads fine in dark mode and is
unreadable in light mode. Added `Theme::onPlotForeground()` (onAccent()
in dark mode, unchanged; white in light mode) and gave `drawFlag()` in
both files an explicit text-colour parameter: `ScopeView`'s time-cursor
calls now pass `onPlotForeground()`, its voltage-cursor calls still pass
`onAccent()` (filled with that channel's own colour instead, a mid-tone
in both modes, so unaffected by any of this), and every call in
`SpectrumView` — all of which share the one `plotForegroundText()` fill —
now uses `onPlotForeground()` too.

## Icons still didn't recolour on a live theme toggle — applyTheme() was only ever re-styling text, never rebuilding the icon pixmap itself

**Reported after the previous fixes shipped: icon colours only updated on
restart, not when the Dark/Light toggle was clicked.** The cause was
narrower than the already-documented `sectionLabel()` limitation — this
hit essentially every icon in the app, not a handful of section headers.
A `QIcon` built through `uiIcon()`/`Icons::icon()` is a `QPixmap`
rendered once, at whatever colour was current the moment it was built.
`m_toolStyledButtons`/`m_gridStyledButtons` (from the original light-mode
work) only ever called `setStyleSheet()` again on toggle — which does
correctly re-evaluate a widget's *text* colour, but does nothing at all
to an already-rendered icon pixmap sitting right next to it.

Replaced both lists with one mechanism that can rebuild either: `QVector<
std::function<void()>> m_themedRefreshers`, one callback per icon-bearing
widget, collected as each was built. Each callback closes over everything
needed to redo that widget's icon *and* style sheet from scratch — for
the widgets built inside a loop (`kTools[]` in both `buildTopBar()` and
`buildToolGrid()`), that means capturing the Font Awesome name and
fallback by value, since the loop's own local array does not outlive the
function. `applyTheme()` now just invokes every entry in the list.
Covers: Single, Force, Freeze, the Open/Save/Full toolbar row, every
sidebar grid button (Math/Measure/Rulers/...), and the detail panel's own
close button — all previously anonymous locals with no member pointer
for `applyTheme()` to reach afterward otherwise.

The Instruments Mode segments (Scope/Spectrum/XY/Persistence) needed a
different fix on top of this: their `normalIcon`/`checkedIcon` were
captured *by value* inside the button's own `toggled` click handler,
which outlives construction indefinitely — a refresher rebuilding new
local `QIcon`s would never reach the copies that handler already
captured. Switched both to `std::shared_ptr<QIcon>`, captured by the
refresher *and* the click handler alike: the refresher mutates
`*normalIcon`/`*checkedIcon` in place, and since both closures hold the
same underlying object through their own copy of the shared_ptr, the
click handler sees the update on its next toggle without needing to be
reconnected.

Three icon-bearing buttons were left out of the list entirely and are
now just refreshed directly, since each already has its own reachable
member pointer for other reasons: `m_mathColour`, and every channel's own
`colour`/`manageProbes` buttons (`m_channel[ch].colour`,
`m_channel[ch].manageProbes`). `updateRecordTabCloseIcons()` — already
its own self-contained, repeatedly-callable function reading `Theme::`
fresh each time — just needed a call added from `applyTheme()`, no
restructuring required. The Run/Stop card's icon needed nothing new: it
already goes through `refreshCards()`, which runs on the existing 250 ms
timer independent of `applyTheme()`, and already recomputes `runColour`
(and now the icon built from it) from scratch every time.

`ProbeDialog`'s own "plus"/"pen"/"trash" icons remain untouched by any of
this — as before, it is a fresh dialog instance built from scratch every
time it opens, so nothing about it can go stale in the first place.

## Google Sans, unified corner radius, and a proper close button

**Google Sans, embedded and applied app-wide.** All three supplied weights
(Regular, Medium, Bold) added under `resources/fonts/`, registered in
`app.qrc`, and loaded through a new `fonts.h`/`fonts.cpp` — deliberately
mirroring `icons.h`/`icons.cpp`'s own shape (an `initialise()` that is
safe to call more than once, a `family()` accessor) rather than
inventing a different pattern for the same kind of problem.
`Fonts::family()` reports the family name registered from the *Regular*
weight specifically — if the three files were ever packaged such that
they disagree on family name (not expected from a well-formed release,
but not verifiable without actually running Qt), Regular is the one
every plain-text widget in this app actually uses by default, so it is
the name worth keeping over Medium's or Bold's. Applied via
`app.setFont()` in `main.cpp`, before `MainWindow` is constructed, so
every widget inherits it as a starting font rather than needing a
second pass afterward — confirmed by checking every other
`setFont()`/`QFont()` call in the codebase first: all of them start from
a widget's own already-inherited font and only adjust size or boldness,
never hardcoding a different family, so nothing fights the new default.
Left alone entirely if registration ever fails — `family()` returns an
empty string in that case, and `main.cpp` checks for that before calling
`setFont()`, rather than risk forcing a family name that does not exist.

**One corner radius, everywhere.** Added `Theme::cornerRadius()` (8,
matching the cards' own existing value) and pointed every other rounded
rectangle at it instead of its own independently-chosen number: the side
panel (`#panelHost`, both the construction site and its `applyTheme()`
re-apply), Record mode's tabs (`recordTabStyle()`, replacing a hardcoded
5px), and — the more involved change — all four plot views. `ScopeView`,
`NavigatorView`, `SpectrumView`, and `XYView` each drew their own
background with a sharp-cornered `fillRect()`; all four now build a
`QPainterPath` with `addRoundedRect()` at that same radius, clip to it,
and fill through the path instead. Clipping rather than only filling
matters here: without it, the grid and traces beneath would still reach
into corners the rounded background no longer occupies, poking out past
the new rounded edge instead of being contained by it. `ScopeView`
specifically toggles antialiasing on only for this fill and back off
immediately after, since its own grid and trace drawing depends on the
crisp, unantialiased lines that setting produces elsewhere in the same
`paintEvent()`; `NavigatorView`, `SpectrumView`, and `XYView` all paint
their entire frame with antialiasing on throughout already, so no
equivalent toggle was needed there.

**The side panel's close button, restyled.** It was a bare `QToolButton`
with only an icon set — no size, no hover state, nothing — so it fell
back to plain, unstyled Fusion chrome, the exact thing
`globalStyleSheet()`'s own comment already notes it cannot reach (every
other `QToolButton` in this app carries its own `setStyleSheet()`; this
one never had. Now: a fixed 26×26 size for a consistent, comfortable
click target, and a soft red tint on hover/press — the familiar
"this dismisses something" convention, independent of theme for the same
reason the over-range warning colours are: a self-contained pairing that
does not need to be read against the surrounding chrome.

## "The font is not applied everywhere" — a Qt style-sheet gotcha, plus the font-size reduction and two navigator-strip fixes

**Root cause of the font report: once any QSS rule matches a widget, Qt's
style-sheet engine takes over that widget's entire font resolution, not
just the properties the rule actually sets — and falls back to Qt's own
default font instead of continuing to inherit `QApplication::font()`,
unless a rule says otherwise.** `theme.h`'s `globalStyleSheet()` has rules
for `QComboBox`, `QSpinBox`, `QDoubleSpinBox`, `QCheckBox`, `QPushButton`,
`QLineEdit`, `QScrollBar`, `QToolTip`, `QStatusBar`, `QMenu`, and
`QGroupBox` — none of which mentioned a font at all, so `main.cpp`
setting Google Sans as the app font never actually reached any of them;
plain, unstyled widgets (labels, the plot views' own text) were the only
things that picked it up correctly. Fixed with a single `* { font-family:
"..."; }` rule prepended ahead of everything else in the style sheet —
this reaches every widget the style sheet touches (and, since the rule
is unqualified, every widget in the app generally) in one place, rather
than adding `font-family` piecemeal to a dozen existing selectors.
`theme.h` now includes `fonts.h` to build this rule from
`Fonts::family()`, skipped entirely (no rule emitted at all) if that
comes back empty — the existing "leave the app font alone rather than
name a family that doesn't exist" reasoning from `main.cpp` applies here
too.

**Font size, reduced ~15%.** `main.cpp` now reads `app.font().pointSizeF()`
*before* switching to Google Sans — this is Qt/Fusion's own platform
default, whatever that happens to be on the machine it runs on — and
applies Google Sans at 85% of that, rather than a fixed point size that
would be proportionately wrong on a system whose default isn't the usual
9–10pt.

**Navigator strip's draggable viewport, rounded to match.** Was a plain
`fillRect()`/`drawRect()`, sharp-cornered like the strip's own background
was before the border-radius work; now `drawRoundedRect()` at the same
`Theme::cornerRadius()`, with antialiasing switched back on for this one
call (off since the background fill earlier in this same `paintEvent()`,
for the crisp unantialiased grid/trace lines in between) since a rounded
rect drawn without it looks visibly jagged at the corners — safe to
leave on afterward, since nothing else draws in this `paintEvent()` once
the viewport is done.

**The stray 1px border, found and removed.** `paintEvent()` ended with
`p.setPen(Theme::border()); p.drawRect(band);` — a sharp-cornered stroke
around the *entire strip*, left over from before the strip's own
background became rounded. Once the background was clipped to a rounded
path, this sharp rectangular stroke sitting on top of it no longer
matched the shape it was outlining, showing as a stray edge past the
rounded corners — most visible along the bottom and right, going by the
report. Removed outright: the rounded background fill already gives the
strip a clearly defined shape, and every other plot view (`ScopeView`,
`SpectrumView`, `XYView`) was checked and confirmed to have no equivalent
outer-border stroke of its own that would need the same fix.

## Five specific places where text read smaller than the rest of the app, each bumped 10%

**A deliberately narrow fix — five named spots, nothing else touched.**
Each of these already computed its own font size as some reduction from
(or, for the two that had no explicit reduction at all, apparently
smaller despite inheriting the same base as everything else) the
inherited base font, so "increase by 10%" meant multiplying whatever that
already-smaller result was by 1.10, not changing the base font itself —
`app.setFont()` in `main.cpp` and everything that reads from it elsewhere
was left exactly as it was.

- **Cards** (`cardbutton.cpp`): `titleFont` (base − 1.5pt) and `valueFont`
  (base − 0.5pt) both now multiply that reduced result by 1.10.
- **Inside the plot views**: seven separate `small.setPointSizeF(std::max(
  FLOOR, base - REDUCTION))` call sites across `scopeview.cpp` (rulers,
  axes, readouts), `spectrumview.cpp` (axes, level cursors, frequency
  cursors), and `xyview.cpp` (axes) — each already independently computed
  the same shape of "shrink, but not below a floor" result; all seven now
  multiply that whole expression (floor included) by 1.10 too.
- **Sidebar buttons** (`gridButtonStyle()` in `mainwindow.cpp`): the only
  one of these five with a genuinely explicit, literal size rather than a
  reduction from the inherited base — `font-size: 10px` in the button's
  own style sheet, raised to `11px`.
- **Record mode's tabs**: no explicit size anywhere in `recordTabStyle()`
  at all — `QTabBar`'s own Fusion rendering just reads smaller than the
  rest of the app despite inheriting the identical base font. Set
  explicitly instead, once, right after `m_recordTabs` is constructed:
  `tabFont.setPointSizeF(tabFont.pointSizeF() * 1.10)`.
- **A combo box's own selected value** (not its dropdown list — that
  stays as it was): also had no explicit reduction to build on, so
  `theme.h`'s `QComboBox` rule now sets `font-size` to 110% of
  `QApplication::font().pointSizeF()` directly (added a `QApplication`
  include for this), while `QComboBox QAbstractItemView` — the dropdown
  popup — gets an explicit `font-size` pinned back to that same 100%
  base right beside it. Without that second rule the popup would have
  silently inherited the QComboBox's own new, larger size too, growing
  the one thing the request explicitly said to leave alone.

## Side panel now slides open/closed instead of appearing/disappearing instantly

**`m_panelHost` (the detail panel — Scope/Trigger/Channel/Instruments/
Math/Rulers/Measure/Settings) was a `setFixedWidth(250)` frame that
`openPage()`/`closePanel()` just showed or hid outright.** Replaced the
fixed width with `setMinimumWidth(0)` (permanent) and
`setMaximumWidth(0)` as the starting, closed state — a `QPropertyAnimation`
(`m_panelAnimation`, built once in `buildDetailPanel()`) now slides
`maximumWidth` between 0 and the new `kPanelWidth` constant (250,
the same value `setFixedWidth()` used to hardcode) over 220ms with an
`OutCubic` easing curve — decelerating into place rather than a linear
slide, which is what actually reads as "fluid" rather than mechanical.
`minimumWidth` stays at 0 always; only `maximumWidth` animates, and the
layout — a plain `QHBoxLayout` sitting between the channel rail and the
plot column — respects that shrinking upper bound on every frame, which
is what actually produces the sliding motion rather than an abrupt
resize.

`openPage()` only starts the open animation when the panel was not
already open (`!m_panelOpen`) — clicking a different card while the
panel is already showing just swaps the `QStackedWidget`'s current page
index instantly, exactly as before; re-running the slide on every page
switch would be a very different (and much more annoying) kind of
"fluid." `closePanel()` always animates closed. Both read
`m_panelHost->maximumWidth()` as the animation's own start value rather
than assuming the panel is currently at a rest position (0 or
`kPanelWidth`) — this is what keeps a rapid open-then-close (or the
reverse) smooth: interrupting a running animation with `stop()` freezes
`maximumWidth` at whatever the in-progress interpolated value was, and
starting the next animation from that exact value continues the motion
seamlessly rather than snapping to the far end first.

`setVisible(true)` happens immediately when an open animation starts
(a width-0-but-still-hidden widget cannot visibly animate). Going the
other direction, `setVisible(false)` is deferred to
`QPropertyAnimation::finished` — connected once, permanently, in
`buildDetailPanel()` — checking `m_panelHost->maximumWidth() == 0` at
that moment to tell a just-finished close from a just-finished open,
since the same slot fires for both and only a finished close should
actually hide the widget. `stop()`ping an animation early (the
interrupt case above) does not itself emit `finished()`, so this never
fires prematurely mid-interrupt — only a genuine, uninterrupted
completion hides the panel.

## Record mode zoom/drag lag — the overview strip was re-reading the entire recording on every single drag frame

**Reported as both the waveform drag AND the (unrelated) panel-open
animation getting laggy together while zoomed into a Record-mode
recording — the shared symptom of one thing blocking the main thread
long enough to stall everything else running on it, animations
included.** `refreshRecordView()` — called on every debounced drag tick,
up to 60/sec via the existing 16ms `m_recordPanDebounce` — read two
separate slices per channel every single time: the small, zoomed-in
window the plot actually needs (cheap — `RecordWriter::readDecimated()`'s
own sequential fast path handles anything up to 8M samples in one read),
and a second, *entire-recording* read (`start=0, length=total`) purely
to feed the overview strip. That full-recording read is only cheap when
the whole thing fits `readDecimated()`'s sequential fast path (a
comment on this same line said as much, and was wrong for anything
larger) — past that threshold it falls back to one disk seek plus read
per output bucket, and at `kOverviewPoints = 1000` that means ~500 seeks
*per channel, per call* for any recording past 16MB. None of that
work was ever necessary on most of those calls: the overview shows the
*whole* recording compressed, which cannot change just from panning or
zooming within it — only the small marker showing where the current
zoomed view sits within that overview needs to move on every drag, and
that part (`m_nav->setViewport()`) was already cheap and unaffected by
any of this.

Fixed with a small per-channel cache: `m_overviewCacheTab` and
`m_overviewCacheTotal` record which recording (and how much of it) the
cached overview was last read for; `m_overviewCacheValid` (one bool per
channel) records whether each channel's own copy is current. All three
live in `MainWindow` rather than anywhere lower, since the decision of
"has anything actually changed" belongs with the caller that already
knows the current tab and already computed `total`, not inside
`RecordWriter` itself. Checked once, before the per-channel loop:
tab or total changing (a different recording loaded, or the current one
still growing during an active capture) invalidates every channel's flag
at once; per-channel state elsewhere — a channel toggled off then back on
— is naturally correct without any extra bookkeeping, since a channel
that was off never got to set its own flag true in the first place, so
turning it back on still finds a stale (false) flag and reads fresh, the
same as it always would have. The expensive read itself now only runs
when a channel's flag says stale; every other call — which, during an
ordinary drag, is nearly all of them — skips it outright and reuses
whatever `m_nav` is already showing.

## Zoom back/forward — two arrows drawn on the plot itself, one history per acquisition mode

**Two new buttons, drawn and hit-tested entirely inside `ScopeView`
(nothing added to the surrounding Qt layout), appear once there is
somewhere to navigate to — see the follow-up entry below for where they
actually ended up and why.** Scoped deliberately narrow: only
`zoomToRect()` (the drag-a-rectangle-to-zoom gesture) pushes to this
history — plain panning never does, and reported/fixed separately, was
never actually wired to. Double-click "reset zoom" (`resetZoom()`) does
not itself become an undoable history entry — going "back" from the
state right after a reset does not land you back in the moment right
before it, since reset was never pushed as a step in the first place —
but seeing the arrows still offer a route back to a zoom that reset just
explicitly asked to discard read as broken, not merely incomplete, so a
later follow-up (below) made reset clear both of the current mode's
stacks outright rather than only declining to add to them.

**`ScopeView`'s own half** mirrors the ruler box's existing
close/minimize buttons exactly, rather than inventing a different
mechanism for "a small clickable thing drawn on the plot": geometry
functions (`zoomBackRect()`/`zoomForwardRect()`) computed fresh from
`plotRect()` each time, drawn last in `paintEvent()` — after
`drawStatusStrip()` — so they sit on top of everything else, and
hit-tested first in `mousePressEvent()`, for the same reason (whatever
else a click might otherwise hit underneath one of these two rects, the
visible button on top wins). Two new signals, `zoomBackRequested()`/
`zoomForwardRequested()`, plus a public `setZoomHistoryAvailable(bool
canBack, bool canForward)` — each arrow only draws (and only accepts a
click) while its own flag is true, and clicking one arrow does nothing
whatsoever about what the *other* button restores; that decision lives
entirely on the `MainWindow` side.

**`MainWindow`'s own half** is one independent back/forward stack pair
per acquisition mode — `m_zoomBack`/`m_zoomForward`, both `std::array<
QVector<ZoomState>, 3>` indexed directly by the existing `AcqMode` enum
(`ModeCapture=0`/`ModeStream=1`/`ModeRecord=2`) — rather than one shared
history across all three. A history built up zooming around a Record
tab has no bearing on Capture's own, and letting "back" jump between
modes as a side effect would be a surprising, unrequested behaviour
change disguised as zoom navigation. `ZoomState` is a small, flat struct
holding every field any mode might need (Capture's own timebase plus
`ScopeView::viewportCentreFraction()` for its pan position; Live's
timebase plus `m_liveViewOffset`; Record's view start/length) —
deliberately not a variant or three separate types, since a flat struct
generic enough to sit in either stack regardless of which mode pushed it
is simpler than making each stack know which type its own mode uses.
`applyZoomState()` only ever touches the fields the *current* mode
actually reads, switched on `m_acquisitionMode` — it never switches
modes itself, since a state is only ever popped from the stack belonging
to whichever mode pushed it in the first place.

`pushZoomHistory()` — called as the very first line of `zoomToRect()`,
before anything else changes — appends the pre-zoom state and clears the
*forward* stack, the same convention a browser's own history uses: a
fresh zoom branches off from here, so whatever was only reachable by
going back past this point is no longer reachable from the new branch.
`navigateZoomHistory(bool forward)` — wired directly to both new
`ScopeView` signals — pops one entry from the requested direction's
stack, pushes the current state onto the *other* stack first (so the
opposite arrow can always undo the navigation itself), then applies the
popped state. `updateZoomHistoryButtons()` re-reads both of the
*current* mode's stack sizes into `ScopeView::setZoomHistoryAvailable()`
— called after every stack change and, separately, at the very end of
`setAcquisitionMode()`, so switching to a different mode immediately
shows that mode's own history rather than whichever mode's stacks were
last touched.

### Follow-up: the arrows sat inside the same region a plain pan-drag starts from, and reset needed to actually clear the history, not just decline to add to it

**Reported: dragging the waveform left and right — an ordinary pan,
nothing to do with the zoom-rectangle gesture — was somehow "picked up
as a zoom level" and jumping between different zoomed views instead of
panning smoothly.** The two arrows' geometry (`zoomBackRect()`/
`zoomForwardRect()`) originally placed them inside `plotRect()`'s own
top-right corner — which is exactly the region a plain drag turns into a
pan (`plot.contains(event->position())` is the very condition that
decides pan-vs-click in `mousePressEvent()`). The arrows' own hit-test,
checked first in that same function so a visible button always wins over
whatever is underneath it, was therefore also sitting inside the active
pan-drag area: a pan gesture that happened to start near that corner was
being caught as a click on whichever arrow was under it instead of ever
starting a pan, and since the two arrows sit side by side, a drag
starting slightly further left vs. slightly further right tended to hit
different arrows — which is exactly "dragging left and right switches
between zoom levels."

Fixed by moving both rects into the gutter strip directly above the
plot (`y` between 0 and `kTopGutter`, i.e. above `plot.top()` rather
than inside it) — genuinely outside `plotRect()`, so nothing there can
ever be mistaken for the start of a pan. Confirmed this gutter band was
actually free first: the per-channel "V" axis labels that also live up
there are drawn in the *side* gutters (`x` at 0 for the left axis,
`x` at `plot.right() + kScalePad` for the right one), never in the strip
directly above the plot's own width, so the two do not overlap. Also
confirmed the repositioned rects will not end up invisible behind an
active clip region: `drawMathTrace()` (the last function to set one, via
`setClipRect(plot)` for its own trace line) already calls
`setClipping(false)` again before returning, and every function between
it and the arrows' own `drawZoomHistoryButtons()` — `drawAxes()`,
`drawRulers()`, `drawAnnotations()`, `drawReadouts()`,
`drawStatusStrip()` — draws in the gutters itself without ever
re-clipping, which is only possible if clipping is already off by the
time each of them runs.

**Also reported in the same message: double-click "reset zoom" should
make the arrows disappear.** `resetZoom()` now clears both of the
*current* mode's stacks (`m_zoomBack[m_acquisitionMode]`/
`m_zoomForward[m_acquisitionMode]`) and calls
`updateZoomHistoryButtons()` at the end, rather than leaving them
untouched as the original implementation did. The distinction from
`pushZoomHistory()`'s own forward-clearing above still holds — reset
does not get appended as an undoable step in its own right, so a "back"
press one arrow-click before a reset still would not, in principle, walk
forward *into* one — but leaving a "back" arrow sitting there, visibly
offering a route back to a zoom the user just explicitly asked to
discard, read as broken rather than merely a narrower interpretation of
"history," so clearing outright is what actually shipped.

## Dragging a channel's vertical position slowly did nothing — the same class of bug already fixed once for horizontal panning

**Reported: dragging a waveform up/down slowly left it stuck in place;
only a relatively fast drag actually moved it.** `offsetDragged`'s
handler computed each new position as `m_channel[ch].offsetPct->value()
+ deltaPct` — reading the `offsetPct` spin box's own *displayed* value
back as the base for the next accumulation. That spin box calls
`setDecimals(0)` — whole percent only — so every delta too small to move
the rounded, displayed value by a whole percent was silently discarded
the moment it was read back on the next drag event. A slow drag is
exactly a long run of such deltas; a fast drag occasionally produces one
single delta large enough to survive the rounding by itself, which is
why only fast dragging ever appeared to work. This is the identical
failure `m_recordPanRemainder` already exists to fix for horizontal
panning (a small, sub-sample fraction thrown away on every event rather
than carried forward) — vertical dragging had never gotten the same
treatment.

Fixed the same way: a new per-channel `m_channelOffsetPctExact` holds
the true, unrounded percentage, kept in sync by
`setChannelPositionPercent()` — the one function every caller already
goes through regardless of *why* the position is changing (dragging,
typing directly into the spin box, settings restore) — rather than
threading a separate "exact value" argument through each call site
individually. `offsetDragged`'s handler now accumulates from
`m_channelOffsetPctExact[ch]` instead of the spin box's own rounded
`value()`; the spin box still only ever *displays* whole percent, but
the actual on-screen position (`m_view->setPositionVolts()`, computed
from the same unrounded value `setChannelPositionPercent()` just
received) now moves on every single drag event, not just the ones large
enough to survive being rounded and read back.

One gap this surfaced along the way: `restoreSettings()` sets `offsetPct`
via `setValue()` wrapped in `blockSignals()` — deliberately, so restoring
saved settings at startup does not trigger each spin box's own
`valueChanged` side effects — which meant `setChannelPositionPercent()`
never actually ran on that path, leaving `m_channelOffsetPctExact` at its
default 0 regardless of whatever non-zero offset had just been restored.
Fixed by explicitly assigning `m_channelOffsetPctExact[ch] =
c.offsetPct->value()` right after that blocked-signals block — without
it, the very first drag after restoring a saved non-zero offset would
have accumulated from 0 instead of the restored value, snapping the
trace to a visibly wrong position on the first drag event after every
launch. Checked for other call sites with the same shape (`setValue()`
inside a `blockSignals()` pair, bypassing `setChannelPositionPercent()`
entirely) and found none — this was the only one.

## Time cursors: bounded dragging, a pinned off-screen flag, and a reset position on re-enable

Three related fixes, all scoped specifically to the time cursors (T1/T2)
— the per-channel voltage cursors were deliberately left untouched
throughout, since none of the three reports mentioned them and they
don't share the same failure modes (a voltage cursor's own value has no
equivalent to "off the currently visible window" the way a time
position does).

**Dragging no longer passes the plot's own edges.** `mouseMoveEvent()`'s
`kRulerT1`/`kRulerT2` handling used to store whatever
`sampleIndexAtX(plot, pos.x())` returned directly — and that function
performs a plain linear mapping with no bound of its own, so a drag past
either edge just kept extrapolating the cursor's position indefinitely.
Now clamped to `[windowStart(total), windowStart(total) + length]` — the
same start/length `sampleIndexAtX()` already computes internally, just
reused at the call site — before being stored. Deliberately did not
change `sampleIndexAtX()` itself: annotations use that same function for
their own time-drag, and a dragged annotation reasonably can point
anywhere, including off the currently visible window, in a way a "where
is this cursor right now" ruler cannot.

**Zooming to a region that no longer includes a cursor no longer just
loses it.** `computeRulerLayout()` already computed `t1x`/`t2x` — the
cursor's true screen position, which can legitimately fall outside
`[plot.left(), plot.right()]` once you zoom into a region that doesn't
contain it — and the vertical dashed line already correctly skipped
drawing itself when that happens. The flag never had the equivalent
check, so it kept trying to draw at that true (and now potentially far
off-screen) x, landing somewhere invisible. New `t1OffScreen`/
`t2OffScreen` fields on `RulerLayout` record when this is happening;
when it is, the flag's own x is pinned flush against whichever edge the
cursor went past — flush against it, not centred exactly on it, since
centring would clip the flag in half rather than keep the whole thing
visible — so it stays exactly where a drag can reach it while the
underlying cursor position itself is completely untouched. A new
`drawPinnedFlag()` style (1px outline, no fill, no text — added
alongside the existing filled `drawFlag()` lambda in `drawRulers()`)
draws it this way specifically so it never looks like it's reporting a
real value at that position, since it structurally cannot be one. The
flag's hit-test (`layout.t1Flag.contains(...)`, already existing,
unchanged) picks up the pinned position automatically, since it reads
the same `RulerLayout` the drawing code does — clicking the pinned flag
and dragging it back into the zoomed view works with no additional
wiring.

**Re-enabling resets to a centred default.** `setRulersTimeEnabled()`
used to just flip `m_rulers.timeEnabled` — a cursor pair turned back on
kept whatever position it had before being turned off, which could
easily no longer even be on screen after zooming or panning in the
meantime. Now mirrors `setRulersEnabled()`'s own one-shot initial
placement (and `setRulersChannelEnabled()`'s identical pattern for
voltage cursors): on every `false → true` transition, not just the very
first time ever, T1/T2 reset to the same 30%/70% spread those two
already use — symmetric around the current view's own centre, but
visibly two separate cursors from the moment they reappear rather than
starting stacked on top of each other.

## Reference waveforms — snapshot or load a trace, overlay it, tracking the live timebase

A new feature end to end: keep a copy of a channel's trace (or a
previously saved one) overlaid on the live Capture/Live display, up to
two at once, each independently coloured, scaled, positioned, and
delayed — modelled on the reference-waveform UI in PicoScope 7 (Add row
of channel buttons, a Manage row for the currently loaded ones, per-
reference Name/Colour/Vertical scale/Vertical offset/Delay controls),
minus the digital-channel rows PicoScope has and this hardware does not.
Capture/Live only, by design — Record mode already has its own tab-based
way of comparing captures, and a reference tracking a *live* timebase
that Record mode is not currently showing would not mean anything
useful there anyway.

**The engine (`ScopeView`).** `ReferenceState` holds one reference's
samples, sample rate, trigger alignment, colour, name, volts/div,
position, and delay; `m_references` is a fixed two-slot array
(`MaxReferences = 2`).

- `addReferenceFromChannel(ch)` is the "Add" button's entire job, kept
  self-contained inside `ScopeView` rather than exposed piecemeal to
  `MainWindow`, since it needs `viewAnchor()` (otherwise private) to
  capture the live channel's current trigger alignment — what makes a
  freshly-added reference, at delay 0, line up exactly where the source
  channel was at that instant. Probe gain/offset, inversion, and DC
  offset are all baked into the stored samples at snapshot time (the same
  transform `valueToY()` applies to a real channel before its own
  volts/div ever comes into it), and the hardware's own input-offset
  (`windowCentre`) is subtracted out, since it is a hardware positioning
  concept with no meaning once the signal is just a saved array of
  numbers. `addReferenceFromData(...)` is the counterpart for restoring
  every field already read back from a `.vsxref` file.
- `referenceValueAt(slot, plot, x)` is the "tracks the live zoom and
  timebase" half of the feature: it converts a screen x to the *live*
  view's own time (`sampleIndexToTime(sampleIndexAtX(...))`, the same
  conversion the live trace and rulers already use), then maps that time
  into the reference's own sample-index space using its own sample rate
  and captured trigger alignment, linearly interpolating between the two
  nearest samples. Returns NaN when the mapped time falls entirely
  outside what was actually captured, which the drawing code reads as a
  gap rather than clamping to an edge value that was never measured
  there — so a reference shorter than the current view correctly shows a
  trace only where it actually has data, and re-tracks correctly as the
  live view is zoomed or panned rather than freezing to whatever view was
  active when it was captured.
- `drawReferenceTrace()` draws one sample per screen column (unlike
  `drawTrace()`'s own min/max-bar technique for a dense live capture,
  a reference is already resampled to the display's own resolution by
  `referenceValueAt()`, so a point per pixel already matches what can be
  shown) in a dashed pen, distinguishing it from both a live channel
  (solid) and Math (solid but dimmed) at a glance. `drawReferenceScale()`
  draws its own gutter column of voltage numbers, the same shape as
  `drawMathScale()`.
- Gutter layout: references stack on the *right*, outward from CH2's own
  column (`rightGutter()` now adds one `kMathColumnWidth` per occupied
  slot), mirroring how Math already stacks on the *left*, outward from
  CH1's. `drawReferenceScale()` compacts around gaps — if slot 0 is
  removed while slot 1 stays occupied, slot 1's column shifts in to fill
  slot 0's old position rather than leaving empty space or drawing past
  the widget's edge, since `rightGutter()` only reserves space for
  however many slots are actually occupied, not up to each slot's own
  fixed index.
- `Theme::referenceColor(slot)` (new, in `theme.h`) gives the two slots
  distinct amber/teal defaults, chosen to sit clearly apart from every
  channel default, from Math's purple, and from each other, retuned per
  light/dark mode the same way `channelColor()` already is.

**The panel (`MainWindow::buildReferencesPage()`, new `PageReferences`).**
Add row (one button per currently-visible channel, shown/hidden via
`refreshReferencesPanel()` reading `m_view->channelVisible(ch)`, disabled
rather than hidden when both slots are already full so it is obvious the
slots — not the channel — are what is blocking it); a File button for
loading a `.vsxref`; a Manage row of two toggle buttons (one per slot,
showing the reference's name once occupied, disabled otherwise) that
switch which slot's fields the Name/Colour/Vertical-scale/Vertical-
offset/Delay controls below are currently editing; Save (writes the
selected slot to `.vsxref`) and Remove. `refreshReferencesPanel()` also
follows the selection to the other slot if the currently-selected one
just emptied out, so the Manage controls never point at nothing while a
real reference sits one click away. Vertical offset is stored as a
percentage of the reference's *own* volts/div (independent of any
channel's own offset convention) purely for a familiar "% of a division"
feel; internally it converts straight to volts via `setReferencePositionVolts()`.

**Gutter budget (`enforceReferenceGutterBudget()`).** The plot has room
for 4 gutter columns at once — CH1, CH2, Math, and each reference all
compete for that budget. With CH1+CH2 fixed and up to 2 references
allowed, a second reference alongside Math would need a 5th, so Math
loses that fight: `enforceReferenceGutterBudget()` sums up currently-
visible channels + Math + reference count, and if it exceeds 4,
unchecks `m_mathEnabled` (letting `applyMathChannel()` run normally from
its own `toggled` signal). Hooked into the end of `refreshCards()` so it
is really an invariant check re-run on every channel/Math/reference
change, not a one-shot reaction — a channel turned on later, after
Math+2 references already filled the budget without it, still correctly
trips the check. Per the user's own specification this only ever turns
Math *off*: removing a reference afterward does not bring Math back on
its own, even though that would free a gutter — the user re-enables it
by hand once there is room again.

**`.vsxref` file format.** A reference is a small, self-contained blob —
one channel's worth of samples plus a handful of scalar fields — so
unlike a recording's own `.vsxrec` (a samples file plus a separate
`meta.ini`, built for a file that grows incrementally while being
written) this is one single file, framed with `QDataStream` end to end:
a magic number, a version, then name/colour/sample-rate/trigger-index/
volts-per-div/position/delay/samples in sequence, written and read in
one pass each way (`saveReferenceToFile()`/`openReferenceFile()`).

**Record-mode isolation — a real gap found and fixed while building
this.** `ScopeView` had no notion of which acquisition mode was active,
so a loaded reference would have kept drawing — and kept consuming
gutter space — even while Record mode was showing something else
entirely, and worse, would have kept resampling against whatever
timebase Record mode happened to be on, which was never a meaningful
alignment to begin with. Fixed with `setReferencesVisible(bool)`: gates
`drawReferenceTrace()`, `drawReferenceScale()`, and `rightGutter()`'s
reference contribution, without touching `m_references` itself, so nothing
is discarded — a reference reappears exactly as it was the moment
Capture/Live is returned to. `setAcquisitionMode()` now calls it
(`mode != ModeRecord`) right after the mode switch is committed.

**Two bugs found only once this actually got built.** First, a compile
error: `referenceValueAt()`'s `std::min(lo + 1, r.samples.size() - 1)`
mixed `int` with `qsizetype` (Qt6's `QVector::size()` return type),
which the template can't deduce a common type for — fixed by casting
the size to `int` explicitly, and double-checked every other
`std::min`/`std::max`/`std::clamp` added this session for the same
pattern (none found). Second, a startup segfault: `buildReferencesPage()`
called `refreshReferencesPanel()` immediately at its own end rather than
from inside a deferred lambda, and that function dereferences `m_view` —
which is still null at that point, because `buildDetailPanel()` (and so
every `buildXxxPage()`, including this new one) runs *before*
`m_view = new ScopeView` actually executes, despite that line sitting
textually earlier in this file. This exact quirk was already hit once
before and documented in `buildRulersPage()`'s own comment on
`m_rulersMathCheck` — missed here on the first pass regardless. Fixed by
guarding both `refreshReferencesPanel()` and `enforceReferenceGutterBudget()`
on `m_view` being non-null, not just on their own panel widgets.

## Four bugs found from actually using the References feature

**Gutters now fill left before overflowing right, for both Math and
references.** Previously Math always extended `leftGutter()` and
references always extended `rightGutter()`, fixed to each side
regardless of what else was already there — so CH1+Math+2 references
came out as 1 gutter on the left against 3 on the right, badly
unbalanced. New `computeGutterAllocation()` is the single source of
truth now: CH1 and CH2 stay fixed to their own sides as before, but
Math and each reference are "extra" columns placed dynamically — left
first (up to 2 gutters total on that side), then overflowing right once
the left side is full. Math gets first claim on an available slot ahead
of either reference (the common case is Math already on before a
reference gets added), then references in slot order — recomputed fresh
from current state on every call rather than tracked historically, so
removing then re-adding anything reflows cleanly with no stale
placement to clean up. `leftGutter()`/`rightGutter()` (for width) and
`drawMathScale()`/`drawReferenceScale()` (for where to actually draw,
now via a shared `extraColumnX()` helper handling both sides' text
alignment) all call this, so none of them can disagree about where
anything actually is.

**A reference's own gutter was dragging the adjacent real channel
instead.** Root cause: the mouse-drag hit-test's check for CH2's own
gutter was `x > plot.right()` — unbounded on the far side, so it caught
every column beyond CH2's too, references included (and now, with the
above change, potentially Math's as well if placed on the right).
Rewrote the hit-test to check Math's and each reference's own *precise*
range first (via `computeGutterAllocation()`/`extraColumnX()`, the same
placement the drawing code uses), before ever falling through to CH1/CH2's
own wider checks. Added `kReferenceDragBase` sentinels (`m_dragChannel`
already had one for Math, `kMathDrag`) and a shared `isGutterDragActive()`
helper used consistently in all three places that check it — worth
calling out because the very first pass of this fix updated the hit-test
in `mousePressEvent()` but missed that `mouseMoveEvent()`'s own separate
check (`m_dragChannel < 0 && m_dragChannel != kMathDrag`) would have
silently discarded every reference drag as if it were never active at
all, since a reference sentinel is `< 0` and isn't `kMathDrag` either.
New `referenceOffsetDragged(slot, deltaVolts)` signal, connected in
`MainWindow` the same way `mathOffsetDragged` already is — reading
`referencePositionVolts()`'s own exact stored value as the accumulation
base rather than a rounded UI spinbox, the same reasoning behind this
session's earlier slow-drag fix for channels, avoided here for free since
neither Math's nor a reference's position was ever backed by a
limited-precision spinbox to begin with.

**Gutters were about 17% too wide** across the board — `kScaleGutter`,
`kNarrowLeft`, `kNarrowRight`, and `kMathColumnWidth` all reduced by
that amount (`kTopGutter`, the vertical one, untouched).

**A saved `.vsxref` didn't show up in File's own listing.**
`saveReferenceToFile()` never appended the extension itself, and Qt's
native save dialog doesn't reliably do this on every platform when the
person types a bare name — fixed by appending `.vsxref` whenever the
chosen path doesn't already end with it.

## Save button — PNG and CSV export done, native format and Record's full-file CSV still pending

The top toolbar's Save button now does something: one
`QFileDialog::getSaveFileName()` with format chosen by filter, Capture/Live
offering the native `.vsxwave` alongside PNG/CSV, Record offering PNG/CSV
only (it already persists continuously via `.vsxrec` and Temporary/
Permanent, so a native re-save would just be a redundant second copy).
`saveCurrentView()` is the dispatcher; `ensureExtension()` (same idea as
the `.vsxref` fix above, now shared rather than duplicated a third time)
guarantees the right suffix regardless of what the person typed.

**PNG** (`saveViewAsPng()`, any mode): `m_view->grab()` — a straight
visual snapshot of the plot exactly as rendered, grid/traces/cursors/
annotations/references and all, rather than anything reconstructed from
the underlying data.

**CSV** (`saveViewAsCsv()`, Capture/Live only for now — see the Record
gap below): one column per enabled physical channel plus Math if it's
on, time as the first column using the same trigger-centred convention
rulers already use (new `ScopeView::timeAtSample()`, a public wrapper
around the previously-private `sampleIndexToTime()`). Channel columns
come from new `ScopeView::channelDisplaySamples()` — probe gain/offset,
inversion, and DC offset all applied, the actual measured value rather
than the raw ADC reading; `addReferenceFromChannel()` was refactored to
call this too instead of duplicating the same transform a second time.
Math's own column is computed fresh from its two sources'
`channelDisplaySamples()` (sum/difference/product of what was actually
measured), not from the existing `mathSamples()`, which combines the
raw, pre-probe values instead. References are deliberately left out of
the data columns — consistent with Open/Save and References being fully
independent systems, per the original design discussion for this
feature.

**A real gap, caught before it shipped silently: Record mode's CSV
export.** `refreshRecordView()`'s own comment says it outright — a
recording can be hundreds of millions of samples, but no more than a
few thousand downsampled points of the current slice ever get loaded
into the channels `ScopeView` actually holds. Calling the same
`channelDisplaySamples()`-based export in Record mode would have
silently produced a CSV missing the vast majority of the recording with
no indication anything was cut, rather than the "entire captured/
displayed buffer" this was supposed to mean. `saveViewAsCsv()` now
detects `ModeRecord` and shows "not implemented yet" instead of doing
that. The real implementation needs to read the `.vsxrec` file directly
from disk rather than what's currently loaded for display, almost
certainly on a background thread with progress/cancel given the file
sizes involved — separate work, not yet started.

**Also not yet implemented:** the native `.vsxwave` format itself
(Save's dialog offers the filter in Capture/Live, but choosing it
currently shows a "not implemented yet" message rather than writing
anything) and matching `.vsxwave` support in Open, which currently still
only recognises `.vsxrec`.

## Math/references didn't move into a disabled channel's own vacated gutter space

`computeGutterAllocation()`'s left-first placement (which side an extra
goes on) was already correct — the bug was in how much space and where
each placed extra actually got drawn once assigned to a side. Every
extra was always treated as the narrower "secondary" `kMathColumnWidth`,
positioned assuming that side's own channel was occupying its full
`kScaleGutter` slot — even when that channel was OFF and no such slot
existed. Two visible symptoms: with CH1 off, Math (or a reference)
placed on the left drew its numbers starting at `x = kScaleGutter`
while `leftGutter()` (via an early `return kNarrowLeft` whenever CH1
wasn't visible, before ever checking whether anything else was placed
there) only reserved 25px — so the column's own text rendered up to
17px inside what should have been live plot area. With CH2 off, the
mismatch ran the other way: `rightGutter()` reserved less width than
where the column actually started plus its own width added up to,
which is exactly what pushed a reference's column mostly or entirely
past the widget's own right edge — invisible, not just misaligned,
which is why the second screenshot showed no column there at all.

Fixed with a new `extraColumnWidth(onLeft, column)`: the first extra on
a side whose own channel is currently off takes over that channel's
full `kScaleGutter` width instead of being confined to the narrower
`kMathColumnWidth` while that space goes unused — this is the actual
"moves into the vacated space" behaviour. Any other extra (a second one
on the same side, or the first one when that side's channel IS visible)
still gets the ordinary narrower width. `extraColumnX()` now walks
through the preceding columns on the same side calling this helper
rather than a flat `column * kMathColumnWidth`, and `leftGutter()`/
`rightGutter()` sum the same per-column widths instead of a flat
`extraCount * kMathColumnWidth` — so a column's reserved space and
where it's actually drawn can never disagree again. Both `drawMathScale()`/
`drawReferenceScale()`'s own text-box width and the gutter mouse-drag
hit-test's column range were also switched from the flat
`kMathColumnWidth` to this same helper, since both had the identical
wrong assumption baked in.

## Native `.vsxwave` format — Save/Open's last missing piece — and Record's CSV export dropped for simplicity

**Record mode's CSV export is gone, not just unimplemented.** `saveCurrentView()` now builds its filter list from scratch per mode rather than always offering CSV and guarding against it inside `saveViewAsCsv()`: Capture/Live gets `.vsxwave`/PNG/CSV, Record gets PNG only. `saveViewAsCsv()`'s own Record-mode check (added when the gap was first caught) was removed too, since the dialog no longer routes Record mode there at all — dead code once the filter itself excludes it.

**`.vsxwave`: a full-state snapshot, both directions now implemented.**
One `QDataStream`-framed file, same shape as `.vsxref` but for an entire
Capture/Live view rather than one trace: magic/version, mode (Capture or
Live only — Save never offers this filter in Record), timebase, sample
rate, trigger point, full trigger config, then per channel (mode, volts/
div, display scale, coupling, probe name, invert, input offset, DC
offset, offset%, colour, and `channelSamples()`'s raw — not display-
scaled — samples), then Math's full config. Deliberately stores actual
values (volts, seconds) rather than combo indices, the same reasoning
`applyTimebase()`'s existing `exactOverride` and `restoreSettings()`'s
timebase restoration already use: an index depends on exactly which
options happen to be available at load time, a value can always be
nearest-matched back onto whatever the combo currently offers. New
`setComboByNearestValue()` generalizes that nearest-match pattern
(previously duplicated inline in three separate places in this file)
for reuse restoring volts/div, display scale, and Math's own volts/div
alongside timebase.

`openNativeWaveform()` replaces the current display, matching Open's
existing `.vsxrec` behaviour: stops acquisition, switches mode first if
needed (identical pattern to Open's own Record-mode switch), then
restores every setting by setting widgets normally — signals firing,
not blocked — so each widget's own existing handler reconfigures the
hardware to match, exactly as if the person had clicked through these
settings themselves. This is also specifically what makes pressing Run
afterward acquire fresh data under the exact restored settings rather
than whatever the instrument was previously configured for. Saved
sample data is injected last, after every widget change above has
already run its own side effects, so it's what actually ends up on
screen rather than being immediately overwritten by one of them. Two
new `ScopeView` getters support this: `triggerPoint()` (so a reload
reproduces the identical trigger-centred time axis) and `channelSamples()`
(the raw, pre-transform values `setSamples()` itself expects — distinct
from `channelDisplaySamples()`, which CSV export uses instead precisely
because it bakes the probe/offset/inversion transform in).

"Open" now accepts both `.vsxrec` and `.vsxwave` in one dialog, the
file's own extension deciding which of the two entirely independent
loading paths runs — matching how Open/Save and Record's own persistence
were designed to be fully separate systems from the start.

**One ordering bug caught before it shipped:** `openNativeWaveform()`
was first written referencing `kWaveMagic`/`kWaveVersion` from an
anonymous namespace that was still defined further down the file, right
before `saveViewAsNative()` — an undeclared-identifier compile error
waiting to happen. Moved the constants above their first use instead
of after it.

## A loaded .vsxwave's samples were getting overwritten with flat lines moments after loading

Reported from actual use: saving then reopening a waveform showed both
channels as flat lines at roughly the right vertical position, even
though the measurement readouts (still showing the original numbers,
never recalculated) made clear the save itself had captured real data
correctly.

Root cause: `openNativeWaveform()`'s own channel restoration deliberately
sets each control's real widget (mode, volts/div, scale, coupling)
rather than blocking signals — the whole point being that each one's
existing handler reconfigures the actual hardware to match, which is
what makes Run afterward acquire fresh data under the exact restored
settings. But reconfiguring the hardware this way can provoke the
device into sending its own `AcquisitionManager::samplesReady` on its
own timing, not synchronously within the call that triggered it — and
`wireUp()`'s existing handler for that signal unconditionally writes
whatever it's given straight into `m_view`/`m_nav`. A stray, empty (or
otherwise stale) frame arriving after the loaded samples were already
injected is exactly what silently replaced them with flat lines,
moments after the file had, in every other respect, loaded correctly.

Fixed with a new guard, `m_suppressSamplesReady`: raised right before
any of the widget changes that can trigger this, and the `samplesReady`
handler now returns immediately whenever it's set rather than acting on
what it's given. Cleared after the loaded samples are injected — but
via `QTimer::singleShot(250, ...)` rather than immediately at the end of
the function, since the stray frame is not guaranteed to arrive
synchronously within `openNativeWaveform()`'s own call stack; clearing
the guard right away would have reopened the exact same window the fix
was for. Verified there's no early-return path between where the flag
is raised and where the deferred clear is scheduled that could leave it
stuck permanently suppressing real data — the function always runs to
completion past that point once reached. `m_nav` (the overview strip)
is now also explicitly fed the loaded samples alongside `m_view` in the
same place, matching what the normal `samplesReady` path already does
for both together — previously only `m_view` ever got them from a
`.vsxwave` load, which would have been a second, quieter symptom of the
same "loaded state doesn't fully match a live one" gap.

**The 250ms timer above did not actually fix it — reported back as the
same exact problem.** Traced further and found the concrete mechanism
this time rather than a plausible-sounding guess: `applyChannel()` —
called repeatedly during the channel restoration loop, since volts/div,
coupling, probe, and both offset fields each trigger it independently —
makes a real hardware call every single time
(`m_acq->setRangeMv()`/`m_stream->setChannelRange()`), and across 4
channels that is potentially a couple dozen separate range-change
requests in quick succession during one load. Any one of those can
provoke the device into a delayed `samplesReady` on its own schedule, so
a fixed delay was never going to be reliable in either direction — too
short and a slow response still slips through (which is what was
observed), too long and it would start swallowing a person's own first
real acquisition if they pressed Run quickly after loading.

Replaced the timer with a well-defined event instead: `startAcquisition()`
now clears `m_suppressSamplesReady` itself, right at its own top, rather
than something elsewhere guessing when it's already safe to. This means
the guard now stays raised indefinitely after a load — deliberately: with
acquisition stopped throughout, nothing should legitimately produce a new
frame until a new one is explicitly requested, so there is no harm in
leaving it up for however long that takes, and no more need to guess a
duration at all.

## A third, different bug from the same feature: the loaded timebase was orders of magnitude too small

Reported after the two fixes above: the samples themselves now loaded
correctly this time (the overview strip showed the real waveform), but
the main plot showed only a tiny, nearly-flat slice of it, and the
ruler read nanoseconds where the timebase readout still said "1ms/div".
`windowLength() = m_secondsPerDiv * HorizontalDivs * m_sampleRate` is
what actually decides how much of the buffer is visible, and with
`m_sampleRate` confirmed correct (the ruler's own time arithmetic,
which uses it directly, was internally consistent, just tiny), the only
way to get a window that small is `m_secondsPerDiv` itself having ended
up wrong.

Traced as far as static reading could confidently go:
`ScopeView::setSecondsPerDiv()` silently ignores any value that isn't
strictly positive, leaving whatever was already there untouched with no
error or signal of any kind — and `openNativeWaveform()`'s own
`applyTimebase(timebase)` call sat *before* the channel, trigger, and
Math restoration loops, several of which fire real signal handlers
(`applyChannel()` in particular, which makes actual hardware calls) that
were never individually audited for whether any of them could touch the
timebase along the way. Rather than continuing to chase a specific line
through every one of those handlers, moved the fix to where it holds
regardless of which one (if any) turns out to be responsible: timebase
restoration now happens *last*, after every other widget in the
restoration sequence has already run and had its own side effects,
immediately before the loaded samples are injected. A non-positive
saved value now also falls back to the timebase combo's own current
value rather than being silently dropped, so a corrupt or unexpectedly
old `.vsxwave` file can't leave the previous, unrelated zoom state on
screen with no explanation.

Flagged here plainly rather than glossed over: this fix is verified
correct in what it does (apply the exactly-loaded timebase after
everything else, with no path left afterward for another widget's own
side effect to still override it), but the previous two rounds on this
same file both looked complete under review and still weren't once
actually run, so this is written up as "here's what changed and why it
should hold" rather than a confirmed root cause.

**Reported still broken in both modes after that fix — found the actual
mechanism this time.** `applyTimebase()` is not the passive "just set
secondsPerDiv" function its `exactOverride` parameter makes it look
like: unconditionally, regardless of that parameter, it also
reconfigures the sample rate — from `m_liveRate`'s current combo
selection in Live, from `m_acq`'s currently-configured device rate in
Capture — every single time it runs. It also recomputes History's
capacity and, in Capture, the capture depth and rate together, none of
which have anything to do with "reopen exactly what was saved." Calling
it at all during restoration meant the correctly-loaded sample rate got
overwritten with whatever the current combo/device state happened to
be — moments before `setSamples()` set it correctly again, which is
believable as a race that could resolve either way depending on exactly
what state the combos and device were in, rather than one that should
have existed to begin with. The combo's own `currentIndexChanged`
handler independently calls `applyTimebase()` with no override at all,
which is what `setComboByNearestValue(m_timebase, ...)` was triggering
on every call throughout both previous attempts, unrelated to the
`applyTimebase(timebase)` call sitting right next to it.

Fixed by not calling `applyTimebase()` anywhere in this function at
all: the timebase combo's selection is now set with signals blocked
(so its own handler never fires), and `m_view->setSecondsPerDiv()` /
`m_captureTimebase` or `m_liveTimebase` are set directly instead —
exactly the one, narrow effect restoration actually needs, with none of
sample rate, History, or capture depth attached to it. `syncNavigator()`
is called explicitly afterward, since that's the one part of
`applyTimebase()`'s own sequence that is genuinely still wanted (it
only syncs the overview strip's viewport highlight box) and has no
side effects of its own to avoid.

**Reported still broken after that fix too — same tiny-window symptom, a
third different saved timebase (100ms/div this time).** Four rounds of
reasoning through this by hand (samples-clobbering fix, timer-vs-explicit
guard clear, moving `applyTimebase()` to run last, removing the call to
it entirely) have each looked complete on review and still weren't once
actually run, which is a strong sign the remaining bug is not somewhere
static reading is finding it. Rather than guess a fifth time,
`openNativeWaveform()`'s own "Loaded" status message temporarily also
reports the raw `timebase`/`sampleRate` read from the file alongside
`m_view->secondsPerDiv()`/`m_view->sampleRate()` as they actually stand
once every restoration step above has finished — whichever of the four
doesn't match what was saved is the real next lead. **This diagnostic
line should come out once that's found** — it's not meant to ship.

**The diagnostic paid off: timebase/sampleRate restoration is confirmed
correct.** The next report showed `file: timebase=0.1 rate=20000`
exactly matching `view: secondsPerDiv=0.1 sampleRate=20000` — the value
that broke three separate rounds is now genuinely fixed. The flat trace
persisted anyway, but as a *different* problem: the person separately
confirmed the overview strip (`m_nav`) draws the real waveform
correctly while the main plot doesn't, which rules out corrupted sample
data (both widgets are fed the same array) and points specifically at
`ScopeView`'s own windowing, which `NavigatorView` doesn't share.

Found a second, real, previously-invisible gap while chasing this:
`viewAnchor()` — what `windowStart()` centres the visible window on —
checks `m_triggerArmed`, a separate flag from `m_triggerPoint`, and
falls back to the middle of the whole buffer whenever it's false.
Restoring the trigger widgets never actually calls `setTrigger()`
(none of those widgets' own signal handlers do it either), so
`m_triggerArmed` was staying at whatever it happened to be before the
load — the correctly-restored `triggerPoint` was likely never even
being consulted. Fixed by calling `m_view->setTrigger(...)` directly
after the trigger widgets are restored, with `armed` unconditionally
true regardless of mode: a loaded `.vsxwave` always represents a
completed capture with a known trigger instant, never an active,
still-arming state. Deliberately not going through `applyTrigger()` to
get there, for the same reason `applyTimebase()` was removed from this
same function earlier — that function makes real hardware calls
`setTrigger()` on its own does not need.

Whether this fully explains the flat trace or is one more real bug
found along the way to a still-undiagnosed one remains open — the
diagnostic message added above is left in place for the next test to
answer that directly rather than guessing further.

**Actual root cause, found from the screenshots rather than the code:**
in every failing report the visible window spanned exactly 2 samples
(650 ns / 800 ns at 2.5 MS/s, 100 us at 20 kS/s) - `windowLength()`'s
clamp minimum. `ScopeView::setSamples()` sets the view-wide
`m_lastLength` to the length of *that call's* array, and
`openNativeWaveform()` fed all four channel slots in order. On this
2-channel hardware CH3/CH4 are saved as empty arrays, so the final call
reset `m_lastLength` to 0 and clamped the window to 2 samples. The
overview strip keeps per-channel data with no shared length, which is
why it always looked right. The normal acquisition path and the
per-mode snapshot restores never hit this because they only feed
channels that actually have data (`valid[ch]`). Fixed by feeding only
non-empty, not-off channels and `clearChannel()`-ing the rest (which
does not touch `m_lastLength`), then calling `syncNavigator()` once the
samples are in. The temporary status-bar diagnostic was removed. The
earlier `applyTimebase()` removal and `setTrigger()` arming were real
issues too, but this was the one producing the flat trace.

**Lesson worth keeping:** any new code path that loads data into
`ScopeView` must never call `setSamples()` with an empty array for a
channel - use `clearChannel()` instead.

## Loaded waveforms: Live panning and surviving mode switches

**Live couldn't pan a loaded file.** Every Live navigation path (drag,
navigator click, box zoom, reset zoom, `refreshLiveView()` itself)
re-reads a window from the stream's ring buffer, which never contains a
loaded file. New `m_liveLoaded` flag (set by `openNativeWaveform()` for
Live files before any restoration runs, cleared only when Live itself
starts acquiring): `refreshLiveView()` returns early, drag and the
navigator use Capture's static-buffer path (`panBySamples()` /
`setViewCentreFraction()`), box zoom uses Capture's branch but writes
`m_liveTimebase`, reset zoom recentres, and `applyTimebase()` no longer
overwrites the view's sample rate with the Live rate combo's value.

**Live saves only ever held one screen**, since ScopeView in Live only
holds the window `refreshLiveView()` last read — nothing to pan to once
reopened. `.vsxwave` is now version 2: a Live save reads the stream's
own history (capped at 4M samples/channel, centred on the visible
window) and appends the view centre fraction and Live's absolute time
origin, so a reopened Live file shows the same window, on the same
"time before newest" axis, with the surrounding history pannable.
Version 1 files still open (no centre/origin: centred, trigger-style
axis).

**Loaded data vanished on switching modes.** `setAcquisitionMode()`
restores each mode from `m_captureSnapshot` / `m_liveSnapshot`, which
the loader never wrote. It now fills the target mode's snapshot (only
channels with data, per the `setSamples()` rule above). Live's restore
assumed a single ring window when computing the time origin; for a
loaded file it now re-applies the file's own origin and the pan position
saved on leaving Live (`m_liveLoadedCentre`).

**Time grid didn't follow a dragged loaded Live file.**
`timeAxisOffsetDivs()`'s absolute-origin branch assumed the buffer's
first sample sits at the left edge (true for a Live ring window and a
Record slice, whose buffer *is* the visible window). A loaded Live
buffer pans within itself, so the branch now adds `windowStart()` /
rate, the same way the trigger-centred branch already accounts for the
window start. Rulers were unaffected (`sampleIndexToTime()` already used
the real index).

## Overview strip showed the current page instead of the whole capture, in Live and Record

**Live fed the overview the same windowed slice as the main plot.**
`refreshLiveView()` called `m_nav->setSamples(ch, samples)` with the
exact same `samples` array `m_view` got — the ring-buffer window sized
for the current timebase, not the full available history. The overview
was never actually an overview; it was a second copy of the main plot.
Fixed by no longer feeding `m_nav` there at all, and instead reading the
full available ring history (`historyAvailable()`, capped at 4M samples
like the `.vsxwave` Live save already does) separately, min/max-decimated
to ~1000 points per channel the same way `drawTrace()`'s own dense-data
path avoids aliasing at a similar ratio. That read is real work, so it is
throttled to roughly Record's own 400ms cadence (new
`m_liveOverviewRefresh`, started alongside Live streaming itself)
rather than running on every 33ms tick.

**Record's overview cache tracked the wrong thing.** `refreshRecordView()`
already read the *whole* recording for the overview and cached the
result to avoid re-reading a possibly large file on every pan or zoom —
correct on its own. But the cache's validity (`m_overviewCacheValid`)
only tracked whether the recording's own data had changed, not whether
`m_nav`'s widget content had — and `m_nav` is shared with Capture/Live,
which overwrite it the moment either mode runs. Switching away from
Record and back left the tab and total unchanged, so the cache stayed
"valid" and skipped re-feeding `m_nav` entirely, leaving it showing
whatever Capture or Live had put there in between. Fixed by caching the
actual decimated arrays (new `m_overviewCacheSamples`), not just a
validity flag, and re-feeding `m_nav` from that cache on every call
regardless of hit or miss — only the disk read itself is skipped on a
hit, never the re-feed.

**Record was still wrong after that — two more causes, found from the
screenshot.** First, the viewport box covered the whole strip:
`applyRecordRate()` runs on entering Record *after* the tab is loaded
and `refreshRecordView()` has positioned the box, then called
`m_view->setSampleRate()` and `syncNavigator()`. In Record ScopeView's
buffer is the decimated page itself, so `syncNavigator()` always reports
it as 100% of the data, and the hardware rate overwrote the page's
windowing rate. It now calls `refreshRecordView()` instead whenever a
recording is on screen. The navigator's double-click
(`followTriggerRequested`) had the same unguarded `syncNavigator()` and
now does nothing in Record, which has no trigger.

Second, the strip drew two dotted lines per channel instead of a band.
`NavigatorView` draws one min/max bar per pixel column from the samples
falling in it; Record's overview is 1000 points (500 min/max pairs) on a
strip ~1170 px wide, so most columns held a single point — a dot at
either the min or the max — and empty columns were skipped. Each column
now spans at least two points. Capture never showed this because its
overview has far more samples than pixels.

## Persistence mode (Instruments, Capture only)

Design agreed with the user: drawn on the main plot itself (replacing the
channel traces), digital-colour heat map by hit count, Capture only.
Math, references, rulers, cursors, annotations and measurements still
draw on top as normal.

**ScopeView.** `setPersistenceEnabled()` / `clearPersistence()`. While
enabled, every `setSamples()` calls `accumulatePersistence(channel)`,
which rasterises that channel's visible window into `m_persistHits` (one
`quint32` per plot pixel): one vertical run per column covering the
column's samples plus the next one, so dense captures behave like
`drawTrace()`'s min/max bars and sparse zoomed ones still join up; each
pixel counts at most once per channel per frame, and runs entirely off
screen are skipped rather than clamped (clamping would pile hits on the
plot edge). `drawPersistence()` rebuilds a cached `QImage` only when the
hits changed, colouring with a log-scaled blue-cyan-green-yellow-red
LUT (log so steady edges don't drown out the rare events persistence is
for).

**Auto-reset keyed to the trigger-relative mapping.**
`persistenceSignature()` records plot size, timebase, sample rate,
window length, pan offset/follow state, trigger-armed, and each
channel's volts/div, display scale, position, offsets, probe and
inversion. Any change resets the accumulation on the next frame.
Deliberately *not* `windowStart()`: the trigger can land at a slightly
different buffer index each capture, and keying on the absolute start
would reset on nearly every frame. Until the next capture rebuilds the
map after a change, `persistenceUsable()` fails and the normal traces
draw instead of a blank plot.

**MainWindow.** `InstrumentMode::Persistence` (button id 3, previously
"not implemented"), no second pane, a "Clear persistence" button
(`m_persistenceControls`) on the Instruments page. Leaving Capture
already forces Scope, which disables it.

**Per-channel colours (follow-up request).** Hit maps and peaks are now
per channel (`m_persistHits[ch]`, `m_persistMax[ch]`), each scaled
against its own peak and coloured with its own LUT from
`persistencePalette(ch)`: CH1/CH3 blue-cyan-green-yellow-red, CH2/CH4
violet-magenta-pink-white, which shares no colours with the first so the
channels stay distinguishable. Channels paint in order, so CH2 draws over
CH1 where both have hits. The shared signature still resets all maps
together.

## Full and Default buttons

**Full** (top bar) — `setFullView(bool)`. Hides everything around the
plot splitter (top bar, channel rail, detail panel, record tabs,
navigator, status bar), zeroes the root layout margins, and goes
`showFullScreen()`; a Spectrum/XY pane, if open, stays since it lives in
the splitter. Each hidden widget's prior visibility, the margins and the
prior window state (maximised / full screen / normal) are saved and put
back on exit. Exit: Esc (a `QShortcut` enabled only while full, so Esc is
untouched otherwise) or a small button created on first use as a child of
the ScopeView, pinned at (3,3) in its top-left corner — a fixed offset,
so no resize handling is needed; it covers CH1's "V" unit label while
full. Its icon/style are re-applied on each entry to follow the theme.

**Default** (sidebar) — `resetToDefaults()`, behind a Yes/No
confirmation. Applies the same fallback values `restoreSettings()` uses
on a first run, through the widgets with signals firing so every handler
reconfigures the hardware: Instruments back to Scope (which also turns
persistence off), rulers off, rates/depth/record size/persistence,
trigger (None, rising, source 0, 0 V, 50%), channels (CH1 manual, others
off; default range, x1, DC, no invert, zero offsets and position,
default colours, measurements off — the on/off handler only listens to
`idClicked`, so that signal is emitted explicitly), Math off with default
sources/page/colour/measurements, timebases, then `applyTrigger()`,
`resetZoom()` and a card refresh. Deliberately kept: reference
waveforms, annotations, recordings, probe library, theme, record
directory.

**Follow-ups.** Full view's on-plot exit button was removed at the
user's request; Esc (shortcut enabled only while full) is the way out.

Persistence no longer resets on opening a side panel, vertical
dragging, or panning. The hit maps moved from screen pixels to data
coordinates: one `PersistGrid` per channel, x in samples relative to
`viewAnchor()`, y in volts before the channel's own `positionVolts`.
Bin sizes come from the view when a grid starts (about one bin per
pixel); the grid spans the whole capture horizontally (so panning shows
history outside the window) and three screen heights vertically centred
on mid-screen (so a trace dragged up to a screen away keeps its
history), capped at 4M cells (16 MB) per channel, beyond which bins get
wider. `drawPersistence()` maps each screen column/row back to the bin
span it covers and takes the busiest bin in it, rebuilding its cached
image whenever the hits or the mapping (`m_persistViewKey`: plot size,
window, anchor, positions) change. `persistenceSignature()` now only
holds what changes bin *meaning* — timebase, sample rate, trigger armed,
and per channel visibility, volts/div, display scale, DC offset, input
offset, probe and inversion — so zooming and volts/div changes still
start over, but resize/drag/pan do not.

## Pre-decoding audit

Scanned for the bug classes that bit this session, then fixed what was
confirmed:

1. **Single after opening a file dropped its capture.** Single calls
   `m_acq->single()` directly, bypassing `startAcquisition()`, the only
   place `m_suppressSamplesReady` was cleared. It now clears it too.
2. **Loader didn't apply channel on/off to hardware.** `setChecked()` on
   `c.mode` doesn't emit `idClicked`, which is all the on/off handler
   listens to; the loader now emits it (as Default does). Startup is
   unaffected — `onDeviceChanged()` re-reads the buttons itself.
3. **Loader didn't apply trigger mode/edge to hardware** (same cause).
   `applyTrigger()` now runs before the view-only `setTrigger()`, which
   must come last since `applyTrigger()` disarms the view in Live.
4. **References were misplaced in Live.** `referenceValueAt()` used
   `sampleIndexToTime()`, which is absolute ("before newest") in Live
   while a reference's `triggerIndex` is anchor-relative — about half a
   screen off. Now uses anchor-relative time directly.
5. **Live overview wrong at long histories.** It read only the newest 4M
   samples while the viewport box used the full history length. It now
   walks the whole history in 1M-sample chunks into exact per-bucket
   min/max (1000 buckets).
6. Hardening: Capture's `samplesReady` handler never passes an empty
   array to `setSamples()` (keeps the last frame instead).

Checked and clean: no `int`/`qsizetype` mixes in any
`std::min`/`max`/`clamp`; every other `setSamples()` call is guarded;
all top-bar and sidebar buttons have handlers except Decoding.

**Known, not fixed (improvements):** Capture's pan position still isn't
kept across mode switches (`viewportCentreFraction()` now makes it easy);
`sectionLabel()` needs a restart after a theme toggle; Record's overview
is 500 buckets (the navigator fix hides gaps, not the coarseness); zoom
back/forward on a loaded Live file restores timebase but not pan; Live
CSV exports only the visible window; two unreachable "not implemented"
fallbacks remain (top bar, Instruments). The Live overview now reads the
whole history every 400 ms — fine at this scope's streaming rates, but
the first thing to throttle if Live ever feels heavy.

## Protocol decoding: CAN / CAN FD, LIN, K-Line (libsigrokdecode)

Requirements agreed with the user: CAN (classic + FD), LIN and K-Line;
decoded values as labelled bus rows on the plot; all three modes; two
decoders at once; CAN probed either single-ended (CAN-H or CAN-L) or
differentially, selectable per decoder; use an existing library.

**Library: libsigrokdecode 0.5.3** (user's Arch package; confirmed it
ships `can` with CAN FD support — `nominal_bitrate`/`fast_bitrate` — plus
`lin` and `uart`). Optional at build time: CMake `pkg_check_modules(SRD
libsigrokdecode)` defines `VSX_HAVE_SRD`; without it the app builds and
the Decoding page says what to install. GPLv3 — matters only if the app
is ever distributed. API calls verified against sigrok-cli / DSView for
the 0.5 form (2-arg `srd_inst_channel_set_all()`, unit size on
`srd_session_send()`); a `#error` guards pre-0.5.

**K-Line** has no sigrok decoder, so `decoders/vsx_kline/` is our own PD
stacked on sigrok's `uart`: KWP2000 framing from the format/length bytes
(target/source, SID with names for common services, data, sum checksum
checked) or ISO 9141-2 gap framing (message ends at an idle gap; gap =
25 ms in bit times). Framing logic was tested offline with a stand-in
`sigrokdecode` module (request/response, bad checksum, gap framing).
CMake copies `decoders/` next to the executable on every build;
`srd_init()` gets that folder as its extra search path.

**Engine** (`decoderengine.h/.cpp`): a `DecoderEngine` QObject living on
its own `QThread` for the app's life, so libsigrokdecode and its embedded
Python are only touched from one thread. A `DecodeJob` carries its own
copy of the analog trace (display volts), the threshold/invert that make
it a logic line, and protocol settings; the engine thresholds to one bit
per byte, builds the stack (`can`, or `uart` + `lin`/`vsx_kline`),
sends the whole buffer, collects annotations from the top decoder only
(mutex: callbacks come from sigrok's instance threads), drops per-bit and
stuff-bit classes, marks warning/error classes, and emits a
`DecodeResult`.

**ScopeView**: `setBusRow()` / `drawBusRows()` — rows stacked along the
bottom of the plot, drawn over traces and under rulers; annotations are
in the view's own sample-index space so they follow zoom/pan; boxes
wholly inside a wider one are dropped (CAN full ID vs its halves),
errors always kept and drawn red on top; too-narrow boxes become ticks;
the longest text that fits is shown. `dataGeneration()` increments on
every data change so decoding re-runs only when needed; `displayValue()`
is the per-sample probe/offset transform.

**MainWindow**: Decoding page (`PageDecoding`) with two slots (protocol,
input, threshold + invert with per-wire defaults — CAN-H 3.0 V inverted,
CAN-L 2.0 V, CH1−CH2 0.9 V inverted, LIN/K-Line 6.0 V — bit rate, FD
data rate, sample point, LIN version, K-Line baud/framing). A 250 ms
timer (`onDecodeTick()`) posts a job for each enabled slot whose data or
settings changed, one in flight per slot so a slow decode never queues.
Capture/Live decode exactly what ScopeView holds; Record reads raw
samples for the visible span from the file (`StreamEngine::read()`, up
to 4M samples, else "zoom in") and maps results back onto the decimated
page via an index scale. Fewer than 4 samples per (nominal) bit shows a
"sample rate too low" message instead of decoding. The destructor stops
the decoder thread before teardown.

**Not yet done:** decoder settings aren't persisted across restarts;
the last, still-open frame at the end of a buffer isn't reported (0.5.3
has no end-of-data call); only the nominal CAN rate is checked against
the sample rate (an FD data phase above it can still be undersampled);
no packet table (bus rows only, as agreed).

The Decoding page is wrapped in a `QScrollArea` (same setup as the channel
pages) — two full slots exceed the panel height on most screens.

**Panel polish (from a screenshot).** `openPage()` had no title for
References or Decoding, so both fell through to the final `else` and read
"Trigger" — added. Combo popups hovered in a dark colour: under the app
style sheet Qt uses a menu-style delegate that ignores the item rules, so
`applyTheme()` now gives every combo a `QStyledItemDelegate` and the
global QSS has explicit `::item:hover`/`:selected` rules in the accent.
The Decoding scroll area was tinted (its viewport auto-filled with the
window colour) — it, its viewport and the page are now transparent. Its
scroll bar sat inset over the content because the panel had a 10 px right
margin around every page: that margin moved onto each page
(`setContentsMargins(0,0,10,0)`) except Decoding, whose scroll area now
reaches the panel edge while its content keeps the 10 px gap. The channel
pages' scroll areas still use the old inset arrangement.

## Annotations

A floating text note (`AnnotationBox`, its own small widget class) with an
optional draggable arrow whose tip tracks a specific sample and channel.

**`AnnotationBox` itself is deliberately minimal**: a bordered, padded,
auto-growing `QPlainTextEdit` and nothing else — no header, no trash button,
no resize grip. It knows nothing about waveforms, samples, selection chrome,
or the arrow. `ScopeView` draws and hit-tests all of that itself, directly
on the canvas, the same way it already draws and hit-tests Rulers' own
close/minimize buttons (see `rulerCloseRect()`/`rulerMinimizeRect()` and
their `drawText()` callers) — painter output plus a rect-contains check, not
real child widgets.

**This split is not a style preference, it is a bug fix.** The first version
of this feature gave `AnnotationBox` its own internal header, trash
`QToolButton`, and resize grip, with the trash button positioned just
outside the box's own right edge (`move(width() + gap, ...)`), reasoning
that Qt widgets happily render outside their parent's rectangle. They do
not: **a child widget is clipped to its parent's geometry**, so a child
positioned even partly outside that rectangle is invisible wherever it falls
outside it — that is exactly what made the trash button silently disappear.
Externalizing all of this chrome into `ScopeView`'s own paint layer (a much
bigger canvas that can comfortably contain positions well outside any one
box) sidesteps the whole class of bug rather than working around one
instance of it. If a future feature needs a small control positioned near
but outside some other child widget in this file, take this as the standing
warning: draw it on the canvas instead, or make the containing widget
deliberately larger than its own visible content.

**Selection model**: `ScopeView::m_selectedAnnotation` is a single index
(not a per-annotation flag), since only one `AnnotationBox` can hold
keyboard focus at a time — there is nothing to gain from tracking it any
more richly than that. `AnnotationBox` has no notion of "selected" itself;
it just reports focus changes on its internal text edit via
`selectionChanged(bool)`, forwarded through an event filter (`eventFilter()`
watching `QEvent::FocusIn`/`FocusOut` on `m_text`) since `QPlainTextEdit`
has no focus-changed signal of its own. Only the selected annotation shows
its external chrome (trash, move handle, 8 resize handles) and, if it has
not been activated yet, its default not-yet-aimed arrow — see
`drawAnnotations()`/`hitTestAnnotations()`, both gated on `i ==
m_selectedAnnotation` for anything that is not already `active`.

**The deselection race, and why it is deferred.** The trash/resize/move
chrome sits *outside* the selected box's own rectangle — on `ScopeView`'s
own surface, not on any child widget. Clicking there therefore moves Qt's
keyboard focus to `ScopeView` itself first (it has `Qt::ClickFocus` — see
below), off the annotation's text edit, *before* `ScopeView::mousePressEvent`
gets a chance to read `m_selectedAnnotation` for that same click. Clearing
`m_selectedAnnotation` synchronously on the resulting `selectionChanged(false)`
would make that a race the click always loses — the chrome would vanish out
from under the very click meant to use it. The fix: the `false` branch of
that connection in `addAnnotation()` defers the actual clear via
`QTimer::singleShot(0, ...)`, re-checking that nothing has since selected a
different annotation before clearing. `mousePressEvent`'s own synchronous
handling always runs first, seeing the still-correct index, regardless of
exactly when within Qt's dispatch the focus reassignment itself happens —
deferring the *state mutation*, not just the signal, is what makes this
robust rather than order-dependent. The `true` branch needs no such
deferral — there is no analogous "in-between" state when *gaining* focus.

**Why `ScopeView` needs `Qt::ClickFocus` at all**: without it, `ScopeView`'s
own background has the `QWidget` default of `Qt::NoFocus`, so clicking the
plot itself does not take focus away from whatever child currently has it —
an annotation's text edit would keep its blinking caret and keep accepting
keystrokes after a click plainly meant to be "away from it". Set once, in
the constructor.

**The arrow**: fully optional. `AnnotationState{channel, timeOffset, volts}`
mirrors a Ruler voltage cursor exactly — `timeOffset` is a sample offset
from `viewAnchor()` (never an absolute index, same jitter-correction
reasoning as Ruler cursors), `volts` is raw pre-probe volts in that
channel's own space, rendered via the same `valueToY()`/`mathValueToY()` a
real trace already uses. A freshly-created (or freshly-reselected) inactive
annotation has no real target yet: `annotationTipPos()` returns a point a
fixed distance straight down from the box's *current* geometry, recomputed
every call rather than ever stored, so it tracks the box around and never
"sticks" to some arbitrary position from when it was created. The first
drag on that default tip (`mousePressEvent`'s `hitTestAnnotations()` branch)
seeds real `timeOffset`/`volts` from exactly where the click landed —
visually seamless, since that is where the default tip already was — and
flips `active = true`, after which it behaves exactly like a Ruler cursor:
permanent, and draggable regardless of selection state.
`hitTestAnnotations()` mirrors the same visibility rule as drawing (skips
an annotation that is neither `active` nor selected) — invisible is never a
valid drag target either.

**The connecting line is anchored at the box's centre, not an edge**,
deliberately: the box widget always paints on top of `ScopeView`'s own
output (normal Qt child-on-top compositing), so whatever part of a
centre-to-tip line falls inside the box's rectangle is simply covered
regardless of direction. This gets a line that appears to emerge from
whichever edge faces the tip — for free, with no per-direction anchor-point
logic — because the geometry that would need to be computed to pick the
"right" edge explicitly is exactly the geometry Qt's own compositing already
throws away for us.

**Resize handles are pushed outward from the box's own border, not centred
on it** (`annotationHandlePoints()`), for the same reason: a handle centred
exactly on the border would have its inner half covered by the box widget.
Drawing and hit-testing both call this one function, so the two can never
drift apart — same principle as `computeRulerLayout()` above.

Session-only, like Rulers' own state — not persisted to `QSettings`. Worth
doing the same way if asked (channel, timeOffset, volts, box geometry, text,
and active/selected state all serialize trivially).

## Measure

A per-channel, per-measurement-type opt-in: `ScopeView::Measurement` (`Vpp`,
`Vrms`, `Mean`, `Max`, `Min`, `Frequency`, `AcRms`, `Period`, `DutyCycle`,
`PositiveWidth`, `NegativeWidth`) and `Channel::activeMeasurements` (a
`std::array<bool, MeasurementCount>`, indexed by that enum). Math has the
same array on its own `MathState`, since it is a selectable source too (see
below). Kept even while the channel itself is off, deliberately — unlike
Rulers, which force-
disables a channel's cursor the moment that channel goes off (see
`refreshRulersChannelList()`). There is no equivalent risk here: `drawReadouts()`
already skips any channel with `!c.visible` entirely, so a channel's stored
measurement set simply has no effect while it is off and picks back up
exactly where it was left if the channel comes back on. Preserving it costs
nothing and avoids losing configuration for no reason.

**This is a behavior change, not a new independent feature bolted on:**
`drawReadouts()`'s per-channel line used to show a fixed, always-on
Vpp/Vrms/Mean/frequency summary for every visible channel unconditionally.
It now shows only whichever of the eleven are checked for that channel — none
of them, by default, matching how every other opt-in feature in this app
starts (Rulers, Math, Annotations all start off too). A channel with nothing
checked shows the plain "CHn ±volts" line and nothing more.

**The panel is one exclusive channel selector, not a per-channel list.**
Rulers' own channel list works because there is exactly one thing to toggle
per channel (its voltage cursor); showing every source's full eleven-
checkbox grid at once here would be a lot of simultaneous UI for what is
normally a "check one channel's numbers" task. `m_measureChannel` is
MainWindow-side UI state — *which* channel's stored set the checkboxes are
currently showing — completely separate from the stored data itself, which
lives entirely in `ScopeView`. Switching the selector only changes what the
checkboxes reflect; it never touches any channel's actual set.
`refreshMeasureChannelSelector()` mirrors `refreshRulersChannelList()`'s own
"hide what does not apply" rule (call sites: `applyChannel()`,
`applyMathChannel()`'s equivalent, and `openPage()`'s `PageMeasure` branch),
plus a fallback for the one case Rulers does not have to handle — the
channel currently *selected in the exclusive control* going off — by
switching to the first channel still on.

**`Measurements::max`/`min` needed the inversion sign to be handled by hand,
not just scaled.** `measure()` already computed raw `hi`/`lo` internally to
derive `vpp`, but never exposed them. The obvious `m.max = *hi * gain +
bias` is wrong once a channel is inverted: `gain` carries the inversion's
sign, so a *negative* gain maps the raw maximum to the smaller displayed
value, not the larger one. Both raw extremes are scaled first, then
`std::max`/`std::min` picks the actual displayed max/min from those two —
correct regardless of inversion, rather than assuming the raw ordering
survives the scaling.

**A font-leak bug found and fixed while wiring up this section's own
display, worth remembering for any future glyph drawing in this file.**
`drawAnnotations()`'s glyph-button helper (trash and move-handle icons) did
`p.setFont(fontAwesomeFont)` to draw a Font Awesome glyph and never restored
it. Every draw call after it in the *same* `paintEvent()` — `drawReadouts()`,
`drawStatusStrip()` — shares that one `QPainter`, so the leaked FontAwesome
font silently carried into all of the plot's ordinary text (axis labels,
channel readouts) the moment any annotation became selected. `p.font()`
captured before the switch and restored right after the `drawText()` call
fixes it. Any future code that switches the painter's font (or family, not
just size — the existing small-font tweaks in `drawRulers()`/`drawReadouts()`/
`drawStatusStrip()` only ever shrink the *current* font and happen to get
away with never restoring it) should restore it explicitly rather than
assume the next drawing function will fix it by re-deriving its own.

**Vpp/Vrms/Mean/Max/Min/AcRms and Frequency/Period/DutyCycle/±Width are two
genuinely different kinds of measurement, computed in one pass for
efficiency, not because they are the same kind of thing.** The first group
needs only the sample values themselves (sum, sum of squares, extremes) and
is always available whenever there is a capture at all. The second group
needs *timing* — where mid-level crossings actually fall — and can
legitimately be unavailable (a DC signal, or too few edges) even when the
first group is perfectly valid. `Measurements::valid` covers only the first
group; each timing value defaults to `0.0` meaning "not measurable" (the
same convention `frequency` used before any of this existed) and
`formatMeasurements()` checks each one individually before showing it,
never assuming that `valid` implies they are all present too.

**Pulse widths and duty cycle come from the same crossing loop frequency
already had, extended rather than duplicated.** The original loop only ever
recorded the *first* and *last* rising-edge index, discarding everything in
between — enough for an averaged frequency over the whole capture, but not
enough to measure any individual pulse. It now tracks the index of
whichever edge was seen last and its direction, and the moment the
*opposite* direction is seen, that gap becomes one pulse's width, added to a
running sum for its side (`sumHighTime`/`sumLowTime`) rather than stored
per-pulse — a multi-cycle capture could have a lot of pulses, and only the
average is ever shown. Duty cycle is `avgHigh / (avgHigh + avgLow)`, not
`avgHigh / period` — self-consistent with the two width numbers next to it
on the same panel, since both come from the same paired transitions, rather
than risking disagreement with a period value derived a different way (from
rising-to-rising spacing) on a signal irregular enough for the two to
diverge.

**AC RMS needed no bias term at all, and that absence is the point, not an
omission.** It is `sqrt(E[x^2] - mean^2) * |gain|` — the variance formula,
scaled by gain's magnitude only. `bias` (which folds in DC offset and probe
offset) drops out completely: subtracting a signal's own mean removes
*any* constant shift by construction, DC offset included, which is exactly
what "AC RMS" is supposed to mean. Inversion's sign also does not matter
here, unlike everywhere else gain is used — flipping a signal upside down
does not change how spread out it is, so this is one of the only places in
`measureCore()` that intentionally discards a sign it computed.

**Math became a selectable source by generalizing `measure()`, not by
writing it a second time.** `measure(const Channel&)` is now a thin wrapper
that resolves gain/bias from a channel's probe/offset/inversion state
exactly as it always did, then calls `measureCore(samples, gain, bias)` —
the one place all of the actual analysis above lives.
`measureMath()` calls the same core with `gain=1, bias=0`, since Math's
samples are already final display-scale values with no probe or DC-offset
concept of their own. `drawReadouts()`'s Math block mirrors the channel
loop's own "skip the whole pass when nothing is checked" guard, and assumes
volts for the unit (Math has no `c.unit` string of its own — a deliberate
simplification, since the overwhelming majority of Math use combines two
voltage channels). The panel's channel selector keeps Math in a separate
`m_measureMathSegment` pointer rather than stretching
`m_measureChannelSegments` to five entries, exactly mirroring how Rulers
already keeps Math in its own separate `m_rulersMathCheck` alongside a
four-entry per-channel array — `RulersMathChannel` (the same
`= MaxChannels` sentinel Rulers and Annotations already use for "this means
Math, not a real channel index") is what lets `setChannelMeasurement()`/
`channelMeasurementEnabled()` route to `MathState::activeMeasurements`
instead of a `Channel`'s, and what the button group uses as that segment's
id.

**Unlike Rulers and Annotations, this persists** — one `quint32` bitmask per
source (`channelN/measurements` for real channels, `math/measurements` for
Math), bit `i` set means `Measurement(i)` is on, written in
`saveSettings()` and read back in `restoreSettings()` alongside everything
else. The bit order only has to agree with itself (`Measurement`'s own
declaration order) — nothing outside that pair of functions ever looks at
the mask directly. An existing settings file from before this existed
simply has no `measurements` key at all, so `settings.value(..., 0u)`
correctly falls back to "nothing checked" — the same default a fresh
install already had, so upgrading changes nothing anyone would notice. The
panel itself is not eagerly re-synced at restore time; like Rulers' own
checkboxes, it picks up the restored values the next time it is opened,
via the same `refreshMeasureChannelSelector()` that already runs from
`openPage(PageMeasure)`.

## Spectrum (Instruments -> Spectrum)

A second, independent plot widget — `SpectrumView` (`src/spectrumview.{h,cpp}`)
— stacked below `ScopeView` in a `QSplitter(Qt::Vertical)` built in
`buildUi()`, shown only while Instruments' Spectrum mode is selected
(`MainWindow::setSpectrumMode()`). It does not live inside `ScopeView` at
all: no shared `plotRect()`, no shared mouse handling, nothing. It is its
own small widget that MainWindow feeds samples and shows or hides, the same
relationship `NavigatorView` already has to `ScopeView`.

**FFTW3, not a hand-rolled FFT.** Linked via `pkg-config`'s `fftw3.pc`
(`pacman -S fftw` on Arch) — see the `find_package(PkgConfig)` /
`pkg_check_modules(FFTW3 ...)` block near the top of `CMakeLists.txt`. The
plan (`fftw_plan_dft_r2c_1d`) is created once, in the constructor, and
reused for the widget's whole lifetime, because `FftSize` is a fixed
constant (2048) rather than something the UI can change yet.

**Why FftSize is fixed, and what a size selector would actually need.** A
selector was left out of this pass's scope — the window function was what
was asked for, not FFT size — but the seam is already there if it is asked
for later: `rebuildWindow()` already regenerates `m_windowTable` from
scratch whenever the window type changes, so a size change would need the
exact same treatment (rebuild the table for the new size) *plus* destroying
and recreating `m_plan`/`m_fftIn`/`m_fftOut` for the new size, in whichever
setter would own that. Nothing about the current structure fights this;
it just is not wired up.

**Up to `SpectrumView::MaxTraces` (2) sources at once, real channels or
Math — the slot mapping lives entirely in MainWindow, not in
`SpectrumView`.** `m_spectrumSources[slot]` is `-1` (nothing), a channel
index 0-3, or `ScopeView::RulersMathChannel` (the same `= MaxChannels`
sentinel Rulers/Annotations/Measure all already use for "this means Math").
`SpectrumView` itself has no notion of "channel" at all — just two `Trace`
slots, each with its own `active`/`color`/`bins`, set via
`setTraceActive()`/`setTraceColor()`/`setTraceSamples()`. One FFTW plan and
one pair of scratch buffers serve both traces in turn, since each
`fftw_execute()` is a complete, self-contained transform — there was never
a need for per-trace FFTW state. `updateSpectrumSources()` is the single
place that reads which segments are currently checked and turns that into
the two slots (fixed order: real channels low-to-high, then Math, so which
slot a source lands in stays predictable regardless of click order) plus
each one's current colour — re-run any time a source could plausibly need
picking up a new colour or availability, rather than wiring a separate
colour-change signal for each of the five possible sources individually.

**The "at most 2" cap is enforced by hand, not by `QButtonGroup`.** The
five segment buttons (CH1-4 + Math) share one `QButtonGroup` with
`setExclusive(false)` — `QButtonGroup` only ever knows how to enforce
"at most one", never "at most two" — so `idToggled`'s handler counts how
many are currently checked and, if a click just made a third, reverts that
one click (`blockSignals` + `setChecked(false)`) rather than silently
evicting whichever was checked first. Silent eviction would have been less
code, but surprising: turning on a third source should not quietly turn
off a first one the person may have forgotten was even selected.

**Samples reach a real channel's slot without any new getter on
`ScopeView`; Math needed exactly one.** MainWindow already has each
channel's raw samples in hand at the exact moment it calls
`m_view->setSamples()` — Capture's `samplesReady` handler, Live's
`refreshLiveView()`, Record's `refreshRecordView()` — so `feedSpectrum()`
(called right alongside every one of those) just passes the same
`QVector<double>` on to whichever slot(s) want that channel. Math is
different: its samples are computed *inside* `ScopeView::recomputeMath()`
and never otherwise reach MainWindow, so it needed one new public getter,
`ScopeView::mathSamples()`, returning `m_math.samples` by const reference.
`feedSpectrum()` calls it whenever the channel that just updated could have
changed Math's own output — it does not check *which* of Math's two
sources `ch` actually is, just re-forwards Math's current samples on every
channel update while a Math slot is active, since `recomputeMath()` only
actually changes anything when `ch` is relevant and re-forwarding
unchanged data through the FFT again is wasted work, not wrong output.

**The dBV/dBm math — full derivation, since "documented reference" was
explicitly asked for.** `setTraceSamples()` windows the newest `FftSize`
samples for that trace, runs the real FFT, then converts each complex bin
to peak-equivalent volts:

```
volts[k] = |X[k]| * (1 / (FftSize * coherentGain)) * (2 if mirrored else 1)
```

- `1/FftSize` is the DFT's own forward-transform normalization (FFTW's
  transforms are unnormalized by convention; something has to divide by N).
- `coherentGain` is the window's own DC response — the mean of its
  coefficients, computed from what `rebuildWindow()` actually generated,
  not a remembered textbook constant, so it is exactly right regardless of
  `FftSize` or edge effects. Dividing by it undoes the window's own average
  attenuation. This is the entire mechanism behind "normalize by coherent
  gain so a 1 V sine reads 0 dBV" — without it, a Hann-windowed 1 V tone
  would read roughly 6 dB low (Hann's coherent gain is about 0.5), a
  Blackman one further still, and the reading would depend on which window
  happened to be selected, which defeats the point of an *amplitude*
  measurement.
- The `*2` reconstructs the energy a one-sided spectrum otherwise splits
  between +f and -f. DC (`k=0`) and, since `FftSize` is even, Nyquist
  (`k=FftSize/2`) are each their own unmirrored bin and never get it —
  `mirrored` in the code is `(k != 0) && !(FftSize % 2 == 0 && k == bins-1)`,
  written to still be correct if `FftSize` were ever made odd.

Converting to dB (`toDb()`) happens at *paint* time, not in
`setTraceSamples()` — each `Trace::bins` stores linear volts, so switching
between dBV and dBm is a `setUnit()` + repaint, never a re-run of the FFT:

- **dBV** here means `20*log10(V_peak / 1V)` — peak-referenced, which is
  what the volts above already are. This is a deliberate departure from the
  audio-engineering convention, where dBV is referenced to 1V *RMS*: peak
  reference is what makes a 1V-peak sine read exactly 0 dBV, the literal,
  testable target this feature was built against. A sine's RMS is its peak
  over `sqrt(2)`, so an RMS-referenced dBV would read this same tone about
  -3.01 dB.
- **dBm** assumes a 50 ohm reference load, the RF/spectrum-analyzer
  convention applied regardless of this instrument's real (much higher)
  input impedance — the same assumption every bench scope's spectrum mode
  makes. `P = Vrms^2/R`, `Vrms = Vpeak/sqrt(2)`, so
  `dBm = 20*log10(Vpeak) - 10*log10(2R) + 30`; with R=50,
  `-10*log10(100) + 30 = +10.0` exactly. The code computes this as
  `dBV + 10.0`, **not** the commonly-cited "dBm = dBV + 13.01" — that
  constant is only correct starting from an *RMS*-referenced dBV, and this
  one is peak-referenced, so the extra 3.01 dB of peak-to-RMS conversion is
  already folded into the `+10.0` instead. Mixing these two conventions up
  is the single easiest way to get this feature subtly wrong; if either
  formula is ever touched, re-derive from `P = V^2/R` rather than patching
  the constant by feel.

**The flat-top coefficients are the standard published ones, verified
before use, not recalled from memory and hoped correct.** MATLAB's
`flattopwin` (a0=0.21557895, a1=0.41663158, a2=0.277263158,
a3=0.083578947, a4=0.006947368) — the same values SRS and NI publish.
Picked specifically for the amplitude-accuracy use case named when this was
asked for: Flat Top's main lobe is unusually wide and flat, so a tone that
does not land exactly on a bin centre still reads close to its true
amplitude, at the cost of much worse frequency resolution than
Hann/Hamming/Blackman — a real, inherent trade-off, not a bug, and worth
saying so if anyone asks why Flat Top's peaks look so much blunter than the
other three.

**Three small, easy-to-get-wrong things this ran into, worth remembering
for any future DSP code in this file:**
- `QColor` is not `constexpr`-constructible in this Qt version —
  `scopeview.cpp`'s own file-scope colour constants are already plain
  `const`, not `constexpr`, for exactly this reason; `SpectrumView`'s match.
- `M_PI` is a POSIX/GNU extension to `<cmath>`, not standard C++17, and
  depends on GNU extensions being enabled (CMake's default via
  `CMAKE_CXX_EXTENSIONS`, but not guaranteed, and not something this project
  had relied on anywhere else). `SpectrumView` defines its own `kPi`
  instead of assuming it.
- The window-function enum is named `WindowFunction`, not `Window` — X11's
  `Xlib.h` defines a global `Window` typedef that Qt's X11 platform plugin
  can pull in transitively. A nested `SpectrumView::Window` would not
  actually have collided with it (different scopes), but there was no
  reason to invite the confusion on a name that common on a project that
  targets Linux.

**The dB floor is -100, not the original -140** — lowered when asked,
because -140 buried the noise floor in visually-empty graticule for no
benefit. **The graticule's division counts are `ScopeView::HorizontalDivs`/
`VerticalDivs` directly** (both 10), not a separate local copy that used to
happen to also be 10 wide but only 8 tall — read directly off `ScopeView`
so the two plots' grids can never drift apart again, which is what "make
the scales of FFT exactly as scales of time domain signals" was asking for.

**Dragging the splitter past 60% snaps the spectrum pane to fully
maximized**, via `QSplitter::splitterMoved` in `buildUi()` — checks the
current `sizes()`, and if the spectrum share is anywhere in `(0.60, 1.0)`,
calls `setSizes({0, total})` to finish the job itself rather than leaving
it wherever the drag stopped. Deliberately one-directional: dragging the
*other* way has no equivalent snap, since only the spectrum side's
maximize was asked for. Dragging back up out of the maximized state still
works normally — the handle stays at the very top of the splitter,
grabbable, even with the scope pane's own size at 0.

**The selected Instruments-mode icon is black, not its usual light-green
tint.** `uiIcon()`'s default colour (`Theme::accentSoft()`) reads fine
against Scope mode's transparent, unchecked background, but the *checked*
background is a bright, near-white-adjacent green (`QPushButton:checked` in
`theme.h`, which already darkens the button's own *text* the same way) —
the icon needs the same treatment, and a `QIcon`'s pixels are baked in at
creation time, not re-tinted by the button's own state the way text colour
is. Each mode button gets both variants up front (`Icons::icon(name,
Qt::black, 30)` alongside the normal one) and swaps between them on its own
`toggled(bool)` signal — connected *before* the initial
`button->setChecked(true)` for Scope, so that first check already fires the
swap rather than needing a separate line to set the initial icon by hand.
Null-safe: a null `blackIcon` (Font Awesome unavailable) just keeps
`normalIcon` regardless of checked state, same fallback behaviour every
other icon in this app already has.

**Why the frequency axis needed 20 ms/div and an 8 KB capture depth to
reach out to 10 kHz — this is inherent, not a bug.** Spectrum runs its FFT
on the exact same buffer Capture/Live/Record already produced for the
time-domain plot; it does not request its own, separately-rate-controlled
acquisition. The frequency axis always tops out at Nyquist
(`sample rate / 2`), and the sample rate is whatever the *time-domain*
settings (timebase, capture depth) happen to produce for that capture —
Spectrum has no say in it. Wanting to see further out in frequency means
wanting a higher sample rate, and on this hardware (like on most scopes
whose spectrum mode works this way, PicoScope included) that means
adjusting the same timebase/depth controls used for viewing the waveform
itself, trading off either total capture duration or how much of it
`FftSize` (a fixed 2048 samples) actually covers. There is a real,
unavoidable tension here between frequency *range* (needs higher sample
rate) and frequency *resolution* (`sample rate / FftSize` — needs a lower
sample rate, or a bigger `FftSize`, to get finer bins). Nothing in this
implementation currently helps with that trade-off beyond what has already
been described (a size selector would help resolution at a given rate, not
range) — worth this project's next round if it comes up again.

**The graticule now genuinely mirrors `ScopeView::drawGraticule()`, not
just an approximation that happened to share some constants.** The first
pass only matched division *counts*; it was missing the centre-axis
dashed crosshair and its 1/5-division tick marks, used the wrong colour
for the border (minor-line colour instead of `kGridMajor`), and put the dB
scale on the *left* — ScopeView shows a single channel's own voltage scale
on the *right* (`drawVoltScale()`'s "channel 1" case, which is what
actually renders whenever only one channel is on, the common case), in
that channel's own colour. All three are fixed: the crosshair/ticks are
copied line-for-line, the border uses `kGridMajor`, and the dB scale now
sits on the right, coloured like the sole active trace when exactly one is
active (mirroring "a channel's numbers are in that channel's colour")
or a neutral grey with zero or two active, since a shared scale has no
single trace to "belong" to the way each channel's own volts axis does.

**Vertical divisions drop from 10 to 8 once the pane is shorter than
roughly 220 px** (`kShortPlotThreshold` in `drawGraticule()`) — asked for
explicitly, for the same reason ScopeView itself would presumably do the
same at a small enough size: 10 evenly spaced dotted rows read as a smear
once each one is only a few pixels tall. The threshold is a plot-height
judgement call, not derived from anything; horizontal divisions never
change, since only "vertical" was asked for and the plot is always wide
enough in practice for 10 columns to stay legible.

**The 60% splitter snap could not actually be reached — `ScopeView`'s own
380 px minimum height, set in its constructor for filling the whole plot
column alone, was the ceiling.** In a typical window, the Spectrum pane
could only ever be dragged to roughly `(total - 380) / total`, which for
most reasonable window sizes lands well under 60% — the snap-past-threshold
logic in `buildUi()` was correct, but unreachable. Fixed with one line,
`m_view->setMinimumHeight(150)`, right where the splitter is built —
`setMinimumHeight()` touches only the height component, leaving
`ScopeView`'s 640 px width minimum untouched (irrelevant to a vertical
split regardless). Any future change to either widget's minimum size
should re-check this arithmetic: the snap threshold only *feels* like a
Spectrum-side constant, but it is really a statement about how much
smaller than its own minimum `ScopeView` can be squeezed.

**A trace's colour did not update until Spectrum mode was toggled off and
back on — because nothing ever re-ran `updateSpectrumSources()` when a
colour actually changed.** `updateSpectrumSources()` was always the
correct single place to re-read each active source's current colour (see
above), but the channel rail's own colour picker (`pickChannelColour()`)
and Math's colour picker (the `m_mathColour` click handler in
`buildMathPage()`) had no reason to know Spectrum existed, and nothing
called it from either. Toggling Spectrum mode off and back on happened to
work purely because `setSpectrumMode(true)` itself calls
`refreshSpectrumChannelSelector()`, which calls `updateSpectrumSources()`
as its last step — the fix is simply calling `updateSpectrumSources()`
directly from both colour pickers too, rather than requiring a detour
through the whole mode toggle to reach the one line that mattered.

**Two more label-collision fixes, both direct copies of how ScopeView
already solved the identical problem.** The unit text ("dBV"/"dBm"/"V")
was overlapping the topmost Y-axis tick because a 16 px top gutter left no
room to separate them — `kTopGutter` is now 28, matching ScopeView's own
exactly, with the unit box sized `kTopGutter - 14` tall (mirrors
`drawVoltScale()`'s own unit-box height) so there is always a clean gap.
Separately, "0 Hz" was overlapping whatever sits to the left of this
widget because every X-axis label, including the two at the very ends, was
centred on its tick — centring the `i=0` label means half its text extends
left of `plot.left()`. `drawAxes()`'s own time axis already special-cases
exactly this (its comment: "the end labels are aligned to the graticule
edge rather than centred on it, so they cannot reach into the gutters");
`drawGraticule()` here now does the same thing, left-aligning `i=0` and
right-aligning `i=divsX`.

**FFT as a Rulers source is a second, independent cursor system, not an
extension of ScopeView's existing one.** ScopeView's Rulers measure
time/volts on ScopeView's own plot; a frequency/level measurement on
`SpectrumView`'s completely different plot has no natural cursor
representation in that system at all — sharing the underlying
`RulerState`/drag machinery would have meant retrofitting units and a
widget it was never built for. Instead `SpectrumView` grew its own small,
self-contained pair: `m_freq1`/`m_freq2` (Hz) and `m_level1`/`m_level2`
(linear peak volts, matching how a trace's own bins are stored — converted
to dB/linear only at display time via the shared `toDb()`/
`valueToPlotFraction()`), a plain `RulerDrag` hit-test enum (unrelated to
ScopeView's own ruler-drag sentinels), and a fixed, non-draggable readout
box rather than ScopeView's minimizable one — two cursor pairs do not need
that much chrome. The only thing actually shared with ScopeView's Rulers
is the panel page it lives on: a new "FFT" checkbox next to Math's, calling
`SpectrumView::setRulersEnabled()` instead of anything on `ScopeView`.
Seeding (25%/75% of Nyquist for frequency, 75%/25% up the current Y range
for level) happens lazily, the first time `setTraceSamples()` sees a
positive sample rate while enabled — there is nothing sensible to seed a
frequency cursor to before any trace has reported one, and checking for it
in the otherwise-`const` `drawRulers()` would have needed `mutable` state
for no real benefit over doing it in the one function that already knows
when a rate first becomes available.

**Y-axis Display (Normal/Average) and Scale (Linear/Log) are both
per-trace-data, not per-render, so switching modes never needs to touch
the FFT itself.** `Trace` gained a second buffer, `avgBins`, an exponential
moving average (`avg = 0.25*new + 0.75*avg`, reinitialized to the raw
value outright on first data or a size change) maintained in
`setTraceSamples()` *alongside* `bins` regardless of which `DisplayMode` is
currently selected — so switching TO Average shows an already-warmed-up
trace rather than starting from zero, and switching back to Normal shows
current data immediately rather than a stale average. `drawTrace()` just
picks whichever buffer `m_displayMode` calls for at paint time. Scale
works the same way: `bins`/`avgBins` always stay in linear peak volts no
matter what, and `YScale::Linear` vs `::Log` is purely a paint-time choice
of how `valueToPlotFraction()` maps that value to the plot — a fixed
0-2 V span for Linear, the existing -100 to +20 dB span for Log. Nothing
about the FFT, the window, or the stored data changes when either combo
box changes; only `update()` is called. `plotFractionToValue()` is the
exact inverse of that mapping (including inverting `toDb()`'s own dBV/dBm
branch), used for dragging a level ruler cursor with the mouse — this is
also why it exists as a real function rather than being inlined only where
`valueToPlotFraction()` is: dragging needs pixel-to-value, drawing needs
value-to-pixel, and they must stay exact inverses of each other or a
level cursor would visibly drift from where the mouse actually is.

**Dragging Spectrum away to nothing now turns Spectrum mode off, mirroring
the maximize snap in the opposite direction.** The same `splitterMoved`
handler that snaps Spectrum to fully maximized past 60% now also checks
`sizes[1] <= 1` first — if the user has resized the pane out of existence,
that gesture itself is treated as "turn Spectrum mode off" (calling
`setSpectrumMode(false)` and checking the Scope segment), not left sitting
at a sliver of a pixel with the mode still nominally active underneath.

**The FFT Rulers checkbox has to be hidden at two separate moments, not
one — a real gotcha worth remembering for anything else gated on Spectrum
mode.** `setSpectrumMode()` hides it on every *later* toggle, but
`m_spectrumMode` already starts `false` at construction, so
`setSpectrumMode(false)` is never actually *called* for that initial
state — its own `if (m_spectrumMode == on) return;` guard sees no change
and returns immediately. The checkbox needed an explicit
`setVisible(false)` right where it is constructed in `buildRulersPage()`
as well, or it would default to visible from startup, before the user had
ever touched Spectrum mode at all. Whenever Spectrum mode goes off — either
through this or through the splitter-collapse case above — the checkbox is
also *unchecked*, not just hidden, which disables `SpectrumView`'s own
overlay via its own `toggled` handler: the same "unavailable means
actually off, not just visually hidden" rule
`refreshRulersChannelList()` already applies to a per-channel Ruler
checkbox when that channel goes off.

**Cursor flags, copied from ScopeView's own `drawFlag()` technique
exactly.** A "flag" is not a separate marker shape next to the cursor line;
it is a solid, rounded, colour-filled badge with the cursor's own value
centred on it in dark text (`QColor(0x10, 0x18, 0x0a)`, the same colour
ScopeView uses) — doubling as a much bigger, easier-to-grab drag target
than the 1 px dashed line itself. Frequency flags sit below the plot,
exactly where ScopeView's own time-cursor flags sit (`plot.bottom() + 3`);
level flags sit beside it on the right, since this widget's Y scale is
always on the right (unlike ScopeView, where a voltage flag's side depends
on which channel it belongs to). Positions and flag rectangles are computed
once by `computeRulerLayout()` and shared between `drawRulers()` and
`hitTestRulers()` — the same "shared layout computation" principle
ScopeView's own Rulers already use, so the two can never drift apart the
way two independent implementations of the same geometry eventually would.
`hitTestRulers()` checks flags *before* falling back to the thin line,
matching ScopeView's own stated reasoning for why flags exist at all: "a
flag is a real target in its own right, not just the thin line." The
old top-left readout box still exists but now shows only the two deltas
(`\u0394F`, `\u0394L`) — each cursor's own value already lives on its flag,
so showing it a second time in the box would just be the same number
twice.

**The reported drag lag had a specific, findable cause: `QPolygonF::mid()`
copying roughly 1025 points on every single repaint.** `drawTrace()`
built one `QPolygonF` including two baseline-anchor points (to close the
filled area), then called `poly.mid(1, n)` to get an anchor-free copy for
the stroked outline. `mid()` allocates a new vector and copies every
element into it — for FftSize/2+1 = 1025 points, that is a real, avoidable
cost, and it ran on *every* `paintEvent()`, including every mouse-move
event fired while dragging a ruler cursor, which is exactly the moment
`update()` gets called most frequently. `QPainter::drawPolyline()` has a
raw-pointer overload (`const QPointF *points, int pointCount`) that reads
directly from `poly`'s own buffer via `constData() + 1`, skipping the
first anchor with zero extra allocation. This is the one substantive fix
here; nothing else in the paint path was doing anything comparably
wasteful on a per-frame basis. Any future change to `drawTrace()` that
reaches for `QPolygonF::mid()`, `first()`/`last()` slicing, or similar
should default to a raw-pointer overload instead wherever the underlying
buffer already has the data in the right place — copying data that is
already sitting in memory in a usable order, once per repaint, is the
general shape of bug this was.

**Level flags dropped their unit suffix — the actual cause of them running
off the edge of the widget.** `plotRect()`'s right gutter is only 54 px,
and a level flag starts 4 px past `plot.right()`, leaving roughly 50 px for
the whole badge. Text like "-12.3 dBV" comfortably exceeded that; the bare
number, "-12.3", does not. The unit was not lost, just no longer repeated
per flag — it already appears once, at the top of the Y scale
(`drawGraticule()`'s own unit label), which is the only place it actually
needs to. Frequency flags were never at risk the same way: they sit below
the plot, which is far wider than 50 px, so they kept their own
`formatFrequency()` output (which already includes a unit suffix,
e.g. "1.234 kHz") unchanged.

**Turning Spectrum mode back on after collapsing its pane to zero left it
at zero — `setSpectrumMode(true)` made the widget visible again without
ever telling the splitter to actually give it any height.** A `QSplitter`
does not restore a hidden child's previous size when it reappears; it just
resumes honouring whatever `sizes()` currently says, which was still
`{total, 0}` from the collapse. This needed `m_plotSplitter` to become a
real member (`QSplitter *m_plotSplitter`, promoted from a `buildUi()`-local
variable) rather than something only the splitter's own `splitterMoved`
lambda could reach, so `setSpectrumMode()` could read and correct it too.
The fix applies whenever Spectrum is turned on with the pane currently at
(or effectively at) zero height — deliberately not unconditionally on
every activation, since a person who intentionally left it at some other
split earlier in the session should not have that overridden every time
they toggle modes. This same "effectively zero" check turns out to also
cover the very first-ever activation for free: a hidden widget in a
`QSplitter` never receives a real layout-computed size to begin with, so
`sizes()` already reports the same `{total, 0}` shape the first time
Spectrum mode is turned on as it does after an explicit collapse — one
check handles both cases without needing to tell them apart.

## XY (Instruments -> XY)

A third, independent plot widget — `XYView` (`src/xyview.{h,cpp}`) — for
classic oscilloscope XY mode: one channel drives X, another drives Y,
consecutive samples connected in order into a Lissajous or hysteresis-style
trace. Like `SpectrumView`, it knows nothing about where its samples come
from or that anything else in this app exists.

**Built as a mode switch on the existing Spectrum infrastructure, not a
parallel system next to it.** Spectrum and XY are mutually exclusive
(Instruments' own mode selector is exclusive), so rather than duplicating
the splitter/pane/40%-restore machinery a second time, this pass first
refactored the single `bool m_spectrumMode` into a proper
`enum class InstrumentMode { Scope, Spectrum, XY }`. Two independent bools
(`m_spectrumMode`, a hypothetical `m_xyMode`) could have drifted into an
invalid "both true" state that needed defending against by hand; an enum
simply cannot represent that state at all. `setSpectrumMode(bool)` became
`setInstrumentMode(InstrumentMode)`, one function handling all three modes
identically wherever their handling is actually identical (showing/hiding
the second pane, restoring 40% height, hiding the FFT Rulers checkbox
outside Spectrum) and branching only where the content genuinely differs
(which controls to show, which channel-selector refresh to run).

**Spectrum and XY share one physical pane, via a `QStackedWidget`
(`m_secondPaneStack`), so the `QSplitter` itself never has to learn about
a third child.** All of the splitter's own logic — the 60% maximize snap,
collapsing to nothing turning Instruments back to Scope, restoring 40% on
reactivation — was written and tested against exactly two children
(`sizes().size() == 2`). Adding `m_xy` as a literal third `QSplitter`
child would have broken every one of those checks and introduced a second,
mostly-meaningless drag handle (since one of Spectrum/XY is always
hidden). Instead `m_secondPaneStack` holds both `m_spectrum` and `m_xy`;
`setInstrumentMode()` calls `setCurrentWidget()` on the stack to choose
which is showing and toggles the stack's own visibility for "is any second
pane showing at all" — the splitter only ever sees `{m_view,
m_secondPaneStack}`, so none of its existing logic needed to change to
support a second kind of content.

**Pairing X and Y needed a small sample cache MainWindow did not
previously keep.** Every other consumer of channel samples in this file
(`ScopeView`, `NavigatorView`, `SpectrumView`, Measure) only ever needs
*one* channel's data at the moment it arrives. XY inherently needs *two*,
X and Y, simultaneously — but they arrive via the same three per-channel
callbacks (Capture's `samplesReady`, Live's `refreshLiveView()`, Record's
`refreshRecordView()`) at slightly different moments in general.
`feedXY()` caches every channel's latest samples unconditionally, in
`m_lastChannelSamples[4]`, regardless of whether XY mode is even active —
so that whichever of X or Y updates *second* still finds the other's
current data waiting, and so that turning XY mode on shows a pairing
immediately rather than waiting for the next tick after either axis
happens to update again. The cost is a handful of extra `QVector<double>`
copies sitting in memory, cheap relative to a capture buffer itself.

**Each axis is fixed to its own assigned channel's own configured
full-scale range, not an auto-fit to the data — a correction from this
feature's first pass, not the original design.** The very first version
auto-scaled: each axis's own peak magnitude across the current pairing,
plus 10% headroom, used as a symmetric half-range. This was flagged as
wrong by direct comparison against a real capture — two channels each
configured for +-2 V (each card showing exactly that) produced an XY plot
whose axes read +-2.30 V and +-2.22 V, drifting with the signal rather
than matching either channel's own fixed range the way the main plot's
own voltage axis, and Spectrum's own dB axis, both already do. `XYView`
no longer computes a range at all: `setRanges(xRange, yRange)` takes it
from outside, and `setSamples()` only ever stores the sample arrays now.
`ScopeView` gained one new getter for this, `channelFullScaleVolts()`,
returning the *exact* same `voltsPerDiv / scale * (VerticalDivs / 2.0)`
formula `drawReadouts()` already uses for a channel's own card ("+-X V") —
deliberately the identical formula, not just a similar one, so the two can
never quietly drift apart from each other. `MainWindow::updateXYRanges()`
reads this for both assigned channels and pushes it to `XYView`; it is
called from `refreshXYChannelSelectors()`'s own tail (alongside
`updateXYColors()`), which was *already* wired into every place that could
plausibly need it — mode activation, either axis's channel assignment
changing, and (via `applyChannel()`) a volts/div change made anywhere
else, the channel card's own steppers or Auto range included — so no new
call sites were needed at all, only the one function itself. The
graticule otherwise mirrors `ScopeView`'s/`SpectrumView`'s exactly
(division counts, centre crosshair and ticks, border/grid colours, the
same short-pane 8-division drop), just with Y numbers on the left and X
numbers on the bottom — the conventional math/engineering XY layout,
rather than `ScopeView`'s "a channel's scale sits on whichever side that
channel's own trace uses" convention, which has no natural meaning here:
this is not one channel's own scale, it is a new coordinate system built
from two, now genuinely tied to both of them rather than to the data.

**The trace takes the Y channel's own colour, by convention** (X is
usually the reference/independent axis, Y the "measured" one) —
`updateXYColors()` re-reads `m_colour[m_xyChannelY]` rather than caching
it, so a colour changed elsewhere (the channel rail's own picker) is
picked up the next time it runs, the same principle
`updateSpectrumSources()` already follows for Spectrum's own traces, and
for the same reason: without it, a colour change made while XY mode was
showing that channel would not visibly update until some unrelated
refresh happened to run.

**The Y axis was redesigned a second time — from "a symmetric range that
happens to be correct" to "the exact same drawing ScopeView's own voltage
scale produces" — because matching the *numbers* turned out not to be the
same thing as matching the *design*.** The previous pass fixed what the
range itself was (a channel's own `channelFullScaleVolts()`, not an
auto-fit); this pass fixed how that range is drawn, since a conventional
left-side math-plot axis with ad hoc mV/V rounding still looked like a
different kind of thing next to the time-domain plot's own scale, even
once the numbers themselves were correct.

`ScopeView::axisNumber()` (the tick-rounding function every one of
ScopeView's own axes already uses — decimal count derived from the tick
step, clamped so a wide number never overflows a narrow gutter, `"-0"`
cleaned up) moved from an anonymous-namespace free function to a public
static method for exactly this: so `XYView` calls the *identical*
function, not a similar one it maintains separately, whichever precision
and rounding behaviour it exposes. `decimalsFor()`, the small helper
`axisNumber()` itself calls, stayed a private file-scope function in
`scopeview.cpp` — nothing outside that file needs it directly. All three
of `ScopeView`'s own internal call sites (its voltage scale, Math's own
voltage scale, and its time axis) needed no changes at all: an unqualified
call to `axisNumber(...)` from inside one of `ScopeView`'s own member
functions resolves to `ScopeView::axisNumber` through ordinary C++ member
lookup, exactly as it did as a free function, once it became a member.

`XYView`'s Y axis now reproduces `drawVoltScale()`'s design line for line:
the widest tick decides millivolts-vs-volts (simplified to always assume a
plain voltage unit, since XY has no per-channel probe unit the way a real
channel does), the unit label sits in its own box ending 14 px before the
first tick — the same gap ScopeView's own unit box leaves — and every tick
number goes through `axisNumber()`. It moved from the left side to the
right for the same reason the range fix moved this widget from
"self-contained" to "reads from ScopeView": ScopeView shows a single
channel's own voltage scale on the right (`drawVoltScale()`'s "channel 1"
case, the common case whenever only one channel is on), so that is where
this needed to sit too, in that channel's own colour, to actually look
like the same design rather than a different one that happens to agree on
the numbers. `kTopGutter` grew from 10 to 28 (matching ScopeView's own
constant) purely to leave room for that unit label — a design detail with
no equivalent in the previous simpler layout at all. The X axis was
deliberately left as it was: only the vertical scale was asked to match,
and introducing `axisNumber()` there too without being asked would have
been a second, unrequested change to the one axis nobody had raised an
issue with.

## Theme / global stylesheet — gotchas

`theme.h`'s `Theme::globalStyleSheet()`, applied once via `qApp->setStyleSheet()`
in `main.cpp` after forcing the Fusion style, replaces Fusion's default
chrome for `QComboBox`, `QCheckBox`, `QSpinBox`/`QDoubleSpinBox`,
`QPushButton`, `QLineEdit`, `QScrollBar`, `QToolTip`, `QStatusBar`, `QMenu`.

**Gotcha 1 — QSS silently overrides QPalette.** Once any app-level
stylesheet rule targets a widget *type* at all — even a rule that never
touches the specific property you're trying to set via `setPalette()` — Qt's
styled paint path can ignore the palette for that property entirely. This
bit the Rulers checkboxes: `theme.h`'s `QCheckBox { spacing: 8px; }` rule
(nothing about colour) was enough to make `setPalette(WindowText, colour)`
silently do nothing. **Fix: use a per-widget `setStyleSheet()`
(`"QCheckBox { color: %1; }"`) for any colour that needs to be set
dynamically on a widget type the global stylesheet touches at all** — a
widget's own stylesheet always wins over the app-level one, regardless of
selector specificity.

**Gotcha 2 — Fusion doesn't implement the CSS zero-size-transparent-border
triangle trick.** An attempt to draw custom combo/spinbox arrows via
`border-left/right: Npx solid transparent; border-top: Npx solid <color>`
(the standard web-CSS way to draw a triangle) rendered as a small solid
square instead — Qt's QSS engine draws the literal border box, it does not
implement the browser's mitred-triangle rendering. **Fix: real embedded PNG
images** (`resources/icons/arrow-up.png`, `arrow-down.png`, generated with
PIL, referenced via `image: url(:/icons/arrow-up.png)`), *not* SVG —
`Qt6::Svg` is not linked in this build and would either fail to load or fail
to build.

**Gotcha 3 — styling any part of a compound widget suppresses the native
style's fallback for the rest of it.** Styling just `QSpinBox::up-button`'s
background/border (without also providing an `::up-arrow` image) made the
arrow disappear entirely rather than fall back to Fusion's own — Qt does not
mix "some subcontrols styled, others native" gracefully. Once you style any
piece, provide all of it.

## Timebase / roll-mode ranges

`kTimebases[]` (Capture/Record, up to 100 ms/div) and a separate
`kRollTimebases[]` (Live only: 100 ms, 200 ms, 500 ms, 1 s, 2 s, 5 s/div) —
added because Live is this app's roll-mode equivalent (the hardware's own
roll mode is unsupported, per README), and roll mode is specifically for
slow timebases a triggered block capture isn't suited to.
`MainWindow::refreshTimebaseOptions()` swaps the combo's contents on mode
change, picking the closest available match from the new list rather than
an arbitrary index. `stepTimebase()`'s bounds check was fixed to clamp
against `m_timebase->count()` (the combo's actual current size), not a fixed
constant sized for the full range — that constant would have let the
stepper silently do nothing near the end of Live's shorter list.

**The History combo is a separate ring-buffer-capacity setting, in seconds,
completely independent of time/div.** It was fine at the old, single 100 ms/
div ceiling (a 1-second window always fit comfortably inside the 5-second
default), but Live's new wider range exposed it directly: selecting, say,
2 s/div (a 20-second window) with History still at its 5-second default
silently truncated the display to whatever the buffer actually held — which
looked identical across every timebase larger than History's current value,
since they were all hitting the same real cap. Fixed in `applyTimebase()`:
History is now auto-bumped *up* (never down, so a deliberately larger History
for manual panning is left alone) to the smallest available option that
covers the newly selected window.

**The "compressing then rolling" transient is correct behaviour, not a
bug** — confirmed directly with the person after they initially suspected
the History fix hadn't worked. Every restart (mode switch, or changing
time/div while running) resets the ring buffer to empty. `windowLength()`
clamps the displayed window to however many samples have actually
accumulated so far, so right after a restart the display shows a genuinely
narrow window that visibly grows toward its target over `historySeconds` of
real elapsed time — which looks like the trace "compressing" as more time
gets crammed into the same screen width. This was explicitly left as-is; a
fixed-width-with-trailing-gap alternative was proposed and declined.

## Live-mode crash — root cause, mitigation, and residual risk

This was tracked down via an actual gdb backtrace (all threads) after a
report of the app freezing then aborting a few seconds into starting Live
mode. Do not re-diagnose this from scratch if it recurs — go straight to a
fresh backtrace (see Debugging protocol above) and compare against this.

**Root cause**: `AcquisitionManager::stop()` — called whenever the mode
switches away from Capture — does nothing SDK-side at all; it only stops our
own polling `QTimer`. So every SDK-internal worker thread spawned for block
capture (`BasicUsbWave::DevThread`, `SendListMng2021::wait`, libusb's own
event threads — all confirmed still alive in the backtrace) is left exactly
as active as during Capture mode. `StreamEngine::start()`'s call to
`SetOscCaptureMode(m_deviceId, 1)` then triggers, *inside the closed-source
SDK*, a full device object teardown and rebuild (`DestoryJlyDevice()` ->
destructor chain -> a blocking `nanosleep`) — while those leftover threads
may still be touching that same object. The backtrace showed the exact
signature of this: `pure virtual method called` -> `std::terminate()` ->
`SIGABRT`, on a *different* thread than the one doing the teardown.

**Separately, and confirmed by the same backtrace**: that `SetOscCaptureMode`
call runs directly on the GUI thread (traced all the way from
`CardButton::mousePressEvent` -> `startAcquisition()` -> `StreamEngine::start()`),
which is why the whole app froze ("not responding") for the duration of the
call, independent of whether the race above actually fires.

**Mitigation applied** (`MainWindow::setAcquisitionMode()`): specifically on
a Capture -> Stream transition, the Run button is disabled for 400 ms via a
non-blocking `QTimer::singleShot` (not a blocking sleep — the UI stays
responsive) before being re-enabled, giving the SDK's leftover threads a
moment to settle before that specific transition fires. **This stopped the
crash for the person and should be treated as a timing-based mitigation for
a suspected race inside a closed-source SDK, not a structurally verified
fix** — there is no way to confirm from outside the SDK that the race window
is actually closed versus just made unlikely. If this crash is ever reported
again, get a fresh backtrace immediately; do not assume it is the same
issue or that the mitigation has failed in the same way.

**Not fixed, and architecturally still true**: any SDK call that blocks
still freezes the GUI thread for its duration, mitigation or not. Moving
`StreamEngine`/`AcquisitionManager`'s SDK calls off the main thread would fix
this class of issue generally, but was judged too risky to attempt in a
session with no ability to compile or test — it would need careful
thread-safety auditing of classes that currently assume they run on the GUI
thread throughout.

## Capture-mode rate selection: kScreensPerCapture — a live, unretested risk

`MainWindow::applyTimebase()`'s Capture-mode rate selection used to size
every triggered capture to span **8 screens** (`kScreensPerCapture = 8.0`).
This was **not** an arbitrary choice — a comment directly above the function
(easy to miss; it was missed once, this session) explains it was specifically
added to fix broken triggering: at a fast time/div with a signal slower than
the visible window, a capture sized to exactly one screen can end up
containing **no trigger edge at all**, so the hardware capture never
completes.

Reduced to `1.02` this session (after the person measured a large,
reproducible rate/update-speed gap against the vendor's own app at 50 ms/div)
to match the vendor app's behaviour, which clearly does not reserve 8 screens
of margin. This fix is confirmed working for the reported scenario. **It has
not been retested against the original problem it reopens**: a fast
time/div with a signal whose period is longer than one screen's worth of
time (the comment's own example: 20 µs/div with a 1 kHz signal, where a
window that only spans 262 µs may contain no edge of a 500 µs-period
signal). If a future report describes Capture simply refusing to trigger at
a fast timebase, this is the first thing to check, and the fix is *not* to
blindly raise `kScreensPerCapture` back toward 8 (that reintroduces the
original slow-update problem) — it likely needs a value that depends on the
relationship between the signal and the timebase, which neither 8 nor 1.02
actually captures correctly for all cases.

## Two C++ pitfalls specific to this codebase — both shipped a real crash

**1. A nested type must be defined before any method's *parameter list*
references it, even within the same class body.** Unlike a method's body
(deferred to a "complete class" context), a method's declaration — its
return type and parameter types — is parsed immediately, in textual order.
`ScopeView::yToValue(const Channel &c, ...)` was declared in a block
inserted *before* `struct Channel { ... }`'s own definition later in the
class body, and failed to compile with a confusing cascade of errors
(`Channel` reinterpreted as `int`). The .cpp *definition* of a method can be
anywhere in the file — this rule is purely about the *declaration*'s
position inside the class body. Caught by the build failing outright, not by
a review.

**2. Source line order is not execution order across function calls.**
`MainWindow::buildUi()` calls `buildDetailPanel()` (which calls
`buildRulersPage()`) at an earlier line number than the line that constructs
`m_view = new ScopeView`, even though `buildRulersPage()`'s own code sits far
*later* in the .cpp file, after that construction line. A direct (not
lambda-deferred) reference to `m_view` inside `buildRulersPage()` ran before
`m_view` existed, segfaulting on a null pointer. Three other `m_view`
references in the same function were safe because they were inside deferred
`connect()` lambdas, which only run later, once the user actually interacts
— that is the general lesson: a reference inside a lambda passed to
`connect()` is safe regardless of construction order; a reference evaluated
immediately, during a `build*()`-style function's own execution, is only
safe if you have actually traced the real call sequence inside the
constructor, not just noted where a member happens to be assigned by line
number. Caught by a gdb backtrace, not by review — reviewed the surrounding
code extensively beforehand and did not find it.

## Windows port (portable folder via MSYS2)

The code needed almost nothing: no POSIX-only calls anywhere, and the SDK
header is Windows-ready (`extern "C" __declspec(dllimport)`, pulls in
`Windows.h`). Changes:

- **CMake:** under MinGW the SDK's `vmmultmso.dll` is linked directly
  (no MSVC `.lib` needed); candidates filtered to the build's
  architecture (X64) and Release. `WIN32_EXECUTABLE ON` (no console).
  `_USE_MATH_DEFINES` on Windows.
- **main.cpp:** if `lib/python3*` exists beside the exe, sets
  `PYTHONHOME` to the exe's folder (and `PYTHONDONTWRITEBYTECODE`) before
  libsigrokdecode starts its interpreter, so the bundled Python is used.
- **packaging/windows/build-windows.sh** (run in MSYS2 UCRT64): pacman
  installs toolchain, CMake/Ninja, Qt 6 (+ tools for windeployqt), FFTW,
  glib, Python; builds libsigrokdecode from git into `~/vsx-deps` once
  (MSYS2 has no package; if it fails, the app builds without decoding);
  fresh CMake build (fresh because `pkg_check_modules` caches a miss);
  bundles into `dist/VimuScopeX`: exe, windeployqt output, the SDK's
  X64/Release DLLs, sigrok's decoders + ours in `decoders/`, the Python
  stdlib trimmed into `lib/python3.X`, then every DLL `ldd` reports from
  `/ucrt64` or `~/vsx-deps` (repeated to a fixed point, `.pyd` included),
  plus Microsoft's C++ runtime DLLs app-locally if the SDK DLLs use them;
  reports any DLL that couldn't be found; zips it.

libsigrokdecode's development branch keeps the annotation layout
(`ann_class`, `char **ann_text`) and the calls already checked against
sigrok-cli, so `decoderengine.cpp` should build against it unchanged.
Untested on real Windows at the time of writing — the libsigrokdecode
source build (newest MSYS2 Python) is the likeliest step to need a fix.
USB: the scope needs its Windows driver (the official Vimu software
installs it).

**First Windows run:** libsigrokdecode (0.6.0-git) built fine, CMake found
Qt, FFTW, libsigrokdecode and the X64/Release SDK DLL — then Ninja looped
"Re-running CMake" until "manifest 'build.ninja' still dirty after 100
tries". Cause: the source zip carries this environment's timestamps,
ahead of the laptop's clock, so CMakeLists.txt always looked newer than
build.ninja (Linux extraction uses `unzip -DD`, which drops them). The
script now `touch`es the source tree and SDK before configuring.

**Second Windows run** built and packaged, but the DLL scan missed
`libsigrokdecode-4.dll`: `~/vsx-deps/bin` wasn't on PATH, so ldd couldn't
resolve it, fell back to listing imports it could only find on PATH, and
so also "missed" `vmmultmso.dll` (present in the folder) and the
`api-ms-win-crt-*` API-set names (not real files). The script now copies
libsigrokdecode explicitly, puts `~/vsx-deps/bin` on PATH and runs ldd
from inside the folder, and ignores `api-ms-win-*`/`ext-ms-win-*`.

## First real CAN capture (Windows): inverted polarity → automatic CAN polarity

The Windows build ran on the car (CH1 on CAN-H, x10, 125 MS/s, 500 kbit/s),
but screenshots showed the decoder reading the bus inverted: the idle line
(~2.05 V, recessive) was decoded as "ID 0 (0x0)", DLC, CRC-15 and
"DB n: 0x00" blocks; a 0x00 payload (5 dominant + stuff bit every 12 us)
came out as 0xff; real frames were garbled into red EOF errors and false
CAN FD (DB 47, CRC-21). That's recessive being fed as 0 — Invert
unchecked or CAN-L chosen for a CAN-H wire. The user's "remove blocks
while the bus is idle" request is the same symptom: with the right
polarity sigrok emits nothing while idle.

Fix: `DecoderEngine` now detects CAN polarity from the thresholded stream —
recessive is the level with the longest unbroken run (>= 11 recessive bits
end every frame; stuffing caps dominant runs at 5, 12 with overlapping
error flags) — and flips the stream if 0 has the longest run. Tested on
synthetic streams both ways (busy 0x00-payload bus where a majority vote
would fail, quiet bus, flat line). The Invert box is hidden for CAN; it
remains for LIN/K-Line, where a LIN break or K-Line fast-init pulse can be
the longest run and would defeat the same rule.

Red boxes = sigrok warning classes (frame-format / stuffing problems). As
far as known, sigrok's CAN decoder doesn't verify the CRC value itself —
a candidate improvement.

## Branding: app icon and launch splash

User-supplied `logo.png` (1250², transparent outside the rounded square)
and `banner.png` (2835×1417).

- **Icon:** cropped to the logo's own bounds and exported to
  `resources/app/app-icon-{16..512}.png` (in the qrc under `/images`);
  `main.cpp` builds a multi-size `QIcon` and calls `app.setWindowIcon()`
  (title bar, taskbar, Alt-Tab). Windows `.exe` icon:
  `resources/windows/VimuScopeX.ico` (16–256) via `VimuScopeX.rc`, added
  with `enable_language(RC)` under `if(WIN32)`.
- **Splash:** `resources/app/splash.png`, the banner at 1440×720, kept
  lossless: a 256-colour quantisation cut it from 1 MB to 360 KB but
  visibly washed out the cyan/blue waves. `main.cpp` shows a
  `QSplashScreen` immediately after the QApplication exists (before fonts,
  theme and MainWindow), sized to 55% of the screen width up to 900 px and
  rendered at the screen's pixel ratio; the MainWindow is built behind it
  and shown 3 s after the splash appeared (at once if construction took
  longer), via `QTimer::singleShot`, which also finishes and deletes the
  splash. Requested by the user as a deliberate 3 s delay.

**Follow-up (GNOME/Wayland on Arch).** Splash appeared ~2.5 s late for
~0.5 s: on Wayland a window can't draw until the compositor answers its
first request, so one `processEvents()` left it unmapped while MainWindow
construction (SDK start-up, ~2.5 s) blocked. `main.cpp` now pumps events
until `splash->windowHandle()->isExposed()` (1 s cap), repaints, and only
then starts the 3 s clock and builds the window.

Icon still generic under GNOME: Wayland shells ignore `setWindowIcon()` and
take the icon from the installed .desktop file matching the app ID. Now
`QGuiApplication::setDesktopFileName("vimuscopex")`, and on Linux
`installDesktopIntegration()` writes `~/.local/share/applications/
vimuscopex.desktop` (Exec = this binary, Icon=vimuscopex,
StartupWMClass=VimuScopeX) and `hicolor/NxN/apps/vimuscopex.png` (16–512)
— only when the entry is missing or points at a different binary. GNOME
may need one launch to pick a new entry up.

## CAN verified on a car; CRC check, consistent labels, theme fixes

A second car capture (CH1 CAN-H, CH2 CAN-L, 250 MS/s) decoded cleanly:
ID 0x12E, DLC 8, `c8 7f fb 7f a0 ff ff 00`, CRC-15 0x67b6, ACK aligned
with the taller ACK spike, no errors, nothing over the idle bus.

- **CRC check** (`checkCanCrc()` in decoderengine.cpp): sigrok shows but
  doesn't verify the CRC. The callback now keeps sigrok's per-bit
  ('bit') and 'stuff-bit' annotations, SOF starts and classic CRC values
  (texts with "CRC" but not CRC-17/21, value parsed after "0x"); after
  decoding, each classic CRC is recomputed (CRC-15, poly 0x4599) over the
  destuffed bits from SOF to the CRC field and a red "CRC error: 0x..,
  expected 0x.." is added on mismatch. Validated against the capture
  above (computes 0x67b6; a leading SOF 0 bit doesn't change it). Guard:
  frames whose destuffed bit count isn't a valid classic length
  (count % 8 in {2,3,6,7}) are skipped, not flagged. CAN FD CRCs are not
  checked (they include stuff bits and a stuff count).
- **Consistent labels:** annotations carry their class (`group`);
  `drawBusRows()` picks one text length per class — the one the narrowest
  visible box of that class fits — instead of the longest that fits each
  box, so a row no longer mixes "DB 0: .." and "Data byte 1: ..".
- **Theme:** annotation boxes kept the dark theme because their text edit
  holds its own palette; new `AnnotationBox::refreshTheme()` rebuilds it
  from the current app palette (accent untouched, it's per box).
  `applyTheme()` now refreshes every `CardButton` and `AnnotationBox` via
  `findChildren` — the Math card was missing from the old fixed list and
  only restyled on hover. The channel pages' scroll areas get the Decoding
  page's treatment (transparent, bar at the panel edge, PageChannel exempt
  from the per-page right margin).

**Annotation background still dark after a theme switch (follow-up).**
`refreshTheme()` did run (the text turned dark) but the background kept
the old Base colour: with the app-wide style sheet active, Qt's
style-sheet engine stores a widget's custom palette and restores it when
it re-polishes the widget, which `qApp->setStyleSheet()` in `applyTheme()`
triggers just after the refresh. The text edit's colours now come from its
own style sheet (`background` + `color`), which every polish re-applies.
Lesson: under the global style sheet, don't theme widgets with
`setPalette()` — use a widget style sheet or paint it yourself.

## Windows text size, darker plot, loading files with no scope attached

- **Text too small on Windows** (Dell Latitude 5490): the base font was
  85% of the platform default, and Windows' default (Segoe UI 9 pt) is far
  below GNOME's (~11 pt), so cards/top bar/side panel came out small while
  the sidebar buttons (fixed 11 px) looked right. The base is now floored
  at 9 pt, which puts card titles (base - 1.5 pt, x1.1) at 11 px — the
  sidebar size. Unchanged wherever 85% of the default is already >= 9 pt.
- **Darker plot background:** `Theme::plotBackground()` dark value
  #0e1217 -> #06080b. One token for the scope plot, overview strip,
  Spectrum, XY and bus rows. Light mode unchanged.
- **Opening a .vsxwave with no scope connected** loaded the samples but
  showed nothing: with no device ever reported, `m_channelCount` stays 0
  (cards hidden, `drawTrace` loops over 0 channels, decoding says "CHx is
  off"). `openNativeWaveform()` now, when `!m_acq->ready()`, sets the
  count from the file (highest channel on or with data), shows those cards
  and passes it to view and navigator. A connected scope's count wins and
  replaces it in `onDeviceChanged()`. Safe: every AcquisitionManager
  setter is already a no-op without a device.

**Same for .vsxrec.** The count logic moved into
`adoptChannelCountWithoutDevice(int)`, shared by both loaders.
`loadRecordTab()` calls it with the highest channel in the recording's
mask (`StreamEngine::recordChannelMask()`) when `!m_acq->ready()`, and also
turns on (Manual, emitting `idClicked`) any recorded channel whose button
is Off — `refreshRecordView()` skips Off channels, and CH2 is often off,
which hid half a two-channel recording. With a scope connected, nothing
changes. Opening the recording itself never needed a device
(`openRecordForReading()` reads meta.ini and the files).

## Recording files kept their full reserved size; reopened size read "0 MB"

- **File size:** `RecordWriter::open()` pre-sizes the single data file to
  the whole record size (one fixed region per recorded channel), and
  nothing shrank it, so a 0.4 MB recording occupied 128 MiB (134.2 MB) or
  64 MiB (67.1 MB). New `compactToWritten()`, called from
  `finishWriting()` once the writer thread has stopped, moves each
  channel's written samples down so regions sit back to back at the
  largest written count, `resize()`s the file, and sets
  `m_samplesPerChannel` (what `channelByteOffset()` reads) to the new
  region size; `finishWriting()` then writes that into
  `record/samplesPerChannel`. Forward chunked copy with every destination
  at or below its source; on any I/O failure it stops and leaves a valid
  (still large) file. Verified by a randomised simulation of the same
  copy (1–3 channels, partial fills, chunks smaller/larger than the
  shift). Recordings made before this keep their old size.
- **Size shown after reopening:** `openRecordForReading()` set
  `m_recordTarget` to the samples written, so the status bar's
  "x / y MB" showed the written amount, rounded to 0 or 1 MB. The chosen
  size is now stored as `record/requestedSamplesPerChannel` at `open()`,
  read back by `openForReading()` (falling back to `samplesPerChannel`,
  which for older recordings is still the original request), and used as
  the target. `recordComplete()` only runs during an active recording, so
  the target has no other effect while browsing.

## Record mode: tabs after auto-completion, per-recording zoom history, measurements

- **Tabs stayed greyed out** when a recording ran to its full size: the
  `recordComplete()` branch in `pullStream()` stopped and redrew but,
  unlike `stopAcquisition()`, never re-enabled the tab bar or refreshed the
  tabs. It does now.
- **Zoom arrows brought back a closed recording:** Record mode had one
  zoom history for all tabs. Now `m_recordZoomHistory` (QMap keyed by
  recording directory) parks the current tab's `m_zoomBack/
  m_zoomForward[ModeRecord]` in `loadRecordTab()` and restores the incoming
  tab's (empty if never zoomed), then `updateZoomHistoryButtons()`.
  Closing a tab removes its entry (if it was shown, the switch to its
  neighbour via currentChanged has already parked it); the no-tabs-left
  branch clears the Record history and hides the arrows.
- **Frequency wrong in Record** (~300 Hz zoomed out for 1 kHz, rising with
  zoom, topping out at 2 kHz): measurements ran on ScopeView's buffer,
  which in Record is the decimated page (~4000 points of min/max pairs),
  so "frequency" tracked how often the page alternated. `Channel` now has
  `measureSamples`/`measureRate`; `measure()` prefers them;
  `measureCore()` takes the sample rate explicitly. `refreshRecordView()`
  reads up to 1M raw samples per channel from the middle of the view
  (`StreamEngine::read()`) and passes them with the true record rate via
  `setMeasurementSource()`. Cleared by `clearChannel()` and when leaving
  Record mode. Math measurements in Record still use the page.

## Low-pass filter per channel and for Math

- **Filter** (`ScopeView::lowPassFilter()`, static): 2nd-order Butterworth
  biquad (RBJ coefficients, Q = 1/sqrt2) run forward then backward — zero
  phase, so edges, trigger and cursors keep their timing. Designed at
  cutoff / (sqrt2-1)^(1/4) so the *requested* frequency is the -3 dB point
  of the squared response. Each pass starts settled on its first sample
  (no end ringing). Returns input unchanged when the design frequency is
  >= 0.45 fs. Verified numerically (1 MS/s, 10 kHz): -3.01 dB at cutoff,
  -17.7 dB at 2x, -48.6 dB at 5x, step midpoint unmoved, DC kept.
- **ScopeView:** `Channel` keeps `rawSamples` (as delivered) and `samples`
  (drawn/measured; filtered when `lowPass`). `setChannelLowPass()`,
  `setMathLowPass()` re-filter from raw; Math filters its own result in
  `recomputeMath()`. `channelSamples()` returns raw (what .vsxwave saves).
  `setLowPassInView(false)` in Record mode, whose samples are a decimated
  page.
- **Record:** `refreshRecordView()` reads the raw span (<= 4M samples per
  channel), filters at the true rate and min/max-decimates into the page
  (same point count, so the windowing rate holds); longer views show the
  page unfiltered with a status note. The raw measurement source is
  filtered too, at any zoom. Math in Record is not filtered.
- **UI:** "Low-pass filter" row (On + `FrequencySpinBox`) on each channel
  page and the Math page. `FrequencySpinBox` (mainwindow.cpp, no
  Q_OBJECT) steps 1-2-5 (tested standalone), shows Hz/kHz/MHz, parses
  "250k", "1.5 MHz", "800". Cards show " LPF". Saved per channel
  (`lowPass`, `lowPassHz`) and `math/lowPass`, `math/lowPassHz`; Default
  turns them off. Filter settings are not stored in .vsxwave files.

## Engine tools (Rulers > Engine): 720-degree cycle, cylinder overlay, relative compression

Agreed design: cycle from CH2 sync when present, else two markers dragged by
hand (the main path for now — no sync probe yet; a one-pulse-per-cycle cam
sensor is the suggested safe sync source later); compression from a current
clamp (peaks) or battery voltage (dips), selectable; results as a bar chart
over the plot.

- **enginetools.h** — header-only, Qt-free, tested standalone on simulated
  cranking (4-cyl 1-3-4-2, cylinder 4 at 75 %, +-15 % speed wobble, noise,
  drift): `parseFiringOrder`, `manualCycles` (marker span repeated across the
  buffer), `syncEdges` (threshold + hysteresis + holdoff),
  `bandsFromCycles` (one band per cylinder centred on its TDC, cylinder 1
  TDC = 0 deg, `offsetDeg` = angle at each cycle start), `findPeaks`
  (O(n) sliding-window max with prominence), `relativeCompression`. The
  compression assignment counts strokes from the peak nearest cylinder 1's
  TDC; a gap of ~k x the median gap advances the count by k (missed
  strokes), so later peaks keep their cylinders — the first version shifted
  everything after a dropped peak (caught by the test). Cycles with any
  missing stroke or abnormal gap are left out; heights are peak minus the
  preceding trough. Results: current 70.1 %, battery voltage 70.4 %, one
  peak missing 67.5 % (others 95–100 %, 5 of 6 cycles used).
- **ScopeView** — `setEngineOverlay/Cycles/CylinderBands/CompressionResult`,
  markers stored as *times* (Capture: trigger-relative; Record: in the
  recording) so they stay on the waveform when Record's page is rebuilt;
  grabbed by flags at the top of the plot, checked before rulers.
  `drawEngineBands()` (alternating shading, red for left-out cycles) under
  the traces; `drawEngineOverlay()` over them: 90-degree ticks with labels
  where they fit, TDC lines with cylinder-number pills, the markers, and the
  bar chart (top right; red below strongest - flag %). The ruler box gains
  a theta row: each cursor's angle and cylinder, and the unwrapped difference.
- **MainWindow** — Engine section on the Rulers page (now in a scroll area,
  exempt from the per-page margin): overlay switch, cylinders + firing
  order (red outline if invalid), 4/2-stroke, cycle source, Reset markers,
  sync threshold/edge/offset, compression switch, channel, signal type, flag
  %. `updateEngineAnalysis()` on a 100 ms timer, keyed on data generation,
  settings revision, mode and marker positions. Compression smooths with the
  low-pass at 150 Hz first, runs in Capture/Record (Live: note), and in
  compression mode the bands and degree cycles come from the detected peaks.
  Setup values saved; the switches start off; Default turns them off.

**Follow-up: stroke timing chart (HScope-style) and draggable readouts.**
- `ScopeView::drawTimingChart()` (style "Stroke timing chart", the new
  default; "Cylinder bands" is the old look): one row per cylinder in
  firing order between the top label rows and the bottom reserved area;
  cylinder at firing position r has compression TDC at r x cycle/n and
  power, exhaust, intake, compression 180 deg each from there (2-stroke:
  power + compression); strokes crossing the cycle end wrap to its start.
  Semi-transparent, under the traces; row number pills at the left and a
  centred legend at the bottom (in `drawEngineOverlay()`). Geometry checked
  by replaying the arithmetic: correct 1-3-4-2 staircase, one of each
  stroke per column.
- Readouts: built by `readoutLines()`, placed by `readoutBlockRect()`;
  the block drags freely and snaps on release to the corner nearest its
  centre (`m_readoutCorner`, saved as `view/readoutCorner`). Top corners
  sit below the engine label rows (`topReservedHeight()`), bottom corners
  above bus rows + chart legend (`bottomReservedHeight()`); right corners
  right-align the lines. Press order: engine marker flags, readouts, rulers.
  The compression chart moves to the top-left when the readouts are
  top-right.

## Crank and cam sensor analysis (cycle source "Crank + cam sensors")

Agreed: TDC from the service-manual angle *and* calibration from a
compression capture; cam check against a spec angle *and* a remembered
known-good value; results in an info panel on the plot.

- **enginetools.h** (tested standalone on simulated 60-2 Hall and
  inductive signals, idle 800 rpm and cranking 250 rpm +-20 %, a one-pulse
  cam at 100 deg): `analyseCrank()` — mid-level crossings with hysteresis
  (both sensor types), gap = interval >= 1.7x both neighbours, pattern from
  the most common (teeth, missing) across revolutions or forced; only gaps
  one revolution apart are kept (a missing tooth's own gap first split a
  revolution, giving a false 762 rpm — caught by the test) and early gaps
  become faults; per-revolution rpm; faults = wrong tooth count or a tooth
  interval jumping vs its neighbour, merged within 3 teeth (one broken tooth
  first read as 4 faults). `crankAngleAt()` interpolates between real tooth
  edges (worst error 0.04 deg while cranking). `analyseCam()` — reference
  edge (all edges for one pulse per cycle; for multi-tooth cams the edge
  after the longest gap), circular-mean crank angle (99.99 vs 100), cycle
  starts = the crank ref of each cam revolution. `tdcFromCompression()` —
  peaks' crank angles folded into one firing interval, circular mean
  (86.00 vs 86): TDC is known only modulo the firing interval.
- **ScopeView** — `setCrankInfo(lines, warn, faults)`: "Crank and cam"
  panel above the compression chart (same corner logic), red fault lines
  with "!" pills at the bottom.
- **MainWindow** — source 2 shows crank/cam channels (cam may be "No cam":
  every other revolution, phase a guess), wheel (auto or 60-2, 36-1, 36-2,
  30-2, 24-1, 12-1), cylinder 1 TDC after the gap (cycle offset = -TDC) +
  Calibrate (picks the calibration candidate nearest the current value, so
  the person's choice of cylinder 1 stands), cam spec (-1 = off) +-
  tolerance, Remember as good / Forget (saved immediately as
  engine/camKnownGood with the app's named QSettings). With a compression
  test and crank mode, bands stay crank-derived; peaks feed calibration.
  "Too few samples per tooth" (median < 6) asks to zoom in — matters in
  Record, whose page is decimated.

## Automotive page

The Engine section (cycle overlay, timing chart, compression, crank and cam)
moved from Rulers to its own page, `PageAutomotive` (enum before
PageSettings; `buildAutomotivePage()` added to the stack in the same place),
with a sidebar button ("Automotive", Font Awesome `car` — `car` and
`car-battery` added to fa_codepoints.h). Scrolls like Decoding (exempt from
the per-page right margin). Charging and battery tests will go here too.

## Guided battery and charging tests (Automotive page)

Agreed: guided (one click sets up, waits, analyses); ripple via CH1 DC +
CH2 AC on the same terminal in one capture; 12 V and 24 V selectable.

- **batterytools.h** (Qt-free, tested): `analyseCranking()` — 1 ms and 50 ms
  moving averages; crank = first fall > 6 % below resting; resting = median
  of the second before; minimum within 3 s (1 ms average); started = 50 ms
  average above resting + 2.5 % for 0.3 s; running = last second's average.
  Simulated: resting 12.60, min 8.54 (8.5), crank 1.21 s (1.2), running
  14.20; weak battery correctly "didn't start". `analyseRipple()` — robust
  Vpp (0.5..99.5 %), humps counted between real dips (local minima >= 8 % Vpp
  deep, window rate/12000), hump height and interval coefficients of
  variation. Counting mean crossings instead (first version) read an open
  diode as one big even hump — caught on a simulated 3-phase bridge.
  Uneven = height spread > 0.30 or timing spread > 0.15 (healthy +-10 %
  phases + noise: 0.17 / 0.07; open diode: 0.62 / 0.34).
- **Cranking test:** Capture tops out at 100 ms/div, so it records instead:
  Record mode, CH1 DC +-20 V (24 V: centred 15 V), CH2 off, 100 kS/s,
  stops after 10 s (`m_guidedStopTimer`) or when stopped; finished from a
  hook at the end of `stopAcquisition()`, reading the raw recording.
- **Charging test:** Capture, CH1 DC as above, CH2 AC +-1 V (24 V: +-2 V),
  5 ms/div, trigger Auto, single shot like the Single button; finished from
  a hook at the end of the samplesReady handler. CH2 low-passed at 20 kHz.
- `prepareTestChannel()` applies channel settings through the widgets and
  refuses (asking for the x10 probe) when page/offset exceed
  `rangeMaxMv()` through the probe — the hardware would otherwise clamp and
  clip. Voltages are computed from raw samples x probe gradient + offset:
  display values subtract the window centre (15 V low in the 24 V setup).
- Results: `ScopeView::setTestPanel()` — titled panel, top right (left if the
  readouts are there), below any engine panels (`m_enginePanelsBottom`).
- Limits (resting charged from, cranking at least, charging from/to, ripple
  at most): 12 V defaults x(system/12); saved under battery/*.
- `m_guidedWaiting` is cleared before any stop in the starters: the stop
  hook queues a finish that would otherwise end the new test immediately.

**Follow-up fixes.** (1) Automotive page slid under its scroll bar: rows
like a limit label + spin box, or two combos side by side, wanted more
width than the panel. After building the page, every QLabel word-wraps and
every spin box / combo / button / line edit gets minimumWidth 40, so the
layout can narrow them instead of overflowing; the compression flag box
lost its long prefix. (2) The guided tests switched mode with
`setAcquisitionMode()` alone, leaving the Scope page's mode selector on
the old mode; they now check `m_acqMode`'s button first, as opening a file
does. (Switching itself is intended: cranking records — Capture tops out at
1 s — and charging captures.) (3) Crank + cam source with no crank signal
now says to connect the sensor or set Cycle from to Markers.

**Follow-up.** `topReservedHeight()` reserved two label rows whenever the
engine overlay was on, leaving an empty strip (readouts and panels pushed
down) when no cycle was found; it now reserves the first row only with
manual markers or a degree scale, the second only with cylinder bands, and
the engine panels/fault lines use it instead of a fixed two rows. The
Cranking/Charging test, Remember/Forget and TDC/Calibrate pairs are now one
widget per row — side by side their labels clipped in the narrow panel.

## CAN bus health (Automotive page)

Agreed: probe either wire or both (each channel recognised automatically);
frame table as a panel on the plot; analyses whatever is on screen.

- **cantools.h** (Qt-free, tested on simulated CAN-H/CAN-L at 15 % and 70 %
  load, a differential probe and a flat wire): `analyseWire()` — levels are
  medians of samples in the outer quarter of the swing each side; recessive
  = the state with the longest run (works at 70 % load, where a majority vote
  wouldn't); kind +1 CAN-H, -1 CAN-L, +2 differential (recessive ~0 V).
  `frameTable()` (per ID: count, errors, mean period, last good data) and
  `busLoad()` (sum of SOF..EOF / span) — exact on synthetic frames.
- **Decoder:** `assembleCanFrames()` builds `DecodeResult::canFrames` from the
  CAN annotations (sof opens, eof closes, id / full-id and data from their
  "0x.." text, any warning or our CRC error marks it); result also carries
  sampleCount, sampleRate, strayErrors. MainWindow keeps the latest CAN
  slot's result.
- **ScopeView:** the guided-test panel became stacked info panels
  (`setInfoPanel(slot, ...)`; 0 tests, 1 CAN health), under any engine
  panels; tab-separated lines align as columns; a panel taller than the plot
  is cut with "... n more".
- **MainWindow::updateCanHealth()** (engine timer, keyed on data generation,
  a CAN revision bumped by each CAN decode, mode): per-channel levels from
  raw x probe (absolute volts), checked against ISO 11898-2 (recessive
  2.0-3.0; dominant H 2.75-4.5, L 0.5-2.25; difference 1.5-3.0, recessive
  difference -0.5..+0.15), with hints (difference > 3 V: termination
  missing; < 1.5 V: extra load/short; recessive off 2.5 V: short to ground /
  battery); H minus L when both are present; frames, seconds, bus load
  (warn > 80 %), errors, then the ID table.

## Polish (group 4 of the remaining list; Freezing deliberately left)

- **Capture pan across mode switches:** `CaptureSnapshot` now keeps
  `centreFraction` and `followingTrigger`, saved on leaving Capture; on
  return, a panned view goes back via `setViewCentreFraction()` (a view that
  was following the trigger keeps following).
- **Section labels after a theme switch:** `sectionLabel()` tags labels
  `objectName "sectionLabel"`; `applyTheme()` recolours every one (their
  colour lives in their own style sheet, which the theme switch didn't touch).
- **Record overview resolution:** 500 -> 1500 buckets (`kOverviewPoints`
  3000), about one per pixel of the strip; still cached per recording size.
  For recordings above RecordWriter's 8M-sample sequential cap the overview
  samples the first 4096 samples of each bucket (one seek per bucket) — so
  it can still miss a brief event mid-bucket on very long recordings.
- **Zoom history on a loaded Live file:** `ZoomState` gains
  `liveCentreFraction`; `applyZoomState()` for a loaded Live file sets the
  timebase on the view and restores the centre (refreshLiveView() ignores
  loaded files, so only the timebase used to come back).

## AC coupling showed the DC (12 V battery) — read-back + software AC

`AcquisitionManager::setAcCoupling()` silently skipped channels whose
`IsSupportAcDc()` is 0, so the UI said AC while the hardware stayed DC. It
now returns whether the hardware is coupled as asked (`GetAcDc` read-back).
When AC is wanted but not confirmed, `applyChannel()` turns on **software
AC**: `ScopeView::setChannelRemoveDc()` takes the mean off what's drawn and
measured (raw samples / saved files keep the DC), the view's window centre is
0, and the hardware window is centred on `m_softAcCentre[ch]` (absolute
volts) — a small range centred on 0 V would only show clipping. The card
reads "AC (sw)" and a status message explains.

`trackSoftwareAc()` (after each Capture frame and Live refresh): a clipped
signal sets `m_softAcSeeking`, which opens the window to the probe's whole
range for one frame; that frame's mean is the true DC, and the narrow window
centres on it. Simulated: settles in 2 frames at 14, 13, 12.6, 27.5, -3,
0.4 V. (Stepping the narrow window toward a clipped signal — the first
version — bounced forever when the DC sat on a window edge, e.g. 13 V on a
+-1 V page.) Inside the window it follows drift beyond 20 % of a page.
Beyond the probe's reach: clamps and asks for the x10 probe.
Charging test: with CH2 on software AC, a first capture reads the DC on CH1
and centres CH2's window there (`m_chargePass`), then a second measures.

**Follow-up: hardware AC now actually reaches the scope (all modes).** The
vendor's app AC-couples this scope fine, so the hardware can. Two causes:
(1) `setAcCoupling()` only called `SetAcDc` when `IsSupportAcDc()` said yes —
it evidently says no here; now `SetAcDc` is always sent and `GetAcDc` read
back (software AC remains only as the fallback when the read-back
disagrees). (2) Entering stream mode (`SetOscCaptureMode(1)`, Live/Record)
and returning to block mode reset the channels — the existing code already
re-sends ranges for exactly this reason — so coupling is now cached in
`StreamEngine` (`setChannelCoupling()`, `m_acCoupling`) and re-sent by
`reapplyCoupling()` after start and after both returns to block mode;
`applyChannel()` feeds it every time. `AcquisitionManager::applyDefaults()`
(every connection) re-applies the last asked coupling instead of forcing DC.

**Follow-up: Live/Record AC.** After the previous fix Capture AC-coupled
correctly, but Live/Record still showed the trace pinned at the top of the
range (a 12 V battery, DC-coupled). Stream mode has its own SDK call —
`SetStreamAcDc` / `GetStreamAcDc`, like `SetStreamChannelRangemV` for
ranges — and the block-mode `SetAcDc` doesn't reach it. `StreamEngine` now
sends `SetStreamAcDc` (`reapplyCoupling()` after entering stream mode,
`setChannelCoupling()` on every change, read back with `GetStreamAcDc`) and
re-sends the block-mode `SetAcDc` after returning to block mode
(`reapplyBlockCoupling()`). `applyChannel()` sets both paths and uses the
read-back of the mode on screen to decide on the software fallback.

**Follow-up: Live/Record "blocky" after the stream coupling fix.** Same
settings (+-100 mV, AC) gave fine resolution in Capture but ~19.5 mV steps in
Live/Record — 5 V / 256, i.e. the device's default +-2.5 V range. Cause:
`StreamEngine::start()` sent the ranges, then `reapplyCoupling()`, and a
coupling switch resets the range. Now coupling first, then ranges;
`setChannelCoupling()` re-sends the channel's range after `SetStreamAcDc`;
and `setAcquisitionMode()` ends by re-applying every channel (coupling then
range, as `applyChannel()` orders them), so whichever mode is entered gets
its own settings in the right order — including Capture after leaving
streaming, where `reapplyBlockCoupling()` could otherwise leave the block
range at default.

**Follow-up: still blocky (now centred ~50 mV) — the range itself.** The
coupling order wasn't it: streaming simply isn't taking +-100 mV (earlier,
+-500 mV *was* honoured: Live clipped at exactly 500 mV). Likely the stream
path refuses small ranges and the device falls back to its default
(19.5 mV steps = 5 V / 256). `SetStreamChannelRangemV`'s return value (1 ok,
0 failed) was never checked. Now `applyStreamRange()` checks it and doubles a
refused range about its centre until accepted (the data callback delivers
volts, so a wider range only coarsens detail), recording what was used;
`reportStreamRanges()` (after StreamCapture starts, and on range changes
while running) reads `GetStreamVoltageResolution` per active channel and
emits `rangeReport` when the range was widened or the step is > 1.8x what the
used range implies, with the SDK's `GetStreamRangeMin/MaxmV`. MainWindow
shows it in the status bar. Next step depends on that message: whether the
scope refuses (widened), accepts but ignores (step mismatch), or its limits.

**Follow-up: the "blocky" Live was software AC.** Screenshot: card "AC (sw)",
status "the scope didn't switch to AC coupling" — the stream read-back
(`GetStreamAcDc`, taken in `applyChannel()`, usually while *not*
streaming) said DC, software AC engaged, and it centred a +-100 mV window on
the battery's DC; streaming can't place a small window up there, so the
range was widened (~19.5 mV steps), and the mean was forced to 0.00 V.
Software AC is now Capture-only (where `GetAcDc` is reliable and it works);
streaming relies on the hardware command (`SetStreamAcDc`, re-sent at every
start). `reportStreamRanges()` now reports every active channel at every
stream start — range used/asked, step, streaming limits, and the coupling
read back *while streaming* — in the status bar for 30 s.

## Opened recordings vanished from Record after a mode switch

The Record tab bar is built only from `recordDirectory()`
(`refreshRecordTabs()`), and Open's own comment called a recording opened from
elsewhere getting no tab "harmless". It wasn't: re-entering Record rebuilds
the tabs, the current one is then some other recording (or there are none),
and `setAcquisitionMode()` loads that or clears the view. Now
`m_openedRecordings` (saved as records/opened, loaded first thing in the
constructor) lists recordings opened from outside the record directory;
Open adds them, `refreshRecordTabs()` appends them via the new public
`RecordWriter::recordingInfo()`, and closing such a tab forgets it. Paths
compared via `QFileInfo::absoluteFilePath()`.

Live: traced the whole mode switch with an opened Live .vsxwave (loader fills
m_liveSnapshot; `setAcquisitionMode()` restores from it; `refreshLiveView()`
bails on m_liveLoaded; nothing after the restore clears it) and found no
cause — asked the user for exact steps.

## Record low-pass at every zoom (filtered overview)

Zoomed out past 4M samples per channel, Record drew the trace unfiltered
(filtering directly would read the whole span on every pan). Now
`ensureFilteredOverview(ch, cutoff)` builds, once per recording / channel /
cutoff / recorded length, a low-passed copy of the whole recording: block
averages (factor = max(rate / (8 x cutoff), ceil(total / 8M))) read ~4M raw
samples at a time, then `ScopeView::lowPassFilter` at the reduced rate,
stored as float in `m_filteredOverview[ch]` (raw units, like the page).
`refreshRecordView()` draws views wider than 4M from it (min/max per page
bucket). Verified offline against full-rate filtering (1 MS/s, 100 Hz,
factor 1250): ~1 % of the signal (1.5 % worst). Not built while recording
(the file grows): the trace stays unfiltered with a note until it stops.
Measurements unchanged (filtered raw, 1M samples from the view's middle).

## Cranking test: start detection fixed, cranking rpm, whole-curve battery verdict

**Bug:** engines that started were reported "didn't start": started meant the
50 ms average > resting x 1.025 for 0.3 s, but smart-charging cars hold the
battery near 12.6-13 V after a start. `analyseCranking()` now finds the end
of cranking (50 ms average back within 3 % of resting, staying within 5 %
for 0.5 s — the starter letting go) and calls it started if the supply is
then > 3x noisier than before cranking (alternator ripple, ignition) or sits
> 1.2 % above resting. Also measured: plateau (median held after the first
150 ms), sag (first third minus last third), compression pulse rate (10 ms
vs 0.5 s averages, crossings with hysteresis; rpm = Hz x 120 / cylinders,
cylinders from the Automotive engine setup), crankedToEnd, noise before /
after.

**Verdict** (`judgeBattery()`, batterytools.h): SOC from resting (BCI OCV
table, 24 V = doubled), surface-charge warning above 12.9 V/12 V, plateau vs
the user's 21 C limit shifted by the BCI temperature table (9.6 V @ 21 C ..
8.5 V @ -18 C; new Battery temperature setting, saved), engagement dip
(< 7 V/12 V flagged), sag (> 0.3 V/12 V), cranking speed (< 150 rpm slow),
start, running voltage (below resting = warning; below the charging limit
but above resting = smart charging, informational). Headline priority:
discharged (charge and retest) > weak > losing capacity > slow cranking at
good voltage (starter/cables/earth) > deep engagement dip (terminals/
cables) > healthy. Tested offline on healthy smart-charging, sagging,
discharged, no-start, crank-to-end, slow-starter and cold-morning
scenarios: started/not exact, crank time within 0.02 s, rpm exact.

## Internal resistance (current clamp) and Live's time axis

**Internal resistance:** optional "Current clamp on CH2" (Battery section;
CH2's probe must be in amps — checked at start; CH2 kept on, DC, range left
as set up) and "Rated CCA". `BatteryTools::internalResistance()` fits
V = a - R*I over the cranking plateau (1 ms averages, one point per ms):
each compression pulse swings the current, so the slope is measured
hundreds of times; falls back to (Vrest - Vcrank)/(Icrank - Irest) when the
swing is under 20 A or r^2 < 0.6; a reversed clamp is handled. Simulated
(V = Voc - I*R + noise, 800 A inrush, 180 +-60 A cranking): 6.00 mOhm (6.0),
18.24 (18.0), fallback 6.00, reversed 6.00. `estimatedCca()` = (Voc - 7.2 V)
/ R (the tester approximation). Verdict: R > 10 mOhm/12 V marginal, > 15
worn; estimated CCA < 80 % of rated losing capacity, < 60 % weak; headlines
"Worn battery" / "Ageing battery" join the priority list. `Cranking` now
carries startIndex / endIndex.

**Live time axis:** `ScopeView::setNowAtRightEdge()` (on in Live): with no
data yet the axis now runs -10..0 divisions (it fell back to -5..5,
centred) and the time-zero line + its ticks are never drawn, as in Record.

## Remote: the plot on a phone, with remote control

Sidebar "Remote" (Font Awesome `mobile-screen-button`), `PageRemote`.
- **RemoteServer** (remoteserver.h/.cpp): HTTP + WebSocket (RFC 6455) on one
  port, written on QTcpServer (QtNetwork, in qt6-base; CMake now finds and
  links Qt6::Network) instead of the optional QtWebSockets. GET /?k=KEY
  serves the page (resource :/remote/index.html); /ws?k=KEY upgrades
  (accept = base64(SHA-1(key + GUID)), checked against the RFC's example);
  wrong key -> 403. Text frames are JSON commands; ping/close handled;
  oversized frames drop the client; a backed-up phone (> 512 KB queued)
  skips frames. Random 12-char key; "New key" disconnects everyone. The
  frame loop re-checks its connection after emitting a command (a new key
  drops it).
- **qrcode.h**: own QR encoder (byte mode, level M, versions 1-10, best of
  8 masks by penalty). Verified with OpenCV over 26 strings across all ten
  versions — every one decodes exactly (one needed OpenCV's second detector
  or another scale: a localisation quirk, not an encoding error). A
  URL with key is version 3.
- **ScopeView::remoteFrame(columns)**: per visible channel min/max per
  column in divisions x100 via `valueToY()` (exactly the plot's mapping),
  null where there's no data; leftTime/perDiv; readouts; panels (crank,
  compression, info panels). MainWindow adds mode and run state and
  broadcasts at the chosen rate (5-25/s, default 15) to connected phones.
- **Commands** emit the same card signals a tap would: runstop
  (`m_runCard->clicked`), single, timebase / trigger / CH scale step up or
  down.
- **Page**: self-contained HTML/JS (no libraries, works offline on a
  hotspot), DPR-sharp canvas: grid, min/max traces, time labels, readouts,
  panels, status chips, big buttons, fullscreen, auto-reconnect.
- Off at every launch; port and rate saved. Addresses listed private-first
  and refreshed when the page opens.

**Remote follow-up.** Desktop: the QR label had a 200 px minimum shorter
than the code, so it got squeezed and the text below spilled over it, and
it left a blank block while sharing was off — now `setFixedSize()` to the
image, hidden with the URL when off; the page scrolls (exempt from the
per-page margin). Frame additions: per channel `centre` (value on the
centre line: windowCentre - positionVolts) for voltage scales; `math`
(min/max per column via `mathRange()`, perDiv = ppd / scale, centre = mid);
`bus` rows (label, colour, message, annotations in view as plot-width
fractions, capped at 400); `seq`. Phone page rewritten: rounded standalone
buttons, full-screen button in the status bar, CH1 scale left / CH2 (or
Math) right, Math trace, bus rows along the bottom with fitting texts, and
vertical trace dragging — nearest trace within 40 px, moves sent every
40 ms as {cmd: position, ch, volts, seq}; MainWindow emits
`m_view->offsetDragged()` (exactly a mouse drag) and echoes the last seq,
and the phone previews only moves not yet echoed (no snap-back).

**Remote follow-up 2.** Phone: no text selection (user-select / touch
callout off); CH1/CH2 labels are on/off toggles (`{cmd: channel, ch}` ->
the channel's mode buttons + idClicked; frames carry `allChannels` with
on/colour); gutters like the desktop — CH1 left, CH2 right, Math to the
side of an off channel else an extra left column (three gutters with
everything on), each with its scale and a zero marker; pressing anywhere
in a gutter drags its trace (even off-screen), the plot picks the nearest
trace, Math drags too (`{cmd: mathPosition}` -> `mathOffsetDragged`, same as
a mouse drag); previews keyed by trace id. Full screen replaced by a
"plot only" focus mode (browser full screen shows the browser's own exit
notice for seconds, which a page can't shorten), floating button to exit.
Desktop: `CardButton::setStepperGlyphs()` shows -/+ on non-interactive
cards (Logic, DIO: inert until implemented); the Math card's -/+ now step
`m_mathVoltsPage` (Auto first).

**Reverted at the user's request:** the phone's plot-only focus mode; the status-bar button is browser full screen again (Fullscreen API toggle).

## Channel filters: low-pass, high-pass, band-pass

Per channel "Filter": Off / Low-pass / High-pass / Band-pass with FrequencySpinBox
counters (lower: high-pass cutoff, band "From"; upper: low-pass cutoff, band
"To"; rows shown per type). `ScopeView::setChannelFilter(ch, type, lowHz,
highHz)` replaces setChannelLowPass; `applyFilter()` dispatches. All share
`zeroPhaseLowPass()` (2nd-order Butterworth, forward + backward). High-pass =
input - `highPassBaseline()` (the matched low-pass, design f = cutoff /
(sqrt2+1)^(1/4)): 1 - 1/(1+(f/f0)^4) is exactly the forward-backward
Butterworth high-pass. Band-pass = high-pass then low-pass. Verified: high-pass
-3.01 dB at the cutoff, -17.7 dB an octave below; band-pass -3.01 dB at both
edges, -0.07 dB mid-band; DC removed; edges stay on their sample. Card notes
LPF / HPF / BPF. Settings: filter, filterLowHz, filterHighHz (old lowPass /
lowPassHz migrate to low-pass). Math keeps its own low-pass.

Record: zoomed-in filtering and measurements via applyFilter; the filtered
overview is keyed by type + both frequencies. Low/band-pass: reduced series
filtered (reduction keeps >= 8x the low-pass / upper edge). High-pass can't
use a reduced copy (it keeps the fast content), so a second read subtracts
the slow part — found at the reduced rate, interpolated to every raw sample —
and stores the exact output's min/max per point (data = pairs, `points`
count). Checked vs full-rate: 0.3 mV worst column error on 0.6 V p-p
(subtracting one level per column instead was 180 mV off: the slow part
drifts within a column).

## Larger record sizes; band-stop filter

**Record sizes** 3, 5, 7, 10 GB added (labels in GB from 1 GB; progress text
switches to GB). Audited for > 4 GB / > 2^31 samples per channel: sizes,
file offsets (qint64), seeks and compaction are 64-bit; the only int(span)
reads are guarded (decode limit, < 4M-sample filtering); the filtered
overview's factor stays small. The stale "held in memory" tooltip fixed
(recordings are disk-backed). `RecordWriter::open()` now checks the
pre-sizing `resize()`: on failure (FAT32's 4 GB file limit, full or
read-only drive) it removes the file and fails with a message (via
StreamEngine::failed) instead of failing silently mid-recording.

**Band-stop** (`FilterBandStop`, card " BSF", From/To counters):
`ScopeView::bandStopFilter()` — 2nd-order notch from the analog prototype
via bilinear with pre-warped edges, forward + backward, Q = sqrt(sqrt2+1) /
(r - 1/r) (r = sqrt of the warped edge ratio) so the squared response is
-3 dB at the edges; null at sqrt(low x high). Verified: 1-10 kHz -3.01 dB at
both edges, -0.03 dB a decade out; 45-55 Hz -44 dB at 50 Hz (null 49.75),
100/150 Hz untouched; DC passes; edges unmoved. (x minus band-pass was
rejected: edges at ~-10 dB.) Record zoomed-out: band-stop keeps fast content,
so it takes the exact two-pass route with `ScopeView::removedPart()` (input
minus output; high-pass uses its baseline).

## Math: 16 operations, unit-aware, on measured values; Math filters; cursor units

**Bug fixed:** Math combined the channels' *raw* (pre-probe) samples — wrong
as soon as a probe wasn't x1 (the CSV export's own comment admitted it). Now
plot, navigator and CSV share `mathops.h` (Qt-free, tested) on measured
values: (raw x probeGain + probeOffset) x (inverted ? -1 : 1). setProbe /
setInverted now recompute Math (view and navigator).

**Operations** (`MathOps::Op`, Add/Subtract/Multiply keep 0/1/2):
A+B, A-B, AxB, A/B, (A+B)/2, max, min | -A, |A|, A^2, sqrt|A|, 1/A,
20log10|A| (dB), dA/dt (central differences), integral A dt (running
trapezoid from the first sample), k.A + c (k, c on the page). Division, 1/A
and dB hold the last value near zero. Units from the probes: VxA = W, V/A =
ohm, A/V = S, like/like = ratio, V^2, sqrt V, 1/V, dBV, V/s, V.s. Labels like
"CH1 x CH2", "d(CH1)/dt" (card title, readout, CSV header). Verified vs
analytic: derivative / integral within 7e-6 of peak; VxA = 10 W; dB 0 / -20 /
+20; k.A+c. Menu grouped (two sources | one source); Source B hidden for
one-source ops; k / c shown for k.A + c. Saved by operation
(math/operation; old math/op index 0-2 maps directly), math/k, math/c.
Also fixed: the card's x symbol was decoded Latin-1 (showed garbled).
`formatEng()` now UTF-8 (units with ohm / ^2 / middle dot).

**Math filter**: the channels' full set (Off / LP / HP / BP / BS) on the
Math result: `setMathFilter(type, lowHz, highHz)` (old math/lowPass and
lowPassHz migrate to low-pass).

**Cursor flags**: `ScaleUnit` / `channelScaleUnit()` — the scale's own
rule (mV while every tick stays under 1 V, else V or the probe's unit),
shared by drawVoltScale() and the flags; Math flags in Math's unit. Flag
values are now what the scale shows at their height (probe, inversion and
offset — the probe alone disagreed on inverted / offset channels), written
with the unit ("-3.58 mV"); the readout's delta row uses the same unit.

**Follow-up:** vertical cursor flags show the number only (in the scale's
mV / V; the scale beside them carries the unit); the readout box rows and
delta keep the unit. Math's Source A / B lists now hold only the channels
present (`refreshMathSources()`: 2 before a scope or file reports, then
m_channelCount; called on device connect and when a file adopts its count;
selections kept by channel where possible) — they listed CH1-CH4 always.

## Known-good waveform library; parasitic drain test

**Library** (sidebar "Library", FA `book`, `PageLibrary`, scrolls).
`waveformlibrary.h` (Qt-free): 16 built-in TYPICAL shapes generated from each
signal's known characteristics, each with suggested timebase / range /
coupling and what-to-check notes: crank inductive + Hall (60-2, 2 ms/div),
cam Hall, ABS inductive, narrowband O2, TPS, MAF, knock (tap), saturated
injector, ignition primary, PWM solenoid, CAN-H, CAN-L, LIN, alternator
ripple, fuel pump current (clamp, A). Generated at 1000 samples/div over
20 divs, key event at the centre (aligns with the live trigger); every
shape rendered and inspected (crank first at 10 ms/div merged into a band
— moved to 2 ms/div so the gap shows). UI marks them "typical shape —
illustrative". "Show on plot": optional setup (channel on, coupling,
smallest range that fits, nearest timebase; leaves Record for Capture),
then a reference at the channel's per-div (range / 4) and position;
replaces the library's own slot when both are full; warns if the probe
unit differs. "Save channel as known-good…": dialog (name, group, notes) ->
AppData/library/<name>-<stamp>.vsxref (same format; `readReferenceFile` /
`writeReferenceFile` helpers) + .json sidecar (name, category, notes, unit,
secondsPerDiv, range, coupling, saved); listed under "My waveforms — group";
deletable.

**Parasitic drain** (Automotive page, after the battery tests):
`BatteryTools::analyseDrain()` on per-second mean / peak (highest 1 ms
average): settled level = median of the last tenth (>= 1 min); settled
from = first point whose next 5 min stay within 1.5x level + 10 mA (10 %
blips allowed); wake-ups after settling (> level + max(50 mA, 2x level),
merged < 3 s apart); Ah used. `judgeDrain()`: vs the acceptable limit
(default 50 mA), days to too-flat-to-start (half capacity), wake list;
a high current that never fell below 0.6x the locking level = never slept
("still awake — run longer" under an hour). Tested: healthy 25 mA (settled
25 min, ~50 days), 180 mA, never sleeps (first version wrongly called a
flat 0.6 A "settled" — fixed), 4 wake-ups found to the second. Flow:
clamp channel must be a probe in A; Record, other channels off, DC;
10 kS/s up to 2 h else 1 kS/s; record size chosen to fit; progress panel
each second (now mA, elapsed); ends on its timer or "Stop and analyse"
(stopAcquisition hook, like cranking). Settings drain/*.

## Library scrapped; startup animation and plug sounds

**Waveform library removed** at the user's request (page, sidebar button,
waveformlibrary.h, the .vsxref read/write helpers it added). Files saved
under AppData/library are left on disk. The drain test stays.

**Optional Qt Multimedia** (CMake `find_package(Qt6 QUIET COMPONENTS
Multimedia MultimediaWidgets)` -> `VSX_HAVE_MULTIMEDIA`; without it the app
builds as before: still splash, no sounds, the two settings disabled with a
hint). Arch: qt6-multimedia + qt6-multimedia-ffmpeg. Windows script adds
$P-qt6-multimedia (windeployqt deploys the plugin; the folder-wide DLL scan
copies FFmpeg's DLLs).

**Startup animation** (main.cpp `playStartupAnimation()`): resources/app/
logo.mp4 (1920x1080, 25 fps, 4.8 s, H.264 + AAC, as supplied) copied to the
cache dir once (backends read files more reliably than qrc), played with its
sound in a frameless window (55 % of screen width, max 900 px, 16:9) to the
end — click / Esc / Space / Enter skips — then a QStackedLayout swaps to the
still (resources/app/splash.png, now the video's last frame, 1440x810) held
while MainWindow is built. Played first because building MainWindow blocks
the GUI ~2.5 s (SDK start) and would freeze the video mid-way; startup is
~2 s longer than the old 3 s still. Falls back to the still splash on
InvalidMedia / error / not started within 2 s; 10 s cap. Settings ->
"Startup animation" (ui/startupAnimation, read by main.cpp).

**Plug sounds**: resources/sounds/plugged_{in,out}.wav (from the user's
MP3s: leading silence trimmed at -45 dB, trailing at -60 dB so fades survive;
48 kHz 16-bit; peaks unchanged -2.3 / -4.6 dB), QSoundEffect from qrc,
played in onDeviceChanged() on every change after the first report (the
state at launch is the starting point). Settings -> "Sound effects"
(ui/sounds).

**Follow-up: video stuck on its first frame, no sound, no plug sounds.**
Common factor: Qt Multimedia's audio output (the FFmpeg backend times
playback by it; on Linux typically PipeWire without pipewire-pulse). Not
reproducible here, so: (1) frames now come from a QVideoSink and are painted
by StartupAnimation itself (QVideoWidget's native child window dropped, and
with it Qt6::MultimediaWidgets); (2) a stall watchdog (no progress for 1.2 s)
restarts the video without audio once, then falls back to the still;
(3) the audio output is chosen explicitly (QMediaDevices::defaultAudioOutput,
none = play muted); (4) "[media]" diagnostics on the terminal: audio
output found, stalls, each sound loaded / failed, and each plug event with
whether a sound played. Asked the user for those lines.

**Follow-up 2: still nothing — and no "[media]" output at all**, so the
media code wasn't compiled in: CMake had cached a failed Qt Multimedia search
(configured before the packages went on) and, being QUIET, built without it
silently; the still banner (now the video's last frame) looked like a frozen
video. CMakeLists now unsets a cached NOTFOUND Qt6Multimedia_DIR before
searching, searches without QUIET and warns loudly when missing. main.cpp
always prints "[media] built with / WITHOUT Qt Multimedia"; all "[media]"
lines go straight to stderr (VSX_MEDIA_LOG, fprintf + fflush), not through
qInfo, which logging rules can filter.

**Follow-up 3: a pause after the animation** (the still held while
MainWindow builds; the SDK start dominates). `AcquisitionManager::
startSdkEarly()` (called first thing in main) runs InitDll() via std::async
during the animation — Linux only (libusb's own hotplug thread; on Windows
device notifications may be tied to the registering thread, so unchanged).
initialize() collects the future instead of calling InitDll. "[startup]"
timings on stderr: InitDll (background), the wait for it, initialize() total
(scan + open), and the whole MainWindow build — to see what remains if the
pause doesn't vanish.

**Follow-up 4: the pause is the window build, not the SDK.** Measured on the
user's machine: InitDll 47 ms, initialize() 102 ms, MainWindow build 1808 ms
(pages, settings, theme — GUI work). (The "~2.5 s SDK" comment was never
measured; startSdkEarly() stays, harmless.) Fix: the intro is now its own
process — `VimuScopeX --intro` (runIntro() in main.cpp) — with its own GUI
thread, so it plays while the app builds its window in parallel. Protocol:
intro prints "done" (video ended, holding the last frame) or "failed" on
stdout; any stdin line/EOF ends it (std::_Exit from a reader thread; 8 s
safety). The app starts it first (ForwardedErrorChannel for its [media]
lines; still splash if it won't start), builds MainWindow, waits for
done/failed/exit (12 s cap), shows the window, and 250 ms later writes
"close" — no gap. Startup ~1.6 s shorter. Clicks on the intro no longer
skip it (Esc / Space / Enter still do).

## Zero calibration

Why it drifts: input amplifier offset drift (uV/C, amplified heavily on
small ranges), input bias current (~doubles per 10 C) through the 1 Mohm
input, ADC / offset-DAC reference and resistor drift — one direction as it
warms, levelling off when the temperature does.

Correction where data enters: `AcquisitionManager::setZeroOffset()`
(subtracted per channel just before samplesReady, after the trigger check;
the hardware trigger level is shifted by the trigger source's offset) and
`StreamEngine::setZeroOffset()` (std::atomic per channel, subtracted in
ingest() on the SDK thread before the ring buffer and the record writer — so
recordings are corrected too). Table `m_zeroTable`: key "path/ch/coupling/
halfMv" (path 0 Capture, 1 streaming; halfMv = the requested hardware
half-range, which selects the SDK's gain stage), input volts; lookup exact
else nearest range on a log scale. `applyChannel()` records the applied
half-range / coupling and calls `applyZeroOffsets()`. Settings ->
"Zero calibration": Apply (zero/enabled), Quick zero (both channels at the
current range / coupling / path, ~1 s), Full calibration (paths x channels x
DC/AC x 12 ranges = kVoltsPerPage for x1 and x10 probes, clamped to the
hardware max; ~half a minute). Runner: confirms inputs disconnected; all
channels on (restored after); offsets zeroed while measuring; per step sets
range / coupling straight on the engine (stream: started first, then the
range, as starting re-applies the UI's), settles (150-700 ms, longer after
coupling changes), then Capture: single + forceTrigger, mean of the frame
(samplesReady intercepted while m_calibrating); streaming: mean of the
newest 0.1 s. 4 s timeout skips a step. Saved under zero/table (keys with
'_'), zero/date; summary panel per channel at its current settings.

**Follow-up:** the calibration progress panel is cleared when it finishes (results panels persist until Clear results, which suits tests, not this); the summary goes to the status bar (8 s) and the per-channel removed offsets are appended to the Settings calibration status label.

## Freeze hunt, step 1: timings (perftrace.h)

Everything talking to the scope runs on the GUI thread (only streamed data
arrives on the SDK's own thread), so slow SDK calls freeze the interface.
Before any thread refactor, measure. `VSX_TRACE("name")` (RAII) times a
scope; a traced call >= 20 ms prints, when its outermost traced caller
ends, as an indented tree on stderr ("[slow] ...", slow callees nested,
quick ones summed). `PerfTrace::startStallWatch()` (main, not the intro):
20 ms heartbeat on the GUI thread + watcher thread; a gap >= 150 ms prints
"[stall] interface blocked N ms in <outermost traced call>" (or "untraced
work"). Traced: all non-trivial AcquisitionManager (acq.*) and StreamEngine
(stream.*) methods except startSdkEarly / setZeroOffset / ingest; MainWindow
applyChannel, setAcquisitionMode, start/stopAcquisition, onDeviceChanged,
applyTimebase, applyRecordRate, refreshLiveView / RecordView / Cards,
restoreSettings, applyTheme, updateEngineAnalysis, updateCanHealth,
stepTimebase, loadRecordTab, refreshRecordTabs, the Capture frame handler
(capture.frame); ScopeView / NavigatorView paintEvent. Tree output checked
standalone. Next: the user's log decides step 2 (targeted fixes vs moving
the SDK to its own thread).

## Freeze hunt, step 2: the log's verdict and the first fixes

User log (621 lines): the SDK is NOT the main cause. Only stream.start
(~170 ms), stream.stop (~200 ms), startRecord (118 ms) are slow SDK calls —
not worth the SDK-thread refactor on this evidence. The real costs were in
app code: applyTheme ~950 ms (3x, ~1 s stalls); refreshLiveView growing
within a run (21 -> 59 ms baseline as the plot's window fills — bounded —
plus 87 / 113 / 143 ms spikes every ~400 ms: the overview strip re-walking
the whole history); six ~600 ms stalls in untraced code during Capture;
capture.frame ~42 ms per 1M-point frame and paints ~29 ms (heavy, not freezes).

Fixes: (1) the theme palette + global style sheet are set BEFORE buildUi(),
so widgets are styled once at creation (setting it afterwards restyled every
widget); applyTheme() skips an unchanged qApp palette / sheet and per-widget
sheets (setSheet helper). (2) Live overview incremental (LiveOverviewCache:
min/max per fixed block — capacity / 8192 samples — folding in only
samples written since the last refresh via written() / historyCapacity();
rebuilt on restart / resize) — verified in simulation across ring wrap:
a single-sample spike lands in the same column as the full walk, bands
within block-boundary noise. (3) Linux "[stall-trace]": the stall watcher
SIGUSR2s the GUI thread at ~200 / ~450 ms into a stall (<= 20 per session);
the handler prints backtrace_symbols_fd to stderr; -rdynamic for names.
Tested standalone (names the blocked function). Next: the user's new log,
with traces for the untraced 600 ms stalls.

## Freeze hunt, step 3: second log

Theme fix confirmed (applyTheme no longer >= 20 ms; startup stall 2.6 s ->
1.8 s). Live was now the dominant cost: refreshLiveView 656x, median 87 ms,
max 130 ms, still growing within a run; 120 stalls ~230 ms "in
refreshLiveView". The stack traces (20) decided it: 10 in ScopeView::
paintEvent -> drawReadouts -> readoutLines -> measureCore, i.e. the readout
measurements recomputed during every paint (readoutLines() is called 2+ times
per paint, plus remoteFrame), three O(n) passes over the whole buffer —
millions of samples in Live, growing as the window fills. The repaint runs
inside refreshLiveView, hence the attribution. Fix: measureCore() caches
per source vector (&samples: channel samples / Record measurement source /
Math): identical inputs (generation, data pointer, size, gain, bias, rate)
return the stored result; new data within 200 ms of the last measurement
returns the stored result and schedules one repaint when the interval ends
(readouts at <= 5/s while streaming; always catch up). Values unchanged
(measureUncached = the old code). m_dataGeneration now also bumps in
setMeasurementSource / clearMeasurementSources / recomputeMath (cache keys).
Remaining slow-but-expected: stream start/stop ~0.2 s, startRecord ~0.13 s,
refreshRecordView ~100 ms (readDecimated from disk) on Record view changes.

## Freeze hunt, step 4: third log

The measurement cache didn't bite: its interval was timed from a
measurement's start, so a ~200 ms measurement had always expired by the next
paint (traces: measureUncached on 14 of 20). And refreshLiveView's ~87 ms
median was mostly something else: benchmarked here at -O2, measureUncached
is ~22 ms per 4M-sample channel, but converting + copying 4M samples a few
times (what a Live refresh does) took ~100 ms because glibc serves 32 MB
blocks by mmap and returns them on free, so every refresh re-faults ~8000
pages per block. Fixes: (1) main(): mallopt(M_MMAP_THRESHOLD 512 MB,
M_TRIM_THRESHOLD 1 GB) under __GLIBC__ — measured 99 -> 19 ms per refresh-like
cycle; freed big blocks stay with the process. (2) Measurement cache:
interval = max(200 ms, 4x the last measurement's cost), timed from when it
finished. (3) pullStream(): the Live tick is spaced to clamp(3x the last
refreshLiveView cost, 33, 1000) ms, so refreshes can't saturate the GUI
thread whatever the window / channels / Math.

## Freeze hunt, step 5: fourth log — mixed, so measure finer

Live spacing worked (185 refreshes vs 1024) but each refresh was slower
(median 137 ms vs 87 — conditions may differ between runs; unknown), and
measureUncached was still on 10 of 20 traces inside ~225 ms paints — far over
the ~22 ms per 4M samples benchmarked here, so the data or the number of
measured sources must be bigger than assumed. No fix this round: finer
timers instead (per the debugging protocol). Added: live.read /
live.convert / live.setSamples / live.feedSpectrumXY / live.overview in
refreshLiveView; view.refilter / view.math in setSamples; view.drawTrace
(per channel) / view.drawReadouts in paintEvent; view.measure, plus
"[measure] N samples took X ms" when >= 50 ms; "[live] window N samples per
channel (s at S/s), channels on, Math on/off" whenever the setup changes.
(Caught while adding: a brace-less if around drawTrace would have drawn
hidden channels — braced, and all insertions scanned.)

## Freeze hunt, step 6: fifth log — the structural fix for Live

Setup: 50 s window at 5 MS/s (4,194,304-sample cap per channel), 2 channels,
Math on. No measurement took >= 50 ms (step 4's cache fix works). Each
refresh ~280 ms, every step O(4M): view.refilter 77 ms (a channel filter on),
view.math 42, live.convert 33 x2, live.overview 56 (rate-proportional at
5 MS/s), drawReadouts 38, drawTrace 23. Root cause: Live handed the plot every
sample of the window. Fix (refreshLiveView): when the window exceeds 1M
samples per channel and no sample-level analysis is on (decoding slots,
engine tools, compression, CAN health), each channel is reduced to 8192
min/max pairs (shown at rate x buckets / got x 2) — peaks kept, like Record's
pages. Filters run before reducing: LP / BP with highHz x 8 <= the reduced
rate via bucket averages filtered at that rate (Record overview's method,
~1 %), otherwise exact on the full-rate data then min/max; the view's own
filtering is off while reducing (setLowPassInView), and m_liveReduced resets
on every mode switch (setAcquisitionMode restores setLowPassInView). Exact
measurements: setMeasurementSource(full-rate, filtered) at most every 250 ms
per channel, only if anyMeasurementActive() (new ScopeView accessor);
cleared when reduction ends. Spectrum / XY get the newest 64k full-rate
samples. Math is computed from the pairs while reducing (display approximate
at that zoom). Not yet measured on hardware.

## Freeze hunt, step 7: sixth log — Live preparation on a worker thread

After reduction: refreshLiveView 278 -> 191 ms median (max 378 -> 204);
remaining per refresh all O(4M): live.measureSource 96 ms (convert + the
channel filter over the full window, every 250 ms), view.measure ~20 ms per
channel in paint, live.reduce ~31 ms per channel, live.overview 37 ms (5 MS/s
of new data). None of it needs the GUI thread. New LiveWorker
(liveworker.h/.cpp, QObject moved to m_liveThread, created after m_stream,
quit + wait first in ~MainWindow): process(LiveJob) reads each channel's
window (ring buffers are mutex-protected, as are written / available /
capacity), filters, reduces to 8192 min/max pairs (same branches as before),
measures the full-rate filtered data with ScopeView::measureSamples() (the
static measurement code, now public; gain / bias via new measureGain /
measureBias), returns a 64k full-rate tail for Spectrum / XY and, every
400 ms, the incremental overview (its own cache). refreshLiveView, when
reducing: builds the job and posts it (QMetaObject::invokeMethod functor),
one in flight at a time (ticks skipped meanwhile), returns at once.
applyLiveResult() (queued back to the GUI): setSamples(pairs, shownRate),
setMeasurementOverride (ScopeView::measure returns it instead of measuring
the pairs), soft-AC tracking, Spectrum / XY tails, navigator overview,
snapshot, then finishLiveRefresh(wanted, rate, available) — the old tail,
shared with the full-rate path. Results are ignored if Live moved on
(mode, loaded file, no longer reduced). Overrides cleared when reduction
ends and on every mode switch. Not yet measured on hardware.

## Freeze hunt: solved (seventh log)

Same setup (50 s at 5 MS/s, 2 channels, Math, a channel filter): no
refreshLiveView or paint stalls at all; worker.liveFrame 229x, median
202 ms, off the GUI thread (~5 exact frames/s on a 50 s window). Remaining
GUI stalls: stream.start ~0.33 s / stream.stop ~0.2 s (SDK, only on Run /
Stop), startup build ~1.6 s (behind the intro), one 0.3 s untraced.

The instrumentation is now opt-in: `VSX_TRACE=1 ./build/VimuScopeX`.
PerfTrace::enabled() (env, read once): VSX_TRACE scopes cost one check when
off, startStallWatch() returns early (no heartbeat, no watcher thread, no
signal handler), and the [startup] / [live] / [measure] lines print only when
on. [media] lines are unchanged (a few, at startup / on plug events).

Summary of what fixed the freezes (each found by measurement, not guessed):
theme style sheet applied before buildUi (~1 s); Live overview incremental;
measurements cached / throttled (from when they finish, >= 4x their cost);
glibc keeps large blocks (mallopt; 99 -> 19 ms per refresh-like cycle);
Live spacing (3x refresh cost); long Live windows reduced to 8192 min/max
pairs with exact full-rate measurements; and that preparation moved to
LiveWorker on its own thread. The SDK-thread refactor proved unnecessary.

## Live long windows: whole-window frames (time axis / overview drag)

Bug (user screenshot, 50 s at 5 MS/s): the Live window was capped at 4M
samples (0.84 s) — long-standing, not from the reduction — and ScopeView maps
whatever it gets across the full width (sampleIndexToX uses min(window,
data)), so 0.84 s was drawn as 50 s and the axis ran 0..45 with 0 at the left;
the navigator viewport (span from the capped wanted) was a sliver. Fix: the
job carries the uncapped window (windowSamples; finishLiveRefresh takes
qint64), and LiveWorker builds B = 8192 buckets over the WHOLE window,
newest at the right edge: windows <= 4M samples are read in full (exact;
the part older than the history is NaN); longer ones come from the history
cache, now Block {min, max, sum, sumSq} per fixed block (folded
incrementally; also feeds the overview strip), buckets before the history =
NaN. ScopeView::setTraceGaps(on) (Live reduced only; off on mode switch):
drawTrace / drawMathTrace lift the pen at NaN (columnRange() skips NaN; the
default path is the old minmax_element). Measurements, block path:
min / max / Vpp / mean / RMS / AC RMS over the whole window exactly from the
block sums (gain / bias applied algebraically), frequency / period / duty /
widths from the newest <= 4M full-rate samples; filtered channels take all
from the newest filtered samples. Filters on block frames: LP / BP from
bucket averages (exact enough at this zoom); HP / BS envelope minus the
removed part (approximate). Simulated (history 20M, window 50M): spike in
the exact column; empty columns correct; window stats match brute force
(exact on the newest window; panned, within one block's samples).

## Live history length and panning responsiveness

History was capped at 64 Mi samples per channel and allocated for all four
ring slots: 30 s at 5 MS/s became 13.4 s (= 64 Mi / 5M), while the panel
still said "History 30 s". Now StreamEngine::start sizes rings only for
channels in the mask (others get 1 sample), from a budget of 35 % of physical
RAM (sysconf / GlobalMemoryStatusEx; clamped 512 MB .. 16 GB) shared by the
active channels; int cap 1.5e9. The Acquisition panel appends "(X held:
memory limit)" when capped. (Windows: NOMINMAX before windows.h.)

Laggy overview drag in reduced Live: each frame re-read and measured the
newest 4M full-rate samples (~200 ms), and requests during a frame were
dropped — the final drag position could be lost. Now: m_livePanClock
(restarted at the three Live pan points: plot drag, overview, zoom change);
within 250 ms of a pan, jobs skip measurements (block-only frames take
milliseconds), and a follow-up refresh 300 ms later restores them. A request
while a frame is in flight sets m_liveJobPending; applyLiveResult() then runs
refreshLiveView() again, so the newest position always lands.

## Known gaps closed: settings, phone panning, decoding

**Settings.** Decoding slots saved / restored (decode/<slot>/...: protocol,
input, rates, LIN version, K-Line framing by currentData; threshold, sample
point, invert; enable last). Annotations: ScopeView::annotationsState() /
restoreAnnotations() (text, box x / y as fractions of the view, size, arrow
target), saved as "annotations", restored 200 ms after construction (laid
out); restore reuses addAnnotation() then drops its focus. .vsxwave
version 3: per-channel filter (type, low, high), Math filter, Math k / c
appended; read only from v3 files; files newer than kWaveVersion are
refused with a message (older apps read v3 and ignore the tail).

**Phone.** A press in the plot decides after 8 px: mostly sideways pans
({cmd: pan, fraction} every 40 ms -> emit m_view->panRequested, i.e. exactly
a mouse drag, each mode its own way), mostly vertical drags the trace near
the finger; gutters always drag their trace.

**Decoding.** CAN FD: CanTools::checkFdCrc(raw bits) — own destuffing,
FDF / DLC (FD lengths), CRC field with fixed stuff bits (complement, before
every 4 bits), Gray stuff count + even parity, CRC-17 0x1685B / CRC-21
0x102899 (x^21 term dropped — first written 0x302899, caught by test),
register 1 followed by zeros, over raw SOF..data end + stuff count. Whether a
dynamic stuff bit due right after the data is inserted is ambiguous from
memory: both readings are tried. Tested: 640/640 valid frames pass for each
reading; corrupted data 600/600, stuff-count flips 640/640, broken fixed
stuff bits 640 + 638 /640 caught; classic and cut-off frames skipped.
decoderengine: checkCanFd() builds the raw stream from bits + any marked
stuff bit without its own bit entry, red annotation over the CRC field on
mismatch. markCutOffFrame(): a last frame whose bits reach the end gets a
non-error "cut off" annotation (assembleCanFrames already drops an unclosed
final frame from the statistics). Sample rate: onDecodeFinished() warns
below 8 samples per bit (CAN nominal; CAN FD data rate once FD traffic is
seen; LIN; K-Line): row name gets a warning mark, status bar explains once.
Not yet tried on real CAN FD traffic.

**Live zoom Back / Forward fixed:** applyZoomState() restored m_liveTimebase and the offset but refreshLiveView() takes its window from m_view->secondsPerDiv(), which wasn't set — it stayed zoomed and jumped to the newest edge. Now sets the view's timebase first (as Capture and loaded Live files already did).

**Annotations per context; first plug sound.** Annotation sets per context
("capture", "live", "record:<dir>"): m_annotationSets, syncAnnotationContext()
after every mode switch and m_currentRecordTab change (5 call sites) stores
the outgoing set and shows the incoming one (ScopeView::clearAnnotations()
new); saved as annotations/sets (empty sets and deleted recordings pruned);
the old single "annotations" list migrates into the startup context; sync is
inert until the startup restore (200 ms) so restoreSettings' own mode / tab
changes can't swap in empty sets. Restored boxes no longer take focus
(addAnnotation(bool focus); the old clearFocus ran while the window wasn't
active yet, so the focus request was applied on activation). Plug sound: with
no scope at launch no device report came, so the first plug-in became the
starting state silently; the state is now marked known right after
m_acq->initialize().

## Text 5 % larger everywhere

`Fonts::kUiScale = 1.05` / `Fonts::scaled()` (fonts.h). main.cpp: the base
font is scaled after its 9 pt floor (9 -> 9.45 pt). Every size derived from
the base by a fixed offset / floor goes through scaled() too (plot labels,
readouts, bus rows, spectrum / XY, card titles / values; ~34 sites in
scopeview / spectrumview / xyview / cardbutton): scaling only the base would
have grown the derived ones by ~6.5 % because their fixed offsets don't
scale (checked: with offsets scaled the ratio is exactly 1.0500 at every base
size). Style sheets: combo boxes etc. derive from QApplication::font() so
follow; the two fixed pixel sizes (whole pixels in Qt style sheets): card
+/- glyphs 19 -> 20 px, sidebar grid buttons 11 -> 12 px (9 % — pixel sizes
can't be fractional; widest label "Annotations" 59.8 -> 65.3 px, inside the
~89 px column). Not scaled: icons (Font Awesome glyph sizes follow their
button's icon size), the phone remote page (CSS / canvas sizes of its own).
Fixed card / gutter sizes were checked for clipping (kCardWidth 178, scale
gutter 42, axis 18): ample room for 5 %. To change the factor: kUiScale.

## Capture rate (3.6 fps at 5 ms/div, 100 kS/s, 8 KB): measuring first

~280 ms per frame vs an 82 ms fill (8192 samples at 100 kS/s). From the
code: arm() re-sends trigger style / mode / source / level / pre-trigger +
Capture() every frame, but the earlier logs show no acq.arm >= 20 ms, so the
whole arm is cheap; the 200 ms guard in poll() is only the retry after a
refused Capture(); poll granularity is 20 ms (kPollIntervalMs). Open
suspects: the scope's own wait (AUTO trigger timeout when no edge crosses the
level), IsDataReady / read call costs, frames discarded for lacking an edge
(Normal trigger, verifyEdge). New opt-in line (VSX_TRACE=1), once a second
from AcquisitionManager::noteArmDone(): "[capture] N frames in S s = X fps |
per frame: scope wait, fill time expected, read, app after data (data ready
-> next arm, includes the samplesReady handlers), arm | IsDataReady calls and
cost | frames dropped (no edge) | trigger Normal/Auto". Next step depends on
which term dominates.

## Capture rate: the scope's auto-trigger timeout (measured, then fixed)

User's [capture] line (5 ms/div, 100 kS/s, 8192 samples, Auto, no edge at
the level): 3.6 fps; scope wait 275 ms per frame vs 82 ms to fill; read /
app / arm all ~0 ms; IsDataReady 0.0 ms a call; 0 dropped. With real
triggers 8.9 fps; fps rises with sample rate (fixed ~190 ms wait, shrinking
fill). Cause: the scope's own AUTO timeout (~190 ms), no SDK setting for it.
Fix: AcquisitionManager::maybeForceAuto() — continuous Auto only (not Normal,
not Single, not draining / reconfiguring): once fill x pre-trigger share
(m_preFraction, as armed) + 10 ms + m_autoWaitMs has passed since arming
without data, TriggerForce() once per arm (documented "force capture once";
the Force button's call). Never lengthens the scope's own wait. Wait chosen
by the new "Auto wait" combo on the trigger page (trigger/autoWait):
Short = max(20 ms, 1 screen), Medium = max(50 ms, 2 screens), Scope default =
off; MainWindow::updateAutoTriggerWait() from the Capture timebase (hooked
in applyTimebase via a 0 ms timer; combo change). Poll interval 20 -> 5 ms
(IsDataReady costs 0.0 ms). The [capture] line gains "N forced after W ms
wait". Expected at the user's settings: ~41 ms pre-fill + 10 + 50 + 41 post
~ 140 ms (~7 fps) vs 275 ms. Untested on hardware: whether TriggerForce is
honoured mid-capture here — the next [capture] line shows (forced count, fps).

Process note: three Python edits in a row aborted midway on non-unique
anchors after their header edits had already been written, leaving header
and source inconsistent; the bracket sweep can't see that. Check declared /
defined / used counts for new symbols after multi-file edits.

## Zero calibration recentred the scope's 2 Vpp cal wave on 0 V

The cal output is a 0-2 V square (mean 1 V). Calibrated with the probe still
on it, the app stored ~1 V as the "offset" of an open input and then
subtracted it. Nothing checked that the inputs were open. Fixes:
(1) calibrationMeasured(volts, peakToPeak): a step is rejected (not stored)
when |mean| > max(30 mV, 2 % of the hardware half-range) or swing >
max(20 mV, 10 % of it) (plausibleOffsetV / plausibleSwingV, top-of-file
anonymous namespace; the scope's real offsets are a few mV, noise ~1-2 LSB);
m_calRejectCount / Mean / Ptp per channel; finishCalibration shows the
rejected count in the summary and a QMessageBox warning naming the channel,
its average and swing. (2) restoreSettings drops stored offsets above the
plausible limit and re-saves the clean table, with a status message.
(3) "Clear calibration" button (Settings). Table saving factored into
saveZeroTable() (const); the zero/date only updates when something was
stored. (An uncontaminated table is unaffected: the user's real offsets were
3-6 mV.) Checked after editing: every new symbol declared / defined / used,
and the helpers defined above their first use (the first attempt put them
after restoreSettings' use — caught by the symbol cross-check).

## Windows link error: multiple definition of TLS init function

perftrace.h declared `inline thread_local std::vector<Frame *> t_stack`. A
thread_local with a non-trivial constructor gets a hidden "TLS init function";
MinGW emits it in every translation unit that includes the header as an
ordinary (non-COMDAT) symbol, so the Windows link failed with "multiple
definition of `TLS init function for PerfTrace::t_stack`" (Linux GCC merges
it, so Linux never showed it). Fix: the stack is now a linked chain through
`Frame::parent` / `depth` and a constant-initialised
`inline thread_local Frame *t_top = nullptr` (no init function exists).
Verified on Linux: four translation units including the header link, `nm`
shows no TLS init / _ZTH symbols, nested "[slow]" output unchanged. The only
other thread_local (streamengine.cpp, function-local in a non-inline member)
is defined once and was not flagged. Rule for headers: no inline thread_local
with a constructor/destructor; if one is needed, use a pointer set to nullptr.
Not rebuilt on Windows here: the user's next build confirms.

## CAN: false CRC errors at 5 MS/s, partial frames at the window edges, huge health report

User screenshots (Windows build, libsigrokdecode 0.6.0-git, 500 kbit/s CAN-H on CH1, DC x1, Record 5 MS/s): two frames flagged "CRC error" with an overlapping, unreadable red box; both had a dominant ACK slot (the receivers' CRC passed, so they're valid frames). Levels: dominant 3.62 V, recessive 2.1-2.5 V (slow tail after dominant bits), decoder threshold 3.0 V: clean margins. The dashed line near 2.41 V is the plot's centre reference, not the threshold.

Simulation (fair model: real-time edge jitter, bus clock error up to 0.5 %, 150-300 ns edge ramps, slow recessive tail): plain single-sample decoding at 5 MS/s (10 samples/bit) reads 500/500 frames even in the rough scenarios; it only fails at 2.5 MS/s (5/bit, 372/500). So 5 MS/s IS enough for 500 kbit/s; sampling resolution is not the cause (an earlier crude +-1-sample-jitter harness suggested otherwise: unrealistic, it favoured 12.5 MS/s). Sub-sample edge placement (interpolated threshold crossings on a finer grid) fixes 2.5 MS/s fully but isn't needed at >= 8/bit: not built.

Cause not proven (no raw data): the decoder-annotation based CRC check (checkCanCrc matched bits to frame boundaries by exact start / end sample comparisons; at a whole number of samples per bit ranges meet exactly, so an off-by-one drops or keeps a bit) is the prime suspect. Fixes: (1) bits belong to a frame / are stuff bits by their MIDPOINT; (2) CanTools::verifyClassicFrame / verifyClassicFrameAnyPoint (cantools.h, Qt-free): re-reads the frame from the logic stream (sample point after each dominant edge, resync on every dominant edge, destuff, stops at the end of the CRC field because ACK delim + EOF are legitimately 8+ recessive bits, CRC-15) at 70 % and 7 other sample points; a CRC error is shown only if NO point gives a valid CRC (tested: 500/500 good frames pass in all scenarios; corrupted CRC field: 3 of 1792 wrongly accepted = 0.17 %). Unreadable frames aren't accused (the decoder flags stuff errors itself). First version of the verifier rejected everything (destuffed past the CRC into the trailer; and failed when a buffer ended early): caught by an ideal-frame test. (3) the red CRC annotation REPLACES the decoder's own CRC box (they overlapped into unreadable text). Diagnostics: VSX_TRACE=1 prints "[can] frame at sample N: decoder bits give CRC x vs y on the wire (k bits used); independent read: readable, CRC OK/mismatch" for every disagreement: if the user still sees errors, ask for those lines.

Partial frames: Record mode decoded exactly the visible span, so a frame cut by the left edge started mid-frame (stuff / form / CRC errors) and one cut on the right ended abruptly. Now (CAN only) buildDecodeJob reads 1000 bit times of context each side (m_decodeLead[slot] = samples before the window); DecodeJob::visibleFrom / visibleTo; decode() starts at the last >= 9-bit recessive gap before the window minus 2 bit times (inside a frame body the longest recessive run is 5 (tested over 20000 frames); ACK delim + EOF make 8; frames are separated by >= 11), reports only what is in the window: annotations overlapping it, frames whose SOF is inside it (each frame counted once however you pan), stray errors inside it, sampleCount = the window. Annotation positions map to the page as (start - lead) * indexScale. Tested: 11184 window starts over a 40-frame stream: decoding never started inside a frame body; 9429 starts inside a frame: the whole frame was included each time. Not done: LIN / K-Line (same issue, less visible); Capture / Live decode the whole buffer (edges of the buffer are inherent).

Health report: the ID table's data column joined every byte (a 64-byte FD frame = a line wider than the plot), and the right-anchored panel's width was unbounded, so it ran off the plot's left edge. Now: data shows 8 bytes then an ellipsis, panels are limited to 62 % of the plot width (last column shrinks, cells elided). Stale stats: when decoding can't run ("Zoom in to decode ...") showDecodeMessage() clears m_canFrames etc. and m_canMessage says why in the panel.

Process: edits applied atomically (all anchors asserted before any write) after earlier half-applied edits; every new symbol cross-checked declared / defined / used. The decoder / MainWindow changes are not compiled here (no Qt / sigrok): the next build is the check.

## Decode context for LIN / K-Line; "DB"-only labels; duration text; darker chrome

Context decoding (Record mode) now covers all three protocols: margin 1000 bit times (CAN, LIN) or 4000 (K-Line: long messages with gaps), capped at kMaxRecordDecode / 2 each side. decode() start rules: CAN = last >= 9-bit recessive gap; LIN = 2 bits before the last break (dominant run >= 11 bit times; inside a frame the longest is 9: a 0x00 byte) starting before the window (scan extends 20 bits past it to catch a break straddling the edge); K-Line = inside the last idle run >= gapBits (25 ms; bytes within a message are <= 20 ms apart). Tested: LIN 9885 window starts, never mid-frame, 8347 cut frames all whole; K-Line 10956 starts, never mid-message, 7377 cut messages all whole.

"DB"-only labels: drawBusRows picks one label length per field group (the longest every box fits); a frame cut by the plot edge (decoded whole since the context change) leaves a sliver box that forced every data box to "DB". Boxes extending beyond the visible area no longer take part in choosing the level; they still pick what fits their visible width.

Health line: "%1 frames in %2 s" used 'f', 3 -> "0.000 s" for windows < 1 ms; now formatSeconds() and frame / frames.

Theme: windowBackground() 1b1f25 -> 15191e (dark), eef1f4 -> e5e9ed (light): top bar, left rail, strip behind the record tabs, status bar. Cards (cardBackground) and unselected tabs (surfaceBackground) unchanged, so they stand out.

Distorted frames (user screenshot, CAN-H + CAN-L): ringing on DOMINANT bits only, in a few frames only, while other frames are square: points at the transmitting node (long stub / branch or a different transceiver; dominant is actively driven, recessive passive), not the probes (they'd affect every frame). Below the 3.0 V threshold's margins: decoding unaffected. The tall spike near each frame's end is the ACK slot (all receivers drive it together): normal.

## Zoom arrows restore zoom only; arrow-key panning; greener secondary accent; tab order

applyZoomState(): Back / Forward apply the saved zoom level only, keeping the current view centre: Capture sets the timebase only (setSecondsPerDiv keeps the centre); Live keeps the centre time (offset recomputed for the new window, clamped to the history; offset 0 = following stays following); loaded Live files timebase only; Record keeps the centre sample, start recomputed for the saved span and clamped to the recording.

Arrow keys: MainWindow::eventFilter (installed on qApp after m_acq->initialize()): in Record mode with a recording open and the window active, Left / Right (no Ctrl / Alt / Meta) emit ScopeView::panRequested(+0.1 / -0.1), i.e. one division (Shift: a whole screen) — exactly a drag (positive = earlier). Not taken when the focus is a QLineEdit, QAbstractSpinBox, QTextEdit / QPlainTextEdit (annotation text), QComboBox, QAbstractItemView or QAbstractSlider. The Record pan handler already defers the redraw, so key repeat is smooth.

accentSoft(): cdf7ab -> bdf59c (dark), 3f7a1a -> 357f14 (light): top bar / left rail icons and labels, close icons, probe dialog.

Record tabs: refreshRecordTabs() stable-sorts recordings by the saved records/tabOrder (unknown = INT_MAX, i.e. on the right, chronological among themselves) and saves the shown order after every rebuild; setRecordTabHidden(dir, false) (Open bringing a closed tab back) removes it from the order so it's appended on the right. First run: empty order = the old chronological order.

Process: the first attempt's anchor missed a blank line (an earlier view had hidden blank lines); the atomic script aborted before writing anything.

## Track-mode rulers

Rulers page: "Track the trace" (under Time cursors; disabled while those are off; saved as rulers/track). ScopeView::setRulersTrack(); active when track && time rulers on (rulersTracking()). computeRulerLayout(): for each enabled channel / Math voltage pair, v1 / v2 come from traceValueAt(ch, anchor + t1Offset / t2Offset) instead of the stored values (stored values untouched, so turning Track off restores the manual rulers). traceValueAt(): linear interpolation between samples; when the samples are min / max pairs (setSamplesArePairs(): Record page with span > points, reduced Live windows; reset on every mode switch) the middle of the column's envelope; false outside the data or in a NaN gap (that flag / line / marker is hidden and the box shows a dash). drawRulers(): ring markers where each voltage ruler meets its time ruler; box adds a signed slope row "dV/dt" per channel (from ruler 1 to 2, channel unit / s; Math in mathUnit() / s); rulerBoxRect() counts it. hitTestRulers(): tracked voltage rulers aren't draggable (drags fall through to the time rulers / panning). traceValueAt tested standalone on the code as written: interpolation, ends, pairs, gaps (8 cases).

## ADC range measurement (12-bit scope, per the user)

Question: is the ADC's vertical resolution fully used? Findings from the code: no resolution is lost after the ADC (doubles / floats throughout); the hardware window is already asymmetric = Input offset +- one page (SetOscChannelRangemV / SetStreamChannelRangemV take min / max); but dragging a trace (Position) is display-only, and the app never checked what window the scope actually applied (the SDK only says it "picks the gain that fits").

Measurement added. adcwindow.h: AdcWindow from the SDK's own conversion, voltage = code x zoom + bias, codes 0 .. 2^bits-1 (documented for ReadADCDatas / ReadStreamADCDatas) -> window bias .. bias + (2^bits-1) x zoom, |zoom| per code; codesOnScreen() = requested span / |zoom| (capped). AcquisitionManager: m_adcBits = GetOscAdcBit at connect; after each channel's ReadVoltageDatas in poll(): ReadADCToVoltageZoom / Bias + GetVoltageResolution into m_adcWindow[ch]; ++m_frameSerial; setRangeMv records the requested min / max. StreamEngine::adcWindow(ch): ReadStreamADCToVoltageZoom / Bias + GetStreamVoltageResolution live while running, requested = m_rangeMinMv / MaxMv. UI: an "ADC" line on every channel page (updateAdcInfo(), 1 s timer): requested vs actual window, per code, screen codes / 4096. Settings > Range report: Capture mode; for each enabled channel, every page from calibrationRangesMv(limit), centred at 0 and shifted up by half and by one page (skipped when the limit squeezes it); per step: setRangeMv, 120 ms settle, single() + forceTrigger() after 80 ms, wait for frameSerial to change (1.5 s timeout), record requested vs actual window, per code, screen codes, raw SDK zoom / bias / resolution; then applyChannel() for all and the previous mode restored; the table goes to the clipboard and a dialog. Raw zoom / bias are listed because the SDK doesn't state units (assumed volts, as ReadVoltageDatas). AdcWindow arithmetic tested (exact, symmetric fallback, inverted zoom, no data). Next: from the user's report decide on (a) the window following the view / an Auto vertical fit, (b) averaging / high-res modes.

## Range report results (user, 12-bit, Capture): the scope's window is always symmetric

Measured: bias = -(half window) for every request; the scope picks the smallest of 15 symmetric hardware ranges covering both ends of the request: +-144.5, 200.4, 395.8, 558.2, 779.2, 1085.8, 1509.0, 2076.5, 4078.8, 5686.2, 8019.2, 11192.9, 15597.4, 21678.1, 29937.4 mV (zoom = SDK resolution = V per code; units confirmed volts). So SetOscChannelRangemV's min / max does NOT offset the window (the old comment claimed it did). Consequences: pages >= +-200 mV centred on 0 get 88-100 % of codes; pages below +-145 mV are digital zoom (+-50 mV 1417 codes, +-5 mV 142); with an input offset the resolution is set by the largest absolute voltage (shifted tests land at 46-49 %).

Fixed: m_appliedHalfMv = max(|min|, |max|) (the range really used), was (max - min) / 2 — with an input offset the zero calibration looked up a range several times smaller than the active one. Comment in applyChannel corrected.

Still open (proposed to the user): calibrate every hardware range (full calibration only requests 1-2-5 pages, so the +-396, 779, 1509, 4079, 8019, 15597, 29937 mV ranges are never measured and get the nearest one's offset); averaging / high-res modes (real extra bits below +-145 mV; noise of a few LSB dithers); suggest AC coupling for small AC on a large DC; mark digital zoom on the channel card; check the stream path (Live / Record) with the ADC line, as only Capture was measured.

## Live: AC-coupled CH1 stuck on the +-2 V range (user report)

User: in Live, CH1 got 2 / 5 / 10 / 24 / 48 / 96 % at +-50 mV / 100 / 200 / 500 / 1 V / 2 V, i.e. always ~1.02 mV per code = the +-2076 mV hardware range; CH2 matched the Capture report; Record fine. In the user's Live screenshots CH1 was AC x1 LPF and CH2 DC; in Record CH1 was DC. Ruled out: stream-start order (reapplyCoupling before applyStreamRange), page-change order (setChannelCoupling then setChannelRange); no trigger in the stream path. Leading explanation: with AC coupling in stream mode the scope won't go below ~+-2 V (either SetStreamChannelRangemV refuses -> applyStreamRange doubles to +-1.6-2 V, or the SDK enlarges it). Not yet confirmed. ADC line now prefixes the coupling and states when the scope refused the range and what was sent (AdcWindow::sentMinMv / sentMaxMv from m_usedMinMv / MaxMv; Capture = requested). Test asked of the user: Live, CH1 at +-50 mV, AC vs DC. If AC-only: consider DC coupling + software DC removal in streaming when the DC level is small (whichever needs the smaller hardware range).

Update: the user's CH1 was DC when stuck, and toggling the coupling AC -> DC cured it (both couplings fine afterwards), so it is NOT an AC limitation: the channel's front end stopped following range commands while streaming (page changes re-send the SAME coupling and the range: no effect; a real coupling change: cured). Root cause unknown (not reproducible here). Self-heal: StreamEngine::resetChannelFrontEnd(ch) = SetStreamAcDc(other) + SetStreamAcDc(own) + applyStreamRange. updateAdcInfo() (1 s) checks every active channel while streaming: stuck when the real half-window > 2.5 x max(sent half, 0.2 V) (scope ranges are <= ~2x apart; 0.2 V floor because pages below the +-145 mV hardware minimum legitimately get a bigger window); reset once per (sent range, coupling) per channel (m_frontEndResetKey), status message. Checked against the user's data: detects the stuck +-50..500 mV pages; none of 9 normal Capture-report windows trip it; +-1 / +-2 V stuck cases are indistinguishable from a normal step (~1 bit lost, not caught).

## Overview strip click; High-res and Capture averaging

Navigator: a press ON the box takes hold of it (m_grabOffset = where in the box) and nothing moves until dragged; the drag moves it by the pointer's movement. A press elsewhere still jumps there. A tiny box is grabbable as 8 px.

High-res: ScopeView::FilterHighRes (= 5) in the channel Filter list; N (4 / 16 / 64 / 256 = +1..+4 bits) from a selector shown instead of the Hz fields, passed as the filter's lowHz. applyFilter(): centred mean of N samples via running sums (ends: mean of what there is; N clamped to the data length). MainWindow::filterLowParam(ch) returns N for High-res, else the low cutoff; every reader goes through it (channel page apply, settings restore, waveform v3 save (N stored in "low"; the loader puts it back in the selector), Live job, Record view). Settings key filterHighResN. Card tag "HRxN" (the old fixed array would have read "BSF" for type 5). Works in Capture (view filtering), Live (worker raw path; the >4M-sample block path shows the unfiltered envelope, measurements filtered) and Record (raw filtering path). Tested as written: 1.0 / 2.0 / 3.0 / 4.0 extra bits on quantised noisy data (0.6-step noise, the +-145 mV range's 70.6 uV step); step 50 % point within half a sample (even N).

Averaging (Capture): Scope page, under Capture depth: Off / 2..128 frames (setting capture/average); averageFrame() in the samplesReady handler before anything is shown: per channel a mean up to N then exponential (weight 1/N), each frame shifted to line up with the average's trigger point; restarted on m_avgGeneration changes (applyChannel / applyTrigger / applyTimebase / setAcquisitionMode / the setting itself) or a change of length or rate; bypassed in free run (status message once). Snapshot, plot, overview, Spectrum and XY all get the average. Scope card note "... avg N" in Capture.

## Live: fast timebase changes crashed the app

No backtrace available. Found: applyTimebase() (Live branch) stopped and restarted the stream on EVERY timebase change while running, although the Live rate comes from its own setting (m_liveRate) — turning the timebase quickly = a storm of SDK stop / start cycles (~0.3 s each) with the Live worker in flight and the new 1 s ADC check querying the SDK. Ring buffer resets are under their lock and the worker copes with shrinking history, so the SDK restart storm is the prime suspect. Fix: restart only when the stream must change (rate differs from m_stream->sampleRate(), or History was bumped for the new window), collapsed into one restart 0.3 s after the changes stop (m_streamRestartTimer); otherwise refreshLiveView() from the history already held (faster too). m_streamStartClock (restarted at the end of startAcquisition()): updateAdcInfo() leaves the stream SDK calls (adcWindow, the stuck check) alone for 1.5 s after a start. If it still crashes: get a backtrace (gdb: run, then bt).

## LIN / K-Line health

serialbus.h (Qt-free, namespace SerialBus): analyseWire (recessive / dominant medians either side of the middle of the 2-98 % span; needs a 1 V swing); decodeUart (8N1 at bit centres from each start edge, stop-bit check, low >= 11 bit times = break / init, bit time measured by least squares over every edge inside every byte); analyseLin (frame = break to next break: sync 0x55 (bit time from its 5 falling edges, as LIN does), PID parity, response = rest, last byte checksum, classic and enhanced both computed (0x3C / 0x3D always classic), otherTypeMatches when it fits the other type; no response; framing; break length; the last frame only once the data has gone quiet after it); analyseKLine (messages split at idle >= gapBits or a break; KWP2000: format byte, addresses, length (byte), sum-mod-256 checksum, length shortfall; max inter-byte gap; init pulses); table() for ID / address rows. Tested on synthetic buses: LIN 40 frames, master clock +0.30 % measured +0.29 % from syncs, each fault (parity, checksum, other checksum type, no response, stop bit, short break) found once; K-Line KWP2000 at -0.70 % measured -0.71 %, checksum + stop-bit errors and a fast-init pulse found.

Decoder: for lin / kline, decode() runs decodeUart + analyseLin / analyseKLine on its own (trimmed) logic stream; frames / messages starting in the visible window go to DecodeResult::linFrames / kline (positions shifted back by `trimmed`), busBitTime = measured bit time. MainWindow: SerialHealthData per slot (filled in onDecodeFinished with the slot's baud and LIN version; cleared with the reason in showDecodeMessage); Automotive page "LIN / K-Line health" (not saved, like CAN's); updateSerialHealth() on m_engineTimer (keyed, like CAN) -> info panel 2: per LIN / K-Line slot, the wire on the slot's input channel (dominant / recessive ratio: LIN <= 40 %, K-Line <= 30 %, recessive >= 7 V; > 20 % noted), LIN bit rate from the syncs (flag > 1.5 %), K-Line baud (flag > 2 %), counts, load, error breakdown, other-checksum-type advice, short breaks, unanswered headers, KWP2000 inter-byte gap (> 20 ms flagged), init pulses, ID / "from -> to" table. Not compiled here.

## SENT (SAE J2716)

sent.h (Qt-free): falling edges; a sync = a period within +-25 % of 56 nominal ticks (if the NEXT period is sync-length too, the first was a pause); tick = sync / 56; status + N data + CRC nibbles at 12..27 ticks (value = round(ticks) - 12); a bad nibble makes an error frame and the search resumes from it; CRC-4 (x^4+x^3+x^2+1, seed 5, data nibbles only) both recommended (one extra zero-nibble step) and legacy; pause = the next period (12..768 ticks) when it isn't a sync; low pulse < 4 ticks; tick change > 1.5625 % vs the previous frame; fastChannel1 = D1..D3, fastChannel2 = D6..D4. Tested: computed CRC table == J2716's published lookup table; 60-frame streams (sensor clock -12 %, 2 MS/s), each fault once (CRC, legacy recognised as such, nibble, short low, 3 % tick jump), pauses to constant 300 ticks AND pauses of exactly 56 ticks (no false errors; such pauses aren't reported as pauses).

Decoder: native branch before the sigrok session (needs the sigrok build like the others): annotations sync (measured tick, drift, short low) / status (binary) / D1..Dn (FC1 / FC2 in D1 / D4 long texts) / CRC (red with expected) / pause; overlap filter on the window; sentFrames (starting in the window) for health; no start alignment (excluded from trimming). Decoding page: "SENT (SAE J2716)", Tick (3..90 us), Data nibbles (6..1); threshold default 2.5 V; saved with the slot. Rate warning below 4 samples per tick. Health panel (now "LIN / K-Line / SENT health"): wire high >= 4.1 V / low <= 0.5 V, measured tick vs nominal (20 % allowed), largest tick change (1.56 %), frames and period, errors (CRC not double-counted with bad nibbles), legacy CRC note, pauses, FC1 / FC2 ranges.

Fixed on the way: updateSerialHealth used the decode input's enum value (InCh1 = 10) as a channel number, so the LIN / K-Line wire check never ran; decodeInputChannel() maps inputs to channels.

## Zero calibration of every hardware range; SENT Slow and SPC

Calibration: the full sweep requested only the 1-2-5 page ranges, which landed on 8 of the scope's 15 hardware ranges; offsets were filed under the REQUESTED half and looked up as the nearest on a log scale (with an input offset this picked the wrong range: 2.9 V request -> +-2 V offset while the scope used +-4.08 V). Now: calibrationGridMv() = pages + geometric midpoints + limit (24 requests, ascending); each measurement is filed under the range really used (ADC readback: m_acq / m_stream adcWindow, half = max |window| in mV, at least the request); m_calCoverage per path / channel / coupling skips requests at or below the largest range already measured (m_calCoveredSkips, excluded from the "no data" count); plausibility checked against the real range; zeroOffsetFor() = the smallest stored range at or above the request (the scope's own rule), else the largest. Simulated on the user's 15 ranges: all 15 calibrated, 15 measured + 9 skipped per combination (120 steps, was 96); lookups match the scope's choice in every case tested (old rule wrong at 2.9 / 3.9 / 6 / 12 / 24 V). Old tables still work but should be redone (Full calibration).

SENT (PicoScope's Fast / Slow / SPC): DecodeJob::sentMode (0 / 1 / 2), the slot's "Show" combo (saved). Slow: Sent::slowChannel() — short (16 frames: bit 3 = 1 then 0 x15; bit 2 = 4-bit ID, 8-bit data, CRC-4 of the three nibbles, either variant) and enhanced (18 frames: bit 3 = 111111 0 C aaaa 0 bbbb 0; bit 2 = CRC-6 then 12 data bits; C=0 8-bit ID / 12-bit data, C=1 4-bit ID / 16-bit data). CRC-6 = x^6+x^4+x^3+1, seed 0x15, six zero bits, over frames 7..18 as (bit2, bit3) or (bit3, bit2) pairs — the order isn't confirmed, so a mismatch is crcVerified = false (shown "not verified", not an error). SPC: Frame::triggerTicks / triggerStart — merged with the sync (sync low >= 8 ticks) or a separate low pulse within 100 ticks before the sync that isn't the previous frame's end; SPC frames need the sensor's end pulse (its falling edge ends the CRC nibble), shown as "End". Trigger widths aren't mapped to sensor IDs (table not known reliably). Tested: 8 messages (4 short, 4 enhanced, both configs, both CRC orders, one corrupted of each: short -> error, enhanced -> not verified); SPC 10/10 frames, triggers 20 (merged) / 12 (separate) ticks; plain SENT no false triggers. Health: serial message counts / CRC / unverified / table (S|E + ID, count, every, data bytes MSB first); SPC trigger count and width range.

Fixed on the way: the SENT option combos (tick, nibbles; now Show) weren't in the decoding slot's re-decode connections, so changing them didn't re-decode.

## OBD-II / UDS on CAN (ISO-TP)

diag.h (Qt-free, namespace Diag): analyse(frames, idMode) — ISO-TP per CAN ID: single frame (CAN FD escape: length byte), first frame (12-bit length or 32-bit escape), consecutive frames (sequence numbers checked; out of sequence = error, message marked incomplete), flow control (status, block size, STmin incl. 100 us steps); one Note per CAN frame (text / brief / tiny) so annotations never overlap; a new first frame on an ID with one open finishes the old as incomplete; open at the end: "(rest not in view)". diagnosticId(): 0 = 7DF, 7E0-7EF, 29-bit 18DA / 18DB; 1 = also 600-7FF; 2 = every ID (bad single frames skipped silently). describe(): negative responses (NRC names; 0x78 = pending, not an error); OBD-II modes 01-0A — mode 01 / 02 with ~45 scaled PIDs (multi-PID responses walked by known lengths), supported-PID bitmaps, monitor status (MIL, DTC count), O2 sensors; modes 03 / 07 / 0A DTCs (count byte + pairs) with descriptions (common generic J2012 codes + patterned misfire / injector / coil codes; otherwise generic / manufacturer-specific + area); mode 09 VIN / calibration ID / ECU name; UDS services by name with session / reset / DID (named F18x / F19x, ASCII values, VIN) / security level / routine / DTC group / 0x19 sub-functions; 0x59 DTC records with status (confirmed / pending / MIL). answers(): pairing for response times (7DF -> 7E8-7EF; 7E0+n -> 7E8+n; 18DA tt ss <-> 18DA ss tt). Tested on realistic traffic: RPM 813, coolant 83 C + speed 50 km/h (multi-PID), VIN over FF + FC + 2 CFs (11-bit OBD and 29-bit UDS F190), stored DTCs P0301 / P0420, UDS DTCs with status, negative (red) vs pending (not), sequence error, 4 / 4 requests paired.

Integration: Decoding protocol "OBD-II / UDS (on CAN)" ("obd") = CAN (same options, inputs, rate warning) + DecodeJob::diagnostics / diagIds ("Diagnostic IDs" combo, saved, re-decodes); decode() runs Diag::analyse on result.canFrames (window, final positions) and replaces the annotations; result.diagMessages. The CAN health panel stays fed by "can" slots only. Automotive page "OBD-II / UDS summary" -> info panel 3 (updateDiagSummary on m_engineTimer, keyed on m_diagRev): messages, requests answered, response time avg / max, negative responses (last), pending count, VIN, fault codes per ECU with descriptions, latest live values per ECU. Not compiled here.

## Hugeicons instead of Font Awesome

resources/icons/*.svg: 31 Hugeicons (Stroke Rounded, 24x24, stroke=currentColor; Add / Close / ZoomLevelBackward / ZoomLevelForward stroke 3, the rest 1.5), added to the existing :/icons qresource (next to arrow-up/down.png). icons.cpp: svgFiles() maps the Font Awesome names callers already use to the files (call sites unchanged), plus zoom-back / zoom-forward for the plot's zoom-history buttons (were text arrows); renderSvg() replaces currentColor with the requested colour (alpha via painter opacity), renders with QSvgRenderer at 2x with the device-pixel ratio set, cached per name / colour / size; Icons::icon() prefers the SVG and falls back to the glyph; new Icons::pixmap() and Icons::hasSvg(). ScopeView: zoom-history buttons and the annotation move / delete buttons draw the SVGs (fallback: the old text / glyph). CMake: Qt6 Svg optional like Multimedia (VSX_HAVE_SVG; warning naming qt6-svg / mingw-w64-ucrt-x86_64-qt6-svg); Windows script installs $P-qt6-svg. Checked here (no Qt): all 31 parse as XML, use only svg / path / circle and M L H V C Z (SVG Tiny 1.2), every mapped name is in app.qrc, qrc parses; preview drawn from the path data in both themes (icons-preview.png).

Build fix (user's Windows build): mainwindow.h used Diag::Message (DiagData) without including diag.h (it came in only via decoderengine.h in .cpp files) -> 'Diag was not declared' in every file including mainwindow.h; the other errors cascaded from it. Included. Checked every project namespace used in the main headers is reachable through their own includes (Theme / MathOps flags were comment-only mentions).

## LIN: decoded at the wrong rate (user screenshot); Auto rate

User's LIN bus runs ~9600 bit/s (sync byte period ~200 us in the screenshot; a 0.95 ms low = a 0x00 data byte at 9600) while the LIN rate defaulted to 19200: red "Sync", a garbled PID, and 0x00 bytes taken for breaks. Now the LIN rate list starts with "Auto (from the sync byte)" (data 0, the default) plus 2400 / 4800 / 9600 / 10417 / 19200 / 20000. SerialBus::linBitTimeFromSync(): after each low run, the next five falling edges (sync 0x55, two bits apart); kept if the four gaps are within 15 % of their mean and the low run was >= 11 such bits; median over frames; linStandardBaud() snaps to a standard rate within 2 %. decode(): a local `baud` (measured for LIN Auto, else job.baud; none found: 19200) replaces job.baud in trimming, the sigrok uart option and the SerialBus analysis; DecodeResult::detectedBaud. MainWindow: Auto -> margins sized as 9600; health / rate warning use the measured rate; row name "LIN 9600" or "LIN ?" with a status message when no sync byte is in view. Tested at 1 MS/s with 8 us slow rising edges and 0x00 bytes: 9600 / 10417 / 19200 within 0.08 %; a +1 % bus still -> 9600; no sync in view -> 0. Saved slot settings with an explicit rate keep it.

## Colours, card step icons, decoded blocks lagging the trace

Dark theme: plotBackground 0c0c0c (was 06080b), windowBackground 191919 (was 15191e), cardBackground 282828 (was 1a1f26); panels / surfaces / elevated keep their blue-grey tint (offered to neutralise). Card step buttons: Hugeicons PlusSign / MinusSign (resources/icons, keys card-plus / card-minus), 12 px in Theme::onAccent(), CardButton::updateStepIcons() (from restyle() and setSteppersInteractive(); text glyphs if no SVG) — the text minus sat below centre.

Lag: decoding ran on a 250 ms poll, and results were placed with the page mapping of when the job started, so while dragging a recording the blocks stayed at the old page positions until a later decode (Capture: a new frame showed the old frame's blocks for up to 250 ms + decode). Now: results are kept as BusBlocks in their own positions (Record: recording samples = m_decodeOrigin (the job's read start) + job position; elsewhere buffer positions) in m_busRows; projectBusRow() places them on the current page (Record: (pos - m_recordViewStart) x pageLength / m_recordViewLength) — called on every result and at the end of refreshRecordView(), so blocks follow the trace immediately while dragging; onDecodeTick() is kicked at once (0 ms timer) after a page refresh, after each Capture frame and after every result (the 250 ms timer remains a fallback). Hidden rows / message rows invalidate m_busRows.

Cards: no visible border — unselected cards draw their 1 px border in their own background colour (no shift on selection); the selected card keeps its accent border. Theme::cardBorder() now unused by cards.

## Compact-card signs, light-mode rim, grid, panel and title bar

- Compact cards (Logic / DIO): setStepperGlyphs(true) shows their signs while not interactive, but last turn's updateStepIcons() cleared any non-interactive card's signs on every restyle — regression. Now m_glyphsShown (set by setStepperGlyphs) is honoured: icons on cards that step or that show their signs.
- Light-mode "border": the 1 px border was painted in the card's own colour and Qt paints the background under the border, leaving a white rim round the side bars on the grey window. Now border transparent + background-clip: padding (no rim, no shift on selection; selected keeps the accent border).
- Theme::onAccentBar(): ink on accent-filled bars (step icons, the panel title bar): onAccent (dark) in dark mode, white in light mode (white on #62d900 is ~2:1 — the user's choice; a deeper light-mode bar would read better).
- Grid stronger: minor 3a3a3a / c3cad1, major 4b4b4b / a9b3bd, axis 626262 / 77828d (dark / light).
- panelBackground light -> white (only the sliding panel uses it).
- Card hover / selected (dark) were still blue-grey and darker than the new 282828 base: now 2e2e2e / 333333.
- Sliding panel title: QFrame#panelTitleBar filled with the accent, 6 px radius, title centred (a 26 px spacer balances the close button), close icon and text in onAccentBar(); restyled through m_themedRefreshers (refreshPanelClose).

Panel title bar: flush with the panel's top and sides (panel layout margins 0, 0, 0, 10; the stacked pages sit in a body layout inset 10 px left), top corners cornerRadius() - 1 (nested in the 1 px border), bottom corners square — the cards' side bars turned to the top. Sidebar grid buttons: the checked text was accent (#62d900) on its own pale tint in light mode (~1.7:1); now accentSoft there (as the icon), accent in dark (gridButtonStyle(), re-applied by the theme refreshers).

Sliding panel: panelHostStyle() (one function, used at creation and on theme change): background = cardBackground(); its QPushButtons / QComboBoxes rest on windowBackground() — a parent sheet overrides the application's regardless of specificity, so hover (232323 / dbe0e5), pressed (141414 / d2d8de), checked (accent) and disabled (elevatedBackground) backgrounds are restated; widgets with their own sheets keep them. Title bar margins 2 px all round (close button 2 px further right, title still centred).

## PicoScope-style panel controls (panelcontrols.h/.cpp) and smaller items

PanelControls::adopt(m_panel, this) runs once after the pages are built (no page code changed; controls keep their signals): exclusive QButtonGroups of QPushButtons in one horizontal layout -> property segment=left/middle/right + spacing 0 (joined, accent-outlined); in a QGridLayout -> flatChoice (transparent, selected filled). QCheckBoxes: >= 2 in one grid -> toggle buttons (property toggle, indicator 0 px); single -> label + Off | On QPushButtons (segment left/right, exclusive group) synced both ways with the box, which stays in the layout at 0x0 as the setting. QComboBoxes and Q(Double)SpinBoxes -> QFrame#stepField compact-card fields (stepMinus / stepPlus bars with the card-minus / card-plus icons, 65 % of the row via stretch 65:35); bars step combos to the previous / next enabled item (no code listens to activated) and spins via stepBy (auto-repeat). Mirror keeps each stand-in's visibility / enabled state in step with the original (explicit flags: isHidden, WA_ForceDisabled). Clicking a spin (or its line edit) opens NumericPad: digits, +/-, point, backspace, unit buttons from the suffix (V: kV/V/mV, s: s/ms/us, Hz: MHz/kHz/Hz, S/s, A, ohm; unknown: the suffix or OK) that commit like Enter, the field's own unit highlighted; a vertical slider over min..max (live); keyboard: digits . , - Backspace Enter (own unit) Esc; a translucent popup with an arrow, above the field if it fits else below. PanelControls::restyle() refreshes the bar icons on theme change. Styles in panelHostStyle() (placeholders %1..%11 verified by simulating Qt's lowest-placeholder filling; %2 kept at 0 px — dropping it would shift every later colour).

Also: panel border removed (title bar radius = cornerRadius()); trigger card bottom-right detail (CardButton::setDetail, hidden in compact) = pre-trigger % in Capture, refreshed when it changes; channel page ADC line and Settings' Range report hidden (code kept); time-axis labels Theme::plotTimeText() (white / dark text). Not compiled here.

Build fix (Linux): theme.h used QApplication::font() without <QFont> (other files included it first; panelcontrols.cpp didn't) — included in theme.h.

Panel controls round 2: every panel QPushButton fixed at 26 px (kButtonHeight; icons 14 px; padding 0 12 px) incl. Off | On, segments, grid choices, toggle check boxes (padding 0 8 px); compact fields 24 px (kFieldHeight, was 30; inner combo / spin min-height 0, padding 0 6 px) with their font at 90 %. Selected segments: background accentSoft, text onAccentBar (panel style %12 / %13; placeholders re-verified); disabled buttons get a panelBorder border. PanelControls::segmentCombo(): combos with property asSegments (channel coupling) become joined segments (one per item, synced both ways, disabled items greyed; the combo stays 0x0 as the setting). Measure source: the scope's channels (m_channelCount, 2 if unknown) + Math always shown, greyed (disabled) when off, others hidden; the current-channel check uses isEnabled / isHidden (isVisible was false whenever the page wasn't shown).

Panel controls round 3: fields one width everywhere (kFieldWidth 150 px; combos AdjustToMinimumContentsLengthWithIcon, min 4 chars), radius 5 px (field and bars); panel buttons, toggle check boxes and section titles (incl. the Off | On labels, now sectionLabel-styled) at 90 % font; selected segments back to the main accent (onAccent text); disabled controls neutral grey (303030 / e2e2e2, border 3a3a3a / d0d0d0; was elevatedBackground, read as dark blue). Uneven gaps (Math, Remote): wrappers grew vertically on pages whose layout had no trailing stretch — wrappers now QSizePolicy::Fixed vertically and every page / scroll-area column without a trailing spacer gets one. Decoding page dropdown texts lose their "(...)" notes before adopt() (item data drives the code; findText is only used on probe combos). Remote: status hidden while off (shown for errors / waiting / phones), explanation removed from the page (now the QR code's tooltip). Bracket sweep note: the regex string in the Decoding pass has one more ')' than '(' — raw counts differ by exactly that; outside strings the file matches the previous compiled version.

Automotive page redesign + panel text + dropdown lists: groupHeading() (bold, text colour, rule above all but the first; recoloured in applyTheme) for Engine / Compression test / Battery and charging / Parasitic drain / Bus health; the CAN / LIN / Diagnostics headings merged into "Bus health" with Off | On titled CAN, LIN / K-Line / SENT, OBD-II / UDS. Titles above every field: Cylinders, Firing order (QLineEdit, field-sized 150x24 and field-styled in the panel), Cycle (4-stroke / 2-stroke, 720/360 in the tooltip), Overlay style (Timing chart / Cylinder bands), Battery system (12 V / 24 V), the five limits (no "Limits" title), Clamp on / Duration / Battery capacity / Acceptable drain; the drain how-to is the Start button's tooltip. One panel text size: adopt() scales explicit-font widgets and then the panel to 90 % (per-widget shrinking removed — the global sheet's QComboBox font-size: 110 % had overridden it, so dropdowns were 20 % bigger than fields); panel sheet sets combos' font-size (%14). Dropdown lists app-wide: Theme::dropdownListStyle() appended to the global sheet (card colour, neutral edge, 6 px radius, padding, rounded item highlights) and each combo's popup window frameless + translucent in applyTheme() (vsxRounded) so the corners show.

Segments: end caps by position in the whole group left the last shown button (CH2 in XY / Measure, CH3 / CH4 hidden) styled as a middle segment, square on the right. SegmentRoles assigns left / middle / right among the shown buttons and redoes it on show / hide (a lone button: no role, fully rounded). Dropdown text: each panel combo gets its own sheet with font-size = the panel's scaled font (the global sheet's 110 % beat code fonts; the panel-sheet value was computed from QApplication::font()), so combos match number fields exactly. Relative compression: the channel and signal dropdowns were side by side (two 150 px fields overflowed) — now under titles Channel / Signal.

Panel text size, for real: setFont on the panel / per widget doesn't hold — the global sheet's "* { font-family }" makes Qt rebuild each widget's font from the application's (style-sheet font resolution), so number fields stayed at 100 % while combos took sheet sizes (110 % global; 90 % from last turn's per-combo sheet). Now adopt() puts "* { font-size: 0.9 x app pt }" on the pages' root (m_panel): one mechanism for every widget, beats the global and panelHost combo rules; bold kept. shrinkFont() and the per-combo sheets removed.

Channel Auto -> Manual on the user's own range change: leaveAutoRange(ch) clicks Manual when the channel is in Auto; called from stepVoltsPerDiv() (card +/-, phone remote) and the volts-per-page combo's activated signal (user picks only; the field's +/- bars now emit activated after stepping). autoRange()'s setCurrentIndex and settings restore don't emit activated, so they never flip the mode. The volts field is enabled in Auto too (enabled unless Off — mode handler, device change, settings restore).

- Grid check boxes (Measure) are now real toggle buttons (wrapToggle: checkable QPushButton, centred text, no indicator; the box stays 0x0 as the setting) — the styled check box couldn't centre text and drew its indicator as a dot.
- Mirror: visibility to the wrapper, enabled state to the sibling controls (bars, Off | On + label, toggle button; segmentCombo folds it into show()) — never to the wrapper: a control inside a disabled parent gets no EnabledChange when re-enabled, so a channel turned off and on kept its volts field greyed until something else refreshed it.
- Exclusive groups in a row span the full width (row stretch removed, buttons Expanding): Acquisition, Measure source, XY channels; Keep already did. Off | On and DC | AC stay compact.
- Record tabs and the panel title bar: one height (kTabStripHeight 30: tabs padding 0, height 29 + 1 px border; title bar fixed 30) and NoTabShiftStyle (QProxyStyle, PM_TabBarTabShift* = 0) on m_recordTabs, so unselected tabs no longer sit 2 px lower.

- Record tabs: unselected tabs in the cards' neutral greys (card / hover backgrounds, 3a3a3a / d0d0d0 borders, 303030 / e2e2e2 disabled) — surfaceBackground etc. read as dark blue.
- Voltage ruler flags span their own gutter (1 px insets): CH1 the scale gutter, Math its column, CH2 the right gutter (was the template width capped at the narrower of CH1 / Math).
- Measure grid: check boxes classified (grid toggles vs single Off | On) before any is converted — converting in one pass emptied the grid, so -Width became an Off | On pair; toggle grids get equal column stretches.
- Status bar: Live no longer shows the latency / dropped counters (debugging); rate, Record progress, the disk-too-slow warning and free space stay.
- OPEN: crash recording CH1 + CH2 at 62.5 MS/s. Read through: SDK callback -> ingest (bounds-checked) -> RecordWriter::append (own buffer, queue capped at 64 blocks ~= 320 MB at 2.5 M-sample blocks) -> writer thread: no fault found. 2 x 62.5 MS/s x 2 B = 250 MB/s is far beyond USB 2.0; the SDK documents no per-channel stream limit. Waiting on a backtrace (gdb) before changing anything.

## Record crash at 62.5 MS/s x 2 channels: the vendor driver
User's gdb backtrace: "pure virtual method called" -> terminate in a thread created as recording starts; only libc / libstdc++ frames (std::thread start -> __cxa_pure_virtual), none of ours. The app's own threads: std::async / std::thread at startup only, the record writer and workers are QThreads (Qt frames). So the thread is libvmmultmso's streaming thread — a driver bug at this rate / channel count (USB 3.0 scope, so not a bandwidth limit). Guard: Record with >= 2 channels caps the rate at 31.25 MS/s (status message); one channel keeps 62.5 MS/s; Live unchanged (not known to crash). 31.25 MS/s x 2 to be confirmed by the user.

Panel stand-ins vs blocked signals: the app updates settings with signals blocked in 37 places (Measure loads a channel's choices into the check boxes that way), so toggles / Off | On / segmented combos following signals went stale (Measure's buttons all showed off while the plot showed them). PanelControls keeps a list of sync functions run every 120 ms while the panel is visible (startSyncing in adopt): toggles, Off | On pairs and segmented combos are corrected from their settings.

Rulers: kMathColumnWidth = kScaleGutter (42 px; was 35), so CH1's and Math's flags (each spanning its gutter) match; voltage-ruler drags clamp y to the plot; time flags' x and voltage flags' y are clamped so the whole flag stays within the plot (centred on the line otherwise).

- Math scale: mathScaleUnit() applies the channels' rule (mV while every tick label < 1 V, from mathRange(plotRect())); drawMathScale() uses it for the unit heading and the tick labels (factor applied), so the scale, ruler flags and readouts agree.
- Compact cards: CardButton::layoutCompact() — with a note, the title on the left and the note on the right (both spanning the card); without, the title centred as before (Logic, DIO). Math card's note = its volts per page ("Auto" when automatic).
- Ruler drags stop where the flags stop: RulerLayout carries flagHeight / timeFlagWidth; voltage drags clamp y to [top + h/2, bottom - h/2], time drags x to [left + w/2, right - w/2] (the line used to reach the edge while its flag stopped half a flag earlier).

## Own dialogs (vsxdialog.h/.cpp)
VsxDialog: frameless (Qt::Dialog | FramelessWindowHint, translucent), painted as a rounded card (cardBackground, neutral edge); accent title bar (30 px, title centred, close X in onAccentBar, drag the bar to move; follows setWindowTitle via changeEvent); contentLayout() for content; addButton(text, primary, fn) bottom right (primary = accent-filled + default). Dialog sheet: buttons / fields / lists / tables / header in the panel's look. VsxDialogs::question / warning replace all 7 QMessageBox calls (if (!question(...)) return;), VsxDialogs::pickColour (32 swatches + hex, invalid on cancel) replaces the 3 QColorDialog calls; ProbeWizard / ProbeManagerDialog derive from VsxDialog (their own green header bar removed: it sat under the native title, the doubled title); the range report uses it too. File open / save stay native (desktop places, recent files, search). Not compiled here.

Colour picker: "Custom colour..." opens the system QColorDialog (visual wheel / field) from the current choice; its result feeds the swatches / preview / hex. Probe editor's unit presets (presetStyle): windowBackground + accent edge, neutral hover, checked = accent fill with onAccent (was surface / selectedTint, read as dark blue), radius 5.

Keypad popup: edge and entry box in the accent (were Theme::border()); slider styled: 6 px rounded groove in windowBackground, add-page (below the handle: min..value) in the accent, round accent handle (12 px + 3 px cardBackground ring, radius 9), lighter on hover / press.

Keypad: unit buttons now select the entry's unit (highlighted; the display shows the entry, or the current value, in it) instead of confirming and closing (with nothing typed the pad just vanished); a check key (accent) and Enter confirm in the selected unit. Units: any letter suffix not otherwise known (a probe's bar, psi...) gets k / base / m; %, degrees, none keep one.
Probe units on the channel page: MainWindow::applyProbeUnits(ch) — "Volts per page" title -> "<unit> per page", its items "±x <unit>" (formatVolts), Input / DC offset suffixes -> the unit (the values were already probe units: ranges scaled by the gradient, centred on the probe offset). Called from the probe combo handler, reloadProbeList() (startup, after Manage probes) and the settings restore that sets the probe with signals blocked. Trigger level already followed the probe.

Cards: m_value spans grid columns 2-3 again with m_detail overlaid at (1,3) — confining the value to column 2 (for the pre-trigger detail) clipped it and squeezed the top note.

References: drawn solid (was dashed); the References page colour button is "Trace colour" with the eye/Color icon like the channels' and Math's.

- Relay clicks at Live / Record start and stop: reapplyCoupling() / reapplyBlockCoupling() sent the coupling to every channel at every start / stop (each call drives the relay); the vendor app is silent and its documented stream sequence sends none. Now they read GetStreamAcDc / GetAcDc and send only where it differs (still corrected if the mode switch did reset it). To confirm on hardware: no clicks with unchanged coupling; coupling still right after start / stop.
- References carry a unit: from the source channel's (probe) unit when taken; reference files v2 store it (v1 = V; newer versions refused with a message); drawReferenceScale uses it, milli- while every label < 1 (was a hard-coded "V").

## Relay clicks, take 2: no mode switch per run
The conditional coupling didn't stop the clicks. Cause: every Live / Record start did SetOscCaptureMode(1) and every stop SetOscCaptureMode(0) + block coupling — each mode switch resets the front end (coupling, ranges -> defaults), then our settings moved the relays back: clicks at start and stop. The vendor app stays in stream mode. Now StreamEngine::m_inStreamMode: start switches (and re-sends coupling + ranges) only when not already in stream mode; stop only StreamStopCapture (stays in stream mode); ranges changed while stopped go out at once (setChannelRange). AcquisitionManager::setBeforeBlockCapture(hook) — MainWindow hooks StreamEngine::leaveStreamMode(); start() / single() call it and, if the scope switched back, reapplyBlockFrontEnd() re-sends block coupling, the requested ranges (m_adcWindow requested) and the trigger settings. attach / detach reset the flag; a failed StreamCapture still returns to block mode. To confirm on hardware: no clicks on Live / Record start / stop; Capture after Live / Record still at the right range, coupling and trigger.

REVERTED (user's call, clicks unchanged): the stay-in-stream-mode change (m_inStreamMode, leaveStreamMode, AcquisitionManager's before-block-capture hook / reapplyBlockFrontEnd) and the conditional coupling — back to the tested path: SetOscCaptureMode(1) + coupling + ranges at every stream start, StreamStopCapture + SetOscCaptureMode(0) + block coupling at every stop. The relay clicks persisted with no mode switch and no coupling commands at start / stop, on every channel combination: they come from the SDK's StreamCapture / StreamStopCapture themselves. Left as is (harmless). Also removed: the "CHn streaming: range ... streaming limits ..." status-bar message (rangeReport handler; diagnostics).

## Charging test ripple verdict
User's car (idle) flagged "Uneven ripple" with healthy-looking humps: pairs of dips every 37.6 ms = 26.6 Hz = a 4-cylinder's firing rate at ~800 rpm (ignition / injector load). The verdict used plain CVs (std/mean) of hump heights / gaps (> 0.30 / 0.15), which outliers dominate; the every-3rd/6th pattern check wasn't in the verdict and dips shifted its modulo count. Simulated (1 MS/s, 1482 humps/s, +-10 % imbalance, noise): old rule — healthy + 120 mV dips UNEVEN (0.50), open diode alone EVEN (0.29), weak phase alone EVEN (0.27). Now batterytools.h: humpSpreadRobust / timingSpreadRobust (1.4826 MAD / median) and the pattern check per run of regular humps (split at gap outliers > 25 %), length-weighted; verdict = pattern (every 3rd / 6th, depth > 0.25) or robust spreads > 0.30 / 0.15. All six cases right: healthy (with / without dips) even (robust 0.08-0.09); open diode (pattern 0.62-0.64) and weak phase (0.34-0.37) uneven with or without dips. Result lines shortened (the panel cuts long lines): "Every 6th hump 64 % off: check diodes / stator", "Uneven ripple: check diodes / stator", "Ripple even: diodes look OK".

- Zoom-history buttons vanished with cursors on: drawRulers() ends with the clip on the plot (for annotations / readouts after it); the buttons, drawn last above the plot, were clipped. drawZoomHistoryButtons() now setClipping(false) first.
- Time cursors: ScopeView::formatAxisTime() — the axis's unit (us / ms / s from the window, as drawAxes()) with decimals for 1/100 division (5 ms/div: "10971.23 ms"; was formatEng "10.97 s" across a 50 ms page); used for both flags and the Rulers panel (delta keeps formatEng). Flag width = the widest the view's edge times can be (digits as zeros), so it changes with the view, not while dragging.

## Save a recording as .vsxrec (Record mode)
Save in Record mode offers "VimuScopeX recording (*.vsxrec)" (and PNG). MainWindow::saveRecordingAs(): the current record tab's folder copied to <chosen folder>/<name>/ (" (2)"... on collision) — the data file renamed <name>.vsxrec, meta.ini and any other files kept; refused while that recording is still being written or when none is open; copied on a detached std::thread in 8 MB chunks with percent progress / result in the status bar (QPointer guard). Works for Temporary recordings (kept before their cleanup) and Permanent ones. Reading: RecordWriter::openForReading uses dataFilePathForReading() — recording.vsxrec, else the folder's single .vsxrec — so Open -> <name>.vsxrec loads the copy. Writing new recordings still uses recording.vsxrec.

Rulers panel times: formatPanelTime() — seconds with decimals for 1/5 division from 1 s up (5 ms/div: "10.971 s"; 500 ms/div: "11.0 s"), formatAxisTime() below 1 s; flags keep the axis unit (RulerLayout t1PanelText / t2PanelText).

Status bar: statusBarStyle() (set at build and in the theme refresh) — windowBackground, no border (was recessedBackground + borderSoft rule, blue-grey), 28 px, 10 px side padding, 90 % font; messages in text colour, the readout label muted; size grip off. Readout as rich text: parts HTML-escaped, joined by accent bullets (was "  |  "), the "blocks lost, disk too slow" part in #ff5a4f.

## Loaded .vsxwave = a view, not a setup change
openNativeWaveform() restored the file's full snapshot (channel modes, pages, coupling, probes, invert, positions, colours, trigger, Math, filters) into the shared live settings, so it leaked into the other modes and into the saved session. Now: saveSettings / restoreSettings refactored into saveSettingsTo(QSettings&) / restoreSettingsFrom(QSettings&, bool session) (session = geometry + annotation sets, startup only). Opening a file first takes a snapshot (takeFileViewSnapshot(): an INI in the temp dir; once — a second file keeps the original) and applies with m_applyingFileView set (scope guard; its own mode switch isn't "leaving"). leaveFileView() — at the start of setAcquisitionMode(), startAcquisition() (Run) and the Single button — restores the snapshot (session = false), removes it, re-applies channels / timebase / trigger / Math and refreshes the cards. While a file is viewed, saveSettings() writes the snapshot's values (the user's setup), not the screen's. File colours are read but never applied (the user's stay).

Record overview strip: refreshRecordView() cleared the main view's unrecorded / switched-off channels but not the navigator's, so a channel another mode had shown (Capture's CH2) stayed in the strip over a recording without it (via an empty Live it was cleared by Live's branch). Now m_nav->setSamples(ch, {}) there too.

## 1.0.0 packaging (packaging/README.md)
Version 1.0.0 (CMakeLists project). Windows: build-windows.sh step 6/6 builds an NSIS installer (packaging/windows/installer.nsi; $P-nsis installed in step 1) -> dist/VimuScopeX-<version>-setup.exe (Program Files, Start menu + desktop shortcuts, uninstaller, Apps & features entry incl. size; user data kept); folder + zip as before. Linux: packaging/linux/build-appimage.sh <SDK> -> dist/VimuScopeX-<version>-x86_64.AppImage via linuxdeploy + plugin-qt (downloaded once to ~/.cache/vsx-appimage-tools, APPIMAGE_EXTRACT_AND_RUN, svg / multimedia / tls / wayland plugins), SDK .so files bundled, sigrok's decoders + ours in usr/bin/decoders, the Python stdlib in usr/lib/pythonX.Y with a custom AppRun setting PYTHONHOME. Arch: packaging/arch/PKGBUILD (VIMU_SDK=... makepkg -si) -> /opt/vimuscopex (rpath $ORIGIN via patchelf), /usr/bin/vimuscopex, desktop entry, hicolor icons, udev rule. udev: packaging/linux/60-vimuscope.rules (d4a2:5663, uaccess) + install-udev-rule.sh. None of the packaging could be run here (no Qt / network): scripts syntax-checked only.
