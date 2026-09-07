# Configuration

Every key in `%LOCALAPPDATA%\FrameCapture\config.toml`, with its type, range,
default, effect, and the schema version that introduced it (SPEC.md §17, §22).

`test_config.ConfigSchema.IsFullyDocumented` fails the build if a key in the schema
table is missing from this file.

---

## Location

```
%LOCALAPPDATA%\FrameCapture\config.toml
```

Never under Program Files (SPEC.md §17) — a per-user location needs no elevation and
survives an in-place upgrade. A missing file is **not** an error: the first launch
runs on defaults and does not write anything until you change a setting. Loading
never has a side effect on disk.

Related files in the same directory:

| File | Written when |
| --- | --- |
| `config.toml.tmp` | During a save. Its presence afterwards means a save was interrupted between flush and rename; it holds the content that was being written. |
| `config.toml.bak.<version>` | Before a schema migration rewrites the file. Never overwritten — a retry writes `.bak.<version>.2` instead, because the first backup may be the only copy of your original. |

## How values are validated

Validation is schema-driven: every range in the table below is read from the same
table the loader uses, so this document cannot describe a bound the code does not
enforce.

Nothing here can crash the engine or stop a recording. Each outcome raises a warning
that the GUI surfaces:

| Situation | What happens | Warning |
| --- | --- | --- |
| Value has the wrong TOML type | Default is used | `wrong_type` |
| Number outside a continuous range | **Clamped** to the nearest bound | `clamped` |
| Value not in a discrete set (`fps`, enums) | Default is used | `unknown_value` |
| Key absent | Default is used | `missing` |
| Key not in this document | **Kept verbatim** | `unknown_key_preserved` |
| `schema_version` newer than this build | File loads, is not rewritten | `schema_too_new` |

A continuous range clamps rather than resetting because asking for a bitrate above
the ceiling means "as high as possible", and silently resetting to the default would
be a surprise. A discrete set has no meaningful nearest value — `fps = 45` could
round either way, and recording at a rate you did not choose is worse than falling
back — so it defaults instead.

> **Note on SPEC.md §17.** The spec says an invalid value "falls back to the
> default". This implementation clamps continuous numeric ranges and defaults
> everything else. Both satisfy the requirement that follows in the same sentence —
> never crash, never silently produce a bad recording — and clamping is the less
> surprising of the two. Flagged here because it is a deliberate narrowing of the
> spec's wording, not an oversight.

**A file that cannot be parsed is an error, not a reset.** The engine reports
`IO_CONFIG_PARSE_FAILED` (6021) and leaves the file alone, because a syntax error is
something you can still fix by hand — overwriting it with defaults would destroy
settings that are merely one typo away from working.

## Unknown keys are preserved

Any key not listed here is kept exactly where it is, in its original section. This is
what makes a downgrade safe: install v2, get a v2-only setting, downgrade to v1, and
v1 will load, ignore, and faithfully rewrite that setting. It survives repeated
load/save cycles, not just one.

## Saves are atomic

`config.toml.tmp` → `FlushFileBuffers` → `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`.
The flush is not optional: without it the rename can become durable before the bytes
do, which is how a power cut yields a zero-length config. The destination is always
either the old file or the new one, never a mixture.

---

## `schema_version`

| Key | Type | Range | Default | Since |
| --- | --- | --- | --- | --- |
| `schema_version` | integer | 1–1000 | `2` | 1 |

Identifies the on-disk shape. On load, a **lower** value runs the ordered migration
chain (`migrate_1_to_2`, `migrate_2_to_3`, …) after backing the file up; a **higher**
value loads read-only and is never rewritten. A missing link is a hard error rather
than a skipped version, so the file can never end up at an intermediate shape while
claiming the target version.

### Migration history

| Step | Change |
| --- | --- |
| `migrate_1_to_2` | Adds `[hotkeys]`, `[overlay]` and `[window]` (M9.6). |

