#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

class StatsTelemetry
{
public:
    struct StreamConfig {
        int width = 0;
        int height = 0;
        int configuredFps = 0;
        int requestedBitrateKbps = 0;
    };

    // This is intentionally a fixed-size POD. publish() runs on the video
    // submission path and copies it into a bounded SPSC queue.
    struct Sample {
        std::int64_t capturedAtUnixMs = 0;
        std::uint64_t sourceWindowSequence = 0;
        std::uint64_t measurementStartUs = 0;
        std::uint64_t measurementDurationUs = 0;

        std::array<char, 64> codec = {};
        bool hdr = false;

        double totalFps = 0;
        double receivedFps = 0;
        double decodedFps = 0;
        double renderedFps = 0;

        std::uint32_t receivedFrames = 0;
        std::uint32_t decodedFrames = 0;
        std::uint32_t renderedFrames = 0;
        std::uint32_t totalFrames = 0;
        std::uint32_t networkDroppedFrames = 0;
        std::uint32_t pacerDroppedFrames = 0;
        std::uint16_t minHostProcessingLatency = 0;
        std::uint16_t maxHostProcessingLatency = 0;
        std::uint32_t totalHostProcessingLatency = 0;
        std::uint32_t framesWithHostProcessingLatency = 0;
        std::uint64_t totalReassemblyTimeUs = 0;
        std::uint64_t totalDecodeTimeUs = 0;
        std::uint64_t totalPacerTimeUs = 0;
        std::uint64_t totalRenderTimeUs = 0;
        std::uint32_t lastRttMs = 0;
        std::uint32_t lastRttVarianceMs = 0;
    };

    enum class IoOperation {
        WorkerStart,
        StartPublished,
        StartRelease,
        Write,
        Flush,
        Sync,
    };

    // Test-only fault injection boundary. Production leaves this null. Actual
    // I/O hooks execute exclusively on the worker. WorkerStart and the two
    // Start* lifecycle barriers execute on the starter so tests can force
    // races and prove that OFF never waits for ON.
    class IoFaultInjector
    {
    public:
        virtual ~IoFaultInjector() = default;
        virtual bool beforeIo(IoOperation operation,
                              const std::atomic<bool>& stopRequested) = 0;
    };

    enum class IoError {
        None = 0,
        Directory,
        Rotation,
        Open,
        Write,
        Flush,
        Sync,
        WorkerStart,
        WorkerException,
        OutstandingSessionLimit,
    };

    struct Options {
        QString directoryPath;
        int sampleIntervalMs = 1000;
        int maxFileCount = 8;
        std::int64_t maxFileBytes = 4 * 1024 * 1024;
        std::shared_ptr<IoFaultInjector> ioFaultInjector;
    };

    StatsTelemetry();
    explicit StatsTelemetry(Options options);
    ~StatsTelemetry();

    StatsTelemetry(const StatsTelemetry&) = delete;
    StatsTelemetry& operator=(const StatsTelemetry&) = delete;

    static QString defaultDirectoryPath();
    static bool environmentAllowsTelemetry(const QByteArray& value);

    void configure(const StreamConfig& config);
    void setGloballyEnabled(bool enabled);
    void setOverlayActive(bool active);

    // Allocation-free, lock-free, and I/O-free. Returns false when telemetry
    // is inactive or the bounded queue is full. Streaming always continues.
    bool publish(const Sample& sample) noexcept;

    bool isSessionActive() const noexcept;
    bool isFileOpen() const noexcept;
    QString currentSessionId() const;
    std::uint64_t ioErrorCount() const noexcept;
    IoError lastIoError() const noexcept;
    std::uint64_t completedSessionCount() const noexcept;
    std::uint64_t nativeSyncCallCountForTests() const noexcept;

    // Lifetime observability for regression tests. Detached workers own all
    // of their state, so this may remain non-zero briefly after an async stop.
    static int liveWorkerCountForTests() noexcept;
    static int liveSessionCountForTests() noexcept;

private:
    class ControlState;
    class SessionState;

    static bool shouldRun(const ControlState* control) noexcept;
    static void reconcile(const std::shared_ptr<ControlState>& control) noexcept;
    static void tryStart(const std::shared_ptr<ControlState>& control) noexcept;
    static bool releaseStart(const std::shared_ptr<ControlState>& control,
                             std::uint64_t ownedLifecycleEpoch) noexcept;
    static void stop(ControlState* control) noexcept;

    std::shared_ptr<ControlState> m_Control;
};
