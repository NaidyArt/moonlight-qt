#include "streaming/video/statstelemetry.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

class BlockingIo final : public StatsTelemetry::IoFaultInjector
{
public:
    explicit BlockingIo(StatsTelemetry::IoOperation operation)
        : m_Operation(operation)
    {
    }

    bool beforeIo(StatsTelemetry::IoOperation operation,
                  const std::atomic<bool>&) override
    {
        if (operation == m_Operation && !m_DidBlock.exchange(true, std::memory_order_acq_rel)) {
            m_Entered.release();
            m_Release.acquire();
        }
        return true;
    }

    bool waitUntilEntered(int timeoutMs)
    {
        return m_Entered.tryAcquire(1, timeoutMs);
    }

    void unblock()
    {
        if (!m_Released.exchange(true, std::memory_order_acq_rel)) {
            m_Release.release();
        }
    }

private:
    const StatsTelemetry::IoOperation m_Operation;
    std::atomic<bool> m_DidBlock {false};
    std::atomic<bool> m_Released {false};
    QSemaphore m_Entered;
    QSemaphore m_Release;
};

class RejectingIo final : public StatsTelemetry::IoFaultInjector
{
public:
    explicit RejectingIo(StatsTelemetry::IoOperation operation)
        : m_Operation(operation)
    {
    }

    bool beforeIo(StatsTelemetry::IoOperation operation,
                  const std::atomic<bool>&) override
    {
        return operation != m_Operation;
    }

private:
    const StatsTelemetry::IoOperation m_Operation;
};

class ThrowingIo final : public StatsTelemetry::IoFaultInjector
{
public:
    explicit ThrowingIo(StatsTelemetry::IoOperation operation)
        : m_Operation(operation)
    {
    }

    bool beforeIo(StatsTelemetry::IoOperation operation,
                  const std::atomic<bool>&) override
    {
        if (operation == m_Operation) {
            throw std::runtime_error("injected telemetry exception");
        }
        return true;
    }

private:
    const StatsTelemetry::IoOperation m_Operation;
};

class BlockingIoReleaseGuard
{
public:
    explicit BlockingIoReleaseGuard(std::shared_ptr<BlockingIo> blocker)
        : m_Blocker(std::move(blocker))
    {
    }

    ~BlockingIoReleaseGuard()
    {
        m_Blocker->unblock();
    }

private:
    const std::shared_ptr<BlockingIo> m_Blocker;
};

class BlockingThreadGuard
{
public:
    BlockingThreadGuard(std::shared_ptr<BlockingIo> blocker, std::thread& thread)
        : m_Blocker(std::move(blocker)),
          m_Thread(thread)
    {
    }

    ~BlockingThreadGuard()
    {
        m_Blocker->unblock();
        if (m_Thread.joinable()) {
            m_Thread.join();
        }
    }

private:
    const std::shared_ptr<BlockingIo> m_Blocker;
    std::thread& m_Thread;
};

class LateStartHandoffIo final : public StatsTelemetry::IoFaultInjector
{
public:
    bool beforeIo(StatsTelemetry::IoOperation operation,
                  const std::atomic<bool>&) override
    {
        if (operation == StatsTelemetry::IoOperation::StartPublished &&
                !m_PublishedBlocked.exchange(true, std::memory_order_acq_rel)) {
            m_PublishedEntered.release();
            m_PublishedRelease.acquire();
        }
        else if (operation == StatsTelemetry::IoOperation::StartRelease &&
                 !m_ReleaseBlocked.exchange(true, std::memory_order_acq_rel)) {
            m_ReleaseEntered.release();
            m_ReleaseRelease.acquire();
        }
        return true;
    }

    bool waitUntilPublished(int timeoutMs)
    {
        return m_PublishedEntered.tryAcquire(1, timeoutMs);
    }

    void releasePublished()
    {
        if (!m_PublishedReleased.exchange(true, std::memory_order_acq_rel)) {
            m_PublishedRelease.release();
        }
    }

    bool waitUntilLateWithdrawalReadyToRelease(int timeoutMs)
    {
        return m_ReleaseEntered.tryAcquire(1, timeoutMs);
    }

    void releaseLateWithdrawal()
    {
        if (!m_ReleaseReleased.exchange(true, std::memory_order_acq_rel)) {
            m_ReleaseRelease.release();
        }
    }

private:
    std::atomic<bool> m_PublishedBlocked {false};
    std::atomic<bool> m_PublishedReleased {false};
    std::atomic<bool> m_ReleaseBlocked {false};
    std::atomic<bool> m_ReleaseReleased {false};
    QSemaphore m_PublishedEntered;
    QSemaphore m_PublishedRelease;
    QSemaphore m_ReleaseEntered;
    QSemaphore m_ReleaseRelease;
};

class LateStartHandoffGuard
{
public:
    LateStartHandoffGuard(std::shared_ptr<LateStartHandoffIo> blocker,
                          std::thread& thread)
        : m_Blocker(std::move(blocker)),
          m_Thread(thread)
    {
    }

    ~LateStartHandoffGuard()
    {
        m_Blocker->releasePublished();
        m_Blocker->releaseLateWithdrawal();
        if (m_Thread.joinable()) {
            m_Thread.join();
        }
    }

private:
    const std::shared_ptr<LateStartHandoffIo> m_Blocker;
    std::thread& m_Thread;
};

class BlockingEveryIo final : public StatsTelemetry::IoFaultInjector
{
public:
    explicit BlockingEveryIo(StatsTelemetry::IoOperation operation)
        : m_Operation(operation)
    {
    }

    bool beforeIo(StatsTelemetry::IoOperation operation,
                  const std::atomic<bool>&) override
    {
        if (operation == m_Operation && m_Blocking.load(std::memory_order_acquire)) {
            m_Entered.release();
            m_Release.acquire();
        }
        return true;
    }

    bool waitUntilNextEntered(int timeoutMs)
    {
        return m_Entered.tryAcquire(1, timeoutMs);
    }

    void unblockAll()
    {
        if (m_Blocking.exchange(false, std::memory_order_acq_rel)) {
            m_Release.release(16);
        }
    }

private:
    const StatsTelemetry::IoOperation m_Operation;
    std::atomic<bool> m_Blocking {true};
    QSemaphore m_Entered;
    QSemaphore m_Release;
};

class BlockingEveryIoReleaseGuard
{
public:
    explicit BlockingEveryIoReleaseGuard(std::shared_ptr<BlockingEveryIo> blocker)
        : m_Blocker(std::move(blocker))
    {
    }

    ~BlockingEveryIoReleaseGuard()
    {
        m_Blocker->unblockAll();
    }

private:
    const std::shared_ptr<BlockingEveryIo> m_Blocker;
};