`migrate_1_to_2` **writes nothing.** All three sections are new and every key in them has a
default, and an absent key already loads as its default — so a v1 file read by this
build produces exactly the values the step would have inserted. What the step
contributes is the version stamp and, through `run_migrations`, the backup to
`config.toml.bak.1` that SPEC.md §17 promises before a file is rewritten. A v1 file is
rewritten the moment any setting is saved, so that backup is not hypothetical.

It exists as a step rather than the chain simply accepting a v1 document because
`run_migrations` refuses a version it has no step for — deliberately, since a silent
version bump is how a document reaches an intermediate shape with a header claiming
otherwise. A step that transforms nothing is still a link, and the chain's value is
that every link is present.

## `[general]`

| Key | Type | Range | Default | Since |
| --- | --- | --- | --- | --- |
| `output_directory` | path | any writable directory | `<Videos>\FrameCapture` | 1 |
| `filename_template` | string | `strftime` format | `FrameCapture_%Y-%m-%d_%H-%M-%S` | 1 |
| `language` | string | BCP-47 tag | `en` | 1 |

- **`output_directory`** — where recordings are written. Validated for writability
  before a recording starts, not on the first frame. Changing it mid-session requires
  explicit confirmation in the GUI (SPEC.md §16.5).
- **`filename_template`** — expanded at recording start. With segmentation on,
  `_part001` etc. is appended (SPEC.md §11).
- **`language`** — GUI language. Does not affect recording.

## `[video]`

| Key | Type | Range | Default | Since |
| --- | --- | --- | --- | --- |
| `width` | integer | `1920` | `1920` | 1 |
| `height` | integer | `1080` | `1080` | 1 |
| `fps` | integer | `30` \| `60` | `60` | 1 |
| `container` | enum | `mp4` \| `mkv` | `mkv` | 1 |
| `codec` | enum | `h264` \| `hevc` \| `av1` | `h264` | 1 |
| `encoder` | enum | `auto` \| `nvenc` \| `amf` \| `x264` | `auto` | 1 |
| `rate_control` | enum | `cqp` \| `vbr` \| `lossless` | `cqp` | 1 |
| `cqp` | integer | 0–51 | `20` | 1 |
| `bitrate_kbps` | integer | 1000–200000 | `20000` | 1 |
| `gop_seconds` | integer | 1–10 | `2` | 1 |
| `max_b_frames` | integer | 0–4 | `2` | 1 |
| `pacing` | enum | `cfr` \| `vfr` | `cfr` | 1 |
| `full_range` | boolean | — | `false` | 1 |

- **`width` / `height`** — locked to 1080p in v1 (SPEC.md §16.4). Present as keys so
  v1.1 can widen the allowed set without changing the file's shape.
- **`fps`** — a discrete choice, not a range. At 60 the video timebase is `1/60000`,
  at 30 it is `1/30000`, giving integer frame durations with no rounding drift over
  hours (SPEC.md §7.2).
- **`container`** — MKV is the default because its cluster structure means a
  truncated file still plays up to the last complete cluster (SPEC.md §10.2). MP4 is
  written fragmented during recording and losslessly remuxed to progressive on a
  clean stop (SPEC.md §10.3).
- **`codec`** — **only `h264` is validated in v1.** The enum defines all three from
  day one so that enabling HEVC or AV1 in v1.1 is a config change and a validation
  pass rather than a refactor (SPEC.md §2.2 item 4). Selecting `hevc` or `av1` is
  rejected with `ENCODE_CODEC_UNSUPPORTED` (4003).
- **`encoder`** — `auto` encodes where the pixels already live, which avoids ~500
  MB/s of pointless PCIe traffic (SPEC.md §5.2). Override only to diagnose.
  `x264` is a software fallback and is **not available in this build** — linking
  libx264 would make the distribution GPLv2 and that decision is still open.
- **`rate_control`** — CBR is deliberately absent: it wastes bits on static screens,
  which is most of a screen recording (SPEC.md §9).
- **`cqp`** — quality target when `rate_control = "cqp"`. Lower is better quality and
  a larger file. 20 is the default; the degradation ladder may raise it to 24 under
  sustained pressure (SPEC.md §13 rung 2).
