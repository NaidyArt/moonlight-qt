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
        ("m_Options.maxFileBytes", "size rotation is missing"),
        ("m_Options.maxFileCount", "file-count rotation is missing"),
        ("::fsync(m_File->handle())", "clean close lacks a durable flush"),
    ):
        require(telemetry_cpp, literal, description)

    publish_body = function_body(
        telemetry_cpp, "bool StatsTelemetry::publish(const Sample& inputSample) noexcept"
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
        if re.search(pattern, publish_body):
            fail(f"hot-path publish contains forbidden operation: {pattern}")
    require(
        publish_body,
        "m_WritePosition.store(nextPosition, std::memory_order_release)",
        "hot-path publish is not a bounded SPSC handoff",
    )

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

    schema_literals = set(re.findall(r'QStringLiteral\("([^"]+)"\)', telemetry_cpp))
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
        "globalDisableOverridesVisibleOverlay",
        "toggleOnWritesStructuredOverlaySample",
        "toggleOffClosesAndStopsWrites",
        "queuedRollingPeaksAreNotCoalesced",
        "periodicTickMarksRepeatedSourceAsStale",
        "retoggleCreatesNewSession",
        "rotationAndPermissionsAreBoundedAndPrivate",
        "ioFailureIsFailOpen",
    ):
        require(tests_cpp, f"void {test_name}()", f"missing lifecycle test: {test_name}")

    if re.search(r"(?:VT_METAL_(?:ASYNC|MAX)|vt_metal_latency_config)", telemetry_h + telemetry_cpp):
        fail("Metal async experiment leaked into telemetry")

    print("PASS: overlay-bound, private, bounded, fail-open telemetry source audit")


if __name__ == "__main__":
    main()