class StdBlockingIo final : public StatsTelemetry::IoFaultInjector
{
public:
    bool beforeIo(StatsTelemetry::IoOperation operation,
                  const std::atomic<bool>&) override
    {
        if (operation != StatsTelemetry::IoOperation::Write) {
            return true;
        }

        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Entered = true;
        m_Condition.notify_all();
        m_Condition.wait(lock, [this]() {
            return m_Released;
        });
        return true;
    }

    bool waitUntilEntered(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        return m_Condition.wait_for(lock, timeout, [this]() {
            return m_Entered;
        });
    }

    void unblock()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Released = true;
        m_Condition.notify_all();
    }

private:
    std::mutex m_Mutex;
    std::condition_variable m_Condition;
    bool m_Entered = false;
    bool m_Released = false;
};

class StatsTelemetryTest : public QObject
{
    Q_OBJECT

private:
    static StatsTelemetry::Options optionsFor(const QTemporaryDir& root,
                                              int intervalMs = 20,
                                              int maxFiles = 8,
                                              qint64 maxBytes = 4 * 1024 * 1024)
    {
        StatsTelemetry::Options options;
        options.directoryPath = QDir(root.path()).absoluteFilePath(QStringLiteral("Moonlight AVSampleBuffer"));
        options.sampleIntervalMs = intervalMs;
        options.maxFileCount = maxFiles;
        options.maxFileBytes = maxBytes;
        return options;
    }

    static StatsTelemetry::StreamConfig streamConfig()
    {
        StatsTelemetry::StreamConfig config;
        config.width = 3456;
        config.height = 2160;
        config.configuredFps = 120;
        config.requestedBitrateKbps = 350000;
        return config;
    }

    static StatsTelemetry::Sample sample()
    {
        StatsTelemetry::Sample value;
        value.capturedAtUnixMs = QDateTime::currentMSecsSinceEpoch();
        value.measurementStartUs = 1000000;
        value.measurementDurationUs = 2000000;
        std::strcpy(value.codec.data(), "HEVC 10-bit HDR");
        value.hdr = true;
        value.totalFps = 120.25;
        value.receivedFps = 114.0;
        value.decodedFps = 113.5;
        value.renderedFps = 113.0;
        value.receivedFrames = 190;
        value.decodedFrames = 190;
        value.renderedFrames = 188;
        value.totalFrames = 200;
        value.networkDroppedFrames = 10;
        value.pacerDroppedFrames = 3;
        value.minHostProcessingLatency = 31;
        value.maxHostProcessingLatency = 43;
        value.totalHostProcessingLatency = 640;
        value.framesWithHostProcessingLatency = 20;
        value.totalReassemblyTimeUs = 1111;
        value.totalDecodeTimeUs = 1472500;
        value.totalPacerTimeUs = 1880;
        value.totalRenderTimeUs = 15040;
        value.lastRttMs = 5;
        value.lastRttVarianceMs = 1;
        return value;
    }

    static QStringList telemetryFiles(const QString& directoryPath)
    {
        return QDir(directoryPath).entryList(QStringList(QStringLiteral("telemetry-*.jsonl")),
                                             QDir::Files | QDir::NoSymLinks,
                                             QDir::Name);
    }

    static QList<QJsonObject> readRecords(const QString& directoryPath)
    {
        QList<QJsonObject> records;
        const QDir directory(directoryPath);
        for (const QString& fileName : telemetryFiles(directoryPath)) {
            QFile file(directory.absoluteFilePath(fileName));
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                continue;
            }
            for (const QByteArray& line : file.readAll().split('\n')) {
                if (line.isEmpty()) {
                    continue;
                }
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(line, &error);
                if (error.error == QJsonParseError::NoError && document.isObject()) {
                    records.append(document.object());
                }
            }
        }
        return records;
    }

    static qint64 totalTelemetryBytes(const QString& directoryPath)
    {
        qint64 bytes = 0;
        const QDir directory(directoryPath);
        for (const QString& fileName : telemetryFiles(directoryPath)) {
            bytes += QFileInfo(directory.absoluteFilePath(fileName)).size();
        }
        return bytes;
    }

    static bool publishUntilBlocked(StatsTelemetry& telemetry,
                                    BlockingIo& blocker,
                                    const StatsTelemetry::Sample& value)
    {
        for (int attempt = 0; attempt < 64; ++attempt) {
            (void)telemetry.publish(value);
            if (blocker.waitUntilEntered(50)) {
                return true;
            }
        }
        return false;
    }

    static bool publishUntilBlocked(StatsTelemetry& telemetry,
                                    BlockingEveryIo& blocker,
                                    const StatsTelemetry::Sample& value)
    {
        for (int attempt = 0; attempt < 64; ++attempt) {
            (void)telemetry.publish(value);
            if (blocker.waitUntilNextEntered(50)) {
                return true;
            }
        }
        return false;
    }