- **`bitrate_kbps`** — ceiling when `rate_control = "vbr"`. Ignored for `cqp` and
  `lossless`.
- **`gop_seconds`** — keyframe interval. 2 s gives fast seeking and clean segment
  boundaries. Open-GOP is never used, because it breaks segment splitting
  (SPEC.md §9).
- **`max_b_frames`** — 0 removes reorder latency. Set to 0 if latency matters.
- **`pacing`** — `cfr` emits duplicate frames with deliberately quantized PTS, which
  is what makes a 12 fps source look like smooth low-fps video instead of a juddering
  mess. `vfr` uses exact QPC deltas at a `1/1000000` timebase and is **fragile in
  MP4**; the GUI recommends MKV when you select it (SPEC.md §7.2, §7.3).

  **`vfr` is not implemented in this build.** Selecting it makes recording fail with
  `INTERNAL_NOT_IMPLEMENTED` rather than silently recording CFR. The key is retained
  in the schema so that a config file written by a future build round-trips through
  this one without losing the setting.
- **`full_range`** — output is BT.709 limited range (16–235 luma) by default. Full
  range is a config option (SPEC.md §6); most players assume limited, so changing this
  is a good way to get output that looks wrong everywhere else.

## `[audio]`

| Key | Type | Range | Default | Since |
| --- | --- | --- | --- | --- |
| `device_id` | string | endpoint id, or empty | `""` (default endpoint) | 1 |
| `channel_layout` | enum | `auto` \| `stereo` \| `5.1` \| `7.1` | `auto` | 1 |
| `bitrate_kbps` | integer | 0–640 | `0` (auto per layout) | 1 |
| `multitrack_enabled` | boolean | — | `false` | 1 |
| `max_tracks` | integer | 1–6 | `6` | 1 |
| `multitrack_targets` | string | comma-separated executable names | `""` (no per-application tracks) | 1 |
| `multitrack_reattach` | boolean | — | `true` | 1 |

- **`device_id`** — empty follows the default render endpoint, including when the
  user changes it mid-recording (SPEC.md §14.1).
- **`channel_layout`** — `auto` takes the endpoint's native layout, up to 7.1. The
  channel count is **pinned for the lifetime of the stream**; if the endpoint changes
  mid-recording, libswresample up/downmixes into the pinned layout, because changing
  an AAC stream's channel count mid-file is invalid in both containers (SPEC.md §8.5).

  **An explicit setting overrides the device downward only (BUG-048).** Choosing fewer
  channels than the device supplies works and is what the setting is for. Choosing
  *more* falls back to the device's own layout, with a warning in the log naming both
  counts, because an up-mix cannot add information — and an eight-channel AAC track
  will not play in Windows' own player, whose decoder accepts 1, 2 and 6 channels. The
  settings dialog says so beside the control when the choice exceeds the device.
- **`bitrate_kbps`** — `0` derives it from the layout: 192 kbps stereo, 384 kbps 5.1,
  512 kbps 7.1 (SPEC.md §8.5). Set explicitly to override. A fixed low value with a
  7.1 layout will sound bad, which is why `0` is the default.
- **`multitrack_enabled`** — Tier B, per-application tracks. **MKV only**: with MP4
  selected the engine rejects the configuration with `MULTITRACK_REQUIRES_MKV` (3021)
  rather than warning, because multi-track MP4 is invisible in most players
  (SPEC.md §8.6, §20 row 16). Opt-in because each track carries its own device clock
  and therefore its own drift loop.
- **`max_tracks`** — includes track 0, which is always the full system mix so the
  file is useful in a player that exposes only the first track.
