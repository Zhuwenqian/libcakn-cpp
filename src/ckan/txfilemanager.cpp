#include "txfilemanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDirIterator>
#include <QAtomicInteger>

namespace ckan {

namespace {
// 跨线程安全的自增序号，用于保证同一毫秒内创建的事务子目录不重名
QAtomicInteger<quint64> g_txCounter{0};
}

TxFileManager::TxFileManager(const QString &txBaseDir)
{
    QDir().mkpath(txBaseDir);
    const quint64 n = g_txCounter.fetchAndAddRelaxed(1);
    m_txDir = txBaseDir + QStringLiteral("/tx_")
            + QString::number(QDateTime::currentMSecsSinceEpoch())
            + QStringLiteral("_") + QString::number(n);
    QDir().mkpath(m_txDir);
}

TxFileManager::~TxFileManager()
{
    if (!m_finished)
        rollback(); // 安全网：未显式提交/回滚则回滚
}

QString TxFileManager::nextFilePath()
{
    return m_txDir + QStringLiteral("/f_") + QString::number(m_seq++);
}

QString TxFileManager::nextDirPath()
{
    return m_txDir + QStringLiteral("/d_") + QString::number(m_seq++);
}

bool TxFileManager::snapshot(const QString &absPath)
{
    const QFileInfo fi(absPath);
    if (!fi.exists()) {
        m_files.append({absPath, QString(), false});
        return true;
    }
    if (fi.isDir())
        return false; // 目录不允许按文件快照，请用 deleteDir
    const QString backup = nextFilePath();
    if (!QFile::copy(absPath, backup))
        return false;
    m_files.append({absPath, backup, true});
    return true;
}

bool TxFileManager::deleteFile(const QString &absPath)
{
    const QFileInfo fi(absPath);
    if (!fi.exists()) {
        m_files.append({absPath, QString(), false});
        return true;
    }
    if (fi.isDir())
        return false; // 文件路径实为目录（注册表数据异常）视为失败
    const QString backup = nextFilePath();
    if (!QFile::copy(absPath, backup))
        return false;
    if (!QFile::remove(absPath))
        return false;
    m_files.append({absPath, backup, true});
    return true;
}

bool TxFileManager::deleteDir(const QString &absPath)
{
    QDir d(absPath);
    if (!d.exists()) {
        m_dirs.append({absPath, QString()});
        return true;
    }
    const QString backup = nextDirPath();
    // 优先整体移动（同卷原子、还原精确）；失败退回复制+删除
    if (QDir().rename(absPath, backup)) {
        m_dirs.append({absPath, backup});
        return true;
    }
    if (!copyDirRecursively(absPath, backup))
        return false;
    if (!d.removeRecursively())
        return false;
    m_dirs.append({absPath, backup});
    return true;
}

bool TxFileManager::copyFile(const QString &src, const QString &absDest)
{
    const QFileInfo di(absDest);
    if (!makePath(di.absolutePath()))
        return false;
    if (!snapshot(absDest))
        return false;
    return copyFileOverwrite(src, absDest);
}

bool TxFileManager::writeFile(const QString &absPath, const QByteArray &content)
{
    const QFileInfo di(absPath);
    if (!makePath(di.absolutePath()))
        return false;
    if (!snapshot(absPath))
        return false;
    QFile f(absPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(content);
    f.close();
    return true;
}

bool TxFileManager::makePath(const QString &absDir)
{
    const QString dir = QDir::cleanPath(absDir);
    // 收集尚不存在的各级目录（深->浅），供回滚时逐级删除
    QStringList toCreate;
    QString cur = dir;
    while (!cur.isEmpty() && !QDir(cur).exists()) {
        toCreate.append(cur);
        const int slash = cur.lastIndexOf(QLatin1Char('/'));
        cur = slash <= 0 ? QString() : cur.left(slash);
    }
    if (!QDir().mkpath(dir))
        return false;
    for (const QString &d : toCreate)
        if (!m_createdDirs.contains(d))
            m_createdDirs.append(d);
    return true;
}

void TxFileManager::commit()
{
    if (m_finished)
        return;
    m_finished = true;
    QDir(m_txDir).removeRecursively();
    m_files.clear();
    m_dirs.clear();
    m_createdDirs.clear();
}

void TxFileManager::rollback()
{
    if (m_finished)
        return;
    m_finished = true;

    // 1) 恢复被整目录删除的目录（先清掉事务期间写入的内容，再还原备份）
    for (const DirBackup &db : m_dirs) {
        if (db.backup.isEmpty())
            continue; // 目录原本不存在，无需恢复
        if (QDir(db.abs).exists())
            QDir(db.abs).removeRecursively();
        if (QDir().rename(db.backup, db.abs))
            continue;
        copyDirRecursively(db.backup, db.abs);
        QDir(db.backup).removeRecursively();
    }

    // 2) 文件级：恢复被覆盖/删除的文件，删除本事务新建的文件
    for (const FileBackup &fb : m_files) {
        if (fb.existed && !fb.backup.isEmpty())
            copyFileOverwrite(fb.backup, fb.abs);
        else
            QFile::remove(fb.abs);
    }

    // 3) 删除本事务新建的目录（由深到浅，仅删空目录）
    for (int i = m_createdDirs.size() - 1; i >= 0; --i)
        QDir().rmdir(m_createdDirs.at(i));

    // 4) 清理事务目录
    QDir(m_txDir).removeRecursively();
    m_files.clear();
    m_dirs.clear();
    m_createdDirs.clear();
}

bool TxFileManager::copyDirRecursively(const QString &src, const QString &dst)
{
    QDir d(dst);
    if (d.exists() && !d.removeRecursively())
        return false;
    if (!QDir().mkpath(dst))
        return false;
    QDirIterator it(src, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString srcAbs = it.next();
        const QString rel = it.fileInfo().isDir()
            ? QString() // 目录由后续文件复制时的 mkpath 创建
            : QDir(src).relativeFilePath(srcAbs);
        if (it.fileInfo().isDir())
            continue;
        const QString dstAbs = dst + QLatin1Char('/') + rel;
        if (!QDir().mkpath(QFileInfo(dstAbs).absolutePath()))
            return false;
        if (!QFile::copy(srcAbs, dstAbs))
            return false;
    }
    return true;
}

bool TxFileManager::copyFileOverwrite(const QString &src, const QString &dst)
{
    if (QFile::exists(dst) && !QFile::remove(dst))
        return false;
    return QFile::copy(src, dst);
}

} // namespace ckan
