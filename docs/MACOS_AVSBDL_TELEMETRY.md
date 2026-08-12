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
- Overlay hidden means no accepting telemetry session, periodic sampling, or
  new telemetry write. A retiring worker may exist only long enough to finish
  an already-entered OS I/O operation and close its own file asynchronously.
- Showing the overlay starts a new telemetry session and opens a new JSONL file.
- Hiding it atomically detaches the accepting session and invalidates its
  generation before returning. It never joins, waits, writes, flushes, closes,
  or calls `fsync` from the SDL/UI/input path. The self-owned worker discards
  queued output, flushes, `fsync`s, closes, and retires asynchronously. If it
  was already inside a kernel I/O call, that old file can remain transiently
  open, but it cannot accept a new sample or be reused by a later session.
- Worker activation/stop uses a counted native semaphore, so an immediate
  ON/OFF cannot lose a wake before the worker begins waiting.
- Lifecycle transitions also advance a counted epoch. If ON arrives while an
  obsolete starter is withdrawing, that owner releases `startInProgress`,
  observes the newer epoch, and iteratively hands ownership to a fresh start.
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

The leaf directory is opened with `O_DIRECTORY|O_NOFOLLOW`, required to be
owned by the current user, and checked/tightened through that descriptor to
mode `0700`; it is never chmod'ed by a re-resolved pathname. Files are created
with `openat()`, `O_EXCL`, `O_NOFOLLOW`, and mode `0600`. File descriptors are
close-on-exec.
I/O failure disables telemetry for that overlay-visible interval and never
changes stream behavior.

Rotation is bounded to eight files of at most 4 MiB each. A single JSON line
may exceed the limit by its own size, but the schema is fixed and normally well
below 4 KiB. Only regular non-symlink files matching `telemetry-*.jsonl` are
eligible for pruning. Enumeration, no-follow stat, inode revalidation, and
deletion are all relative to the already-open directory descriptor.

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

The source-window sequence advances for every active-session publication
attempt before the queue-capacity check, so queue loss is visible both in the
raw drop counter and as an exact sequence gap.

## Asynchronous retirement and error visibility

Each overlay-visible interval owns an immutable stream configuration, UUID,
SPSC queue, file state, and worker lifetime. A rapid off/on starts a wholly new
session; a slow old `write`, `flush`, or `fsync` cannot write into, close, or
otherwise mutate the new session. Detached workers retain only their own
session and a small shared control block, never `Session`, the decoder, or the
`StatsTelemetry` facade, which prevents use-after-free during bounded stream
shutdown.

At most two session states can exist process-wide, which permits the normal
current writer plus one asynchronously retiring writer. This also bounds
workers left by an earlier stream/session object. If both are pinned inside OS
I/O, further overlay-on attempts fail open without creating a thread, timer,
or file; the condition is visible through the telemetry error counter. Once a
slot retires, a later overlay toggle can start telemetry again. Repeated
blocked off/on fault-injection covers write, flush, and sync and proves the
resource count remains at two.

Thread creation is native and detached at `pthread_create()` time; there is no
throwing `std::thread::detach()` transition. The native entry point is
`noexcept` and catches every worker exception, records it atomically, closes
native resources fail-open, and never propagates into streaming.

The queue is genuinely SPSC in production. `FFmpegVideoDecoder` always enables
the pull-renderer capability, so Moonlight Common does not call the push submit
callback. Both decode-unit submission sites execute serially inside the single
`FFDecoder` thread, and that method contains the sole production telemetry
publication call. The source audit pins this topology.

### Qt teardown contract

Overlay-off is dispatched before decoder/session cleanup while
`QCoreApplication` is still alive. Normal nonblocking serialization therefore
retires during that grace period. On macOS, JSON/timestamp/filename
serialization is standard C++, and all Qt construction is complete before the
worker starts. Before the worker enters an injectable or native
write/flush/sync operation. If one of those operations remains blocked across
application teardown, its continuation uses only a raw POSIX descriptor,
standard strings/atomics/mutexes, and Qt-free session/control members. It does
not call `QFile`, `QDateTime`, JSON APIs, `qWarning`, the event loop, `Session`,
or the decoder after stop. The process image remains the ultimate lifetime of
detached code; process termination may discard an indefinitely blocked worker,
with the process-wide quota limiting that state to two.

The unit executable includes a final lifecycle probe outside the normal QtTest
suite: it blocks the raw write boundary, destroys the `StatsTelemetry` facade,
destroys `QCoreApplication`, then releases the worker and requires both worker
and session counts to reach zero. This directly exercises the supported
post-Qt retirement path without making application shutdown wait for I/O.

All storage operations, including the durable `fsync`, occur only on the
background worker. Tests expose a test-only atomic native-sync counter: while
the injected Sync boundary is blocked it is unchanged; after OFF and release it
advances exactly when the real `fsync` call is issued. Failures update a public atomic error count and last-error
code, then disable that telemetry session without logging, allocating, or
changing stream behavior. Tests inject blocking and failing write/flush/sync
operations to verify bounded overlay toggles and destruction, zero post-off
publication, separated rapid retoggles, a two-session resource ceiling, and
eventual worker/session cleanup.

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