- **`multitrack_targets`** — which applications get their own track, as executable
  names: `chrome.exe, game.exe`. Empty means Tier B records nothing extra even with
  `multitrack_enabled = true`, which is the state a fresh install is in — the setting
  and the targets are two halves of one opt-in.

  **Names rather than process ids**, because SPEC.md §8.6 identifies a target by
  executable name and polls for it at 2 Hz: a name survives the application being
  restarted, and a pid written to a config file is meaningless by the next boot. A
  target that is not running when recording starts gets a track that is silent until
  it appears; a target that exits gets one that is silent to the end of the file
  (§20 row 15).

  **A string rather than a TOML array** because §17's schema table has no array type,
  and adding one for a key the GUI presents as a text field would be a schema change
  carried by a single caller. `config::parse_multitrack_targets` is the one place that
  decides what the string means — it trims, drops blanks, and drops duplicates, since
  two tracks aimed at one executable would resolve to the same process tree and carry
  identical audio on two of the six slots.

  Targets beyond `max_tracks - 1` are dropped with `MULTITRACK_TRACK_LIMIT_EXCEEDED`
  logged and the recording continuing, rather than refused: losing the sixth
  application is a smaller loss than losing the recording.

  The settings dialog offers a **picker** of applications running now, which appends to
  this field rather than replacing it — so a game that is not running yet can be typed
  and a browser that is can be clicked, and both end up in the same list.
- **`multitrack_reattach`** — whether a track follows its executable back when the
  application is closed and reopened mid-recording. **Default on, decided 2026-08-06.**

  SPEC.md §8.6 said a track whose target exits "continues as pure silence to the end of
  the file" and left open whether it keeps looking. It keeps looking, and it follows the
  **executable** — the same identity §8.6 already uses for a target that has not started
  yet — so a browser that crashes and is reopened lands back on its own track with the
  gap silence-filled.

  **The track's length is identical either way**: its silence generator never stops, so
  the file is never ragged under either setting. What this changes is only whether a user
  who restarts an application gets the rest of its audio or silence. `false` gives §8.6's
  literal reading.

  A track pinned to a **pid** rather than a name never re-attaches, whatever this says.
  There is nothing to resolve, and a different process wearing that number later is a
  different application.

## `[segmentation]`

| Key | Type | Range | Default | Since |
| --- | --- | --- | --- | --- |
| `enabled` | boolean | — | **`false`** | 1 |
| `split_by_duration` | boolean | — | `true` | 1 |
| `duration_minutes` | integer | 1–1440 | `30` | 1 |
| `split_by_size` | boolean | — | `false` | 1 |
| `size_mb` | integer | 100–102400 | `4096` | 1 |

- **`enabled`** — **off by default, always** (SPEC.md §11, CLAUDE.md §2 rule 7).
  Segmentation produces multiple files, which is surprising if you did not ask for it;
  the GUI requires explicit confirmation to turn it on. The other keys in this section
  have no effect while this is `false`.
- **`split_by_duration` / `split_by_size`** — both may be armed; whichever fires first
  wins.
- **`duration_minutes` / `size_mb`** — the thresholds. Splits are keyframe-aligned:
  an IDR is requested and awaited, so the next file starts at a keyframe. Splitting
  mid-GOP would make the next file's opening seconds unplayable garbage.

Each segment restarts timestamps at 0 and is independently valid. A sidecar
`<basename>.segments.json` records each segment's global start offset.

## `[advanced]`

| Key | Type | Range | Default | Since |
| --- | --- | --- | --- | --- |
| `capture_backend` | enum | `auto` \| `wgc` \| `dda` | `auto` | 1 |
| `capture_cursor` | boolean | — | `true` | 1 |
| `hdr_tonemap` | boolean | — | `true` | 1 |
| `log_level` | enum | `trace` \| `debug` \| `info` \| `warn` \| `error` \| `critical` | `info` | 1 |
| `gpu_override` | string | adapter LUID, or empty | `""` (automatic) | 1 |

- **`capture_backend`** — `auto` prefers WGC, which is the only API that reliably
  captures across hybrid-GPU boundaries and is tear-free because DWM composites it.
  Force `dda` only for diagnosis: DDA has an adapter-affinity constraint that is the
  single most common cause of all-black recordings (SPEC.md §5.1), and it can tear on
  fullscreen-exclusive targets (SPEC.md §7.4).
- **`capture_cursor`** — whether the pointer is drawn into the recording.
- **`hdr_tonemap`** — when the source surface is FP16 scRGB (system HDR on), tone-map
  to SDR. Disabling this does **not** make HDR output work; it makes the engine refuse
  the source instead of reinterpreting the bits, which would produce the black or
  neon-green frames described in SPEC.md §4.2.
