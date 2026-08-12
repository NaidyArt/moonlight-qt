# macOS AVSampleBuffer telemetry source review

Date: 2026-08-12

Review base: `f103de24cc89bc60a9f89c32914abbac4f71437a`

Pinned upstream base: `2e13ed9977bc31c73caf8428f08f58d793313ece`

Status: **SOURCE PASS; COMPILE/CI PENDING**

## Requirement audit

| Requirement | Authoritative source evidence | Result |
|---|---|---|
| Exact visible-overlay lifecycle | `OverlayManager::setOverlayState()` notifies `Session`; `Session::notifyOverlayStateChanged()` drives `StatsTelemetry::setOverlayActive()` | PASS |
| Overlay off has no writer, wake, open file, or writes | `reconcileLocked()` owns the only writer; off calls `stopLocked()` and joins; lifecycle unit fixtures cover off and post-close | PASS |
| Retoggle is a new session | Each `WriterThread` creates a fresh UUID; retoggle fixture requires two distinct IDs | PASS |
| 1 Hz while visible and stall visibility | Writer-only timed wait; unchanged source produces `source_stale` with source age | PASS |
| Preserve short loss peaks | Every queued fresh rolling window is written in source order; the consumer does not coalesce to latest; dedicated peak fixture | PASS |
| No blocking telemetry work on video path | `publish()` is a fixed POD SPSC copy with atomics only; audit rejects file, JSON, mutex, allocation, open, flush, and wait calls in its body | PASS |
| Same metrics and denominators as overlay | FFmpeg publishes `lastTwoWndStats` inside the same `OverlayDebug` branch after the overlay string is generated | PASS |
| Required structured fields/raw counters | Schema audit enumerates resolution/FPS/codec/HDR/bitrate, FPS triplet, host, drop, RTT, timing, and raw rolling counters | PASS |
| Private storage | Leaf directory ownership/symlink validation plus `0700`; `openat(O_EXCL|O_NOFOLLOW|O_CLOEXEC)` plus `0600` | PASS |
| Bounded rotation | Eight files, 4 MiB each; only regular `telemetry-*.jsonl` entries are pruned | PASS |
| No identity, address, secret, key, or input payload | Telemetry accepts only stream config and numeric video stats; forbidden-schema audit; no host object is reachable | PASS |
| Fail-open I/O | Writer failure disables acceptance and exits without propagating to stream; invalid-directory fixture preserves its blocker file | PASS |
| Clean close | Stop rejects new producers, invalidates the generation, waits in-flight POD publishers, wakes writer, drains, flushes, `fsync`s, closes, and joins | PASS |
| Global disable | Variant-only preference defaults on; UI selector plus `MOONLIGHT_AVSBDL_TELEMETRY=0/false/off/no`; disabled fixture | PASS |
| No Metal async changes | Existing source-isolation audit still rejects the Metal experiment patterns | PASS |
| Exact rollback | Design document pins installed executable SHA-256 `d96842406ca2358034595f7cb69814666a8f8b1447723f22bca3ff8160c6c0f4` and recoverable replacement steps | PASS |

## Review corrections already applied

- The first consumer draft retained only the newest queued window. It now
  serializes every fresh window in order so a short 32% loss peak cannot vanish
  between writer ticks.
- Stop/start now uses a generation fence plus an in-flight publisher count, so
  a producer paused across a rapid off/on transition cannot enter the next
  telemetry session or underflow teardown accounting.
- `QFile` is constructed and destroyed inside `WriterThread::run()`, avoiding
  cross-thread QObject affinity.
- Directory creation uses native mode `0700` directly rather than creating a
  permissive leaf and tightening it afterward.

## Checks completed before CI

- `scripts/verify-macos-avsbdl-telemetry-source.py`: PASS.
- `scripts/verify-macos-avsbdl-source.sh`: PASS.
- `tests/macos/test-avsbdl-packaging.sh`: PASS.
- `tests/macos/test-upstream-skip-dmg.sh`: PASS.
- `git diff --check`: PASS.

The C++ unit target and universal application build intentionally remain
pending until this source review is recorded. No candidate has been installed
or executed on the Mac, and Sunshine/TV state is untouched.