private slots:
    void cleanup()
    {
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveWorkerCountForTests(), 0, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveSessionCountForTests(), 0, 3000);
    }

    void offCreatesNoWriterAndNoFiles()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto options = optionsFor(root);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);

        QVERIFY(!telemetry.isSessionActive());
        QVERIFY(!telemetry.isFileOpen());
        QVERIFY(!telemetry.publish(sample()));
        QTest::qWait(options.sampleIntervalMs * 3);
        QVERIFY(!QFileInfo::exists(options.directoryPath));
        QCOMPARE(StatsTelemetry::liveWorkerCountForTests(), 0);
    }

    void immediateOffCannotLoseWorkerWake()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto options = optionsFor(root, 20);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);

        for (int attempt = 0; attempt < 20; ++attempt) {
            telemetry.setOverlayActive(true);
            telemetry.setOverlayActive(false);
            QVERIFY(!telemetry.isSessionActive());
            QVERIFY(!telemetry.publish(sample()));
            QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveWorkerCountForTests(), 0, 1000);
            QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveSessionCountForTests(), 0, 1000);
        }
    }

    void overlayOffDoesNotWaitForBlockedWorkerStart()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto blocker = std::make_shared<BlockingIo>(StatsTelemetry::IoOperation::WorkerStart);
        BlockingIoReleaseGuard releaseGuard(blocker);
        auto options = optionsFor(root, 20);
        options.ioFaultInjector = blocker;
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);

        std::thread starter([&telemetry]() {
            telemetry.setOverlayActive(true);
        });
        BlockingThreadGuard starterGuard(blocker, starter);
        QVERIFY(blocker->waitUntilEntered(2000));

        QElapsedTimer timer;
        timer.start();
        telemetry.setOverlayActive(false);
        QVERIFY2(timer.elapsed() < 250, "overlay OFF waited for a blocked ON/start path");
        QVERIFY(!telemetry.isSessionActive());
        QVERIFY(!telemetry.publish(sample()));

        blocker->unblock();
        starter.join();
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveWorkerCountForTests(), 0, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveSessionCountForTests(), 0, 2000);
        QVERIFY(!QFileInfo::exists(options.directoryPath));
    }

    void destructionDoesNotWaitForBlockedWorkerStart()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto blocker = std::make_shared<BlockingIo>(StatsTelemetry::IoOperation::WorkerStart);
        BlockingIoReleaseGuard releaseGuard(blocker);
        auto options = optionsFor(root, 20);
        options.ioFaultInjector = blocker;
        auto telemetry = std::make_unique<StatsTelemetry>(options);
        telemetry->configure(streamConfig());
        telemetry->setGloballyEnabled(true);

        StatsTelemetry* const rawTelemetry = telemetry.get();
        std::thread starter([rawTelemetry]() {
            rawTelemetry->setOverlayActive(true);
        });
        BlockingThreadGuard starterGuard(blocker, starter);
        QVERIFY(blocker->waitUntilEntered(2000));

        // At this point setOverlayActive() has retained ControlState and no
        // longer dereferences the facade. Its destruction must only flip
        // atomics and detach a published session, never wait for this start.
        QElapsedTimer timer;
        timer.start();
        telemetry.reset();
        QVERIFY2(timer.elapsed() < 250, "destruction waited for a blocked ON/start path");

        blocker->unblock();
        starter.join();
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveWorkerCountForTests(), 0, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveSessionCountForTests(), 0, 2000);
        QVERIFY(!QFileInfo::exists(options.directoryPath));
    }

    void concurrentOnAfterLateWithdrawalIsHandedOff()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto blocker = std::make_shared<LateStartHandoffIo>();
        auto options = optionsFor(root, 20);
        options.ioFaultInjector = blocker;
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);

        std::thread oldStarter([&telemetry]() {
            telemetry.setOverlayActive(true);
        });
        LateStartHandoffGuard starterGuard(blocker, oldStarter);
        QVERIFY(blocker->waitUntilPublished(2000));

        // Force the already-published old starter to take its late-withdrawal
        // path, then stop it exactly after that decision but before it releases
        // startInProgress.
        telemetry.setOverlayActive(false);
        QVERIFY(!telemetry.isSessionActive());
        blocker->releasePublished();
        QVERIFY(blocker->waitUntilLateWithdrawalReadyToRelease(2000));

        // This ON observes startInProgress=true and cannot become the owner.
        // The old owner must consume the lifecycle epoch after releasing it.
        telemetry.setOverlayActive(true);
        QVERIFY(!telemetry.isSessionActive());
        blocker->releaseLateWithdrawal();
        oldStarter.join();

        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isSessionActive(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        const QString handedOffSession = telemetry.currentSessionId();
        QVERIFY(!handedOffSession.isEmpty());
        QVERIFY(telemetry.publish(sample()));
        QTRY_VERIFY_WITH_TIMEOUT(!readRecords(options.directoryPath).isEmpty(), 2000);

        telemetry.setOverlayActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);
    }

    void injectedExceptionsAreContained_data()
    {
        QTest::addColumn<int>("operation");
        QTest::newRow("worker-start") << static_cast<int>(StatsTelemetry::IoOperation::WorkerStart);
        QTest::newRow("write") << static_cast<int>(StatsTelemetry::IoOperation::Write);
        QTest::newRow("flush") << static_cast<int>(StatsTelemetry::IoOperation::Flush);
        QTest::newRow("sync") << static_cast<int>(StatsTelemetry::IoOperation::Sync);
    }

    void injectedExceptionsAreContained()
    {
        QFETCH(int, operation);
        const auto ioOperation = static_cast<StatsTelemetry::IoOperation>(operation);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto options = optionsFor(root, 20, 8,
                                  ioOperation == StatsTelemetry::IoOperation::Sync ?
                                      1024 : 4 * 1024 * 1024);
        options.ioFaultInjector = std::make_shared<ThrowingIo>(ioOperation);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);

        if (ioOperation != StatsTelemetry::IoOperation::WorkerStart) {
            QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
            if (ioOperation == StatsTelemetry::IoOperation::Sync) {
                for (int attempt = 0; attempt < 64 && telemetry.ioErrorCount() == 0; ++attempt) {
                    (void)telemetry.publish(sample());
                    QTest::qWait(options.sampleIntervalMs);
                }
                QVERIFY(telemetry.ioErrorCount() > 0);
                telemetry.setOverlayActive(false);
            }
            else {
                for (int attempt = 0; attempt < 32 && telemetry.isSessionActive(); ++attempt) {
                    (void)telemetry.publish(sample());
                    QTest::qWait(options.sampleIntervalMs);
                }
            }
        }

        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isSessionActive(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveWorkerCountForTests(), 0, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveSessionCountForTests(), 0, 3000);
        QVERIFY(telemetry.ioErrorCount() > 0);
        QVERIFY(!telemetry.publish(sample()));
        telemetry.setOverlayActive(false);
    }

    void globalDisableOverridesVisibleOverlay()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto options = optionsFor(root);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(false);
        telemetry.setOverlayActive(true);

        QVERIFY(!telemetry.isSessionActive());
        QVERIFY(!telemetry.isFileOpen());
        QVERIFY(!telemetry.publish(sample()));
        QTest::qWait(options.sampleIntervalMs * 3);
        QVERIFY(!QFileInfo::exists(options.directoryPath));
        QCOMPARE(StatsTelemetry::liveWorkerCountForTests(), 0);
    }

    void toggleOnWritesStructuredOverlaySample()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto options = optionsFor(root);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);

        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        QVERIFY(telemetry.publish(sample()));
        QTRY_VERIFY_WITH_TIMEOUT(!readRecords(options.directoryPath).isEmpty(), 2000);
        telemetry.setOverlayActive(false);

        QVERIFY(!telemetry.isSessionActive());
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);
        const QList<QJsonObject> records = readRecords(options.directoryPath);
        QVERIFY(!records.isEmpty());

        const QJsonObject record = records.first();
        QCOMPARE(record.value(QStringLiteral("schema_version")).toInt(), 1);
        QVERIFY(!record.value(QStringLiteral("session_id")).toString().isEmpty());
        QVERIFY(record.value(QStringLiteral("timestamp_utc")).toString().endsWith(QLatin1Char('Z')));

        const QJsonObject stream = record.value(QStringLiteral("stream")).toObject();
        QCOMPARE(stream.value(QStringLiteral("width")).toInt(), 3456);
        QCOMPARE(stream.value(QStringLiteral("height")).toInt(), 2160);
        QCOMPARE(stream.value(QStringLiteral("configured_fps")).toInt(), 120);
        QCOMPARE(stream.value(QStringLiteral("codec")).toString(), QStringLiteral("HEVC 10-bit HDR"));
        QCOMPARE(stream.value(QStringLiteral("hdr")).toBool(), true);
        QCOMPARE(stream.value(QStringLiteral("requested_bitrate_kbps")).toInt(), 350000);

        const QJsonObject fps = record.value(QStringLiteral("fps")).toObject();
        QCOMPARE(fps.value(QStringLiteral("incoming")).toDouble(), 114.0);
        QCOMPARE(fps.value(QStringLiteral("decode")).toDouble(), 113.5);
        QCOMPARE(fps.value(QStringLiteral("render")).toDouble(), 113.0);

        const QJsonObject host = record.value(QStringLiteral("host_processing_ms")).toObject();
        QCOMPARE(host.value(QStringLiteral("min")).toDouble(), 3.1);
        QCOMPARE(host.value(QStringLiteral("max")).toDouble(), 4.3);
        QCOMPARE(host.value(QStringLiteral("average")).toDouble(), 3.2);

        const QJsonObject drops = record.value(QStringLiteral("drops_percent")).toObject();
        QCOMPARE(drops.value(QStringLiteral("network")).toDouble(), 5.0);
        QVERIFY(std::abs(drops.value(QStringLiteral("pacer_jitter")).toDouble() - (300.0 / 190.0)) < 0.000001);

        const QJsonObject network = record.value(QStringLiteral("network")).toObject();
        QCOMPARE(network.value(QStringLiteral("rtt_ms")).toInt(), 5);
        QCOMPARE(network.value(QStringLiteral("rtt_variance_ms")).toInt(), 1);

        const QJsonObject timing = record.value(QStringLiteral("timing")).toObject();
        QVERIFY(qFuzzyCompare(timing.value(QStringLiteral("decode_ms")).toDouble(), 7.75));
        QVERIFY(qFuzzyCompare(timing.value(QStringLiteral("frame_queue_ms")).toDouble(), 0.01));
        QVERIFY(qFuzzyCompare(timing.value(QStringLiteral("render_including_vsync_ms")).toDouble(), 0.08));

        const QJsonObject raw = record.value(QStringLiteral("raw")).toObject();
        QCOMPARE(raw.value(QStringLiteral("received_frames")).toInt(), 190);
        QCOMPARE(raw.value(QStringLiteral("network_dropped_frames")).toInt(), 10);
        QCOMPARE(raw.value(QStringLiteral("measurement_duration_us")).toInt(), 2000000);

        const QByteArray compact = QJsonDocument(record).toJson(QJsonDocument::Compact).toLower();
        QVERIFY(!compact.contains("rikey"));
        QVERIFY(!compact.contains("private_key"));
        QVERIFY(!compact.contains("host_address"));
    }

    void toggleOffClosesAsynchronouslyAndStopsWrites()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto options = optionsFor(root);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        QVERIFY(telemetry.publish(sample()));
        QTRY_VERIFY_WITH_TIMEOUT(!readRecords(options.directoryPath).isEmpty(), 2000);

        QElapsedTimer toggleTimer;
        toggleTimer.start();
        telemetry.setOverlayActive(false);
        QVERIFY2(toggleTimer.elapsed() < 250, "overlay-off performed blocking retirement work");
        QVERIFY(!telemetry.isSessionActive());
        QVERIFY(!telemetry.publish(sample()));

        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);
        const qint64 bytesAfterClose = totalTelemetryBytes(options.directoryPath);
        QTest::qWait(options.sampleIntervalMs * 4);
        QCOMPARE(totalTelemetryBytes(options.directoryPath), bytesAfterClose);
    }

    void overlayOffPerformsNativeDurableSyncOnWorker()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto options = optionsFor(root, 20);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        QVERIFY(telemetry.publish(sample()));
        QTRY_VERIFY_WITH_TIMEOUT(!readRecords(options.directoryPath).isEmpty(), 2000);

        const std::uint64_t syncCallsBeforeOff = telemetry.nativeSyncCallCountForTests();
        telemetry.setOverlayActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);
        QCOMPARE(telemetry.nativeSyncCallCountForTests(), syncCallsBeforeOff + 1);
    }

    void queuedRollingPeaksAreNotCoalesced()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto options = optionsFor(root, 20);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);

        auto first = sample();
        first.networkDroppedFrames = 1;
        auto peak = sample();
        peak.networkDroppedFrames = 64;
        auto third = sample();
        third.networkDroppedFrames = 2;
        QVERIFY(telemetry.publish(first));
        QVERIFY(telemetry.publish(peak));
        QVERIFY(telemetry.publish(third));

        QTRY_VERIFY_WITH_TIMEOUT(readRecords(options.directoryPath).size() >= 3, 2000);
        telemetry.setOverlayActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);

        const QList<QJsonObject> records = readRecords(options.directoryPath);
        QVERIFY(records.size() >= 3);
        QCOMPARE(records.at(0).value(QStringLiteral("raw")).toObject()
                     .value(QStringLiteral("network_dropped_frames")).toInt(), 1);
        QCOMPARE(records.at(1).value(QStringLiteral("raw")).toObject()
                     .value(QStringLiteral("network_dropped_frames")).toInt(), 64);
        QCOMPARE(records.at(2).value(QStringLiteral("raw")).toObject()
                     .value(QStringLiteral("network_dropped_frames")).toInt(), 2);
    }

    void queuePressureCreatesObservableSourceSequenceGaps()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto blocker = std::make_shared<BlockingIo>(StatsTelemetry::IoOperation::Write);
        BlockingIoReleaseGuard releaseGuard(blocker);
        auto options = optionsFor(root, 20);
        options.ioFaultInjector = blocker;

        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        QVERIFY(telemetry.publish(sample()));
        QVERIFY(blocker->waitUntilEntered(2000));

        int accepted = 0;
        for (int i = 0; i < 20; ++i) {
            accepted += telemetry.publish(sample()) ? 1 : 0;
        }
        QCOMPARE(accepted, 15);

        blocker->unblock();
        QTRY_VERIFY_WITH_TIMEOUT(readRecords(options.directoryPath).size() >= 16, 3000);
        QVERIFY(telemetry.publish(sample()));
        const auto sequenceWasWritten = [&]() {
            for (const QJsonObject& record : readRecords(options.directoryPath)) {
                if (record.value(QStringLiteral("source_window_sequence")).toInt() == 22) {
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(sequenceWasWritten(), 3000);
        telemetry.setOverlayActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);

        const QList<QJsonObject> records = readRecords(options.directoryPath);
        QSet<int> sequences;
        bool sequence22HasDropCount = false;
        for (const QJsonObject& record : records) {
            const int sequence = record.value(QStringLiteral("source_window_sequence")).toInt();
            sequences.insert(sequence);
            if (sequence == 22 && record.value(QStringLiteral("raw")).toObject()
                                      .value(QStringLiteral("telemetry_queue_dropped_samples")).toInt() == 5) {
                sequence22HasDropCount = true;
            }
        }
        for (int sequence = 1; sequence <= 16; ++sequence) {
            QVERIFY(sequences.contains(sequence));
        }
        for (int sequence = 17; sequence <= 21; ++sequence) {
            QVERIFY(!sequences.contains(sequence));
        }
        QVERIFY(sequences.contains(22));
        QVERIFY(sequence22HasDropCount);
    }

    void periodicTickMarksRepeatedSourceAsStale()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto options = optionsFor(root, 20);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        QVERIFY(telemetry.publish(sample()));
        QTRY_VERIFY_WITH_TIMEOUT(readRecords(options.directoryPath).size() >= 2, 2000);
        telemetry.setOverlayActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);

        const QList<QJsonObject> records = readRecords(options.directoryPath);
        QVERIFY(records.size() >= 2);
        QCOMPARE(records.at(0).value(QStringLiteral("source_stale")).toBool(), false);
        QCOMPARE(records.at(1).value(QStringLiteral("source_stale")).toBool(), true);
        QCOMPARE(records.at(0).value(QStringLiteral("source_window_sequence")).toInt(),
                 records.at(1).value(QStringLiteral("source_window_sequence")).toInt());
        QVERIFY(records.at(1).value(QStringLiteral("source_age_ms")).toInt() >=
                records.at(0).value(QStringLiteral("source_age_ms")).toInt());
    }

    void retoggleCreatesNewSession()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto options = optionsFor(root);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);

        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        const QString firstSession = telemetry.currentSessionId();
        QVERIFY(telemetry.publish(sample()));
        QTRY_VERIFY_WITH_TIMEOUT(!readRecords(options.directoryPath).isEmpty(), 2000);
        telemetry.setOverlayActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);

        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        const QString secondSession = telemetry.currentSessionId();
        QVERIFY(telemetry.publish(sample()));
        const auto secondSessionWasWritten = [&]() {
            for (const QJsonObject& record : readRecords(options.directoryPath)) {
                if (record.value(QStringLiteral("session_id")).toString() == secondSession) {
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(secondSessionWasWritten(), 2000);
        telemetry.setOverlayActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);

        QVERIFY(!firstSession.isEmpty());
        QVERIFY(!secondSession.isEmpty());
        QVERIFY(firstSession != secondSession);

        QSet<QString> sessionIds;
        for (const QJsonObject& record : readRecords(options.directoryPath)) {
            sessionIds.insert(record.value(QStringLiteral("session_id")).toString());
        }
        QCOMPARE(sessionIds.size(), 2);
        QVERIFY(sessionIds.contains(firstSession));
        QVERIFY(sessionIds.contains(secondSession));
    }

    void rapidRetoggleKeepsBlockedSessionsSeparated_data()
    {
        QTest::addColumn<int>("operation");
        QTest::newRow("write") << static_cast<int>(StatsTelemetry::IoOperation::Write);
        QTest::newRow("sync") << static_cast<int>(StatsTelemetry::IoOperation::Sync);
    }

    void rapidRetoggleKeepsBlockedSessionsSeparated()
    {
        QFETCH(int, operation);
        const auto ioOperation = static_cast<StatsTelemetry::IoOperation>(operation);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto blocker = std::make_shared<BlockingIo>(ioOperation);
        BlockingIoReleaseGuard releaseGuard(blocker);
        auto options = optionsFor(root, 20, 8,
                                  ioOperation == StatsTelemetry::IoOperation::Sync ?
                                      1024 : 4 * 1024 * 1024);
        options.ioFaultInjector = blocker;

        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        const QString firstSession = telemetry.currentSessionId();
        auto first = sample();
        first.networkDroppedFrames = 71;
        QVERIFY(publishUntilBlocked(telemetry, *blocker, first));

        int firstRecordsAtOff = 0;
        for (const QJsonObject& record : readRecords(options.directoryPath)) {
            if (record.value(QStringLiteral("session_id")).toString() == firstSession) {
                ++firstRecordsAtOff;
            }
        }
        QCOMPARE(firstRecordsAtOff,
                 ioOperation == StatsTelemetry::IoOperation::Sync ? 1 : 0);

        telemetry.setOverlayActive(false);
        QVERIFY(!telemetry.isSessionActive());
        telemetry.setOverlayActive(true);
        const QString secondSession = telemetry.currentSessionId();
        QVERIFY(!secondSession.isEmpty());
        QVERIFY(firstSession != secondSession);

        auto second = sample();
        second.networkDroppedFrames = 2;
        QVERIFY(telemetry.publish(second));
        const auto hasSecondSessionRecord = [&]() {
            for (const QJsonObject& record : readRecords(options.directoryPath)) {
                if (record.value(QStringLiteral("session_id")).toString() == secondSession) {
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(hasSecondSessionRecord(), 2000);

        telemetry.setOverlayActive(false);
        QVERIFY(!telemetry.publish(sample()));
        blocker->unblock();
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveWorkerCountForTests(), 0, 3000);

        const QList<QJsonObject> records = readRecords(options.directoryPath);
        QVERIFY(!records.isEmpty());
        int firstRecords = 0;
        int secondRecords = 0;
        for (const QJsonObject& record : records) {
            const QString sessionId = record.value(QStringLiteral("session_id")).toString();
            const int dropped = record.value(QStringLiteral("raw")).toObject()
                                    .value(QStringLiteral("network_dropped_frames")).toInt();
            if (sessionId == firstSession) {
                ++firstRecords;
                QCOMPARE(dropped, 71);
            }
            else {
                QCOMPARE(sessionId, secondSession);
                ++secondRecords;
                QCOMPARE(dropped, 2);
            }
        }
        QCOMPARE(firstRecords, firstRecordsAtOff);
        QVERIFY(secondRecords >= 1);
    }

    void blockedIoDoesNotBlockOverlayOff_data()
    {
        QTest::addColumn<int>("operation");
        QTest::newRow("write") << static_cast<int>(StatsTelemetry::IoOperation::Write);
        QTest::newRow("flush") << static_cast<int>(StatsTelemetry::IoOperation::Flush);
        QTest::newRow("sync") << static_cast<int>(StatsTelemetry::IoOperation::Sync);
    }

    void blockedIoDoesNotBlockOverlayOff()
    {
        QFETCH(int, operation);
        const auto ioOperation = static_cast<StatsTelemetry::IoOperation>(operation);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto blocker = std::make_shared<BlockingIo>(ioOperation);
        BlockingIoReleaseGuard releaseGuard(blocker);
        auto options = optionsFor(root, 20, 8,
                                  ioOperation == StatsTelemetry::IoOperation::Sync ?
                                      1024 : 4 * 1024 * 1024);
        options.ioFaultInjector = blocker;

        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        QVERIFY(publishUntilBlocked(telemetry, *blocker, sample()));
        const std::uint64_t syncCallsWhileBlocked = telemetry.nativeSyncCallCountForTests();

        QElapsedTimer timer;
        timer.start();
        telemetry.setOverlayActive(false);
        QVERIFY2(timer.elapsed() < 250, "blocked telemetry I/O delayed the overlay/input path");
        QVERIFY(!telemetry.isSessionActive());
        QVERIFY(!telemetry.publish(sample()));
        if (ioOperation == StatsTelemetry::IoOperation::Sync) {
            QCOMPARE(telemetry.nativeSyncCallCountForTests(), syncCallsWhileBlocked);
        }

        if (ioOperation != StatsTelemetry::IoOperation::Write) {
            const int recordsAtOff = readRecords(options.directoryPath).size();
            QTest::qWait(options.sampleIntervalMs * 3);
            QCOMPARE(readRecords(options.directoryPath).size(), recordsAtOff);
        }

        blocker->unblock();
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveWorkerCountForTests(), 0, 3000);
        if (ioOperation == StatsTelemetry::IoOperation::Sync) {
            QCOMPARE(telemetry.nativeSyncCallCountForTests(), syncCallsWhileBlocked + 1);
        }
        const int recordsAfterRetirement = readRecords(options.directoryPath).size();
        QTest::qWait(options.sampleIntervalMs * 3);
        QCOMPARE(readRecords(options.directoryPath).size(), recordsAfterRetirement);
        QCOMPARE(recordsAfterRetirement, ioOperation == StatsTelemetry::IoOperation::Write ? 0 : 1);
    }

    void blockedIoDoesNotBlockDestruction_data()
    {
        blockedIoDoesNotBlockOverlayOff_data();
    }

    void blockedIoDoesNotBlockDestruction()
    {
        QFETCH(int, operation);
        const auto ioOperation = static_cast<StatsTelemetry::IoOperation>(operation);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto blocker = std::make_shared<BlockingIo>(ioOperation);
        BlockingIoReleaseGuard releaseGuard(blocker);
        auto options = optionsFor(root, 20, 8,
                                  ioOperation == StatsTelemetry::IoOperation::Sync ?
                                      1024 : 4 * 1024 * 1024);
        options.ioFaultInjector = blocker;

        const int workerBaseline = StatsTelemetry::liveWorkerCountForTests();
        const int sessionBaseline = StatsTelemetry::liveSessionCountForTests();
        auto telemetry = std::make_unique<StatsTelemetry>(options);
        telemetry->configure(streamConfig());
        telemetry->setGloballyEnabled(true);
        telemetry->setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry->isFileOpen(), 2000);
        QVERIFY(publishUntilBlocked(*telemetry, *blocker, sample()));

        QElapsedTimer timer;
        timer.start();
        telemetry.reset();
        QVERIFY2(timer.elapsed() < 250, "StatsTelemetry destruction waited for worker I/O");

        QVERIFY(StatsTelemetry::liveWorkerCountForTests() >= workerBaseline + 1);
        QVERIFY(StatsTelemetry::liveSessionCountForTests() >= sessionBaseline + 1);

        blocker->unblock();
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveWorkerCountForTests(), workerBaseline, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveSessionCountForTests(), sessionBaseline, 3000);
    }

    void repeatedBlockedRetogglesStayResourceBounded_data()
    {
        QTest::addColumn<int>("operation");
        QTest::newRow("write") << static_cast<int>(StatsTelemetry::IoOperation::Write);
        QTest::newRow("flush") << static_cast<int>(StatsTelemetry::IoOperation::Flush);
        QTest::newRow("sync") << static_cast<int>(StatsTelemetry::IoOperation::Sync);
    }

    void repeatedBlockedRetogglesStayResourceBounded()
    {
        QFETCH(int, operation);
        const auto ioOperation = static_cast<StatsTelemetry::IoOperation>(operation);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto blocker = std::make_shared<BlockingEveryIo>(ioOperation);
        BlockingEveryIoReleaseGuard releaseGuard(blocker);
        auto options = optionsFor(root, 20, 8,
                                  ioOperation == StatsTelemetry::IoOperation::Sync ?
                                      1024 : 4 * 1024 * 1024);
        options.ioFaultInjector = blocker;

        const int workerBaseline = StatsTelemetry::liveWorkerCountForTests();
        const int sessionBaseline = StatsTelemetry::liveSessionCountForTests();
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);

        QSet<QString> blockedSessionIds;
        std::unique_ptr<StatsTelemetry> secondStreamTelemetry;
        for (int index = 0; index < 2; ++index) {
            StatsTelemetry* target = &telemetry;
            if (index == 1) {
                secondStreamTelemetry = std::make_unique<StatsTelemetry>(options);
                secondStreamTelemetry->configure(streamConfig());
                secondStreamTelemetry->setGloballyEnabled(true);
                target = secondStreamTelemetry.get();
            }
            target->setOverlayActive(true);
            QVERIFY(target->isSessionActive());
            const QString sessionId = target->currentSessionId();
            QVERIFY(!sessionId.isEmpty());
            QVERIFY(!blockedSessionIds.contains(sessionId));
            blockedSessionIds.insert(sessionId);
            QVERIFY(publishUntilBlocked(*target, *blocker, sample()));

            QElapsedTimer offTimer;
            offTimer.start();
            target->setOverlayActive(false);
            QVERIFY2(offTimer.elapsed() < 250, "blocked retirement delayed repeated overlay-off");
            QVERIFY(!target->isSessionActive());
            QVERIFY(!target->publish(sample()));

        }

        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveWorkerCountForTests(), workerBaseline + 2, 2000);
        QCOMPARE(StatsTelemetry::liveSessionCountForTests(), sessionBaseline + 2);

        // Both bounded slots are pinned in injected I/O. Repeated ON/OFF must
        // fail open without creating another thread, session, timer, or file.
        for (int attempt = 0; attempt < 20; ++attempt) {
            QElapsedTimer toggleTimer;
            toggleTimer.start();
            telemetry.setOverlayActive(true);
            QVERIFY2(toggleTimer.elapsed() < 250, "session-limit rejection blocked overlay-on");
            QVERIFY(!telemetry.isSessionActive());
            QVERIFY(!telemetry.publish(sample()));
            telemetry.setOverlayActive(false);
            QCOMPARE(StatsTelemetry::liveWorkerCountForTests(), workerBaseline + 2);
            QCOMPARE(StatsTelemetry::liveSessionCountForTests(), sessionBaseline + 2);
        }
        QVERIFY(telemetry.ioErrorCount() >= 20);
        QCOMPARE(static_cast<int>(telemetry.lastIoError()),
                 static_cast<int>(StatsTelemetry::IoError::OutstandingSessionLimit));

        StatsTelemetry nextStreamTelemetry(options);
        nextStreamTelemetry.configure(streamConfig());
        nextStreamTelemetry.setGloballyEnabled(true);
        nextStreamTelemetry.setOverlayActive(true);
        QVERIFY(!nextStreamTelemetry.isSessionActive());
        QCOMPARE(StatsTelemetry::liveWorkerCountForTests(), workerBaseline + 2);
        QCOMPARE(StatsTelemetry::liveSessionCountForTests(), sessionBaseline + 2);
        QVERIFY(nextStreamTelemetry.ioErrorCount() > 0);
        QCOMPARE(static_cast<int>(nextStreamTelemetry.lastIoError()),
                 static_cast<int>(StatsTelemetry::IoError::OutstandingSessionLimit));
        nextStreamTelemetry.setOverlayActive(false);

        blocker->unblockAll();
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveWorkerCountForTests(), workerBaseline, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(StatsTelemetry::liveSessionCountForTests(), sessionBaseline, 3000);

        // Once the bounded retired state clears, telemetry can start again.
        const int recordsBeforeRecovery = readRecords(options.directoryPath).size();
        telemetry.setOverlayActive(true);
        QVERIFY(telemetry.isSessionActive());
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        QVERIFY(telemetry.publish(sample()));
        const auto wroteRecoveryRecord = [&]() {
            return readRecords(options.directoryPath).size() > recordsBeforeRecovery;
        };
        QTRY_VERIFY_WITH_TIMEOUT(wroteRecoveryRecord(), 2000);
        telemetry.setOverlayActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);
    }

    void syncFailureIsObservableAndFailOpen()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto options = optionsFor(root, 20);
        options.ioFaultInjector = std::make_shared<RejectingIo>(StatsTelemetry::IoOperation::Sync);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        QVERIFY(telemetry.publish(sample()));
        QTRY_VERIFY_WITH_TIMEOUT(!readRecords(options.directoryPath).isEmpty(), 2000);

        QElapsedTimer timer;
        timer.start();
        telemetry.setOverlayActive(false);
        QVERIFY(timer.elapsed() < 250);
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);
        QVERIFY(telemetry.ioErrorCount() > 0);
        QCOMPARE(static_cast<int>(telemetry.lastIoError()),
                 static_cast<int>(StatsTelemetry::IoError::Sync));
        QVERIFY(!telemetry.publish(sample()));
        QCOMPARE(telemetry.nativeSyncCallCountForTests(), std::uint64_t(0));
    }

    void rotationAndPermissionsAreBoundedAndPrivate()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto options = optionsFor(root, 5, 3, 1024);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        QVERIFY(telemetry.publish(sample()));
        QTest::qWait(100);
        telemetry.setOverlayActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);

        const QStringList files = telemetryFiles(options.directoryPath);
        QVERIFY(!files.isEmpty());
        QVERIFY(files.size() <= options.maxFileCount);

