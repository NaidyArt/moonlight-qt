#include "statstelemetry.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <locale>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#ifdef Q_OS_DARWIN
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/sync_policy.h>
#include <pthread/qos.h>
#endif

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#endif

namespace {

constexpr std::uint32_t QueueCapacity = 16;
constexpr int MaxOutstandingSessions = 2;

#ifdef Q_OS_DARWIN
static_assert(std::atomic<void*>::is_always_lock_free,
              "macOS telemetry requires lock-free pointer atomics");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "macOS telemetry requires lock-free 64-bit atomics");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "macOS telemetry requires lock-free 32-bit atomics");
static_assert(std::atomic<bool>::is_always_lock_free,
              "macOS telemetry requires lock-free boolean atomics");
#endif

struct TelemetryProcessState {
    std::atomic<int> outstandingSessions {0};
    std::atomic<int> liveWorkers {0};
    std::atomic<int> liveSessions {0};
    std::mutex fileRegistryMutex;
    std::set<std::string> activeFilePaths;
};

std::shared_ptr<TelemetryProcessState> sharedTelemetryProcessState()
{
    // The function-static reference may retire during process shutdown, but
    // every detached worker holds its own shared reference through ControlState.
    static const auto state = std::make_shared<TelemetryProcessState>();
    return state;
}

std::string nativePath(const QString& path)
{
#ifdef Q_OS_UNIX
    const QByteArray encoded = QFile::encodeName(path);
    return std::string(encoded.constData(), static_cast<std::size_t>(encoded.size()));
#else
    return path.toStdString();
#endif
}

std::int64_t currentUnixMilliseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

bool utcTime(std::int64_t unixMs, std::tm& utc) noexcept
{
    const std::time_t seconds = static_cast<std::time_t>(unixMs / 1000);
#ifdef Q_OS_WIN
    return gmtime_s(&utc, &seconds) == 0;
#else
    return gmtime_r(&seconds, &utc) != nullptr;
#endif
}

std::string utcTimestamp(std::int64_t unixMs)
{
    std::tm utc = {};
    if (!utcTime(unixMs, utc)) {
        return "1970-01-01T00:00:00.000Z";
    }
    const int milliseconds = static_cast<int>((unixMs % 1000 + 1000) % 1000);
    char buffer[32] = {};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc.tm_year + 1900,
                  utc.tm_mon + 1,
                  utc.tm_mday,
                  utc.tm_hour,
                  utc.tm_min,
                  utc.tm_sec,
                  milliseconds);
    return buffer;
}

std::string compactUtcTimestamp(std::int64_t unixMs)
{
    std::tm utc = {};
    if (!utcTime(unixMs, utc)) {
        return "19700101T000000000Z";
    }
    const int milliseconds = static_cast<int>((unixMs % 1000 + 1000) % 1000);
    char buffer[32] = {};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%04d%02d%02dT%02d%02d%02d%03dZ",
                  utc.tm_year + 1900,
                  utc.tm_mon + 1,
                  utc.tm_mday,
                  utc.tm_hour,
                  utc.tm_min,
                  utc.tm_sec,
                  milliseconds);
    return buffer;
}

void appendJsonString(std::ostringstream& output, const std::string& value)
{
    static constexpr char Hex[] = "0123456789abcdef";
    output << '"';
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte < 0x20) {
                output << "\\u00" << Hex[(byte >> 4) & 0x0f] << Hex[byte & 0x0f];
            }
            else {
                output << static_cast<char>(byte);
            }
        }
    }
    output << '"';
}

void appendNullableNumber(std::ostringstream& output, double value, bool valid)
{
    if (!valid || !std::isfinite(value)) {
        output << "null";
    }
    else {
        output << value;
    }
}

void appendNullableRatio(std::ostringstream& output, double numerator, double denominator)
{
    appendNullableNumber(output,
                         denominator > 0.0 ? numerator / denominator : 0.0,
                         denominator > 0.0);
}

void setTelemetryWorkerPriority()
{
#ifdef Q_OS_DARWIN
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#endif
}

// A counted wake primitive has no condition-variable notification window.
// signal() is lock-free from the caller's perspective and never waits for the
// telemetry worker, which keeps OverlayDebug OFF and Session destruction
// independent of a stalled worker or stalled ON path.
class WakeSignal final
{
public:
    WakeSignal()
    {
#ifdef Q_OS_DARWIN
        if (semaphore_create(mach_task_self(), &m_Semaphore, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
            throw std::runtime_error("unable to create telemetry wake semaphore");
        }
#elif defined(Q_OS_UNIX)
        if (sem_init(&m_Semaphore, 0, 0) != 0) {
            throw std::runtime_error("unable to create telemetry wake semaphore");
        }
#elif defined(Q_OS_WIN)
        m_Semaphore = CreateSemaphoreW(nullptr, 0, LONG_MAX, nullptr);
        if (m_Semaphore == nullptr) {
            throw std::runtime_error("unable to create telemetry wake semaphore");
        }
#else
#error Unsupported StatsTelemetry wake platform
#endif
    }

    ~WakeSignal()
    {
#ifdef Q_OS_DARWIN
        if (m_Semaphore != SEMAPHORE_NULL) {
            semaphore_destroy(mach_task_self(), m_Semaphore);
        }
#elif defined(Q_OS_UNIX)
        sem_destroy(&m_Semaphore);
#elif defined(Q_OS_WIN)
        if (m_Semaphore != nullptr) {
            CloseHandle(m_Semaphore);
        }
#endif
    }

    WakeSignal(const WakeSignal&) = delete;
    WakeSignal& operator=(const WakeSignal&) = delete;

    void signal() noexcept
    {
#ifdef Q_OS_DARWIN
        (void)semaphore_signal(m_Semaphore);
#elif defined(Q_OS_UNIX)
        (void)sem_post(&m_Semaphore);
#elif defined(Q_OS_WIN)
        (void)ReleaseSemaphore(m_Semaphore, 1, nullptr);
#endif
    }

    void waitFor(int timeoutMs) noexcept
    {
        timeoutMs = std::max(0, timeoutMs);
#ifdef Q_OS_DARWIN
        mach_timespec_t timeout = {
            static_cast<unsigned int>(timeoutMs / 1000),
            static_cast<clock_res_t>((timeoutMs % 1000) * 1000000),
        };
        while (semaphore_timedwait(m_Semaphore, timeout) == KERN_ABORTED) {
        }
#elif defined(Q_OS_UNIX)
        struct timespec deadline = {};
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            return;
        }
        deadline.tv_sec += timeoutMs / 1000;
        deadline.tv_nsec += static_cast<long>((timeoutMs % 1000) * 1000000L);
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
        while (sem_timedwait(&m_Semaphore, &deadline) != 0 && errno == EINTR) {
        }
#elif defined(Q_OS_WIN)
        (void)WaitForSingleObject(m_Semaphore, static_cast<DWORD>(timeoutMs));
#endif
    }

