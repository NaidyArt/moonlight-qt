# Moonlight AVSampleBuffer structured performance telemetry

## Status and scope

This source candidate is based on the installed release commit
`f103de24cc89bc60a9f89c32914abbac4f71437a` (upstream base
`2e13ed9977bc31c73caf8428f08f58d793313ece`). It does not include or modify the
experimental asynchronous Metal renderer. It must not replace the installed
app until the source review, unit tests, universal CI build, artifact audit, and
explicit runtime acceptance gate all pass.

The installed rollback executable is still the authoritative baseline:

`d96842406ca2358034595f7cb69814666a8f8b1447723f22bca3ff8160c6c0f4`

## Exact lifecycle

- Telemetry follows the actual `OverlayDebug` state, including the existing
  `Ctrl+Alt+Shift+S` and `Select+L1+R1+X` shortcuts.
- Overlay hidden means no telemetry writer thread, no telemetry periodic wake,
  no sampling publication, no open telemetry file, and no writes.
- Showing the overlay starts a new telemetry session and opens a new JSONL file.
- Hiding it stops acceptance first, drains any in-flight POD publication, writes
  a pending final sample at most once, flushes, `fsync`s, closes, and joins the
  writer before the state change returns.
- Showing it again creates a different UUID session and a different file.
- Session shutdown forces the same overlay-off path before destroying the
  decoder.
- The variant-only preference `statsTelemetryAvsbdlVariant` is enabled by
  default and can be disabled in Settings. Setting
  `MOONLIGHT_AVSBDL_TELEMETRY=0` (also `false`, `off`, or `no`) is a hard
  process-level kill switch.

## Storage, bounds, and privacy

Files are written under:

`~/Library/Logs/Moonlight AVSampleBuffer/`

The leaf directory is required to be owned by the current user, must not be a
symlink, and is forced to mode `0700`. Files are created with `openat()`,
`O_EXCL`, `O_NOFOLLOW`, and mode `0600`. File descriptors are close-on-exec.
I/O failure disables telemetry for that overlay-visible interval and never
changes stream behavior.

Rotation is bounded to eight files of at most 4 MiB each. A single JSON line
may exceed the limit by its own size, but the schema is fixed and normally well
below 4 KiB. Only regular non-symlink files matching `telemetry-*.jsonl` are
eligible for pruning.

The producer accepts no host object, address, hostname, application name,
pairing data, token, key, or input event. The schema contains stream mechanics
only. Session UUIDs are local random identifiers and are not Moonlight pairing
or Sunshine session credentials.

## Sampling model and fields

The video thread does no telemetry I/O, JSON construction, allocation, or
mutex acquisition. While the overlay is visible, it copies the exact rolling
two-window statistics already used to render that overlay into a bounded SPSC
POD queue. The low-priority writer emits at 1 Hz. If no new video window arrives
during a stall, it repeats the last source window with `source_stale: true` and
an increasing `source_age_ms`, so a transport stall is visible without
misrepresenting the record as fresh. If several fresh rolling windows queue
between writer ticks, all are serialized in source order rather than coalesced;
this preserves short loss peaks. The fixed queue bound remains observable in
`telemetry_queue_dropped_samples` if storage stalls long enough to fill it.

Each line includes:

- schema version, session UUID, output and source-window sequences;
- UTC output timestamp, source-window timestamp, age, and freshness;
- width, height, configured FPS, overlay total FPS, codec, HDR state, and
  requested bitrate in kb/s;
- incoming, decoding, and rendering FPS;
- host processing latency minimum, maximum, and average in milliseconds;
- network-drop and pacer/jitter-drop percentages;
- RTT and RTT variance in milliseconds, or JSON `null` when unavailable;
- average decode, frame-queue, and render-including-V-sync milliseconds;
- raw rolling counters and denominators: received, decoded, rendered, total,
  network-dropped, pacer-dropped, host-latency count and total, reassembly,
  decode, queue, render microseconds, measurement start/duration, and internal
  telemetry queue drops.

The percentages use the same denominators as the visible overlay:
`network_dropped_frames / total_frames` and
`pacer_dropped_frames / decoded_frames`. Timing averages likewise use decoded
or rendered frame counts exactly as the overlay does. Raw counters represent an
overlapping approximately two-second rolling window; they are not cumulative
session counters.

## Monitoring after a separately approved install

No screenshots are needed once this candidate is installed and accepted. A
read-only monitor can follow the newest file while the overlay is visible:

```zsh
log_dir="$HOME/Library/Logs/Moonlight AVSampleBuffer"
files=("$log_dir"/telemetry-*.jsonl(N))
(( ${#files[@]} > 0 )) || exit 1
latest=$(/bin/ls -1t "${files[@]}" | /usr/bin/head -1)
/usr/bin/tail -F -- "$latest"
```

The monitoring side should parse each line independently as JSON, track
`session_id`, and treat `source_stale: true` or a growing `source_age_ms` as a
stall signal.

## Exact rollback to the installed baseline

Before any future candidate installation, retain the already audited f103de24
DMG/ZIP and verify its manifest. To roll back after a candidate test:

1. Quit bundle `art.naidy.moonlight-avsamplebuffer` and require the process to
   exit.
2. Move the candidate app to the Trash; do not overwrite it in place.
3. Reinstall `Moonlight AVSampleBuffer.app` from the audited f103de24 artifact.
4. Verify the restored executable SHA-256 is exactly
   `d96842406ca2358034595f7cb69814666a8f8b1447723f22bca3ff8160c6c0f4`, its
   bundle ID is unchanged, both `arm64` and `x86_64` slices exist, and
   `codesign --verify --deep --strict` passes.
5. The old binary ignores `statsTelemetryAvsbdlVariant`. For a logical settings
   rollback after both apps are closed, delete only that key. Telemetry JSONL
   files are inert user data; move their dedicated directory to the Trash only
   if the user also wants them removed recoverably.

This rollback does not touch hosts, pairings, the shared stream profile,
Sunshine, either TV, or the separate official Moonlight bundle.
