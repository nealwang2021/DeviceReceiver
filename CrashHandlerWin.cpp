#include "CrashHandlerWin.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <atomic>
#include <exception>
#include <mutex>

namespace CrashHandlerWin {

namespace {
std::mutex g_stateMutex;
QString g_sessionTag;
LogCallback g_logCallback = nullptr;
std::atomic<bool> g_installed {false};
std::atomic<bool> g_handlingCrash {false};

QString executableDirPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        return appDir;
    }
#ifdef Q_OS_WIN
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (len > 0) {
        return QFileInfo(QString::fromWCharArray(modulePath, static_cast<int>(len))).absolutePath();
    }
#endif
    return QDir::currentPath();
}

QString ensureCrashDir()
{
    const QString dirPath = QDir(executableDirPath()).filePath(QStringLiteral("crash"));
    QDir dir(dirPath);
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }
    return dirPath;
}

void emitLog(const QString& msg)
{
    const QByteArray utf8 = msg.toUtf8();
    if (g_logCallback) {
        g_logCallback(utf8.constData());
    }
#ifdef Q_OS_WIN
    OutputDebugStringA(utf8.constData());
    OutputDebugStringA("\n");
#endif
}

QString buildDumpPath(const char* reasonTag)
{
    const QString now = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString session = g_sessionTag.isEmpty() ? QStringLiteral("nosession") : g_sessionTag;
    const qint64 pid = static_cast<qint64>(QCoreApplication::applicationPid());
    return QDir(ensureCrashDir()).filePath(
        QStringLiteral("realtime_data_%1_pid%2_%3_%4.dmp")
            .arg(session, QString::number(pid), QString::fromLatin1(reasonTag), now));
}

#ifdef Q_OS_WIN
bool writeMiniDump(const char* reasonTag, EXCEPTION_POINTERS* exceptionPointers)
{
    if (g_handlingCrash.exchange(true)) {
        return false;
    }

    const QString dumpPath = buildDumpPath(reasonTag);
    HANDLE hFile = CreateFileW(reinterpret_cast<LPCWSTR>(dumpPath.utf16()),
                               GENERIC_WRITE,
                               FILE_SHARE_READ,
                               nullptr,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        emitLog(QStringLiteral("[CrashHandler] 创建 dump 文件失败: %1").arg(dumpPath));
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exInfo;
    exInfo.ThreadId = GetCurrentThreadId();
    exInfo.ExceptionPointers = exceptionPointers;
    exInfo.ClientPointers = FALSE;

    const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);

    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(),
                                      GetCurrentProcessId(),
                                      hFile,
                                      dumpType,
                                      exceptionPointers ? &exInfo : nullptr,
                                      nullptr,
                                      nullptr);
    CloseHandle(hFile);

    if (!ok) {
        emitLog(QStringLiteral("[CrashHandler] MiniDumpWriteDump 失败, gle=%1").arg(GetLastError()));
        return false;
    }
    emitLog(QStringLiteral("[CrashHandler] 已写入 dump: %1").arg(dumpPath));
    return true;
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exceptionPointers)
{
    emitLog(QStringLiteral("[CrashHandler] 捕获未处理异常，准备写入 dump"));
    writeMiniDump("seh", exceptionPointers);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void terminateHandler()
{
    emitLog(QStringLiteral("[CrashHandler] 触发 std::terminate，准备写入 dump"));
#ifdef Q_OS_WIN
    writeMiniDump("terminate", nullptr);
#endif
    std::abort();
}

} // namespace

void setLogCallback(LogCallback callback)
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_logCallback = callback;
}

void setSessionTag(const QString& sessionTag)
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_sessionTag = sessionTag;
}

void installHandlers()
{
    if (g_installed.exchange(true)) {
        return;
    }
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
#endif
    std::set_terminate(terminateHandler);
    emitLog(QStringLiteral("[CrashHandler] 异常捕获钩子已安装"));
}

QString crashDirectoryPath()
{
    return ensureCrashDir();
}

} // namespace CrashHandlerWin
