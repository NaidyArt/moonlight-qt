# Moonlight AVSampleBuffer macOS candidate

## Scope and provenance

- Clean upstream base: `moonlight-stream/moonlight-qt` commit
  `2e13ed9977bc31c73caf8428f08f58d793313ece`.
- The separate `codex/macos-m2max-latency-ab` working tree and its experimental
  asynchronous Metal changes are not merged into this candidate.
- Target: MacBook Pro M2 Max, macOS 13 or newer, universal `arm64` + `x86_64`.
- Build inputs: GitHub `macos-26`, Qt 6.11.1, Xcode from the runner image,
  `create-dmg` 6.0.0, and the exact upstream submodule commits.

The runner image and Apple toolchain are not content-addressed, so the build
recipe is repeatable but the DMG is not claimed to be byte-for-byte
reproducible across runner-image updates. Every produced artifact is bound by
`SHA256SUMS.txt`.

## Deliberate changes

1. macOS uses `RS_AVSBDL` when no renderer preference exists.
2. Renderer selection remains visible and persistent under the variant-only
   `rendererAvsbdlVariant` key. A prior Auto/Metal choice from official
   Moonlight is neither imported nor overwritten.
3. Bundle ID is `art.naidy.moonlight-avsamplebuffer`, display name is
   `Moonlight AVSampleBuffer`, and the installed bundle path is distinct.
4. QSettings application name remains `Moonlight` so existing pairings, hosts,
   and the validated 3456x2160/120/HEVC 10-bit HDR profile remain available;
   only renderer selection is isolated.
5. CI applies and verifies an ad-hoc signature. Notarization is not claimed.
6. While the performance overlay is actually visible, the variant can write a
   private, bounded 1 Hz JSONL stream containing the same rolling statistics.
   Hiding the overlay synchronously flushes and closes it; see
   `docs/MACOS_AVSBDL_TELEMETRY.md` for the exact schema and rollback.

## Measured reason for the default

On the target M2 Max, stable Metal received and decoded 118.23 FPS but rendered
83.74 FPS, with 30.42% pacing drops and 21.65 ms average frame-queue delay.
The same workload through AVSampleBufferDisplayLayer produced 119.98 FPS for
network, decode, and render, 0% drops, 0.01 ms queue delay, and 0.39 ms render
time while retaining HEVC 10-bit HDR.

## Build and verification

Run the manually dispatched `Build macOS AVSampleBuffer candidate` workflow.
It performs:

- source-isolation checks;
- structured telemetry source audit and macOS unit tests;
- a universal Release build using Moonlight's official dependency and
  deployment path; its script stops after `macdeployqt`, before upstream
  signing/DMG, and hands the staged app to the isolated variant packager;
- ad-hoc signing after renaming the app bundle;
- bundle ID, display name, architecture, and deep-signature verification;
- equality of the executable SHA-256 in the staged app, DMG, and app ZIP;
- DMG and app ZIP packaging;
- SHA-256 manifest verification; and
- upload of the installer, portable app ZIP, recovery uninstaller, and README.

Local platform-independent checks:

```sh
bash scripts/verify-macos-avsbdl-source.sh
sh tests/macos/test-avsbdl-packaging.sh
python3 scripts/verify-macos-avsbdl-telemetry-source.py
```

## Mac-side acceptance gate

Do not replace `/Applications/Moonlight.app`. Install the candidate beside it,
then verify the DMG hash, bundle identity, signature, renderer log, and the same
3456x2160/120/HEVC 10-bit HDR stream. Acceptance requires incoming, decoded,
and rendered rates to remain aligned at roughly 120 FPS with zero pacing drops
and no material queue regression.

The candidate is ad-hoc signed and not notarized because no Apple Developer ID
certificate or notary credentials are available. First launch may require
Finder's **Open** confirmation; disabling Gatekeeper globally is not an
acceptable workaround.
