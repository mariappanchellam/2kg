#include "DhakshaFlightLog.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QSysInfo>
#include <atomic>
#include <mutex>

#if defined(Q_OS_ANDROID) || defined(Q_OS_LINUX)
#define DHAKSHA_CRASH_HANDLER
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <unwind.h>

#include <cstdint>
#include <cstring>
#endif

namespace {

constexpr int kMaxFiles = 20;
constexpr qint64 kMaxBytes = 20 * 1024 * 1024;
constexpr qint64 kTailBytes = 64 * 1024;
const char kFilePrefix[] = "DhakshaFlight_";
const char kCleanExit[] = "=== Clean exit";
const char kCrashMark[] = "=== CRASH";

std::mutex gMutex;
QFile* gFile = nullptr;
qint64 gBytes = 0;
bool gFull = false;
std::atomic<int> gFd{-1};

void writeLocked(const QByteArray& text)
{
    if (gFile && (gFile->write(text) > 0)) {
        gBytes += text.size();
    }
}

/// What the newest earlier log says about how that run ended
QString previousRunSummary(const QDir& dir)
{
    const QStringList files =
        dir.entryList({QStringLiteral("%1*.log").arg(QLatin1String(kFilePrefix))}, QDir::Files, QDir::Name);
    if (files.isEmpty()) {
        return QStringLiteral("Previous run: no earlier log");
    }
    QFile previous(dir.filePath(files.last()));
    if (!previous.open(QIODevice::ReadOnly)) {
        return QStringLiteral("Previous run: %1 could not be read").arg(files.last());
    }
    if (previous.size() > kTailBytes) {
        (void) previous.seek(previous.size() - kTailBytes);
    }
    const QByteArray tail = previous.readAll();
    if (tail.contains(kCleanExit)) {
        return QStringLiteral("Previous run (%1) ended cleanly").arg(files.last());
    }
    if (tail.contains(kCrashMark)) {
        return QStringLiteral("Previous run (%1) CRASHED: its crash report is at the end of that file").arg(files.last());
    }
    return QStringLiteral("Previous run (%1) did not end cleanly and recorded no crash: the app was swiped away or "
                          "closed by Android, the phone lost power, or the app was stopped by a fault the log "
                          "cannot catch")
        .arg(files.last());
}

void removeOldFiles(const QDir& dir)
{
    QStringList files =
        dir.entryList({QStringLiteral("%1*.log").arg(QLatin1String(kFilePrefix))}, QDir::Files, QDir::Name);
    while (files.size() >= kMaxFiles) {
        (void) QFile::remove(dir.filePath(files.takeFirst()));
    }
}

#ifdef DHAKSHA_CRASH_HANDLER

// Only async-signal-safe calls from here on: write(2) to the already open file, no allocation, no Qt.
// dladdr is not formally async-signal-safe but is what crash reporters use to name the library.

constexpr int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
constexpr int kSignalCount = sizeof(kCrashSignals) / sizeof(kCrashSignals[0]);
constexpr int kMaxStackFrames = 40;
struct sigaction gPreviousActions[kSignalCount];
std::atomic<bool> gCrashing{false};
alignas(16) char gAltStack[64 * 1024];

void rawWrite(const char* text)
{
    const int fd = gFd.load();
    if (fd >= 0) {
        (void) !::write(fd, text, strlen(text));
    }
}

void rawWriteHex(uintptr_t value)
{
    char buffer[2 + (sizeof(uintptr_t) * 2) + 1];
    char* end = buffer + sizeof(buffer) - 1;
    *end = '\0';
    char* position = end;
    do {
        *--position = "0123456789abcdef"[value & 0xF];
        value >>= 4;
    } while (value != 0);
    *--position = 'x';
    *--position = '0';
    rawWrite(position);
}

void rawWriteDecimal(long value)
{
    char buffer[24];
    char* position = buffer + sizeof(buffer) - 1;
    *position = '\0';
    const bool negative = value < 0;
    unsigned long magnitude = negative ? static_cast<unsigned long>(-value) : static_cast<unsigned long>(value);
    do {
        *--position = static_cast<char>('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0);
    if (negative) {
        *--position = '-';
    }
    rawWrite(position);
}

const char* signalName(int signal)
{
    switch (signal) {
        case SIGSEGV:
            return "SIGSEGV (invalid memory access)";
        case SIGABRT:
            return "SIGABRT (abort)";
        case SIGBUS:
            return "SIGBUS (bus error)";
        case SIGFPE:
            return "SIGFPE (arithmetic error)";
        case SIGILL:
            return "SIGILL (illegal instruction)";
        default:
            return "unknown signal";
    }
}

/// One line per address: library + offset (what ndk-stack / addr2line need) and the symbol when exported
void rawWriteAddress(const char* label, uintptr_t pc)
{
    rawWrite(label);
    rawWriteHex(pc);
    Dl_info info;
    if ((dladdr(reinterpret_cast<void*>(pc), &info) != 0) && info.dli_fname) {
        const char* slash = strrchr(info.dli_fname, '/');
        rawWrite("  ");
        rawWrite(slash ? slash + 1 : info.dli_fname);
        rawWrite(" + ");
        rawWriteHex(pc - reinterpret_cast<uintptr_t>(info.dli_fbase));
        if (info.dli_sname) {
            rawWrite("  (");
            rawWrite(info.dli_sname);
            rawWrite(")");
        }
    }
    rawWrite("\n");
}

uintptr_t crashPc(void* context)
{
    if (!context) {
        return 0;
    }
    const ucontext_t* ucontext = static_cast<const ucontext_t*>(context);
#if defined(__aarch64__)
    return static_cast<uintptr_t>(ucontext->uc_mcontext.pc);
#elif defined(__arm__)
    return static_cast<uintptr_t>(ucontext->uc_mcontext.arm_pc);
#elif defined(__x86_64__)
    return static_cast<uintptr_t>(ucontext->uc_mcontext.gregs[REG_RIP]);
#elif defined(__i386__)
    return static_cast<uintptr_t>(ucontext->uc_mcontext.gregs[REG_EIP]);
#else
    (void) ucontext;
    return 0;
#endif
}

struct StackState
{
    uintptr_t frames[kMaxStackFrames];
    int count = 0;
};

_Unwind_Reason_Code collectFrame(_Unwind_Context* context, void* argument)
{
    StackState* state = static_cast<StackState*>(argument);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc != 0) {
        if (state->count >= kMaxStackFrames) {
            return _URC_END_OF_STACK;
        }
        state->frames[state->count++] = pc;
    }
    return _URC_NO_REASON;
}

void crashHandler(int signal, siginfo_t* info, void* context)
{
    int slot = 0;
    while ((slot < kSignalCount) && (kCrashSignals[slot] != signal)) {
        ++slot;
    }
    // Hand the signal back to the previous handler (Android's crash reporter writes its tombstone) when we return
    if (slot < kSignalCount) {
        (void) sigaction(signal, &gPreviousActions[slot], nullptr);
    }
    if (gCrashing.exchange(true)) {
        return;
    }

    rawWrite("\n");
    rawWrite(kCrashMark);
    rawWrite(": ");
    rawWrite(signalName(signal));
    rawWrite(", code ");
    rawWriteDecimal(info ? info->si_code : 0);
    rawWrite(", fault address ");
    rawWriteHex(info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0);
    rawWrite(", thread ");
    rawWriteDecimal(static_cast<long>(syscall(SYS_gettid)));
    rawWrite("\n");
    rawWriteAddress("Crashed at  ", crashPc(context));

    StackState stack;
    _Unwind_Backtrace(collectFrame, &stack);
    rawWrite("Call stack (crash handler frames first):\n");
    for (int i = 0; i < stack.count; ++i) {
        rawWriteAddress("  ", stack.frames[i]);
    }
    rawWrite("=== End of crash report\n");
    const int fd = gFd.load();
    if (fd >= 0) {
        (void) fsync(fd);
    }

    // A fault repeats with the previous handler when we return; an abort is raised again for it
    if (signal == SIGABRT) {
        (void) raise(signal);
    }
}

void installCrashHandler()
{
    stack_t altStack = {};
    altStack.ss_sp = gAltStack;
    altStack.ss_size = sizeof(gAltStack);
    (void) sigaltstack(&altStack, nullptr);

    struct sigaction action = {};
    action.sa_sigaction = crashHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int i = 0; i < kSignalCount; ++i) {
        (void) sigaction(kCrashSignals[i], &action, &gPreviousActions[i]);
    }
}

#endif  // DHAKSHA_CRASH_HANDLER

}  // namespace