    void drain() noexcept
    {
#ifdef Q_OS_DARWIN
        const mach_timespec_t noWait = {0, 0};
        while (semaphore_timedwait(m_Semaphore, noWait) == KERN_SUCCESS) {
        }
#elif defined(Q_OS_UNIX)
        while (sem_trywait(&m_Semaphore) == 0) {
        }
#elif defined(Q_OS_WIN)
        while (WaitForSingleObject(m_Semaphore, 0) == WAIT_OBJECT_0) {
        }
#endif
    }

private:
#ifdef Q_OS_DARWIN
    semaphore_t m_Semaphore = SEMAPHORE_NULL;
#elif defined(Q_OS_UNIX)
    sem_t m_Semaphore = {};
#elif defined(Q_OS_WIN)
    HANDLE m_Semaphore = nullptr;
#endif
};

}

class StatsTelemetry::ControlState
{
public:
    explicit ControlState(const Options& initialOptions)
        : directoryPath(nativePath(initialOptions.directoryPath)),
          sampleIntervalMs(initialOptions.sampleIntervalMs),
          maxFileCount(initialOptions.maxFileCount),
          maxFileBytes(initialOptions.maxFileBytes),
          ioFaultInjector(initialOptions.ioFaultInjector)
    {
    }

    class AccessGuard
    {
    public:
        explicit AccessGuard(ControlState* control) noexcept
            : m_Control(control)
        {
            // This counter and currentSession form a two-atomic lifetime
            // handshake. Sequential consistency guarantees that either a
            // retiring worker observes this accessor, or this accessor
            // observes the retired null pointer before dereferencing it.
            m_Control->activeAccessors.fetch_add(1, std::memory_order_seq_cst);
        }

        ~AccessGuard()
        {
            m_Control->activeAccessors.fetch_sub(1, std::memory_order_seq_cst);
        }

        AccessGuard(const AccessGuard&) = delete;
        AccessGuard& operator=(const AccessGuard&) = delete;

    private:
        ControlState* const m_Control;
    };

    void recordError(IoError error) noexcept
    {
        lastError.store(static_cast<int>(error), std::memory_order_release);
        errorCount.fetch_add(1, std::memory_order_release);
    }

    void waitUntilUnreferenced() const noexcept
    {
        int spins = 0;
        while (activeAccessors.load(std::memory_order_seq_cst) != 0) {
            if (++spins < 64) {
#ifdef Q_OS_UNIX
                (void)sched_yield();
#elif defined(Q_OS_WIN)
                (void)SwitchToThread();
#endif
            }
            else {
#ifdef Q_OS_UNIX
                const struct timespec pause = {0, 1000000L};
                (void)nanosleep(&pause, nullptr);
#elif defined(Q_OS_WIN)
                Sleep(1);
#endif
            }
        }
    }

