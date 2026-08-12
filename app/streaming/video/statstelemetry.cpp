#include "statstelemetry.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

QString utcTimestamp(std::int64_t unixMs)
{
    return QDateTime::fromMSecsSinceEpoch(unixMs, Qt::UTC).toString(Qt::ISODateWithMs);
}

QJsonValue nullableRatio(double numerator, double denominator)
{
    if (denominator <= 0.0) {
        return QJsonValue(QJsonValue::Null);
    }

    const double value = numerator / denominator;
    return std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}

QJsonValue nullableNumber(double value, bool valid)
{
    return valid && std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}

double jsonInteger(std::uint64_t value)
{
    // Window counters cover roughly two seconds and are therefore far below
    // JSON's exact integer limit (2^53). Keeping this conversion in one place
    // makes that boundary explicit.
    return static_cast<double>(value);
}

}

class StatsTelemetry::WriterThread final : public QThread
{
public:
    WriterThread(StatsTelemetry* owner,
                 const StatsTelemetry::Options& options,
                 const StatsTelemetry::StreamConfig& streamConfig)
        : QThread(nullptr),
          m_Owner(owner),
          m_Options(options),
          m_StreamConfig(streamConfig),
          m_SessionId(QUuid::createUuid().toString(QUuid::WithoutBraces)),
          m_SessionStartedAtUnixMs(QDateTime::currentMSecsSinceEpoch())
    {
        setObjectName("Stats telemetry writer");
    }

    QString sessionId() const
    {
        return m_SessionId;
    }

