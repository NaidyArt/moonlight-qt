# macOS AVSampleBuffer telemetry source review

Date: 2026-08-12

Review base: `f103de24cc89bc60a9f89c32914abbac4f71437a`

Pinned upstream base: `2e13ed9977bc31c73caf8428f08f58d793313ece`

Status: **SECOND SOURCE CANDIDATE; INDEPENDENT RE-REVIEW AND REPLACEMENT COMPILE/CI PENDING**

The first artifact from run `31552499918` is rejected and must not be
installed. Independent review found a synchronous `wait()`/flush/`fsync`
retirement path and a source sequence that hid queue-full attempts. The
revised source below closes both findings; it requires a new commit, CI run,
artifact hashes, and independent audit.

A later pre-commit review rejected an intermediate asynchronous draft. That
NO-GO remains historical evidence: it found a condition-variable lost-wake
window, an OFF/destructor path that could wait behind lifecycle thread
creation, an uncaught detached-thread exception/`detach()` failure path,
Sync fixtures that blocked only after OFF, and pathname TOCTOU in directory
permission and rotation handling. The current candidate replaces those paths;
it is not approved until a fresh independent review confirms the replacement.

The first re-review of the replacement also remained NO-GO: it found one
late-withdrawal interleaving where a new ON observed `startInProgress=true`,
the old owner had already decided not to retry, and neither became the next
owner. The current source adds a counted lifecycle epoch and central release
handoff on every owned start exit. A two-barrier fixture reproduces that exact
ordering and requires the final ON state to own an active session.

## Requirement audit

| Requirement | Authoritative source evidence | Result |
|---|---|---|
| Exact visible-overlay lifecycle | `OverlayManager::setOverlayState()` notifies `Session`; `Session::notifyOverlayStateChanged()` drives `StatsTelemetry::setOverlayActive()` | PASS |
| Overlay-off/input path is bounded | `stop()` contains only lock-free atomics, a raw-pointer hazard guard, and a counted semaphore signal; no lifecycle mutex exists | PASS (local source gate) |
| Worker wake cannot be lost | Activation/stop use a counted native semaphore; immediate ON/OFF is repeated without a condition-variable notification window | PASS (local source gate) |
| Worker start cannot hold OFF/destruction | Native detached-thread creation runs outside the facade; an injected blocked `WorkerStart` proves concurrent OFF and facade destruction return below 250 ms | PASS (local source gate) |
| Concurrent ON cannot be lost at late withdrawal | Every lifecycle transition advances a sequentially-consistent epoch; every start-owner exit releases then revalidates it in one iterative handoff | PASS (local source gate; independent re-review pending) |
| Detached exceptions cannot terminate streaming | `pthread_create()` receives `PTHREAD_CREATE_DETACHED` up front; the `noexcept` trampoline runs a total `catch (...)` barrier and fail-open cleanup | PASS (local source gate) |
| Blocked old I/O cannot own application state | A detached standard worker owns its immutable `SessionState`; no QThread or pointer to Session/decoder/facade survives | PASS |
| Repeated blocked retirements stay bounded | Process-wide atomic quota allows at most current + one retiring session, including across stream objects; further starts record an error without creating a thread/file | PASS |
| Retoggle is a new isolated session | Each `SessionState` owns a fresh UUID, queue, config, and file; blocked-I/O rapid-retoggle fixture rejects cross-session records | PASS |
| 1 Hz while visible and stall visibility | Writer-only timed wait; unchanged source produces `source_stale` with source age | PASS |
| Preserve short loss peaks and queue gaps | Every queued fresh rolling window is written in source order; attempt sequence increments before capacity check; peak and exact-gap fixtures | PASS |
| No blocking telemetry work on video path | `publish()` is a fixed POD SPSC copy with atomics only; audit rejects file, JSON, mutex, allocation, open, flush, and wait calls in its body | PASS |
| SPSC producer contract is real | FFmpeg unconditionally selects pull mode; both submission sites and the sole telemetry call run serially on its one decoder thread; audit pins call topology | PASS |
| Same metrics and denominators as overlay | FFmpeg publishes `lastTwoWndStats` inside the same `OverlayDebug` branch after the overlay string is generated | PASS |
| Required structured fields/raw counters | Schema audit enumerates resolution/FPS/codec/HDR/bitrate, FPS triplet, host, drop, RTT, timing, and raw rolling counters | PASS |
| Private storage | Leaf is opened `O_DIRECTORY|O_NOFOLLOW`, then ownership/mode are checked and tightened through `fstat`/`fchmod`; files use `openat(O_EXCL|O_NOFOLLOW|O_CLOEXEC)` and `0600` | PASS (local source gate) |
| Bounded rotation | Enumeration/deletion stay under the directory fd (`openat(".")`/`fdopendir`/`fstatat(AT_SYMLINK_NOFOLLOW)`/inode revalidation/`unlinkat`); symlink fixtures preserve targets | PASS (local source gate) |
| No identity, address, secret, key, or input payload | Telemetry accepts only stream config and numeric video stats; forbidden-schema audit; no host object is reachable | PASS |
| Fail-open and observable I/O | Writer failure disables only its session and updates public atomic error count/last-code state without logging, allocation, or propagation to stream | PASS |
| Async durable close | Stop rejects and invalidates immediately; only the self-owned worker flushes/`fsync`s/closes. Sync is blocked before OFF/destruction/retoggle; a native-call counter proves the real `fsync` executes after release | PASS (local source gate) |
| Qt teardown safety | macOS serializes JSON/timestamps/filenames with standard C++ before native I/O; post-stop blocked-I/O continuation is POSIX/standard-only; final test destroys `QCoreApplication` before releasing it | PASS |
| Global disable | Variant-only preference defaults on; UI selector plus `MOONLIGHT_AVSBDL_TELEMETRY=0/false/off/no`; disabled fixture | PASS |
| No Metal async changes | Existing source-isolation audit still rejects the Metal experiment patterns | PASS |
| Exact rollback | Design document pins installed executable SHA-256 `d96842406ca2358034595f7cb69814666a8f8b1447723f22bca3ff8160c6c0f4` and recoverable replacement steps | PASS |