#ifdef Q_OS_UNIX
        struct stat directoryStat = {};
        const QByteArray encodedDirectory = QFile::encodeName(options.directoryPath);
        QCOMPARE(::stat(encodedDirectory.constData(), &directoryStat), 0);
        QCOMPARE(static_cast<int>(directoryStat.st_mode & 0777), 0700);

        const QDir directory(options.directoryPath);
        for (const QString& fileName : files) {
            struct stat fileStat = {};
            const QByteArray encodedFile = QFile::encodeName(directory.absoluteFilePath(fileName));
            QCOMPARE(::stat(encodedFile.constData(), &fileStat), 0);
            QCOMPARE(static_cast<int>(fileStat.st_mode & 0777), 0600);
        }
#endif
    }

#ifdef Q_OS_UNIX
    void directorySymlinkIsRejectedWithoutChangingTargetMode()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString targetPath = QDir(root.path()).absoluteFilePath(QStringLiteral("target"));
        const QString linkPath = QDir(root.path()).absoluteFilePath(QStringLiteral("Moonlight AVSampleBuffer"));
        QVERIFY(QDir().mkpath(targetPath));
        const QByteArray encodedTarget = QFile::encodeName(targetPath);
        const QByteArray encodedLink = QFile::encodeName(linkPath);
        QCOMPARE(::chmod(encodedTarget.constData(), 0755), 0);
        QCOMPARE(::symlink(encodedTarget.constData(), encodedLink.constData()), 0);

        auto options = optionsFor(root, 20);
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isSessionActive(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.ioErrorCount() > 0, 2000);
        QCOMPARE(static_cast<int>(telemetry.lastIoError()),
                 static_cast<int>(StatsTelemetry::IoError::Directory));

        struct stat targetStat = {};
        QCOMPARE(::stat(encodedTarget.constData(), &targetStat), 0);
        QCOMPARE(static_cast<int>(targetStat.st_mode & 0777), 0755);
        struct stat linkStat = {};
        QCOMPARE(::lstat(encodedLink.constData(), &linkStat), 0);
        QVERIFY(S_ISLNK(linkStat.st_mode));
        telemetry.setOverlayActive(false);
    }

    void rotationNeverFollowsOrDeletesTelemetrySymlink()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto options = optionsFor(root, 20, 1, 4 * 1024 * 1024);
        QVERIFY(QDir().mkpath(options.directoryPath));

        const QString sentinelPath = QDir(root.path()).absoluteFilePath(QStringLiteral("sentinel"));
        QFile sentinel(sentinelPath);
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        QCOMPARE(sentinel.write("keep"), qint64(4));
        sentinel.close();

        const QString linkPath = QDir(options.directoryPath)
                                     .absoluteFilePath(QStringLiteral("telemetry-19700101T000000000Z-link-p000.jsonl"));
        const QByteArray encodedSentinel = QFile::encodeName(sentinelPath);
        const QByteArray encodedLink = QFile::encodeName(linkPath);
        QCOMPARE(::symlink(encodedSentinel.constData(), encodedLink.constData()), 0);

        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        QVERIFY(telemetry.publish(sample()));
        QTRY_VERIFY_WITH_TIMEOUT(!readRecords(options.directoryPath).isEmpty(), 2000);
        telemetry.setOverlayActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isFileOpen(), 2000);

        struct stat linkStat = {};
        QCOMPARE(::lstat(encodedLink.constData(), &linkStat), 0);
        QVERIFY(S_ISLNK(linkStat.st_mode));
        QVERIFY(sentinel.open(QIODevice::ReadOnly));
        QCOMPARE(sentinel.readAll(), QByteArray("keep"));
        QCOMPARE(telemetryFiles(options.directoryPath).size(), 1);
    }
