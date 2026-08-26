#ifndef CKAN_MODULEINSTALLER_H
#define CKAN_MODULEINSTALLER_H

#include <QString>
#include <QVector>
#include <QObject>
#include <atomic>

#include "ckan_export.h"
#include "ckanmodule.h"
#include "installedmodule.h"
#include "txfilemanager.h"

namespace ckan {

class GameInstance;
class Repository;

// 安装结果
struct CKAN_API InstallResult {
    bool    ok = false;
    QString error;
    QStringList installedIdentifiers;
};

// 模块安装器：下载 zip -> miniz 解压 -> 按 install 规则复制到 GameData -> 更新 registry。
class CKAN_API ModuleInstaller : public QObject
{
    Q_OBJECT
public:
    explicit ModuleInstaller(GameInstance *instance, QObject *parent = nullptr);

    // 实例级网络代理配置（空=直连），替代原 Downloader 全局静态。
    void setProxyUrl(const QString &proxyUrl);
    QString proxyUrl() const { return m_proxyUrl; }

    // 安装一批模块（已由解析器展开依赖）。downloadDir 为 zip 缓存目录。
    // 便捷入口：先 downloadModules 下载全部，再 installFromCache 写入。
    InstallResult install(const QVector<CkanModule> &modules,
                          const QString &downloadDir,
                          const QStringList &foldersToDelete = {},
                          const QStringList &mirrorPrefixes = {},
                          bool preferModuleMirrors = false);

    // 阶段一：下载全部模块的 zip 到 downloadDir（复用已存在且有效的缓存，只补缺失/损坏的）。
    // 全程报 byteProgress 聚合进度，支持 cancel() 中止。返回是否全部就绪。
    // maxConcurrent 为并行下载数（>1 时多模块同时下载），默认 3。
    // cancelFlag 为外部取消标志（与成员 m_cancelRequested 共同生效），可为空。
    bool downloadModules(const QVector<CkanModule> &modules,
                         const QString &downloadDir,
                         const QStringList &mirrorPrefixes,
                         bool preferModuleMirrors,
                         QString *error,
                         int maxConcurrent = 3,
                         std::atomic_bool *cancelFlag = nullptr);

    // 阶段二：从缓存安装（不再下载）。zip 缺失/损坏时返回错误。
    // foldersToDelete 为相对 GameData 的顶层文件夹名：写入前若命中，先递归删除旧文件夹。
    //
    // tx 为空时自动创建内部事务：任一步失败整体回滚（恢复被覆盖/删除的文件、删除本批
    // 已写入的文件、还原注册表），成功则提交。传入外部 tx 时（如升级：卸载旧版+安装新版
    // 合并为单事务），文件操作计入该事务，本方法不做保存/回滚，由调用方负责提交或回滚。
    InstallResult installFromCache(const QVector<CkanModule> &modules,
                                   const QString &downloadDir,
                                   const QStringList &foldersToDelete = {},
                                   TxFileManager *tx = nullptr);

    // 以 zip 实际内容为准，返回该模块将写入的 GameData 顶层文件夹名列表（相对 GameData）。
    // 读取 zip 条目后套用 install 规则推导真实目标，绝不依赖预估。可用于下载后冲突检测。
    static QStringList actualGameDataFolders(const QString &zipPath, const CkanModule &mod,
                                             QString *error = nullptr);

    // 卸载模块：删除 registry 记录的文件，更新 registry。
    // tx 语义同 installFromCache：为空时自动事务（失败整体回滚），否则计入外部事务。
    InstallResult uninstall(const QString &identifier, TxFileManager *tx = nullptr);

    // 请求中止当前安装任务（线程安全）。正在下载的模组会被中止，
    // 已下载的临时数据不会写入缓存文件。
    void cancel();

    // 供测试/外部：从 zip 提取安装文件列表
    static bool listZipEntries(const QString &zipPath, QStringList *entries, QString *error);

    // 清洗缓存文件名中的非法字符（Windows 不含冒号/斜杠等）。
    // version 可能带 epoch（如 "1:3.4.0"），冒号在 NTFS 上会变成 ADS 分隔符导致读写错位。
    static QString safeCacheFileName(const QString &s);

    // 官方 CKAN 缓存文件名：{SHA1(下载URL)[:8]}-{identifier}-{version}.zip，
    // version 中非 [A-Za-z0-9_.-] 的字符替换为 '-'。
    // 哈希前缀对应官方 NetFileCache.CacheKey（如 6F5B077A-FreeIva-0.2.20.2.zip）。
    // downloadUrl 为空时退化为无前缀形式 {identifier}-{version}.zip（兼容手动下载文件）。
    static QString officialCacheFileName(const QString &identifier, const QString &version,
                                         const QString &downloadUrl = QString());

    // 查找缓存目录中该模块实际存在的有效缓存文件：
    // 优先官方格式 {hash}-{identifier}-{version}.zip，其次手动下载 {identifier}-{version}.zip，
    // 最后本启动器格式 {identifier}_{safeVersion}.zip。
    // 均不存在或无效（有效 zip，声明 sha256 时一并校验）返回空字符串。
    static QString findCacheZip(const QString &downloadDir, const CkanModule &mod);

    // 估算安装/下载所需磁盘空间（字节）：非元包模块 downloadSize 之和 × bufferFactor（默认 1.15）。
    // 供磁盘空间预检使用；downloadSize 未知的模块按 1 字节计，避免误判为零。
    static qint64 estimateRequiredBytes(const QVector<CkanModule> &modules, double bufferFactor = 1.15);

signals:
    void installProgress(const QString &identifier, int percent);
    void moduleInstalled(const QString &identifier);
    // 字节级进度：identifier 当前模组，doneBytes 全部已下载字节数，
    // totalBytes 批量总字节数，speedBps 当前模组实时速度（字节/秒）。
    void byteProgress(const QString &identifier, qint64 doneBytes, qint64 totalBytes, qint64 speedBps);

private:
    GameInstance *m_instance;
    QString m_proxyUrl;
    std::atomic_bool m_cancelRequested{false};

    // 单个模组下载任务（并行下载 worker 的输入）
    struct DownloadTask {
        CkanModule mod;
        QString zipPath;      // 缓存写入路径
        QStringList mirrors;  // 拼接了镜像前缀的备用 URL
        qint64 size = 1;      // downloadSize（未知时为 1）
    };
    // 单个模组下载结果
    struct DownloadOutcome {
        bool ok = false;
        QString identifier;
        QString error;
    };
    // 下载单个模组（在并发池线程内执行）。doneBytes 为跨线程累计的已下载字节数。
    DownloadOutcome downloadOneTask(const DownloadTask &task, std::atomic<qint64> &doneBytes,
                                    qint64 totalBytes, bool preferModuleMirrors,
                                    std::atomic_bool *cancelFlag);
};

} // namespace ckan

#endif // CKAN_MODULEINSTALLER_H