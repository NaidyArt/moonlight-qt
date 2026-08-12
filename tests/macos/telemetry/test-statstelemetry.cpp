#include "streaming/video/statstelemetry.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#include <cmath>
#include <cstring>

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

private slots:
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
        QVERIFY(!telemetry.isFileOpen());
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

    void toggleOffClosesAndStopsWrites()
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
        QVERIFY(!telemetry.isFileOpen());
        QVERIFY(!telemetry.isSessionActive());

        qint64 bytesAfterClose = 0;
        const QDir directory(options.directoryPath);
        for (const QString& fileName : telemetryFiles(options.directoryPath)) {
            bytesAfterClose += QFileInfo(directory.absoluteFilePath(fileName)).size();
        }

        QVERIFY(!telemetry.publish(sample()));
        QTest::qWait(options.sampleIntervalMs * 4);

        qint64 bytesAfterWait = 0;
        for (const QString& fileName : telemetryFiles(options.directoryPath)) {
            bytesAfterWait += QFileInfo(directory.absoluteFilePath(fileName)).size();
        }
        QCOMPARE(bytesAfterWait, bytesAfterClose);
    }

    void queuedRollingPeaksAreNotCoalesced()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto options = optionsFor(root, 1000);
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

        // Stop before the periodic tick. Clean shutdown must drain every source
        // window in order rather than retaining only the newest one.
        telemetry.setOverlayActive(false);
        const QList<QJsonObject> records = readRecords(options.directoryPath);
        QCOMPARE(records.size(), 3);
        QCOMPARE(records.at(0).value(QStringLiteral("raw")).toObject()
                     .value(QStringLiteral("network_dropped_frames")).toInt(), 1);
        QCOMPARE(records.at(1).value(QStringLiteral("raw")).toObject()
                     .value(QStringLiteral("network_dropped_frames")).toInt(), 64);
        QCOMPARE(records.at(2).value(QStringLiteral("raw")).toObject()
                     .value(QStringLiteral("network_dropped_frames")).toInt(), 2);
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
        telemetry.setOverlayActive(false);

        telemetry.setOverlayActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(telemetry.isFileOpen(), 2000);
        const QString secondSession = telemetry.currentSessionId();
        QVERIFY(telemetry.publish(sample()));
        telemetry.setOverlayActive(false);

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

    void ioFailureIsFailOpen()
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

QTEST_GUILESS_MAIN(StatsTelemetryTest)

#include "test-statstelemetry.moc"