#endif

    void ioFailureIsFailOpenAndObservable()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString invalidDirectory = QDir(root.path()).absoluteFilePath(QStringLiteral("not-a-directory"));
        QFile blocker(invalidDirectory);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        QCOMPARE(blocker.write("keep"), qint64(4));
        blocker.close();

        StatsTelemetry::Options options;
        options.directoryPath = invalidDirectory;
        options.sampleIntervalMs = 10;
        StatsTelemetry telemetry(options);
        telemetry.configure(streamConfig());
        telemetry.setGloballyEnabled(true);
        telemetry.setOverlayActive(true);

        QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isSessionActive(), 2000);
        QVERIFY(!telemetry.isFileOpen());
        QVERIFY(!telemetry.publish(sample()));
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.ioErrorCount() > 0, 2000);
        QCOMPARE(static_cast<int>(telemetry.lastIoError()),
                 static_cast<int>(StatsTelemetry::IoError::Directory));
        telemetry.setOverlayActive(false);

        QVERIFY(blocker.open(QIODevice::ReadOnly));
        QCOMPARE(blocker.readAll(), QByteArray("keep"));
    }

    void environmentDisableValues()
    {
        QVERIFY(StatsTelemetry::environmentAllowsTelemetry(QByteArray()));
        QVERIFY(StatsTelemetry::environmentAllowsTelemetry("1"));
        QVERIFY(StatsTelemetry::environmentAllowsTelemetry("yes"));
        QVERIFY(!StatsTelemetry::environmentAllowsTelemetry("0"));
        QVERIFY(!StatsTelemetry::environmentAllowsTelemetry(" FALSE "));
        QVERIFY(!StatsTelemetry::environmentAllowsTelemetry("off"));
        QVERIFY(!StatsTelemetry::environmentAllowsTelemetry("No"));
    }
};

