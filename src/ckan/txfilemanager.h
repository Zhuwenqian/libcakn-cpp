#ifndef CKAN_TXFILEMANAGER_H
#define CKAN_TXFILEMANAGER_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QByteArray>

#include "ckan_export.h"

namespace ckan {

// 事务文件管理器：让文件操作可回滚（对应官方 CKAN 的 ChinhDo.Transactions.TxFileManager）。
//
// 用法：以实例 CKAN 目录下的一个独立子目录作为备份存储，
//       所有写/删/覆盖操作都先备份原文件/目录到事务目录；
//       commit() 提交（丢弃备份）；rollback() 回滚（恢复所有原始内容）。
//       未显式提交/回滚即析构时，自动执行回滚作为安全网。
//
// 回滚语义：
//   - 被覆盖/删除的文件   -> 从备份恢复原内容
//   - 本事务新建的文件     -> 删除
//   - 被整目录删除的目录   -> 恢复备份（保持事务开始前的精确状态）
//   - 本事务 mkpath 新建的目录 -> 从最深到最浅删除空目录
class CKAN_API TxFileManager
{
public:
    // txBaseDir 为事务存储的父目录（如 <gameDir>/CKAN/transactions）。
    // 构造时在其中创建唯一的子目录保存备份。
    explicit TxFileManager(const QString &txBaseDir);
    ~TxFileManager();

    TxFileManager(const TxFileManager &) = delete;
    TxFileManager &operator=(const TxFileManager &) = delete;

    // 若 absPath 不存在则仅记录；是目录返回 false（目录用 deleteDir）。
    bool snapshot(const QString &absPath);

    // 事务化删除文件（不存在视为成功，是目录视为错误）
    bool deleteFile(const QString &absPath);

    // 事务化递归删除目录（不存在视为成功）
    bool deleteDir(const QString &absPath);

    // 事务化复制 src -> absDest（覆盖已存在的目标）
    bool copyFile(const QString &src, const QString &absDest);

    // 事务化写入文件（自动创建父目录，覆盖已存在内容）
    bool writeFile(const QString &absPath, const QByteArray &content);

    // 创建目录（含中间层），记录新建目录以便回滚时清除
    bool makePath(const QString &absDir);

    // 提交：删除全部备份，事务结束
    void commit();

    // 回滚：恢复所有备份、删除本事务新建的文件/目录，事务结束
    void rollback();

    QString txDir() const { return m_txDir; }
    bool finished() const { return m_finished; }

private:
    struct FileBackup {
        QString abs;      // 原路径
        QString backup;   // 备份文件路径（existed=true 时有效）
        bool    existed;  // 快照时文件是否存在
    };
    struct DirBackup {
        QString abs;      // 原目录路径
        QString backup;   // 备份目录路径（rename 或复制结果）
    };

    QString    m_txDir;
    quint64    m_seq = 0;
    bool       m_finished = false;
    QVector<FileBackup> m_files;
    QVector<DirBackup>  m_dirs;
    QStringList         m_createdDirs; // 本事务新建的目录（含中间层，深->浅）

    QString nextFilePath();   // 下一个文件备份路径
    QString nextDirPath();    // 下一个目录备份路径
    // 递归复制目录（复制成功后返回 true；dst 已存在时先清空）
    static bool copyDirRecursively(const QString &src, const QString &dst);
    // 覆盖式复制文件（先删后拷）
    static bool copyFileOverwrite(const QString &src, const QString &dst);
};

} // namespace ckan

#endif // CKAN_TXFILEMANAGER_H