## Review corrections already applied

- Rejected run `31552499918` synchronously joined its writer from
  `setOverlayActive(false)`. Retirement is now asynchronous and self-owned;
  the SDL/UI/input and Session shutdown paths contain no join, wait, file I/O,
  flush, close, `fsync`, or polling loop.
- A retiring session never shares queue/file state with a rapid reactivation.
  Fault injection holds old write/flush/sync operations across off/on and
  destruction, then verifies bounded calls, separated UUIDs/data, and eventual
  zero worker/session lifetime counts without UAF.
- At most two session states can exist process-wide. A multi-retoggle fault
  test pins writers from two independent stream/control objects in
  write/flush/sync, repeats twenty further toggles, requires no additional
  worker/session, observes the limit error, releases the I/O, and verifies
  telemetry can start again.
- The macOS writer no longer owns `QFile` or Qt value members; worker JSON,
  timestamps, and filenames use standard C++ before native I/O.
  A final out-of-harness probe destroys the facade and `QCoreApplication`
  before releasing an injected raw write, then requires full retirement.
- Source sequence now increments before the full-queue check. The regression
  fills the 15-slot SPSC queue and requires the next written sequence to jump
  from 16 to 22 with five explicitly reported queue drops.
- The condition-variable wake was removed. A counted Mach semaphore on macOS
  retains activation/stop signals even if the worker has not begun waiting.
- The facade lifecycle mutex was removed. Concurrent ON may allocate or be
  held at the pre-thread fault boundary, while OFF/destruction only invalidate
  atomics and signal any already-published session.
- `std::thread::detach()` was removed. macOS creates a pthread already detached
  in its attributes, and its `noexcept` entry point contains a total exception
  firewall plus native fail-open cleanup.
- Sync fault tests now force file rotation and wait at the Sync boundary before
  OFF/destruction/retoggle. They assert the actual native `fsync` counter
  advances after the blocker is released.
- Directory permissions and rotation no longer mutate/delete by a re-resolved
  pathname. Both operate from validated descriptors with no-follow and inode
  revalidation; symlink fixtures assert target mode/content remain unchanged.
- A deterministic `StartPublished`/`StartRelease` two-barrier regression forces
  OFF before the old owner's late withdrawal and ON after that decision but
  before owner release. Start acquisition, owner release, and the counted
  lifecycle epoch share sequentially consistent ordering, so the losing ON is
  handed back to an iterative starter and must leave a new session active.

- The first consumer draft retained only the newest queued window. It now
  serializes every fresh window in order so a short 32% loss peak cannot vanish
  between writer ticks.
- Stop/start uses an atomic current-session pointer, generation fence, and
  accessor hazard count. The pointer/accessor handshake is sequentially
  consistent and compile-time lock-free on macOS; reclamation waits only on
  the retiring background worker after the session has become unreachable.
- The macOS worker uses a raw descriptor; `QFile` remains only as the inactive
  non-Unix portability fallback.
- Directory creation uses native mode `0700` directly rather than creating a
  permissive leaf and tightening it afterward.

## Checks completed before independent re-review

- `scripts/verify-macos-avsbdl-telemetry-source.py`: PASS.
- `scripts/verify-macos-avsbdl-source.sh`: PASS.
- `tests/macos/test-avsbdl-packaging.sh`: PASS.
- `tests/macos/test-upstream-skip-dmg.sh`: PASS.
- `git diff --check`: PASS.
- Darwin-preprocessed tree-sitter C++ syntax check for implementation, header,
  and Qt tests: PASS (not a substitute for the macOS Qt compile).

The earlier run and both NO-GO source drafts are historical evidence only and
are not install candidates. The revised C++ unit target and universal
application build remain pending until independent re-review passes and this
source is frozen into a new commit. No candidate has been installed or executed
on the Mac, and Sunshine/TV state is untouched.