static bool waitForDetachedTelemetryAfterQtTeardown();

static bool prepareQtTeardownProbe(std::shared_ptr<StdBlockingIo>& blocker,
                                   std::unique_ptr<QTemporaryDir>& root)
{
    root = std::make_unique<QTemporaryDir>();
    if (!root->isValid()) {
        return false;
    }

    blocker = std::make_shared<StdBlockingIo>();
    StatsTelemetry::Options options;
    options.directoryPath = QDir(root->path()).absoluteFilePath(QStringLiteral("Moonlight AVSampleBuffer"));
    options.sampleIntervalMs = 10;
    options.ioFaultInjector = blocker;

    auto telemetry = std::make_unique<StatsTelemetry>(options);
    StatsTelemetry::StreamConfig config;
    config.width = 3456;
    config.height = 2160;
    config.configuredFps = 120;
    config.requestedBitrateKbps = 350000;
    telemetry->configure(config);
    telemetry->setGloballyEnabled(true);
    telemetry->setOverlayActive(true);

    QElapsedTimer openTimer;
    openTimer.start();
    while (!telemetry->isFileOpen() && openTimer.elapsed() < 2000) {
        QTest::qWait(10);
    }
    if (!telemetry->isFileOpen()) {
        telemetry.reset();
        waitForDetachedTelemetryAfterQtTeardown();
        return false;
    }

    StatsTelemetry::Sample value;
    value.capturedAtUnixMs = QDateTime::currentMSecsSinceEpoch();
    value.totalFrames = 1;
    value.receivedFrames = 1;
    value.decodedFrames = 1;
    value.renderedFrames = 1;
    std::strcpy(value.codec.data(), "HEVC");
    if (!telemetry->publish(value) ||
            !blocker->waitUntilEntered(std::chrono::milliseconds(2000))) {
        blocker->unblock();
        telemetry.reset();
        waitForDetachedTelemetryAfterQtTeardown();
        return false;
    }

    QElapsedTimer destructionTimer;
    destructionTimer.start();
    telemetry.reset();
    return destructionTimer.elapsed() < 250;
}

