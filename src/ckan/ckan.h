#ifndef CKAN_CKAN_H
#define CKAN_CKAN_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>
#include <QMutex>
#include <functional>
#include <atomic>

#include "ckan_export.h"
#include "ckanconfig.h"
#include "ckanmodule.h"
#include "installedmodule.h"
#include "registry.h"
#include "repository.h"
#include "gameinstance.h"
#include "version.h"
#include "relationshipresolver.h"
#include "moduleinstaller.h"
#include "repoindex.h"

namespace ckan {

// libckan 顶层门面：为启动器提供统一、简洁的接口。
// 封装仓库索引、注册表、依赖解析、安装/卸载（含下载阶段、冲突检测、事务回滚）。
// 启动器只依赖本类与 CKanConfig，不直接触碰内部实现类。
class CKAN_API CKan
{
public:
    // config 为库运行配置（缓存目录、代理、镜像前缀等），缺省使用默认镜像。
    explicit CKan(const QString &gameDir, const QString &instanceName,
                  const CKanConfig &config = {});
    ~CKan();

    // ---- 实例基本信息（不暴露 GameInstance） ----
    QString gameDir() const;
    GameVersion detectedVersion() const;   // 当前实例实际检测到的 KSP 版本（失败返回无效版本）
    void reloadRegistry();                 // 重新从 registry.json 加载已安装数据

    // ---- 仓库索引（mod 列表） ----
    // 从多个仓库下载并按优先级合并建立索引。repos 为仓库列表（priority 值越小优先级越高）。
    // 镜像前缀与索引缓存目录取自构造传入的 CKanConfig（仅对 GitHub 托管的仓库生效）。
    // force=true 时忽略本地缓存强制重新下载；否则使用缓存（单仓库失败回退其旧缓存）。
    // maxAgeSecs 为缓存有效期（秒）；preferMirror=true 时镜像优先（否则官方优先）。
    // onProgress 下载进度回调(repoName, received, total)，cancelFlag 置真则中止索引下载。
    bool refreshIndex(const QVector<Repository> &repos,
                      QString *error = nullptr,
                      bool force = false, qint64 maxAgeSecs = RepoIndex::kDefaultCacheAgeSecs,
                      bool preferMirror = false,
                      const std::function<void(const QString &, qint64, qint64)> &onProgress = {},
                      std::atomic_bool *cancelFlag = nullptr);
    QVector<CkanModule> search(const QString &query) const;    // 按名称/标识符搜索
    QVector<CkanModule> versionsOf(const QString &identifier) const;
    CkanModule latestOf(const QString &identifier) const;
    bool indexReady() const { return m_indexReady; }
    int  indexSize() const { return static_cast<int>(m_index.size()); }
    QStringList allIdentifiers() const;   // 索引中全部标识符（用于精确清理下载缓存）
    // 某标识符的下载次数（来自仓库 download_counts.json）；无数据返回 -1
    int  downloadCount(const QString &identifier) const { return m_downloadCounts.value(identifier, -1); }
    bool hasDownloadCount(const QString &identifier) const { return m_downloadCounts.contains(identifier); }

    // ---- 已安装查询 ----
    QString installedVersion(const QString &identifier) const;
    bool isInstalled(const QString &identifier) const;
    QVector<InstalledModule> installedModules() const;

    // ---- 手动安装模组（DLL 扫描，AD） ----
    void scanUnmanagedDlls();   // 扫描 GameData 下 .dll 写入 registry.installedDlls 并保存
    bool isAutoDetected(const QString &identifier) const;

    // ---- 依赖解析 ----
    // 解析安装某模块所需的完整集合（含依赖）
    ResolutionResult resolveInstall(const CkanModule &mod, bool autoInstallRecommends = true,
                                    bool withSuggests = false);
    // 一次性解析多个模块的完整安装集（含相互依赖，用于批量安装）
    ResolutionResult resolveInstallMany(const QVector<CkanModule> &mods,
                                        bool autoInstallRecommends = true,
                                        bool withSuggests = false);

    // ---- 安装流程（分两阶段，供启动器后台线程调用） ----
    // 阶段一：下载全部模块 zip 到 downloadDir，并按 zip 实际内容计算与手动占用文件夹的冲突。
    // preferModuleMirrors=true 时镜像优先；concurrency 为并行下载数（1~8）。
    // conflicts 输出冲突的 GameData 顶层文件夹（相对 GameDir，如 "GameData/SomeMod"，已排序去重）。
    // onByteProgress 字节进度回调(identifier, doneBytes, totalBytes, speedBps)；
    // cancelFlag 置真则中止下载（也可经 cancelInstall() 中止）。
    bool downloadModules(const QVector<CkanModule> &modules,
                         const QString &downloadDir,
                         bool preferModuleMirrors,
                         int concurrency = 3,
                         QStringList *conflicts = nullptr,
                         QString *error = nullptr,
                         const std::function<void(const QString &, qint64, qint64, qint64)> &onByteProgress = {},
                         std::atomic_bool *cancelFlag = nullptr);

    // 阶段二：单事务安装——先卸载 preUninstall 的旧版，再 installFromCache 写入。
    // foldersToDelete 为相对 GameData 的顶层文件夹名（写入前先递归删除旧文件夹）。
    // 任一步失败（含用户取消）整体回滚：恢复被删旧文件、删除新写入文件、还原注册表。
    // onInstallProgress 安装进度回调(identifier, percent 0~100)，在后台线程调用。
    InstallResult installFromCache(const QVector<CkanModule> &modules,
                                   const QString &downloadDir,
                                   const QStringList &foldersToDelete = {},
                                   const QStringList &preUninstall = {},
                                   const std::function<void(const QString &, int)> &onInstallProgress = {});

    // 卸载单个模组（单事务，失败整体回滚）。
    InstallResult uninstall(const QString &identifier);

    // 请求中止当前安装/下载任务（线程安全）。
    void cancelInstall();
    // 安装流程结束后释放内部安装器（下载/安装均完成后由启动器调用）。
    void releaseInstaller();

    // ---- 下载缓存辅助 ----
    // 清洗缓存文件名中的非法字符（Windows 不含冒号/斜杠等），供启动器精确清理缓存。
    static QString safeCacheFileName(const QString &s);

    // ---- 静态工具 ----
    // 从游戏目录检测 KSP 版本（供启动器发现实例时显示版本号，无需构造实例）。
    // 检测失败返回无效版本。
    static GameVersion detectVersionFromDir(const QString &gameDir);

private:
    ModuleInstaller *ensureInstaller(); // 按 config 惰性创建内部安装器（供下载/安装两阶段复用）
    QStringList computeFolderConflicts(const QVector<CkanModule> &modules,
                                       const QString &downloadDir) const;

    GameInstance m_instance;
    CKanConfig m_config;
    QMutex m_installerMutex;            // 保护 m_installer 的创建/取消/释放（跨线程）
    ModuleInstaller *m_installer = nullptr;
    std::function<void(const QString &, qint64, qint64, qint64)> m_byteProgress;
    std::function<void(const QString &, int)> m_installProgress;
    QMap<QString, QVector<CkanModule>> m_index;
    QMap<QString, int> m_downloadCounts; // identifier -> 下载次数（高优先级仓库优先）
    bool m_indexReady = false;
};

} // namespace ckan

#endif // CKAN_CKAN_H
