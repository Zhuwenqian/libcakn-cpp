#ifndef CKAN_FILELOCK_H
#define CKAN_FILELOCK_H

#include <QString>
#include "ckan_export.h"

namespace ckan {

// 跨进程文件锁：通过独占创建锁文件（registry.locked）实现互斥。
// 对应官方 CKAN 的 RegistryManager.GetLock/ReleaseLock。
// 写入持有者 PID 供陈旧锁检测；持有者退出/崩溃后锁可被接管。
class CKAN_API FileLock
{
public:
    FileLock() = default;
    ~FileLock() { release(); }

    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

    // 尝试锁定 lockPath。成功或已持有返回 true；被其他活跃进程占用返回 false。
    // 检测到陈旧锁（PID 已不存在/文件损坏）时自动清除并重试一次。
    bool acquire(const QString &lockPath);

    // 释放锁（删除锁文件）。未持有则无操作。
    void release();

    bool held() const { return m_held; }
    QString path() const { return m_path; }

private:
    // 锁文件内容是否为陈旧锁（PID 不可用或对应进程已退出）
    static bool isStaleLock(const QString &lockPath);
    // 指定 PID 的进程是否仍存活（跨平台）
    static bool processAlive(qint64 pid);

    QString m_path;
    bool    m_held = false;
};

} // namespace ckan

#endif // CKAN_FILELOCK_H