namespace DhakshaFlightLog {

void start(const QString& directory, const QString& appVersion)
{
    std::lock_guard<std::mutex> locker(gMutex);
    if (gFile) {
        return;
    }
    QStringList candidates;
    if (!directory.isEmpty()) {
        candidates << directory;
    }
    candidates << QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                      .filePath(QStringLiteral("DhakshaGroundControl/Logs"));
    candidates << QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("Logs"));

    for (const QString& candidate : candidates) {
        QDir dir(candidate);
        if (!dir.mkpath(QStringLiteral("."))) {
            continue;
        }
        const QString summary = previousRunSummary(dir);
        removeOldFiles(dir);
        const QString name = QStringLiteral("%1%2.log")
                                 .arg(QLatin1String(kFilePrefix),
                                      QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_hh-mm-ss")));
        QFile* file = new QFile(dir.filePath(name));
        if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Unbuffered)) {
            delete file;
            continue;
        }
        gFile = file;
        gBytes = 0;
        gFull = false;
        gFd.store(file->handle());

        const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz "));
        writeLocked((stamp + QStringLiteral("=== DhakshaGroundControl %1 started on %2 (%3, %4)\n")
                                  .arg(appVersion, QSysInfo::prettyProductName(), QSysInfo::kernelVersion(),
                                       QSysInfo::currentCpuArchitecture()))
                        .toUtf8());
        writeLocked((stamp + summary + QLatin1Char('\n')).toUtf8());
#ifdef DHAKSHA_CRASH_HANDLER
        installCrashHandler();
#endif
        return;
    }
}

void write(const QString& line)
{
    const QByteArray text =
        (QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz ")) + line + QLatin1Char('\n'))
            .toUtf8();
    std::lock_guard<std::mutex> locker(gMutex);
    if (!gFile || gFull) {
        return;
    }
    if ((gBytes + text.size()) > kMaxBytes) {
        gFull = true;
        writeLocked(QByteArrayLiteral("=== Log size limit reached: later lines are not written\n"));
        return;
    }
    writeLocked(text);
}

void finish()
{
    write(QLatin1String(kCleanExit));
}

QString filePath()
{
    std::lock_guard<std::mutex> locker(gMutex);
    return gFile ? gFile->fileName() : QString();
}

}  // namespace DhakshaFlightLog