    bool tryAcquireSessionSlot() noexcept
    {
        int observed = process->outstandingSessions.load(std::memory_order_relaxed);
        while (observed < MaxOutstandingSessions) {
            if (process->outstandingSessions.compare_exchange_weak(observed,
                                                                   observed + 1,
                                                                   std::memory_order_acq_rel,
                                                                   std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void releaseSessionSlot() noexcept
    {
        process->outstandingSessions.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::atomic<SessionState*> currentSession {nullptr};
    std::atomic<std::uint64_t> generation {0};
    std::atomic<std::uint32_t> activeAccessors {0};
    std::atomic<int> openFileCount {0};
    std::atomic<std::uint64_t> errorCount {0};
    std::atomic<int> lastError {static_cast<int>(IoError::None)};
    std::atomic<std::uint64_t> completedSessions {0};
    std::atomic<std::uint64_t> nativeSyncCalls {0};
    std::atomic<bool> globalEnabled {false};
    std::atomic<bool> overlayActive {false};
    std::atomic<bool> destroying {false};
    std::atomic<bool> startInProgress {false};
    std::atomic<std::uint64_t> lifecycleEpoch {0};

    const std::string directoryPath;
    const int sampleIntervalMs;
    const int maxFileCount;
    const std::int64_t maxFileBytes;
    const std::shared_ptr<IoFaultInjector> ioFaultInjector;
    std::mutex configMutex;
    StreamConfig streamConfig;

    // Process-wide production budget, active-file registry, and test lifetime
    // counters survive the facade and Qt application through shared ownership.
    const std::shared_ptr<TelemetryProcessState> process = sharedTelemetryProcessState();
};

class StatsTelemetry::SessionState final
{
public:
    SessionState(std::shared_ptr<ControlState> control,
                 StreamConfig streamConfig,
                 std::uint64_t generation)
        : m_Control(control),
          m_DirectoryPath(control->directoryPath),
          m_SampleIntervalMs(control->sampleIntervalMs),
          m_MaxFileCount(control->maxFileCount),
          m_MaxFileBytes(control->maxFileBytes),
          m_IoFaultInjector(control->ioFaultInjector),
          m_StreamConfig(streamConfig),
          m_Generation(generation),
          m_SessionId(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()),
          m_SessionStartedAtUnixMs(currentUnixMilliseconds())
    {
        m_Control->process->liveSessions.fetch_add(1, std::memory_order_relaxed);
    }

    ~SessionState()
    {
        m_Control->releaseSessionSlot();
        m_Control->process->liveSessions.fetch_sub(1, std::memory_order_relaxed);
    }

    const std::string& sessionId() const noexcept
    {
        return m_SessionId;
    }

    std::uint64_t generation() const noexcept
    {
        return m_Generation;
    }

    bool accepting() const noexcept
    {
        return m_Accepting.load(std::memory_order_acquire);
    }

    void requestStop() noexcept
    {
        m_Accepting.store(false, std::memory_order_release);
        m_StopRequested.store(true, std::memory_order_release);
        m_WakeSignal.signal();
    }

    void activate() noexcept
    {
        m_Activated.store(true, std::memory_order_release);
        m_WakeSignal.signal();
    }

    bool beforeWorkerStartForTests() noexcept
    {
        return beforeIo(IoOperation::WorkerStart);
    }

    void startPublishedBoundaryForTests() noexcept
    {
        (void)beforeIo(IoOperation::StartPublished);
    }

    void startReleaseBoundaryForTests() noexcept
    {
        (void)beforeIo(IoOperation::StartRelease);
    }

    bool startDetachedWorker(const std::shared_ptr<SessionState>& self) noexcept
    {
        auto* context = new (std::nothrow) std::shared_ptr<SessionState>(self);
        if (context == nullptr) {
            return false;
        }

#ifdef Q_OS_UNIX
        pthread_attr_t attributes;
        if (pthread_attr_init(&attributes) != 0) {
            delete context;
            return false;
        }
        if (pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) != 0) {
            pthread_attr_destroy(&attributes);
            delete context;
            return false;
        }

        pthread_t worker;
        const int createResult = pthread_create(&worker, &attributes, &SessionState::unixWorkerEntry, context);
        pthread_attr_destroy(&attributes);
        if (createResult != 0) {
            delete context;
            return false;
        }
        return true;
#elif defined(Q_OS_WIN)
        uintptr_t workerHandle = _beginthreadex(nullptr,
                                               0,
                                               &SessionState::windowsWorkerEntry,
                                               context,
                                               0,
                                               nullptr);
        if (workerHandle == 0) {
            delete context;
            return false;
        }
        CloseHandle(reinterpret_cast<HANDLE>(workerHandle));
        return true;
#endif
    }

    bool enqueue(const Sample& inputSample) noexcept
    {
        // Increment for every active-session publication attempt before the
        // capacity check. Queue pressure is therefore visible as sequence gaps.
        const std::uint64_t attemptSequence =
            m_SourceWindowAttemptSequence.fetch_add(1, std::memory_order_relaxed) + 1;

        const std::uint32_t writePosition = m_WritePosition.load(std::memory_order_relaxed);
        const std::uint32_t nextPosition = (writePosition + 1) % QueueCapacity;
        if (nextPosition == m_ReadPosition.load(std::memory_order_acquire)) {
            m_QueueDroppedSamples.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        Sample sample = inputSample;
        sample.sourceWindowSequence = attemptSequence;
        m_Queue[writePosition] = sample;

        // A stop or retoggle may race the POD copy. Do not publish that slot
        // unless this exact generation is still the accepting current session.
        if (!m_Accepting.load(std::memory_order_acquire) ||
                m_Control->generation.load(std::memory_order_acquire) != m_Generation ||
                m_Control->currentSession.load(std::memory_order_seq_cst) != this) {
            return false;
        }

        m_WritePosition.store(nextPosition, std::memory_order_release);
        return true;
    }

    void runProtected() noexcept
    {
        try {
            run();
        }
        catch (...) {
            m_Control->recordError(IoError::WorkerException);
            m_Accepting.store(false, std::memory_order_release);
            m_StopRequested.store(true, std::memory_order_release);
            forceCloseResourcesNoexcept();
            finish();
        }
    }

private:
#ifdef Q_OS_UNIX
    static void* unixWorkerEntry(void* opaque) noexcept
    {
        auto* context = static_cast<std::shared_ptr<SessionState>*>(opaque);
        std::shared_ptr<SessionState> session = std::move(*context);
        delete context;
        session->runProtected();
        return nullptr;
    }
#elif defined(Q_OS_WIN)
    static unsigned __stdcall windowsWorkerEntry(void* opaque) noexcept
    {
        auto* context = static_cast<std::shared_ptr<SessionState>*>(opaque);
        std::shared_ptr<SessionState> session = std::move(*context);
        delete context;
        session->runProtected();
        return 0;
    }
#endif

    void run()
    {
        m_Control->process->liveWorkers.fetch_add(1, std::memory_order_relaxed);
        setTelemetryWorkerPriority();

        while (!m_Activated.load(std::memory_order_acquire) &&
               !m_StopRequested.load(std::memory_order_acquire)) {
            // The counted semaphore prevents a notification window. The
            // timeout is only a fail-open fallback for an OS signaling error.
            m_WakeSignal.waitFor(1000);
        }
        m_WakeSignal.drain();

        if (m_StopRequested.load(std::memory_order_acquire)) {
            finish();
            return;
        }

#ifndef Q_OS_UNIX
        m_File = std::make_unique<QFile>();
#endif
        if (!prepareDirectory() || !openNextFile()) {
            if (!m_StopRequested.load(std::memory_order_acquire)) {
                failCurrentSession(m_LastOpenError);
            }
            closeResources(false);
#ifndef Q_OS_UNIX
            m_File.reset();
#endif
            finish();
            return;
        }

        Sample latestSample = {};
        bool haveLatestSample = false;

        while (!m_StopRequested.load(std::memory_order_acquire)) {
            m_WakeSignal.waitFor(m_SampleIntervalMs);

            if (m_StopRequested.load(std::memory_order_acquire)) {
                break;
            }

            Sample queuedSample;
            bool wroteFreshSample = false;
            while (!m_StopRequested.load(std::memory_order_acquire) && dequeue(queuedSample)) {
                latestSample = queuedSample;
                haveLatestSample = true;
                const WriteResult result = writeSample(queuedSample, false);
                if (result == WriteResult::Cancelled) {
                    break;
                }
                if (result == WriteResult::Failed) {
                    failCurrentSession(m_LastWriteError);
                    break;
                }
                wroteFreshSample = true;
            }

            if (!accepting()) {
                break;
            }

            if (haveLatestSample && !wroteFreshSample) {
                const WriteResult result = writeSample(latestSample, true);
                if (result == WriteResult::Cancelled) {
                    break;
                }
                if (result == WriteResult::Failed) {
                    failCurrentSession(m_LastWriteError);
                    break;
                }
            }
        }

        closeResources(true);
#ifndef Q_OS_UNIX
        m_File.reset();
#endif
        finish();
    }

    enum class WriteResult {
        Success,
        Cancelled,
        Failed,
    };

    bool beforeIo(IoOperation operation)
    {
        try {
            if (m_IoFaultInjector &&
                    !m_IoFaultInjector->beforeIo(operation, m_StopRequested)) {
                return false;
            }
            return true;
        }
        catch (...) {
            return false;
        }
    }

    void finish() noexcept
    {
        if (m_Finished.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        m_Accepting.store(false, std::memory_order_release);
        SessionState* expected = this;
        if (m_Control->currentSession.compare_exchange_strong(expected,
                                                              nullptr,
                                                              std::memory_order_seq_cst,
                                                              std::memory_order_seq_cst)) {
            m_Control->generation.fetch_add(1, std::memory_order_acq_rel);
        }

        // No new accessor can obtain this session after currentSession is null.
        // Waiting happens only on the retiring worker, never on SDL/UI/input.
        m_Control->waitUntilUnreferenced();
        m_Control->completedSessions.fetch_add(1, std::memory_order_relaxed);
        m_Control->process->liveWorkers.fetch_sub(1, std::memory_order_relaxed);
    }

    void forceCloseResourcesNoexcept() noexcept
    {
#ifdef Q_OS_UNIX
        if (m_FileFd >= 0) {
            (void)::close(m_FileFd);
            m_FileFd = -1;
            m_FileBytes = 0;
        }
        if (m_DirectoryFd >= 0) {
            (void)::close(m_DirectoryFd);
            m_DirectoryFd = -1;
        }
#else
        try {
            if (m_File) {
                m_File->close();
                m_File.reset();
            }
        }
        catch (...) {
        }
#endif

        if (m_FileRegistered.exchange(false, std::memory_order_acq_rel)) {
            try {
                std::lock_guard<std::mutex> registryLock(m_Control->process->fileRegistryMutex);
                m_Control->process->activeFilePaths.erase(m_ActiveFilePath);
            }
            catch (...) {
            }
            m_ActiveFilePath.clear();
            m_Control->openFileCount.fetch_sub(1, std::memory_order_release);
        }
    }

    void failCurrentSession(IoError error) noexcept
    {
        m_Control->recordError(error);
        m_Accepting.store(false, std::memory_order_release);
        m_StopRequested.store(true, std::memory_order_release);
        SessionState* expected = this;
        if (m_Control->currentSession.compare_exchange_strong(expected,
                                                              nullptr,
                                                              std::memory_order_seq_cst,
                                                              std::memory_order_seq_cst)) {
            m_Control->generation.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    bool prepareDirectory()
    {
        if (m_StopRequested.load(std::memory_order_acquire)) {
            return false;
        }

#ifdef Q_OS_UNIX
        if (::mkdir(m_DirectoryPath.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
            m_LastOpenError = IoError::Directory;
            return false;
        }

        const int directoryFd = ::open(m_DirectoryPath.c_str(),
                                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directoryFd < 0) {
            m_LastOpenError = IoError::Directory;
            return false;
        }

        struct stat directoryStat = {};
        if (::fstat(directoryFd, &directoryStat) != 0 ||
                !S_ISDIR(directoryStat.st_mode) ||
                directoryStat.st_uid != ::geteuid() ||
                ::fchmod(directoryFd, S_IRWXU) != 0 ||
                ::fstat(directoryFd, &directoryStat) != 0 ||
                (directoryStat.st_mode & 0777) != S_IRWXU) {
            (void)::close(directoryFd);
            m_LastOpenError = IoError::Directory;
            return false;
        }

        m_DirectoryFd = directoryFd;
#else
        const QString directoryPath = QString::fromStdString(m_DirectoryPath);
        if (!QDir().mkpath(directoryPath) ||
                !QFile::setPermissions(directoryPath,
                                       QFileDevice::ReadOwner |
                                       QFileDevice::WriteOwner |
                                       QFileDevice::ExeOwner)) {
            m_LastOpenError = IoError::Directory;
            return false;
        }
#endif

        return !m_StopRequested.load(std::memory_order_acquire);
    }

    bool pruneForNewFileLocked()
    {
#ifdef Q_OS_UNIX
        struct Candidate {
            std::string name;
            std::string registryPath;
            dev_t device = 0;
            ino_t inode = 0;
            std::int64_t modifiedSeconds = 0;
            long modifiedNanoseconds = 0;
        };

        // openat(".") creates an independent directory file description.
        // dup() would share the directory offset and make later scans start at
        // EOF after the first rotation pass.
        const int scanFd = ::openat(m_DirectoryFd,
                                    ".",
                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (scanFd < 0) {
            m_LastOpenError = IoError::Rotation;
            return false;
        }
        DIR* const directory = ::fdopendir(scanFd);
        if (directory == nullptr) {
            (void)::close(scanFd);
            m_LastOpenError = IoError::Rotation;
            return false;
        }

        std::vector<Candidate> files;
        int scanError = 0;
        while (true) {
            errno = 0;
            dirent* const entry = ::readdir(directory);
            if (entry == nullptr) {
                scanError = errno;
                break;
            }
            const std::string name(entry->d_name);
            if (name.size() <= std::strlen("telemetry-.jsonl") ||
                    name.compare(0, std::strlen("telemetry-"), "telemetry-") != 0 ||
                    name.compare(name.size() - std::strlen(".jsonl"),
                                 std::strlen(".jsonl"),
                                 ".jsonl") != 0) {
                continue;
            }

            struct stat candidateStat = {};
            if (::fstatat(m_DirectoryFd,
                          name.c_str(),
                          &candidateStat,
                          AT_SYMLINK_NOFOLLOW) != 0 ||
                    !S_ISREG(candidateStat.st_mode) ||
                    candidateStat.st_uid != ::geteuid()) {
                continue;
            }

            Candidate candidate;
            candidate.name = name;
            candidate.registryPath = m_DirectoryPath + "/" + name;
            candidate.device = candidateStat.st_dev;
            candidate.inode = candidateStat.st_ino;
#ifdef Q_OS_DARWIN
            candidate.modifiedSeconds = candidateStat.st_mtimespec.tv_sec;
            candidate.modifiedNanoseconds = candidateStat.st_mtimespec.tv_nsec;
#else
            candidate.modifiedSeconds = candidateStat.st_mtim.tv_sec;
            candidate.modifiedNanoseconds = candidateStat.st_mtim.tv_nsec;
#endif
            files.emplace_back(std::move(candidate));
        }
        ::closedir(directory);
        if (scanError != 0) {
            m_LastOpenError = IoError::Rotation;
            return false;
        }

        std::sort(files.begin(), files.end(), [](const Candidate& left, const Candidate& right) {
            if (left.modifiedSeconds != right.modifiedSeconds) {
                return left.modifiedSeconds < right.modifiedSeconds;
            }
            if (left.modifiedNanoseconds != right.modifiedNanoseconds) {
                return left.modifiedNanoseconds < right.modifiedNanoseconds;
            }
            return left.name < right.name;
        });

        int remaining = static_cast<int>(files.size());
        for (const Candidate& candidate : files) {
            if (remaining < m_MaxFileCount) {
                break;
            }
            if (m_Control->process->activeFilePaths.find(candidate.registryPath) !=
                    m_Control->process->activeFilePaths.end()) {
                continue;
            }

            // Revalidate the exact directory entry immediately before the
            // fd-relative unlink. Symlinks are never followed or removed as
            // if they were telemetry files.
            struct stat currentStat = {};
            if (::fstatat(m_DirectoryFd,
                          candidate.name.c_str(),
                          &currentStat,
                          AT_SYMLINK_NOFOLLOW) != 0 ||
                    !S_ISREG(currentStat.st_mode) ||
                    currentStat.st_uid != ::geteuid() ||
                    currentStat.st_dev != candidate.device ||
                    currentStat.st_ino != candidate.inode ||
                    ::unlinkat(m_DirectoryFd, candidate.name.c_str(), 0) != 0) {
                m_LastOpenError = IoError::Rotation;
                return false;
            }
            --remaining;
        }

        if (remaining >= m_MaxFileCount) {
            m_LastOpenError = IoError::Rotation;
            return false;
        }
        return true;
#else
        QDir directory(QString::fromStdString(m_DirectoryPath));
        QFileInfoList files = directory.entryInfoList(
            QStringList(QStringLiteral("telemetry-*.jsonl")),
            QDir::Files | QDir::NoSymLinks,
            QDir::Time | QDir::Reversed);

        int remaining = files.size();
        for (const QFileInfo& candidate : files) {
            if (remaining < m_MaxFileCount) {
                break;
            }
            const std::string candidatePath = candidate.absoluteFilePath().toStdString();
            if (candidate.isSymLink() || !candidate.isFile() ||
                    m_Control->process->activeFilePaths.find(candidatePath) !=
                        m_Control->process->activeFilePaths.end()) {
                continue;
            }
            if (!QFile::remove(candidate.absoluteFilePath())) {
                m_LastOpenError = IoError::Rotation;
                return false;
            }
            --remaining;
        }

        if (remaining >= m_MaxFileCount) {
            m_LastOpenError = IoError::Rotation;
            return false;
        }
        return true;
#endif
    }

    bool openNextFile()
    {
        if (m_StopRequested.load(std::memory_order_acquire)) {
            return false;
        }

        std::lock_guard<std::mutex> registryLock(m_Control->process->fileRegistryMutex);
        if (m_StopRequested.load(std::memory_order_acquire) || !pruneForNewFileLocked()) {
            return false;
        }

#ifdef Q_OS_UNIX
        std::ostringstream fileNameBuilder;
        fileNameBuilder.imbue(std::locale::classic());
        fileNameBuilder << "telemetry-" << compactUtcTimestamp(m_SessionStartedAtUnixMs)
                        << '-' << m_SessionId
                        << "-p" << std::setw(3) << std::setfill('0') << m_FileSequence++
                        << ".jsonl";
        const std::string fileName = fileNameBuilder.str();
        const std::string absolutePath = m_DirectoryPath + "/" + fileName;
        const int fd = ::openat(m_DirectoryFd,
                                fileName.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                                S_IRUSR | S_IWUSR);
        if (fd < 0 || ::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
            if (fd >= 0) {
                ::close(fd);
                ::unlinkat(m_DirectoryFd, fileName.c_str(), 0);
            }
            m_LastOpenError = IoError::Open;
            return false;
        }

        m_FileFd = fd;
        m_FileBytes = 0;
#else
        const QString startStamp = QDateTime::fromMSecsSinceEpoch(m_SessionStartedAtUnixMs, Qt::UTC)
                                       .toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz'Z'"));
        const QString fileName = QStringLiteral("telemetry-%1-%2-p%3.jsonl")
                                     .arg(startStamp,
                                          QString::fromStdString(m_SessionId),
                                          QString::number(m_FileSequence++).rightJustified(3, QLatin1Char('0')));
        const QString absolutePath = QDir(QString::fromStdString(m_DirectoryPath)).absoluteFilePath(fileName);
        m_File->setFileName(absolutePath);
        if (!m_File->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text) ||
                !m_File->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            m_File->close();
            m_LastOpenError = IoError::Open;
            return false;
        }
#endif

#ifdef Q_OS_UNIX
        m_ActiveFilePath = absolutePath;
#else
        m_ActiveFilePath = absolutePath.toStdString();
#endif
        m_Control->process->activeFilePaths.insert(m_ActiveFilePath);
        m_FileRegistered.store(true, std::memory_order_release);
        m_Control->openFileCount.fetch_add(1, std::memory_order_release);
        return true;
    }

    std::string serializeSample(const Sample& sample, bool sourceStale)
    {
        const std::int64_t emittedAtUnixMs = currentUnixMilliseconds();
        const std::int64_t sourceAgeMs = std::max<std::int64_t>(0, emittedAtUnixMs - sample.capturedAtUnixMs);
        const auto codecEnd = std::find(sample.codec.cbegin(), sample.codec.cend(), '\0');
        const std::string codec(sample.codec.data(),
                                static_cast<std::size_t>(std::distance(sample.codec.cbegin(), codecEnd)));
        const bool haveHostLatency = sample.framesWithHostProcessingLatency > 0;
        const bool haveRtt = sample.lastRttMs != 0;

        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << std::setprecision(17);
        output << "{\"schema_version\":1,\"session_id\":";
        appendJsonString(output, m_SessionId);
        output << ",\"sample_sequence\":" << ++m_OutputSequence
               << ",\"source_window_sequence\":" << sample.sourceWindowSequence
               << ",\"timestamp_utc\":";
        appendJsonString(output, utcTimestamp(emittedAtUnixMs));
        output << ",\"timestamp_unix_ms\":" << emittedAtUnixMs
               << ",\"source_window_timestamp_utc\":";
        appendJsonString(output, utcTimestamp(sample.capturedAtUnixMs));
        output << ",\"source_age_ms\":" << sourceAgeMs
               << ",\"source_stale\":" << (sourceStale ? "true" : "false")
               << ",\"stream\":{\"width\":" << m_StreamConfig.width
               << ",\"height\":" << m_StreamConfig.height
               << ",\"configured_fps\":" << m_StreamConfig.configuredFps
               << ",\"total_fps\":";
        appendNullableNumber(output, sample.totalFps, true);
        output << ",\"codec\":";
        appendJsonString(output, codec);
        output << ",\"hdr\":" << (sample.hdr ? "true" : "false")
               << ",\"requested_bitrate_kbps\":" << m_StreamConfig.requestedBitrateKbps
               << "},\"fps\":{\"incoming\":";
        appendNullableNumber(output, sample.receivedFps, true);
        output << ",\"decode\":";
        appendNullableNumber(output, sample.decodedFps, true);
        output << ",\"render\":";
        appendNullableNumber(output, sample.renderedFps, true);
        output << "},\"host_processing_ms\":{\"min\":";
        appendNullableNumber(output,
                             static_cast<double>(sample.minHostProcessingLatency) / 10.0,
                             haveHostLatency);
        output << ",\"max\":";
        appendNullableNumber(output,
                             static_cast<double>(sample.maxHostProcessingLatency) / 10.0,
                             haveHostLatency);
        output << ",\"average\":";
        appendNullableRatio(output,
                            static_cast<double>(sample.totalHostProcessingLatency) / 10.0,
                            sample.framesWithHostProcessingLatency);
        output << "},\"drops_percent\":{\"network\":";
        appendNullableRatio(output,
                            static_cast<double>(sample.networkDroppedFrames) * 100.0,
                            sample.totalFrames);
        output << ",\"pacer_jitter\":";
        appendNullableRatio(output,
                            static_cast<double>(sample.pacerDroppedFrames) * 100.0,
                            sample.decodedFrames);
        output << "},\"network\":{\"rtt_ms\":";
        appendNullableNumber(output, sample.lastRttMs, haveRtt);
        output << ",\"rtt_variance_ms\":";
        appendNullableNumber(output, sample.lastRttVarianceMs, haveRtt);
        output << "},\"timing\":{\"decode_ms\":";
        appendNullableRatio(output,
                            static_cast<double>(sample.totalDecodeTimeUs) / 1000.0,
                            sample.decodedFrames);
        output << ",\"frame_queue_ms\":";
        appendNullableRatio(output,
                            static_cast<double>(sample.totalPacerTimeUs) / 1000.0,
                            sample.renderedFrames);
        output << ",\"render_including_vsync_ms\":";
        appendNullableRatio(output,
                            static_cast<double>(sample.totalRenderTimeUs) / 1000.0,
                            sample.renderedFrames);
        output << "},\"raw\":{\"received_frames\":" << sample.receivedFrames
               << ",\"decoded_frames\":" << sample.decodedFrames
               << ",\"rendered_frames\":" << sample.renderedFrames
               << ",\"total_frames\":" << sample.totalFrames
               << ",\"network_dropped_frames\":" << sample.networkDroppedFrames
               << ",\"pacer_dropped_frames\":" << sample.pacerDroppedFrames
               << ",\"frames_with_host_processing_latency\":"
               << sample.framesWithHostProcessingLatency
               << ",\"total_host_processing_latency_tenths_ms\":"
               << sample.totalHostProcessingLatency
               << ",\"total_reassembly_time_us\":" << sample.totalReassemblyTimeUs
               << ",\"total_decode_time_us\":" << sample.totalDecodeTimeUs
               << ",\"total_frame_queue_time_us\":" << sample.totalPacerTimeUs
               << ",\"total_render_time_us\":" << sample.totalRenderTimeUs
               << ",\"measurement_start_monotonic_us\":" << sample.measurementStartUs
               << ",\"measurement_duration_us\":" << sample.measurementDurationUs
               << ",\"telemetry_queue_dropped_samples\":"
               << m_QueueDroppedSamples.load(std::memory_order_relaxed)
               << "}}\n";
        return output.str();
    }

    WriteResult writeSample(const Sample& sample, bool sourceStale)
    {
        if (m_StopRequested.load(std::memory_order_acquire)) {
            return WriteResult::Cancelled;
        }

        const std::string line = serializeSample(sample, sourceStale);
#ifdef Q_OS_UNIX
        const std::int64_t currentBytes = m_FileBytes;
#else
        const std::int64_t currentBytes = m_File->size();
#endif
        if (currentBytes > 0 &&
                currentBytes + static_cast<std::int64_t>(line.size()) > m_MaxFileBytes) {
            closeFile(true);
            if (m_StopRequested.load(std::memory_order_acquire)) {
                return WriteResult::Cancelled;
            }
            if (!openNextFile()) {
                m_LastWriteError = m_LastOpenError;
                return WriteResult::Failed;
            }
        }

        if (!beforeIo(IoOperation::Write)) {
            m_LastWriteError = IoError::Write;
            return WriteResult::Failed;
        }
        if (m_StopRequested.load(std::memory_order_acquire)) {
            return WriteResult::Cancelled;
        }
#ifdef Q_OS_UNIX
        if (m_StopRequested.load(std::memory_order_acquire)) {
            return WriteResult::Cancelled;
        }
        std::size_t offset = 0;
        while (offset < line.size()) {
            const ssize_t written = ::write(m_FileFd,
                                            line.data() + offset,
                                            line.size() - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                m_LastWriteError = IoError::Write;
                return WriteResult::Failed;
            }
            offset += static_cast<std::size_t>(written);
        }
        m_FileBytes += static_cast<std::int64_t>(line.size());
#else
        if (m_File->write(line.data(), static_cast<qint64>(line.size())) !=
                static_cast<qint64>(line.size())) {
            m_LastWriteError = IoError::Write;
            return WriteResult::Failed;
        }
#endif

        if (m_StopRequested.load(std::memory_order_acquire)) {
            return WriteResult::Cancelled;
        }

        if (!beforeIo(IoOperation::Flush)) {
            m_LastWriteError = IoError::Flush;
            return WriteResult::Failed;
        }
#ifndef Q_OS_UNIX
        if (m_StopRequested.load(std::memory_order_acquire)) {
            return WriteResult::Cancelled;
        }
        if (!m_File->flush()) {
            m_LastWriteError = IoError::Flush;
            return WriteResult::Failed;
        }
#endif

        return WriteResult::Success;
    }

    void closeFile(bool durableFlush)
    {
#ifdef Q_OS_UNIX
        if (m_FileFd < 0) {
#else
        if (!m_File || !m_File->isOpen()) {
#endif
            return;
        }

        bool flushSucceeded = true;
        if (durableFlush) {
            flushSucceeded = beforeIo(IoOperation::Flush);
        }
#ifndef Q_OS_UNIX
        if (durableFlush) {
            flushSucceeded = flushSucceeded && m_File->flush();
        }
#endif
        if (!flushSucceeded) {
            m_Control->recordError(IoError::Flush);
        }
#ifdef Q_OS_UNIX
        if (durableFlush) {
            const bool syncBoundarySucceeded = beforeIo(IoOperation::Sync);
            bool nativeSyncSucceeded = false;
            if (syncBoundarySucceeded) {
                m_Control->nativeSyncCalls.fetch_add(1, std::memory_order_relaxed);
                nativeSyncSucceeded = ::fsync(m_FileFd) == 0;
            }
            if (!syncBoundarySucceeded || !nativeSyncSucceeded) {
                m_Control->recordError(IoError::Sync);
            }
        }
        ::close(m_FileFd);
        m_FileFd = -1;
        m_FileBytes = 0;
#else
        (void)durableFlush;
        m_File->close();
#endif

        if (m_FileRegistered.exchange(false, std::memory_order_acq_rel)) {
            {
                std::lock_guard<std::mutex> registryLock(m_Control->process->fileRegistryMutex);
                m_Control->process->activeFilePaths.erase(m_ActiveFilePath);
            }
            m_ActiveFilePath.clear();
            m_Control->openFileCount.fetch_sub(1, std::memory_order_release);
        }
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

    bool dequeue(Sample& sample) noexcept
    {
        const std::uint32_t readPosition = m_ReadPosition.load(std::memory_order_relaxed);
        if (readPosition == m_WritePosition.load(std::memory_order_acquire)) {
            return false;
        }

        sample = m_Queue[readPosition];
        m_ReadPosition.store((readPosition + 1) % QueueCapacity, std::memory_order_release);
        return true;
    }

    const std::shared_ptr<ControlState> m_Control;
    const std::string m_DirectoryPath;
    const int m_SampleIntervalMs;
    const int m_MaxFileCount;
    const std::int64_t m_MaxFileBytes;
    const std::shared_ptr<IoFaultInjector> m_IoFaultInjector;
    const StreamConfig m_StreamConfig;
    const std::uint64_t m_Generation;
    const std::string m_SessionId;
    const std::int64_t m_SessionStartedAtUnixMs;

    std::array<Sample, QueueCapacity> m_Queue = {};
    std::atomic<std::uint32_t> m_WritePosition {0};
    std::atomic<std::uint32_t> m_ReadPosition {0};
    std::atomic<std::uint64_t> m_SourceWindowAttemptSequence {0};
    std::atomic<std::uint64_t> m_QueueDroppedSamples {0};
    std::atomic<bool> m_Accepting {true};
    std::atomic<bool> m_Activated {false};
    std::atomic<bool> m_StopRequested {false};
    std::atomic<bool> m_Finished {false};
    std::atomic<bool> m_FileRegistered {false};
    WakeSignal m_WakeSignal;
    std::string m_ActiveFilePath;
    std::uint64_t m_OutputSequence = 0;
    int m_FileSequence = 0;
    IoError m_LastOpenError = IoError::Open;
    IoError m_LastWriteError = IoError::Write;
#ifdef Q_OS_UNIX
    int m_DirectoryFd = -1;
    int m_FileFd = -1;
    std::int64_t m_FileBytes = 0;
#else
    std::unique_ptr<QFile> m_File;
#endif
};

StatsTelemetry::StatsTelemetry()
    : StatsTelemetry(Options())
{
}

StatsTelemetry::StatsTelemetry(Options options)
{
    if (options.directoryPath.isEmpty()) {
        options.directoryPath = defaultDirectoryPath();
    }
    options.directoryPath = QDir(options.directoryPath).absolutePath();
    options.sampleIntervalMs = std::max(1, options.sampleIntervalMs);
    options.maxFileCount = std::max(1, options.maxFileCount);
    options.maxFileBytes = std::max<std::int64_t>(1024, options.maxFileBytes);
    m_Control = std::make_shared<ControlState>(options);
}

StatsTelemetry::~StatsTelemetry()
{
    ControlState* const control = m_Control.get();
    control->destroying.store(true, std::memory_order_release);
    control->overlayActive.store(false, std::memory_order_release);
    control->globalEnabled.store(false, std::memory_order_release);
    control->lifecycleEpoch.fetch_add(1, std::memory_order_seq_cst);
    stop(control);
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
    const auto control = m_Control;
    if (control->currentSession.load(std::memory_order_seq_cst) != nullptr ||
            control->startInProgress.load(std::memory_order_acquire)) {
        return;
    }
    try {
        std::lock_guard<std::mutex> configLock(control->configMutex);
        if (control->currentSession.load(std::memory_order_seq_cst) == nullptr &&
                !control->startInProgress.load(std::memory_order_acquire)) {
            control->streamConfig = config;
        }
    }
    catch (...) {
        control->recordError(IoError::WorkerStart);
    }
}

void StatsTelemetry::setGloballyEnabled(bool enabled)
{
    const auto control = m_Control;
    control->globalEnabled.store(enabled, std::memory_order_release);
    control->lifecycleEpoch.fetch_add(1, std::memory_order_seq_cst);
    reconcile(control);
}

void StatsTelemetry::setOverlayActive(bool active)
{
    const auto control = m_Control;
    control->overlayActive.store(active, std::memory_order_release);
    control->lifecycleEpoch.fetch_add(1, std::memory_order_seq_cst);
    reconcile(control);
}

bool StatsTelemetry::publish(const Sample& sample) noexcept
{
    ControlState* const control = m_Control.get();
    ControlState::AccessGuard access(control);
    SessionState* const session = control->currentSession.load(std::memory_order_seq_cst);
    if (session == nullptr || !session->accepting() ||
            control->generation.load(std::memory_order_acquire) != session->generation()) {
        return false;
    }
    return session->enqueue(sample);
}

bool StatsTelemetry::isSessionActive() const noexcept
{
    ControlState* const control = m_Control.get();
    ControlState::AccessGuard access(control);
    SessionState* const session = control->currentSession.load(std::memory_order_seq_cst);
    return session != nullptr && session->accepting() &&
           control->generation.load(std::memory_order_acquire) == session->generation() &&
           shouldRun(control);
}

bool StatsTelemetry::isFileOpen() const noexcept
{
    return m_Control->openFileCount.load(std::memory_order_acquire) != 0;
}

QString StatsTelemetry::currentSessionId() const
{
    ControlState* const control = m_Control.get();
    ControlState::AccessGuard access(control);
    SessionState* const session = control->currentSession.load(std::memory_order_seq_cst);
    return session != nullptr && session->accepting() &&
            control->generation.load(std::memory_order_acquire) == session->generation() &&
            shouldRun(control) ?
        QString::fromStdString(session->sessionId()) : QString();
}

std::uint64_t StatsTelemetry::ioErrorCount() const noexcept
{
    return m_Control->errorCount.load(std::memory_order_acquire);
}

StatsTelemetry::IoError StatsTelemetry::lastIoError() const noexcept
{
    return static_cast<IoError>(m_Control->lastError.load(std::memory_order_acquire));
}

std::uint64_t StatsTelemetry::completedSessionCount() const noexcept
{
    return m_Control->completedSessions.load(std::memory_order_acquire);
}

std::uint64_t StatsTelemetry::nativeSyncCallCountForTests() const noexcept
{
    return m_Control->nativeSyncCalls.load(std::memory_order_acquire);
}

int StatsTelemetry::liveWorkerCountForTests() noexcept
{
    return sharedTelemetryProcessState()->liveWorkers.load(std::memory_order_acquire);
}

int StatsTelemetry::liveSessionCountForTests() noexcept
{
    return sharedTelemetryProcessState()->liveSessions.load(std::memory_order_acquire);
}

bool StatsTelemetry::shouldRun(const ControlState* control) noexcept
{
    return !control->destroying.load(std::memory_order_acquire) &&
           control->globalEnabled.load(std::memory_order_acquire) &&
           control->overlayActive.load(std::memory_order_acquire);
}

void StatsTelemetry::reconcile(const std::shared_ptr<ControlState>& control) noexcept
{
    if (shouldRun(control.get())) {
        if (control->currentSession.load(std::memory_order_seq_cst) == nullptr) {
            tryStart(control);
        }
    }
    else {
        stop(control.get());
    }
}

void StatsTelemetry::tryStart(const std::shared_ptr<ControlState>& control) noexcept
{
    for (;;) {
    bool expectedStart = false;
    if (!control->startInProgress.compare_exchange_strong(expectedStart,
                                                          true,
                                                          std::memory_order_seq_cst,
                                                          std::memory_order_seq_cst)) {
        return;
    }
    const std::uint64_t ownedLifecycleEpoch =
        control->lifecycleEpoch.load(std::memory_order_seq_cst);

    if (!shouldRun(control.get()) ||
            control->currentSession.load(std::memory_order_seq_cst) != nullptr) {
        if (releaseStart(control, ownedLifecycleEpoch)) {
            continue;
        }
        return;
    }

    // One live writer plus one asynchronously retiring writer is sufficient
    // for a seamless retoggle. If kernel I/O pins both indefinitely, reject
    // further telemetry sessions instead of growing detached resources without
    // bound. Streaming and the visible overlay continue unaffected.
    if (!control->tryAcquireSessionSlot()) {
        control->recordError(IoError::OutstandingSessionLimit);
        if (releaseStart(control, ownedLifecycleEpoch)) {
            continue;
        }
        return;
    }

    const std::uint64_t generation =
        control->generation.load(std::memory_order_acquire);
    std::shared_ptr<SessionState> session;
    try {
        StreamConfig streamConfig;
        {
            std::lock_guard<std::mutex> configLock(control->configMutex);
            streamConfig = control->streamConfig;
        }
        session = std::make_shared<SessionState>(control,
                                                 streamConfig,
                                                 generation);
    }
    catch (...) {
        control->releaseSessionSlot();
        control->recordError(IoError::WorkerStart);
        if (releaseStart(control, ownedLifecycleEpoch)) {
            continue;
        }
        return;
    }

    if (!session->beforeWorkerStartForTests()) {
        control->recordError(IoError::WorkerStart);
        session.reset();
        if (releaseStart(control, ownedLifecycleEpoch)) {
            continue;
        }
        return;
    }

    if (!shouldRun(control.get()) ||
            control->generation.load(std::memory_order_acquire) != generation) {
        session.reset();
        if (releaseStart(control, ownedLifecycleEpoch)) {
            continue;
        }
        return;
    }

    if (!session->startDetachedWorker(session)) {
        control->recordError(IoError::WorkerStart);
        session.reset();
        if (releaseStart(control, ownedLifecycleEpoch)) {
            continue;
        }
        return;
    }

    SessionState* expectedSession = nullptr;
    if (!control->currentSession.compare_exchange_strong(expectedSession,
                                                         session.get(),
                                                         std::memory_order_seq_cst,
                                                         std::memory_order_seq_cst)) {
        session->requestStop();
        session->activate();
        session.reset();
        if (releaseStart(control, ownedLifecycleEpoch)) {
            continue;
        }
        return;
    }

    // Test-only barrier used to deterministically place OFF between pointer
    // publication and the late withdrawal check. Production has no injector.
    session->startPublishedBoundaryForTests();

    // OFF can race the publish CAS from either side. A post-publish check
    // closes the only window where OFF could have exchanged null immediately
    // before this start made its session visible.
    if (!shouldRun(control.get()) ||
            control->generation.load(std::memory_order_acquire) != generation) {
        expectedSession = session.get();
        (void)control->currentSession.compare_exchange_strong(expectedSession,
                                                              nullptr,
                                                              std::memory_order_seq_cst,
                                                              std::memory_order_seq_cst);
        session->requestStop();
    }

    // This barrier is deliberately after the late-withdrawal decision and
    // before startInProgress is released. It makes the historical lost-ON
    // interleaving reproducible without changing the production path.
    session->startReleaseBoundaryForTests();
    session->activate();
    session.reset();
    if (!releaseStart(control, ownedLifecycleEpoch)) {
        return;
    }
    }
}

bool StatsTelemetry::releaseStart(const std::shared_ptr<ControlState>& control,
                                  std::uint64_t ownedLifecycleEpoch) noexcept
{
    // Every owner exit funnels through this handoff. If an ON transition saw
    // startInProgress=true, its seq_cst epoch increment precedes our release
    // and is observed here. Conversely, if it occurs after this load, that ON
    // sees startInProgress=false and becomes the next owner itself.
    control->startInProgress.store(false, std::memory_order_seq_cst);
    const std::uint64_t releasedLifecycleEpoch =
        control->lifecycleEpoch.load(std::memory_order_seq_cst);
    return releasedLifecycleEpoch != ownedLifecycleEpoch &&
           shouldRun(control.get()) &&
           control->currentSession.load(std::memory_order_seq_cst) == nullptr;
}

void StatsTelemetry::stop(ControlState* control) noexcept
{
    // Invalidate the generation before detaching the raw pointer so every
    // concurrent publish/start attempt observes OFF on at least one side of
    // its final validation. No mutex, allocation, join, or telemetry I/O is
    // reachable from this path.
    control->generation.fetch_add(1, std::memory_order_acq_rel);
    ControlState::AccessGuard access(control);
    SessionState* const session =
        control->currentSession.exchange(nullptr, std::memory_order_seq_cst);
    if (session != nullptr) {
        session->requestStop();
    }
}