- **`log_level`** — `trace` is per-frame and off by default; enabling it produces very
  large logs and is only for diagnosis (SPEC.md §18).
- **`gpu_override`** — pins capture and encode to one adapter. Empty uses the §5.2
  policy, which is almost always what you want. A wrong value here reproduces the
  adapter/output mismatch bug on purpose.

## `[updates]`

| Key | Type | Range | Default | Since |
| --- | --- | --- | --- | --- |
| `check_enabled` | boolean | — | `true` | 1 |

Checks a static signed JSON manifest over HTTPS from the **GUI** process. The engine
binds no sockets and contains no network code at all — FFmpeg is built with
`--disable-network` (see `ports/ffmpeg/framecapture-whitelist.cmake`), so this setting
cannot cause the engine to reach the network. Updates never install while a recording
is active (SPEC.md §21.3).

## `[hotkeys]`

| Key | Type | Range | Default | Since |
| --- | --- | --- | --- | --- |
| `enabled` | boolean | — | `true` | 2 |
| `start` | string | key sequence | `Ctrl+Shift+F9` | 2 |
| `stop` | string | key sequence | `Ctrl+Shift+F9` | 2 |
| `pause_resume` | string | key sequence | `Ctrl+Shift+F10` | 2 |

SPEC.md §16.5's rebindable global hotkeys. **The engine stores these and never acts on
them** — `RegisterHotKey` belongs to the GUI, which is the process with a message pump
to receive `WM_HOTKEY` on. They live here because §17 makes `config.toml` the single
settings store, and a separate GUI settings file would be the second source of truth
§17 exists to prevent.

- **Sequences** are written the way a user would: modifiers joined by `+`, then one
  key — `Ctrl+Shift+F9`, `Alt+F10`, `Ctrl+Alt+Home`. At least one modifier is
  required; a bare key would swallow that key system-wide.
- **Nothing here validates a sequence.** Whether it *parses* is the GUI's
  `parse_sequence`, and whether it can actually be *bound* is Windows' answer to
  `RegisterHotKey` — another application may already own the combination. Neither can
  be answered from the engine, so an unusable sequence costs one accelerator and a
  reported conflict, never a failed config load.
- **`start` and `stop` share a default deliberately.** That one sequence is the
  start/stop toggle FrameCapture shipped with; expressing it as two identical bindings
  keeps the familiar behaviour while making the two actions separable for anyone who
  wants separate keys. When two actions name the same sequence, one hotkey is
  registered and dispatched as a toggle by the current recording state.
- **`enabled = false`** unregisters everything. The in-window controls are unaffected.

## `[overlay]`

| Key | Type | Range | Default | Since |
| --- | --- | --- | --- | --- |
| `pill_enabled` | boolean | — | `true` | 2 |
| `pill_corner` | enum | `top-left` `top-right` `bottom-left` `bottom-right` | `bottom-right` | 2 |
| `pill_monitor` | string | display device path | `""` (the captured monitor) | 2 |
| `pill_x` | integer | −32768–32767 | `-1` (unplaced) | 2 |
| `pill_y` | integer | −32768–32767 | `-1` (unplaced) | 2 |
| `toasts_enabled` | boolean | — | `true` | 2 |
| `toast_corner` | enum | `top-left` `top-right` `bottom-left` `bottom-right` | `top-right` | 2 |
| `toast_duration_s` | integer | 1–60 | `4` | 2 |
| `toast_max_visible` | integer | 1–8 | `4` | 2 |

The floating recording pill and the toast notifications. Both are GUI windows; the
engine stores their placement and draws nothing. Stored here for the same reason as
`[hotkeys]`.

What keeps them **out of the recording** is not a setting: every overlay window is
stamped with `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)`, which makes DWM
composite the capture surface as though the window were not there. The older
`WDA_MONITOR` would paint a black rectangle instead, which is the "black cutout"
defect and is why that constant is a banned pattern in `scripts/lint.ps1`.