    void requestStop()
    {
        m_StopRequested.store(true, std::memory_order_release);
        QMutexLocker locker(&m_WaitMutex);
        m_WaitCondition.wakeAll();
    }

protected:
    void run() override
    {
        // QFile is created and destroyed on the writer thread so no QObject
        // with main-thread affinity is used for telemetry I/O.
        m_File = std::make_unique<QFile>();
        if (!prepareDirectory() || !openNextFile()) {
            m_Owner->m_AcceptingSamples.store(false, std::memory_order_release);
            closeResources(false);
            m_File.reset();
            return;
        }

        Sample latestSample = {};
        bool haveLatestSample = false;

        for (;;) {
            {
                QMutexLocker locker(&m_WaitMutex);
                if (!m_StopRequested.load(std::memory_order_acquire)) {
                    m_WaitCondition.wait(&m_WaitMutex, static_cast<unsigned long>(m_Options.sampleIntervalMs));
                }
            }

            Sample queuedSample;
            bool wroteFreshSample = false;
            bool ioSucceeded = true;
            while (m_Owner->dequeue(queuedSample)) {
                latestSample = queuedSample;
                haveLatestSample = true;
                if (!writeSample(queuedSample, false)) {
                    ioSucceeded = false;
                    break;
                }
                wroteFreshSample = true;
            }

            const bool stopping = m_StopRequested.load(std::memory_order_acquire);
            if (ioSucceeded && !stopping && haveLatestSample && !wroteFreshSample) {
                ioSucceeded = writeSample(latestSample, true);
            }

            if (!ioSucceeded) {
                m_Owner->m_AcceptingSamples.store(false, std::memory_order_release);
                break;
            }

            if (stopping) {
                break;
            }
        }

        closeResources(true);
        m_File.reset();
    }

private:
    bool prepareDirectory()
    {
#ifdef Q_OS_UNIX
        const QByteArray encodedPath = QFile::encodeName(m_Options.directoryPath);
        if (::mkdir(encodedPath.constData(), S_IRWXU) != 0 && errno != EEXIST) {
            return false;
        }

        struct stat pathStat = {};
        if (::lstat(encodedPath.constData(), &pathStat) != 0 ||
                !S_ISDIR(pathStat.st_mode) || S_ISLNK(pathStat.st_mode) ||
                pathStat.st_uid != ::geteuid()) {
            return false;
        }

        if (::chmod(encodedPath.constData(), S_IRWXU) != 0) {
            return false;
        }

        m_DirectoryFd = ::open(encodedPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (m_DirectoryFd < 0 || ::fchmod(m_DirectoryFd, S_IRWXU) != 0) {
            return false;
        }
#else
        if (!QDir().mkpath(m_Options.directoryPath)) {
            return false;
        }

        if (!QFile::setPermissions(m_Options.directoryPath,
                                   QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
            return false;
        }
#endif

        return true;
    }

    bool pruneForNewFile()
    {
        QDir directory(m_Options.directoryPath);
        QFileInfoList files = directory.entryInfoList(
            QStringList(QStringLiteral("telemetry-*.jsonl")),
            QDir::Files | QDir::NoSymLinks,
            QDir::Time | QDir::Reversed);

        while (files.size() >= m_Options.maxFileCount) {
            const QFileInfo oldest = files.takeFirst();
            if (oldest.isSymLink() || !oldest.isFile() || !QFile::remove(oldest.absoluteFilePath())) {
                return false;
            }
        }

        return true;
    }

    bool openNextFile()
    {
        if (!pruneForNewFile()) {
            return false;
        }

        const QString startStamp = QDateTime::fromMSecsSinceEpoch(m_SessionStartedAtUnixMs, Qt::UTC)
                                       .toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz'Z'"));
        const QString fileName = QStringLiteral("telemetry-%1-%2-p%3.jsonl")
                                     .arg(startStamp,
                                          m_SessionId,
                                          QString::number(m_FileSequence++).rightJustified(3, QLatin1Char('0')));

#ifdef Q_OS_UNIX
        const QByteArray encodedName = QFile::encodeName(fileName);
        const int fd = ::openat(m_DirectoryFd,
                                encodedName.constData(),
                                O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                                S_IRUSR | S_IWUSR);
        if (fd < 0 || ::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
            if (fd >= 0) {
                ::close(fd);
                ::unlinkat(m_DirectoryFd, encodedName.constData(), 0);
            }
            return false;
        }

        if (!m_File->open(fd,
                          QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text,
                          QFileDevice::AutoCloseHandle)) {
            ::close(fd);
            return false;
        }
#else
        m_File->setFileName(QDir(m_Options.directoryPath).absoluteFilePath(fileName));
        if (!m_File->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text) ||
                !m_File->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            m_File->close();
            return false;
        }
#endif

        m_Owner->m_FileOpen.store(true, std::memory_order_release);
        return true;
    }

    QByteArray serializeSample(const Sample& sample, bool sourceStale)
    {
        const std::int64_t emittedAtUnixMs = QDateTime::currentMSecsSinceEpoch();
        const std::int64_t sourceAgeMs = std::max<std::int64_t>(0, emittedAtUnixMs - sample.capturedAtUnixMs);

        QJsonObject stream;
        stream.insert(QStringLiteral("width"), m_StreamConfig.width);
        stream.insert(QStringLiteral("height"), m_StreamConfig.height);
        stream.insert(QStringLiteral("configured_fps"), m_StreamConfig.configuredFps);
        stream.insert(QStringLiteral("total_fps"), sample.totalFps);
        const auto codecEnd = std::find(sample.codec.cbegin(), sample.codec.cend(), '\0');
        stream.insert(QStringLiteral("codec"),
                      QString::fromLatin1(sample.codec.data(),
                                          static_cast<int>(std::distance(sample.codec.cbegin(), codecEnd))));
        stream.insert(QStringLiteral("hdr"), sample.hdr);
        stream.insert(QStringLiteral("requested_bitrate_kbps"), m_StreamConfig.requestedBitrateKbps);

        QJsonObject fps;
        fps.insert(QStringLiteral("incoming"), sample.receivedFps);
        fps.insert(QStringLiteral("decode"), sample.decodedFps);
        fps.insert(QStringLiteral("render"), sample.renderedFps);

        const bool haveHostLatency = sample.framesWithHostProcessingLatency > 0;
        QJsonObject hostProcessing;
        hostProcessing.insert(QStringLiteral("min"),
                              nullableNumber(static_cast<double>(sample.minHostProcessingLatency) / 10.0,
                                             haveHostLatency));
        hostProcessing.insert(QStringLiteral("max"),
                              nullableNumber(static_cast<double>(sample.maxHostProcessingLatency) / 10.0,
                                             haveHostLatency));
        hostProcessing.insert(QStringLiteral("average"),
                              haveHostLatency ?
                                  nullableRatio(static_cast<double>(sample.totalHostProcessingLatency) / 10.0,
                                                sample.framesWithHostProcessingLatency) :
                                  QJsonValue(QJsonValue::Null));

        QJsonObject drops;
        drops.insert(QStringLiteral("network"),
                     nullableRatio(static_cast<double>(sample.networkDroppedFrames) * 100.0,
                                   sample.totalFrames));
        drops.insert(QStringLiteral("pacer_jitter"),
                     nullableRatio(static_cast<double>(sample.pacerDroppedFrames) * 100.0,
                                   sample.decodedFrames));

        const bool haveRtt = sample.lastRttMs != 0;
        QJsonObject network;
        network.insert(QStringLiteral("rtt_ms"),
                       haveRtt ? QJsonValue(static_cast<int>(sample.lastRttMs)) : QJsonValue(QJsonValue::Null));
        network.insert(QStringLiteral("rtt_variance_ms"),
                       haveRtt ? QJsonValue(static_cast<int>(sample.lastRttVarianceMs)) : QJsonValue(QJsonValue::Null));

        QJsonObject timing;
        timing.insert(QStringLiteral("decode_ms"),
                      nullableRatio(static_cast<double>(sample.totalDecodeTimeUs) / 1000.0,
                                    sample.decodedFrames));
        timing.insert(QStringLiteral("frame_queue_ms"),
                      nullableRatio(static_cast<double>(sample.totalPacerTimeUs) / 1000.0,
                                    sample.renderedFrames));
        timing.insert(QStringLiteral("render_including_vsync_ms"),
                      nullableRatio(static_cast<double>(sample.totalRenderTimeUs) / 1000.0,
                                    sample.renderedFrames));

        QJsonObject raw;
        raw.insert(QStringLiteral("received_frames"), jsonInteger(sample.receivedFrames));
        raw.insert(QStringLiteral("decoded_frames"), jsonInteger(sample.decodedFrames));
        raw.insert(QStringLiteral("rendered_frames"), jsonInteger(sample.renderedFrames));
        raw.insert(QStringLiteral("total_frames"), jsonInteger(sample.totalFrames));
        raw.insert(QStringLiteral("network_dropped_frames"), jsonInteger(sample.networkDroppedFrames));
        raw.insert(QStringLiteral("pacer_dropped_frames"), jsonInteger(sample.pacerDroppedFrames));
        raw.insert(QStringLiteral("frames_with_host_processing_latency"),
                   jsonInteger(sample.framesWithHostProcessingLatency));
        raw.insert(QStringLiteral("total_host_processing_latency_tenths_ms"),
                   jsonInteger(sample.totalHostProcessingLatency));
        raw.insert(QStringLiteral("total_reassembly_time_us"), jsonInteger(sample.totalReassemblyTimeUs));
        raw.insert(QStringLiteral("total_decode_time_us"), jsonInteger(sample.totalDecodeTimeUs));
        raw.insert(QStringLiteral("total_frame_queue_time_us"), jsonInteger(sample.totalPacerTimeUs));
        raw.insert(QStringLiteral("total_render_time_us"), jsonInteger(sample.totalRenderTimeUs));
        raw.insert(QStringLiteral("measurement_start_monotonic_us"), jsonInteger(sample.measurementStartUs));
        raw.insert(QStringLiteral("measurement_duration_us"), jsonInteger(sample.measurementDurationUs));
        raw.insert(QStringLiteral("telemetry_queue_dropped_samples"),
                   jsonInteger(m_Owner->m_QueueDroppedSamples.load(std::memory_order_relaxed)));

        QJsonObject root;
        root.insert(QStringLiteral("schema_version"), 1);
        root.insert(QStringLiteral("session_id"), m_SessionId);
        root.insert(QStringLiteral("sample_sequence"), jsonInteger(++m_OutputSequence));
        root.insert(QStringLiteral("source_window_sequence"), jsonInteger(sample.sourceWindowSequence));
        root.insert(QStringLiteral("timestamp_utc"), utcTimestamp(emittedAtUnixMs));
        root.insert(QStringLiteral("timestamp_unix_ms"), jsonInteger(emittedAtUnixMs));
        root.insert(QStringLiteral("source_window_timestamp_utc"), utcTimestamp(sample.capturedAtUnixMs));
        root.insert(QStringLiteral("source_age_ms"), jsonInteger(sourceAgeMs));
        root.insert(QStringLiteral("source_stale"), sourceStale);
        root.insert(QStringLiteral("stream"), stream);
        root.insert(QStringLiteral("fps"), fps);
        root.insert(QStringLiteral("host_processing_ms"), hostProcessing);
        root.insert(QStringLiteral("drops_percent"), drops);
        root.insert(QStringLiteral("network"), network);
        root.insert(QStringLiteral("timing"), timing);
        root.insert(QStringLiteral("raw"), raw);

        QByteArray line = QJsonDocument(root).toJson(QJsonDocument::Compact);
        line.append('\n');
        return line;
    }

    bool writeSample(const Sample& sample, bool sourceStale)
    {
        const QByteArray line = serializeSample(sample, sourceStale);
        if (m_File->size() > 0 && m_File->size() + line.size() > m_Options.maxFileBytes) {
            closeFile(true);
            if (!openNextFile()) {
                return false;
            }
        }

        if (m_File->write(line) != line.size() || !m_File->flush()) {
            return false;
        }

        return true;
    }

    void closeFile(bool durableFlush)
    {
        if (!m_File || !m_File->isOpen()) {
            m_Owner->m_FileOpen.store(false, std::memory_order_release);
            return;
        }

        m_File->flush();
#ifdef Q_OS_UNIX
        if (durableFlush) {
            ::fsync(m_File->handle());
        }
#else
        Q_UNUSED(durableFlush);
#endif
        m_File->close();
        m_Owner->m_FileOpen.store(false, std::memory_order_release);
    }

    void closeResources(bool durableFlush)
    {
        closeFile(durableFlush);
#ifdef Q_OS_UNIX
        if (m_DirectoryFd >= 0) {
            ::close(m_DirectoryFd);
            m_DirectoryFd = -1;
        }
#endif
    }

    StatsTelemetry* const m_Owner;
    const StatsTelemetry::Options m_Options;
    const StatsTelemetry::StreamConfig m_StreamConfig;
    const QString m_SessionId;
    const std::int64_t m_SessionStartedAtUnixMs;
    std::atomic<bool> m_StopRequested {false};
    QMutex m_WaitMutex;
    QWaitCondition m_WaitCondition;
    std::unique_ptr<QFile> m_File;
    std::uint64_t m_OutputSequence = 0;
    int m_FileSequence = 0;
#ifdef Q_OS_UNIX
    int m_DirectoryFd = -1;
#endif
};

StatsTelemetry::StatsTelemetry()
    : StatsTelemetry(Options())
{
}

StatsTelemetry::StatsTelemetry(Options options)
    : m_Options(std::move(options))
{
    if (m_Options.directoryPath.isEmpty()) {
        m_Options.directoryPath = defaultDirectoryPath();
    }
    m_Options.sampleIntervalMs = std::max(1, m_Options.sampleIntervalMs);
    m_Options.maxFileCount = std::max(1, m_Options.maxFileCount);
    m_Options.maxFileBytes = std::max<std::int64_t>(1024, m_Options.maxFileBytes);
}

StatsTelemetry::~StatsTelemetry()
{
    QMutexLocker locker(&m_LifecycleMutex);
    m_OverlayActive.store(false, std::memory_order_release);
    stopLocked();
}

QString StatsTelemetry::defaultDirectoryPath()
{
    return QDir::home().absoluteFilePath(QStringLiteral("Library/Logs/Moonlight AVSampleBuffer"));
}

bool StatsTelemetry::environmentAllowsTelemetry(const QByteArray& value)
{
    const QByteArray normalized = value.trimmed().toLower();
    return normalized.isEmpty() ||
           (normalized != "0" && normalized != "false" && normalized != "off" && normalized != "no");
}

void StatsTelemetry::configure(const StreamConfig& config)
{
    QMutexLocker locker(&m_LifecycleMutex);
    if (m_Writer) {
        return;
    }
    m_StreamConfig = config;
}

void StatsTelemetry::setGloballyEnabled(bool enabled)
{
    QMutexLocker locker(&m_LifecycleMutex);
    m_GlobalEnabled.store(enabled, std::memory_order_release);
    reconcileLocked();
}

void StatsTelemetry::setOverlayActive(bool active)
{
    QMutexLocker locker(&m_LifecycleMutex);
    m_OverlayActive.store(active, std::memory_order_release);
    reconcileLocked();
}

bool StatsTelemetry::publish(const Sample& inputSample) noexcept
{
    const std::uint64_t generation = m_SessionGeneration.load(std::memory_order_acquire);
    m_ActivePublishers.fetch_add(1, std::memory_order_acq_rel);
    if (!m_AcceptingSamples.load(std::memory_order_acquire) ||
            generation != m_SessionGeneration.load(std::memory_order_acquire)) {
        m_ActivePublishers.fetch_sub(1, std::memory_order_release);
        return false;
    }

    const std::uint32_t writePosition = m_WritePosition.load(std::memory_order_relaxed);
    const std::uint32_t nextPosition = (writePosition + 1) % QueueCapacity;
    if (nextPosition == m_ReadPosition.load(std::memory_order_acquire)) {
        m_QueueDroppedSamples.fetch_add(1, std::memory_order_relaxed);
        m_ActivePublishers.fetch_sub(1, std::memory_order_release);
        return false;
    }

    Sample sample = inputSample;
    sample.sourceWindowSequence = m_SourceWindowSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    m_Queue[writePosition] = sample;
    m_WritePosition.store(nextPosition, std::memory_order_release);
    m_ActivePublishers.fetch_sub(1, std::memory_order_release);
    return true;
}

bool StatsTelemetry::isSessionActive() const noexcept
{
    return m_AcceptingSamples.load(std::memory_order_acquire);
}

bool StatsTelemetry::isFileOpen() const noexcept
{
    return m_FileOpen.load(std::memory_order_acquire);
}

QString StatsTelemetry::currentSessionId() const
{
    QMutexLocker locker(&m_LifecycleMutex);
    return m_Writer ? m_Writer->sessionId() : QString();
}

void StatsTelemetry::reconcileLocked()
{
    const bool shouldRun = m_GlobalEnabled.load(std::memory_order_acquire) &&
                           m_OverlayActive.load(std::memory_order_acquire);
    if (shouldRun && !m_Writer) {
        startLocked();
    }
    else if (!shouldRun && m_Writer) {
        stopLocked();
    }
}

void StatsTelemetry::startLocked()
{
    m_WritePosition.store(0, std::memory_order_relaxed);
    m_ReadPosition.store(0, std::memory_order_relaxed);
    m_SourceWindowSequence.store(0, std::memory_order_relaxed);
    m_QueueDroppedSamples.store(0, std::memory_order_relaxed);
    m_FileOpen.store(false, std::memory_order_relaxed);

    m_Writer = std::make_unique<WriterThread>(this, m_Options, m_StreamConfig);
    m_SessionGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_AcceptingSamples.store(true, std::memory_order_release);
    m_Writer->start(QThread::LowPriority);
}

void StatsTelemetry::stopLocked()
{
    if (!m_Writer) {
        m_AcceptingSamples.store(false, std::memory_order_release);
        m_FileOpen.store(false, std::memory_order_release);
        return;
    }

    m_AcceptingSamples.store(false, std::memory_order_release);
    m_SessionGeneration.fetch_add(1, std::memory_order_acq_rel);
    while (m_ActivePublishers.load(std::memory_order_acquire) != 0) {
        QThread::yieldCurrentThread();
    }

    m_Writer->requestStop();
    m_Writer->wait();
    m_Writer.reset();
    m_FileOpen.store(false, std::memory_order_release);
}

bool StatsTelemetry::dequeue(Sample& sample) noexcept
{
    const std::uint32_t readPosition = m_ReadPosition.load(std::memory_order_relaxed);
    if (readPosition == m_WritePosition.load(std::memory_order_acquire)) {
        return false;
    }

    sample = m_Queue[readPosition];
    m_ReadPosition.store((readPosition + 1) % QueueCapacity, std::memory_order_release);
    return true;
}
