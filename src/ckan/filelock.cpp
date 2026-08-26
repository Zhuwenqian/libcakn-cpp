#include "filelock.h"

#include <QFile>
#include <QCoreApplication>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <signal.h>
#  include <unistd.h>
#  include <cerrno>
#endif

namespace ckan {

bool FileLock::acquire(const QString &lockPath)
{
    if (m_held) return true;

    // 独占创建（QIODevice::NewOnly 对应 O_CREAT|O_EXCL）：文件已存在则打开失败。
    // 若存在陈旧锁（持有者已退出/崩溃残留），清除后重试一次。
    for (int attempt = 0; attempt < 2; ++attempt) {
        QFile f(lockPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            f.write(QByteArray::number(QCoreApplication::applicationPid()));
            f.flush();
            f.close();
            m_path = lockPath;
            m_held = true;
            return true;
        }
        if (isStaleLock(lockPath)) {
            QFile::remove(lockPath);
            continue; // 清除陈旧锁后重试
        }
        return false; // 存在且持有者进程存活：被占用
    }
    return false;
}

void FileLock::release()
{
    if (!m_held) return;
    QFile::remove(m_path);
    m_path.clear();
    m_held = false;
}

bool FileLock::isStaleLock(const QString &lockPath)
{
    QFile f(lockPath);
    if (!f.open(QIODevice::ReadOnly)) return false; // 无法读取：保守视为占用

    bool ok = false;
    const qint64 pid = f.readAll().trimmed().toLongLong(&ok);
    f.close();
    // 内容缺失/损坏（如崩溃在写入 PID 前）——视为陈旧锁
    if (!ok || pid <= 0) return true;
    // 本进程残留的锁（同一进程内多次加载）——视为陈旧
    if (pid == QCoreApplication::applicationPid()) return true;
    return !processAlive(pid);
}

bool FileLock::processAlive(qint64 pid)
{
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) return false; // 无法打开（权限/不存在）视为已退出
    CloseHandle(h);
    return true;
#else
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;  // 存活
    return errno == EPERM;                                    // 存活但无信号权限
#endif
}

} // namespace ckan