static bool waitForDetachedTelemetryAfterQtTeardown()
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((StatsTelemetry::liveWorkerCountForTests() != 0 ||
            StatsTelemetry::liveSessionCountForTests() != 0) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return StatsTelemetry::liveWorkerCountForTests() == 0 &&
           StatsTelemetry::liveSessionCountForTests() == 0;
}

int main(int argc, char** argv)
{
    int testResult = 0;
    bool teardownProbePrepared = false;
    std::shared_ptr<StdBlockingIo> teardownBlocker;
    std::unique_ptr<QTemporaryDir> teardownRoot;
    std::string teardownRootPath;

    {
        QCoreApplication application(argc, argv);
        StatsTelemetryTest testObject;
        testResult = QTest::qExec(&testObject, argc, argv);
        if (testResult == 0) {
            teardownProbePrepared = prepareQtTeardownProbe(teardownBlocker, teardownRoot);
        }
        if (teardownRoot) {
            teardownRootPath = teardownRoot->path().toStdString();
            teardownRoot->setAutoRemove(false);
            teardownRoot.reset();
        }
    }

    if (testResult != 0) {
        return testResult;
    }
    if (!teardownProbePrepared) {
        if (teardownBlocker) {
            teardownBlocker->unblock();
            waitForDetachedTelemetryAfterQtTeardown();
        }
        std::error_code cleanupError;
        if (!teardownRootPath.empty()) {
            std::filesystem::remove_all(teardownRootPath, cleanupError);
        }
        std::fprintf(stderr, "FAIL: unable to prepare detached Qt teardown probe\n");
        return 1;
    }

    // QCoreApplication is gone. On macOS, the stopped worker must now execute
    // only its standard/POSIX cancellation and close path before destroying
    // Qt-free session/control state.
    teardownBlocker->unblock();
    const bool retired = waitForDetachedTelemetryAfterQtTeardown();
    std::error_code cleanupError;
    if (!teardownRootPath.empty()) {
        std::filesystem::remove_all(teardownRootPath, cleanupError);
    }
    if (!retired) {
        std::fprintf(stderr, "FAIL: detached telemetry survived Qt teardown timeout\n");
        return 1;
    }
    return 0;
}

#include "test-statstelemetry.moc"