- **`pill_enabled`** — when the exclusion probe at startup reports the affinity was
  refused, the pill is suppressed regardless of this setting and the GUI says why.
  Showing an overlay that would land in the file is never the fallback.
- **`pill_monitor`** — a display device path, not an index; indices reshuffle on
  hotplug (SPEC.md §4.1). Empty means "whichever monitor is being captured".
- **`pill_x` / `pill_y`** — virtual-desktop coordinates of the pill's top-left once the
  user has dragged it. `-1` means never dragged, so `pill_corner` decides. `-1` rather
  than `0` because `0,0` is a real position — the top-left of the primary monitor — and
  "unplaced" has to be distinguishable from "placed there deliberately". A remembered
  position on a monitor that no longer exists is clamped back on screen at startup.
- **`toast_duration_s`** — how long an *informational* toast stays up. Warnings get
  longer and errors stay until dismissed; both are the GUI's policy and this is the
  number they scale from.
- **`toast_max_visible`** — how many stack at once. Further toasts queue, and the queue
  is capped so an error burst cannot grow it without bound.

---

## `[window]`

| Key | Type | Range | Default | Since |
| --- | --- | --- | --- | --- |
| `show_preview` | boolean | — | `true` | 2 |
| `show_sources` | boolean | — | `true` | 2 |
| `show_audio_mixer` | boolean | — | `true` | 2 |
| `show_controls` | boolean | — | `true` | 2 |
| `show_status` | boolean | — | `true` | 2 |
| `always_on_top` | boolean | — | `false` | 2 |

Which parts of the main window are on screen. Written by the GUI's **View** menu; the
engine stores them and has no window of its own. Stored here for the same reason as
`[hotkeys]` and `[overlay]`: SPEC.md §17 makes this file the only settings store, and a
second one for the GUI's own keys would be the second source of truth §17 forbids.

Every panel defaults to shown. SPEC.md §16.2 draws the whole window, so a panel is
absent only because a user hid it — and a config file written before this milestone,
which has no `[window]` section at all, loads as the layout the spec draws.

- **`always_on_top`** — the exception, and it defaults **off**. It changes how the
  window behaves against every other application on the machine, which is not a default
  anyone asked for.
- **View ▸ Reset layout** restores every key in this section to its default in one
  save. It deliberately leaves `[overlay]` alone: the pill and the notifications are not
  layout, and a user who turned the notifications off did not ask for them back because
  they wanted their panels rearranged.

---

## Example

A complete file at defaults. Everything is optional; omitted keys use the default.

```toml
schema_version = 2

[general]
output_directory = "C:\\Users\\you\\Videos\\FrameCapture"
filename_template = "FrameCapture_%Y-%m-%d_%H-%M-%S"
language = "en"

[video]
width = 1920
height = 1080
fps = 60
container = "mkv"
codec = "h264"
encoder = "auto"
rate_control = "cqp"
cqp = 20
bitrate_kbps = 20000
gop_seconds = 2
max_b_frames = 2
pacing = "cfr"
full_range = false

[audio]
device_id = ""
channel_layout = "auto"
bitrate_kbps = 0
multitrack_enabled = false
max_tracks = 6
multitrack_targets = ""
multitrack_reattach = true

[segmentation]
enabled = false
split_by_duration = true
duration_minutes = 30
split_by_size = false
size_mb = 4096

[advanced]
capture_backend = "auto"
capture_cursor = true
hdr_tonemap = true
log_level = "info"
gpu_override = ""

[updates]
check_enabled = true

[hotkeys]
enabled = true
start = "Ctrl+Shift+F9"
stop = "Ctrl+Shift+F9"
pause_resume = "Ctrl+Shift+F10"

[overlay]
pill_enabled = true
pill_corner = "bottom-right"
pill_monitor = ""
pill_x = -1
pill_y = -1
toasts_enabled = true
toast_corner = "top-right"
toast_duration_s = 4
toast_max_visible = 4

[window]
show_preview = true
show_sources = true
show_audio_mixer = true
show_controls = true
show_status = true
always_on_top = false
```
