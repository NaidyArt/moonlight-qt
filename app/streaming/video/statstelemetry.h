#pragma once

#include <QByteArray>
#include <QMutex>
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
    // submission path and copies it into a lock-free SPSC queue.
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

    struct Options {
        QString directoryPath;
        int sampleIntervalMs = 1000;
        int maxFileCount = 8;
        std::int64_t maxFileBytes = 4 * 1024 * 1024;
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

    // Lock-free and allocation-free. Returns false when telemetry is inactive
    // or the bounded queue is full. Streaming always continues either way.
    bool publish(const Sample& sample) noexcept;

    bool isSessionActive() const noexcept;
    bool isFileOpen() const noexcept;
    QString currentSessionId() const;

private:
    class WriterThread;

    static constexpr std::uint32_t QueueCapacity = 16;

    void reconcileLocked();
    void startLocked();
    void stopLocked();
    bool dequeue(Sample& sample) noexcept;

    Options m_Options;
    StreamConfig m_StreamConfig;
    mutable QMutex m_LifecycleMutex;
    std::unique_ptr<WriterThread> m_Writer;

    std::array<Sample, QueueCapacity> m_Queue = {};
    std::atomic<std::uint32_t> m_WritePosition {0};
    std::atomic<std::uint32_t> m_ReadPosition {0};
    std::atomic<std::uint64_t> m_SourceWindowSequence {0};
    std::atomic<std::uint64_t> m_QueueDroppedSamples {0};
    std::atomic<std::uint64_t> m_SessionGeneration {0};
    std::atomic<std::uint32_t> m_ActivePublishers {0};
    std::atomic<bool> m_AcceptingSamples {false};
    std::atomic<bool> m_GlobalEnabled {false};
    std::atomic<bool> m_OverlayActive {false};
    std::atomic<bool> m_FileOpen {false};
};
