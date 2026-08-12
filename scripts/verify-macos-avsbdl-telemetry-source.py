#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def require(text: str, literal: str, description: str) -> None:
    if literal not in text:
        fail(description)


def function_body(text: str, signature: str) -> str:
    start = text.find(signature)
    if start < 0:
        fail(f"missing function: {signature}")
    opening = text.find("{", start)
    if opening < 0:
        fail(f"missing body for: {signature}")
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1 : index]
    fail(f"unterminated body for: {signature}")
    raise AssertionError("unreachable")


def main() -> None:
    telemetry_h = read("app/streaming/video/statstelemetry.h")
    telemetry_cpp = read("app/streaming/video/statstelemetry.cpp")
    ffmpeg_cpp = read("app/streaming/video/ffmpeg.cpp")
    overlay_h = read("app/streaming/video/overlaymanager.h")
    overlay_cpp = read("app/streaming/video/overlaymanager.cpp")
    session_cpp = read("app/streaming/session.cpp")
    prefs_cpp = read("app/settings/streamingpreferences.cpp")
    tests_cpp = read("tests/macos/telemetry/test-statstelemetry.cpp")

    require(
        telemetry_cpp,
        'Library/Logs/Moonlight AVSampleBuffer',
        "telemetry does not use the dedicated macOS log directory",
    )
    for literal, description in (
        ("S_IRWXU", "directory mode 0700 is not enforced"),
        ("S_IRUSR | S_IWUSR", "file mode 0600 is not enforced"),
        ("O_NOFOLLOW", "telemetry paths do not reject symlinks"),
        ("O_CLOEXEC", "telemetry descriptors are inherited across exec"),
        ("const std::int64_t maxFileBytes", "size rotation is missing"),
        ("const int maxFileCount", "file-count rotation is missing"),
        ("line.size()) > m_MaxFileBytes", "worker does not enforce byte rotation"),
        ("remaining < m_MaxFileCount", "worker does not enforce file-count rotation"),
        ("::fsync(m_FileFd)", "clean close lacks a durable flush"),
    ):
        require(telemetry_cpp, literal, description)
    if "qWarning" in telemetry_cpp:
        fail("detached telemetry can enter Qt logging during process teardown")
    write_body = function_body(telemetry_cpp, "WriteResult writeSample(const Sample& sample, bool sourceStale)")
    if not (0 <= write_body.find("const std::string line = serializeSample") <
            write_body.find("beforeIo(IoOperation::Write)")):
        fail("Qt serialization state can survive into the blocking write boundary")
    close_body = function_body(telemetry_cpp, "void closeFile(bool durableFlush)")
    finish_body = function_body(telemetry_cpp, "void finish() noexcept")
    serialize_body = function_body(
        telemetry_cpp, "std::string serializeSample(const Sample& sample, bool sourceStale)"
    )
    for token in ("QDateTime", "QJson", "QString", "QByteArray", "qWarning"):
        if token in serialize_body:
            fail(f"macOS worker serialization still invokes Qt API: {token}")
        if token in close_body + finish_body:
            fail(f"post-stop retirement still invokes Qt API: {token}")
    sync_count_position = close_body.find("nativeSyncCalls.fetch_add")
    native_sync_position = close_body.find("::fsync(m_FileFd)")
    if not (0 <= sync_count_position < native_sync_position):
        fail("native fsync is not explicitly invoked and counted on worker close")
    if "m_StopRequested" in close_body[sync_count_position:native_sync_position]:
        fail("overlay OFF still suppresses the native durable fsync")
    for literal, description in (
        ("std::string serializeSample", "worker does not release Qt JSON state before I/O"),
        ("::write(m_FileFd", "macOS writer still depends on QFile during blocking I/O"),
        ("::fsync(m_FileFd)", "macOS retirement is not native after blocked I/O"),
        ("const std::string m_DirectoryPath", "retired session retains a Qt path object"),
        ("const std::string m_SessionId", "retired session retains a Qt UUID object"),
        ("currentUnixMilliseconds()", "worker timestamping still depends on Qt"),
        ("compactUtcTimestamp", "worker filename generation still depends on Qt"),
    ):
        require(telemetry_cpp, literal, description)

    publish_body = function_body(
        telemetry_cpp, "bool StatsTelemetry::publish(const Sample& sample) noexcept"
    )
    enqueue_body = function_body(
        telemetry_cpp, "bool enqueue(const Sample& inputSample) noexcept"
    )
    forbidden_hot_path = (
        r"\bQFile\b",
        r"\bQJson",
        r"\bQMutex",
        r"\bQWaitCondition",
        r"\bnew\b",
        r"\bopen(at)?\s*\(",
        r"\bflush\s*\(",
        r"\bwait\s*\(",
    )
    for pattern in forbidden_hot_path:
        if re.search(pattern, publish_body + enqueue_body):
            fail(f"hot-path publish contains forbidden operation: {pattern}")
    require(
        enqueue_body,
        "m_WritePosition.store(nextPosition, std::memory_order_release)",
        "hot-path publish is not a bounded SPSC handoff",
    )
    attempt_position = enqueue_body.find("m_SourceWindowAttemptSequence.fetch_add")
    full_position = enqueue_body.find("nextPosition == m_ReadPosition.load")
    if not (0 <= attempt_position < full_position):
        fail("source sequence is not incremented before the queue-full check")

    stop_body = function_body(
        telemetry_cpp, "void StatsTelemetry::stop(ControlState* control) noexcept"
    )
    request_stop_body = function_body(telemetry_cpp, "void requestStop() noexcept")
    destructor_body = function_body(telemetry_cpp, "StatsTelemetry::~StatsTelemetry()")
    for pattern in (
        r"\bwait\s*\(",
        r"\bjoin\s*\(",
        r"\bflush\s*\(",
        r"\bfsync\s*\(",
        r"\bclose\s*\(",
        r"\bsleep",
        r"\byield",
        r"\bQFile\b",
        r"\bstd::mutex\b",
        r"\block_guard\b",
        r"\bQMutex\b",
    ):
        if re.search(pattern, stop_body + request_stop_body + destructor_body):
            fail(f"overlay-off path contains forbidden retirement work: {pattern}")
    require(
        stop_body,
        "control->currentSession.exchange(nullptr, std::memory_order_seq_cst)",
        "overlay-off does not immediately detach the accepting session",
    )
    require(
        stop_body,
        "control->generation.fetch_add(1, std::memory_order_acq_rel)",
        "overlay-off does not immediately invalidate the session generation",
    )
    for literal, description in (
        (
            "activeAccessors.fetch_add(1, std::memory_order_seq_cst)",
            "raw session acquisition lacks the ordered lifetime handshake",
        ),
        (
            "activeAccessors.load(std::memory_order_seq_cst)",
            "retired session lifetime is not ordered against new accessors",
        ),
        (
            "currentSession.load(std::memory_order_seq_cst)",
            "raw session dereference can race asynchronous destruction",
        ),
        (
            "std::atomic<void*>::is_always_lock_free",
            "macOS hot-path pointer atomic is not compile-time lock-free",
        ),
    ):
        require(telemetry_cpp, literal, description)
    start_body = function_body(telemetry_cpp, "void StatsTelemetry::tryStart(")
    slot_position = start_body.find("tryAcquireSessionSlot()")
    thread_position = start_body.find("startDetachedWorker(session)")
    if not (0 <= slot_position < thread_position):
        fail("detached telemetry workers are not capped before thread creation")
    for literal, description in (
        ("MaxOutstandingSessions = 2", "blocked retired-session bound is missing"),
        (
            "const std::shared_ptr<TelemetryProcessState> process",
            "production worker budget is not owned by shared control state",
        ),
        (
            "process->outstandingSessions.compare_exchange_weak",
            "shared control state does not enforce the production worker budget",
        ),
        (
            "IoError::OutstandingSessionLimit",
            "retired-session limit rejection is not observable",
        ),
    ):
        require(telemetry_cpp, literal, description)
    if "QThread" in telemetry_h + telemetry_cpp:
        fail("telemetry still owns a QThread that can outlive synchronous teardown")
    for literal, description in (
        ("PTHREAD_CREATE_DETACHED", "macOS worker is not detached at pthread creation"),
        ("pthread_create", "native fail-open macOS worker creation is missing"),
        ("void runProtected() noexcept", "detached worker lacks a total exception barrier"),
        ("catch (...)", "detached worker exceptions can terminate the process"),
        ("std::shared_ptr<ControlState>", "retired worker state is not self-contained"),
        ("IoFaultInjector", "writer I/O fault-injection boundary is missing"),
        ("liveWorkerCountForTests", "worker lifetime is not test-observable"),
        ("ioErrorCount", "asynchronous I/O failures are not observable"),
    ):
        require(telemetry_h + telemetry_cpp, literal, description)
    if "std::thread" in telemetry_cpp or ".detach()" in telemetry_cpp:
        fail("std::thread detach can terminate if detach itself throws")
    for literal, description in (
        ("class WakeSignal final", "counted worker wake primitive is missing"),
        ("semaphore_create", "macOS wake is not backed by a counted semaphore"),
        ("m_WakeSignal.signal()", "OFF cannot wake a pre-wait worker"),
        ("std::atomic<bool> startInProgress", "concurrent ON start serialization is missing"),
        ("std::atomic<std::uint64_t> lifecycleEpoch", "lost-ON handoff epoch is missing"),
        (
            "lifecycleEpoch.fetch_add(1, std::memory_order_seq_cst)",
            "lifecycle transitions do not publish a counted handoff epoch",
        ),
        ("IoOperation::StartPublished", "late-withdrawal setup barrier is missing"),
        ("IoOperation::StartRelease", "late-withdrawal release barrier is missing"),
    ):
        require(telemetry_cpp, literal, description)
    if "m_LifecycleMutex" in telemetry_h + telemetry_cpp:
        fail("OFF/destructor can still wait behind lifecycle start work")
    release_start_body = function_body(
        telemetry_cpp,
        "bool StatsTelemetry::releaseStart(const std::shared_ptr<ControlState>& control,",
    )
    release_position = release_start_body.find(
        "startInProgress.store(false, std::memory_order_seq_cst)"
    )
    epoch_position = release_start_body.find(
        "lifecycleEpoch.load(std::memory_order_seq_cst)"
    )
    state_position = release_start_body.find("shouldRun(control.get())")
    if not (0 <= release_position < epoch_position < state_position):
        fail("start-owner release does not revalidate the counted lifecycle epoch after handoff")
    if telemetry_cpp.count("startInProgress.store(false") != 1:
        fail("a start-owner exit bypasses the centralized lifecycle handoff")
    if not re.search(
        r"startInProgress\.compare_exchange_strong\(expectedStart,\s*"
        r"true,\s*std::memory_order_seq_cst,\s*std::memory_order_seq_cst\)",
        telemetry_cpp,
    ):
        fail("a failed ON acquisition is not ordered before the start-owner release")
    if start_body.count("releaseStart(control, ownedLifecycleEpoch)") < 8:
        fail("not every owned start exit funnels through the lifecycle handoff")
    if "tryStart(control);" in release_start_body:
        fail("lifecycle handoff uses recursive retry instead of the bounded owner loop")

    for literal, description in (
        ("::fstat(directoryFd", "directory validation is not descriptor-relative"),
        ("::fchmod(directoryFd", "directory permissions retain a pathname race"),
        ("::fdopendir", "rotation does not enumerate through the directory descriptor"),
        ("::fstatat(m_DirectoryFd", "rotation does not validate entries fd-relative"),
        ("AT_SYMLINK_NOFOLLOW", "rotation can follow a replacement symlink"),
        ("::unlinkat(m_DirectoryFd", "rotation deletion is not descriptor-relative"),
    ):
        require(telemetry_cpp, literal, description)
    if "::chmod(" in telemetry_cpp:
        fail("directory permission tightening still has a pathname TOCTOU")

    submit_body = function_body(ffmpeg_cpp, "int FFmpegVideoDecoder::submitDecodeUnit(PDECODE_UNIT du)")
    overlay_guard = "isOverlayEnabled(Overlay::OverlayDebug)"
    publish_call = "publishStatsTelemetry(lastTwoWndStats)"
    require(submit_body, overlay_guard, "stats telemetry is not guarded by the real overlay state")
    require(submit_body, publish_call, "the overlay rolling window is not published")
    guard_position = submit_body.find(overlay_guard)
    publish_position = submit_body.find(publish_call)
    block_end = submit_body.find("// Accumulate these values into the global stats", guard_position)
    if not (guard_position < publish_position < block_end):
        fail("telemetry publish escaped the OverlayDebug-only branch")

    decoder_thread_body = function_body(ffmpeg_cpp, "void FFmpegVideoDecoder::decoderThreadProc()")
    require(
        ffmpeg_cpp,
        "capabilities |= CAPABILITY_PULL_RENDERER",
        "FFmpeg does not guarantee the single pull-decoder producer",
    )
    require(
        ffmpeg_cpp,
        'SDL_CreateThread(FFmpegVideoDecoder::decoderThreadProcThunk, "FFDecoder"',
        "FFmpeg pull submission is not owned by its single decoder thread",
    )
    require(
        session_cpp,
        "m_VideoCallbacks.submitDecodeUnit = nullptr",
        "pull mode still exposes a concurrent push submission callback",
    )
    if decoder_thread_body.count("submitDecodeUnit(du)") != 2:
        fail("unexpected telemetry producer topology in the FFmpeg decoder thread")
    if ffmpeg_cpp.count("publishStatsTelemetry(lastTwoWndStats)") != 1:
        fail("structured telemetry has more than one production call site")

    require(overlay_h, "IOverlayStateListener", "overlay state listener contract is missing")
    require(
        overlay_cpp,
        "m_StateListener->notifyOverlayStateChanged(type, enabled)",
        "overlay toggles do not synchronously notify telemetry lifecycle",
    )
    require(
        session_cpp,
        "m_StatsTelemetry.setOverlayActive(enabled)",
        "Session does not follow the exact OverlayDebug state",
    )
    require(
        session_cpp,
        "m_OverlayManager.setOverlayState(Overlay::OverlayDebug, false);",
        "session shutdown does not close telemetry through the overlay lifecycle",
    )
    session_exec = function_body(session_cpp, "void Session::exec()")
    telemetry_off_position = session_exec.find(
        "m_OverlayManager.setOverlayState(Overlay::OverlayDebug, false);"
    )
    async_log_exit_position = session_exec.find("StreamUtils::exitAsyncLoggingMode()")
    if not (0 <= telemetry_off_position < async_log_exit_position):
        fail("telemetry stop is not dispatched before Session shutdown advances")
    require(
        prefs_cpp,
        '#define SER_STATS_TELEMETRY "statsTelemetryAvsbdlVariant"',
        "global disable preference is not isolated to the variant",
    )
    require(
        session_cpp,
        'qgetenv("MOONLIGHT_AVSBDL_TELEMETRY")',
        "environment kill switch is missing",
    )

    schema_literals = set(re.findall(r'\\"([a-z0-9_]+)\\"', telemetry_cpp))
    forbidden_schema_keys = {
        "address",
        "host_address",
        "hostname",
        "ip",
        "key",
        "password",
        "remote_address",
        "rikey",
        "token",
    }
    leaked_keys = sorted(schema_literals & forbidden_schema_keys)
    if leaked_keys:
        fail(f"forbidden identity/secret fields in telemetry schema: {', '.join(leaked_keys)}")
    if "m_Computer" in telemetry_h + telemetry_cpp + function_body(
        ffmpeg_cpp, "void FFmpegVideoDecoder::publishStatsTelemetry(const VIDEO_STATS& stats)"
    ):
        fail("host object is reachable from the telemetry implementation")

    required_fields = {
        "session_id",
        "timestamp_utc",
        "width",
        "height",
        "configured_fps",
        "total_fps",
        "codec",
        "hdr",
        "requested_bitrate_kbps",
        "incoming",
        "decode",
        "render",
        "host_processing_ms",
        "network",
        "pacer_jitter",
        "rtt_ms",
        "rtt_variance_ms",
        "decode_ms",
        "frame_queue_ms",
        "render_including_vsync_ms",
        "received_frames",
        "decoded_frames",
        "rendered_frames",
        "total_frames",
        "network_dropped_frames",
        "pacer_dropped_frames",
        "measurement_duration_us",
    }
    missing_fields = sorted(required_fields - schema_literals)
    if missing_fields:
        fail(f"required telemetry fields missing: {', '.join(missing_fields)}")

    for test_name in (
        "offCreatesNoWriterAndNoFiles",
        "immediateOffCannotLoseWorkerWake",
        "overlayOffDoesNotWaitForBlockedWorkerStart",
        "destructionDoesNotWaitForBlockedWorkerStart",
        "concurrentOnAfterLateWithdrawalIsHandedOff",
        "injectedExceptionsAreContained",
        "globalDisableOverridesVisibleOverlay",
        "toggleOnWritesStructuredOverlaySample",
        "toggleOffClosesAsynchronouslyAndStopsWrites",
        "overlayOffPerformsNativeDurableSyncOnWorker",
        "queuedRollingPeaksAreNotCoalesced",
        "queuePressureCreatesObservableSourceSequenceGaps",
        "periodicTickMarksRepeatedSourceAsStale",
        "retoggleCreatesNewSession",
        "rapidRetoggleKeepsBlockedSessionsSeparated",
        "blockedIoDoesNotBlockOverlayOff",
        "blockedIoDoesNotBlockDestruction",
        "repeatedBlockedRetogglesStayResourceBounded",
        "syncFailureIsObservableAndFailOpen",
        "rotationAndPermissionsAreBoundedAndPrivate",
        "directorySymlinkIsRejectedWithoutChangingTargetMode",
        "rotationNeverFollowsOrDeletesTelemetrySymlink",
        "ioFailureIsFailOpenAndObservable",
    ):
        require(tests_cpp, f"void {test_name}()", f"missing lifecycle test: {test_name}")

    for signature, terminal, description in (
        (
            "void blockedIoDoesNotBlockOverlayOff()",
            "telemetry.setOverlayActive(false)",
            "blocked Sync is not entered before overlay OFF",
        ),
        (
            "void blockedIoDoesNotBlockDestruction()",
            "telemetry.reset()",
            "blocked Sync is not entered before destruction",
        ),
        (
            "void rapidRetoggleKeepsBlockedSessionsSeparated()",
            "telemetry.setOverlayActive(false)",
            "blocked Sync is not entered before rapid retoggle",
        ),
        (
            "void repeatedBlockedRetogglesStayResourceBounded()",
            "target->setOverlayActive(false)",
            "blocked Sync is not entered before resource-bound retoggle",
        ),
    ):
        body = function_body(tests_cpp, signature)
        if not (0 <= body.find("publishUntilBlocked") < body.find(terminal)):
            fail(description)
    require(
        tests_cpp,
        "static bool prepareQtTeardownProbe",
        "detached worker is not exercised across QCoreApplication teardown",
    )
    require(
        tests_cpp,
        "syncCallsWhileBlocked + 1",
        "blocked Sync fixture does not prove native fsync runs after OFF",
    )
    late_handoff_test = function_body(
        tests_cpp, "void concurrentOnAfterLateWithdrawalIsHandedOff()"
    )
    ordered_handoff_markers = (
        "waitUntilPublished",
        "setOverlayActive(false)",
        "releasePublished",
        "waitUntilLateWithdrawalReadyToRelease",
        "setOverlayActive(true)",
        "releaseLateWithdrawal",
        "QTRY_VERIFY_WITH_TIMEOUT(telemetry.isSessionActive()",
    )
    cursor = -1
    for marker in ordered_handoff_markers:
        cursor = late_handoff_test.find(marker, cursor + 1)
        if cursor < 0:
            fail(f"late-withdrawal ON handoff fixture is missing ordered barrier: {marker}")
    retoggle_test = function_body(tests_cpp, "void retoggleCreatesNewSession()")
    second_session_position = retoggle_test.find("const QString secondSession")
    second_record_position = retoggle_test.find(
        'record.value(QStringLiteral("session_id")).toString() == secondSession'
    )
    second_off_position = retoggle_test.rfind("telemetry.setOverlayActive(false)")
    if not (0 <= second_session_position < second_record_position < second_off_position):
        fail("retoggle fixture does not wait for the second session record before OFF")
    test_main = function_body(tests_cpp, "int main(int argc, char** argv)")
    if not (0 <= test_main.find("QCoreApplication application") <
            test_main.find("teardownBlocker->unblock()")):
        fail("Qt teardown probe does not release blocked I/O after QCoreApplication scope")

    if re.search(r"(?:VT_METAL_(?:ASYNC|MAX)|vt_metal_latency_config)", telemetry_h + telemetry_cpp):
        fail("Metal async experiment leaked into telemetry")

    print("PASS: overlay-bound, private, bounded, fail-open telemetry source audit")


if __name__ == "__main__":
    main()
