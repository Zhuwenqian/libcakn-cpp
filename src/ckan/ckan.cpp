#include "ckan.h"

#include <QSet>
#include <algorithm>

#include "repoindex.h"
#include "relationshipresolver.h"
#include "moduleinstaller.h"
#include "txfilemanager.h"

namespace ckan {

CKan::CKan(const QString &gameDir, const QString &instanceName, const CKanConfig &config)
    : m_instance(gameDir, instanceName), m_config(config)
{
}

CKan::~CKan()
{
    delete m_installer;
}

QString CKan::gameDir() const
{
    return m_instance.gameDir();
}

GameVersion CKan::detectedVersion() const
{
    return m_instance.detectVersion();
}

void CKan::reloadRegistry()
{
    m_instance.loadRegistry();
}

bool CKan::refreshIndex(const QVector<Repository> &repos,
                        QString *error, bool force, qint64 maxAgeSecs, bool preferMirror,
                        const std::function<void(const QString &, qint64, qint64)> &onProgress,
                        std::atomic_bool *cancelFlag)
{
    // 镜像前缀与索引缓存目录来自构造传入的 CKanConfig
    m_indexReady = RepoIndex::buildManyCached(repos, m_config.indexMirrorPrefixes,
                                              &m_index, &m_downloadCounts, error,
                                              force, maxAgeSecs, onProgress, cancelFlag,
                                              preferMirror, m_config.indexCacheDir,
                                              m_config.proxyUrl);
    return m_indexReady;
}

QVector<CkanModule> CKan::search(const QString &query) const
{
    QVector<CkanModule> out;
    const QString q = query.trimmed().toLower();
    for (auto it = m_index.constBegin(); it != m_index.constEnd(); ++it) {
        // 必须按版本排序取最新，不能取 m_index 中首个（tar 字母序）版本
        const CkanModule latest = RepoIndex::latestFor(m_index, it.key());
        if (!latest.isValid()) continue;
        if (q.isEmpty()
            || latest.identifier.toLower().contains(q)
            || latest.name.toLower().contains(q)
            || latest.abstract.toLower().contains(q)) {
            out.append(latest);
        }
    }
    // 按名称排序
    std::sort(out.begin(), out.end(), [](const CkanModule &a, const CkanModule &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QVector<CkanModule> CKan::versionsOf(const QString &identifier) const
{
    return RepoIndex::versionsFor(m_index, identifier);
}

CkanModule CKan::latestOf(const QString &identifier) const
{
    return RepoIndex::latestFor(m_index, identifier);
}

QStringList CKan::allIdentifiers() const
{
    return m_index.keys();
}

QString CKan::installedVersion(const QString &identifier) const
{
    return m_instance.registry()->installedVersion(identifier);
}

bool CKan::isInstalled(const QString &identifier) const
{
    return m_instance.registry()->isInstalled(identifier);
}

QVector<InstalledModule> CKan::installedModules() const
{
    QVector<InstalledModule> out;
    const Registry *reg = m_instance.registry();
    for (auto it = reg->installedModules.constBegin();
         it != reg->installedModules.constEnd(); ++it)
        out.append(it.value());
    return out;
}

void CKan::scanUnmanagedDlls()
{
    Registry *reg = m_instance.registry();
    reg->installedDlls = m_instance.scanUnmanagedDlls();
    m_instance.saveRegistry();
}

bool CKan::isAutoDetected(const QString &identifier) const
{
    return m_instance.registry()->installedDlls.contains(identifier);
}

ResolutionResult CKan::resolveInstall(const CkanModule &mod, bool autoInstallRecommends,
                                      bool withSuggests)
{
    QVector<CkanModule> toInstall;
    toInstall.append(mod);
    return resolveInstallMany(toInstall, autoInstallRecommends, withSuggests);
}

ResolutionResult CKan::resolveInstallMany(const QVector<CkanModule> &mods,
                                          bool autoInstallRecommends,
                                          bool withSuggests)
{
    RelationshipResolver resolver(m_index);
    return resolver.resolve(mods, *m_instance.registry(), autoInstallRecommends, withSuggests);
}

ModuleInstaller *CKan::ensureInstaller()
{
    QMutexLocker locker(&m_installerMutex);
    if (!m_installer) {
        m_installer = new ModuleInstaller(&m_instance);
        m_installer->setProxyUrl(m_config.proxyUrl);
        // 把安装器的字节进度信号桥接到 m_byteProgress 回调（下载在后台线程执行）
        QObject::connect(m_installer, &ModuleInstaller::byteProgress, m_installer,
                         [this](const QString &id, qint64 done, qint64 total, qint64 speed) {
            if (m_byteProgress) m_byteProgress(id, done, total, speed);
        });
        QObject::connect(m_installer, &ModuleInstaller::installProgress, m_installer,
                         [this](const QString &id, int percent) {
            if (m_installProgress) m_installProgress(id, percent);
        });
    }
    return m_installer;
}

bool CKan::downloadModules(const QVector<CkanModule> &modules,
                           const QString &downloadDir,
                           bool preferModuleMirrors,
                           int concurrency,
                           QStringList *conflicts,
                           QString *error,
                           const std::function<void(const QString &, qint64, qint64, qint64)> &onByteProgress,
                           std::atomic_bool *cancelFlag)
{
    ModuleInstaller *inst = ensureInstaller();
    m_byteProgress = onByteProgress;
    const bool ok = inst->downloadModules(modules, downloadDir,
                                          m_config.moduleMirrorPrefixes,
                                          preferModuleMirrors, error, concurrency, cancelFlag);
    m_byteProgress = std::function<void(const QString &, qint64, qint64, qint64)>();
    if (ok && conflicts)
        *conflicts = computeFolderConflicts(modules, downloadDir);
    return ok;
}

QStringList CKan::computeFolderConflicts(const QVector<CkanModule> &modules,
                                         const QString &downloadDir) const
{
    const QStringList manual = m_instance.manualGameDataFolders();
    if (manual.isEmpty()) return QStringList();
    const QSet<QString> manualSet(manual.begin(), manual.end());

    QSet<QString> conflictSet;
    QStringList out;
    for (const CkanModule &m : modules) {
        if (m.isMetapackage()) continue;
        const QString zipPath = downloadDir + QLatin1Char('/') + m.identifier + QLatin1Char('_')
                              + ModuleInstaller::safeCacheFileName(m.version) + QStringLiteral(".zip");
        QString err;
        const QStringList fols = ModuleInstaller::actualGameDataFolders(zipPath, m, &err);
        for (const QString &f : fols) {
            if (manualSet.contains(f) && !conflictSet.contains(f)) {
                conflictSet.insert(f);
                out << f;
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

InstallResult CKan::installFromCache(const QVector<CkanModule> &modules,
                                     const QString &downloadDir,
                                     const QStringList &foldersToDelete,
                                     const QStringList &preUninstall,
                                     const std::function<void(const QString &, int)> &onInstallProgress)
{
    ModuleInstaller *inst = ensureInstaller();
    GameInstance *g = &m_instance;
    m_installProgress = onInstallProgress;

    // 单事务：先卸载旧版再安装新版，作为一个原子操作。
    // 任一步失败（含用户取消）整体回滚——恢复被删除的旧版文件、删除已写入的新文件、还原注册表。
    TxFileManager tx(g->ckanDir() + QStringLiteral("/transactions"));
    const QByteArray regSnapshot = g->registry()->toJson();
    for (const QString &id : preUninstall) {
        const InstallResult ru = inst->uninstall(id, &tx);
        if (!ru.ok) {
            tx.rollback();
            g->restoreRegistrySnapshot(regSnapshot);
            m_installProgress = std::function<void(const QString &, int)>();
            return ru;
        }
    }
    InstallResult r = inst->installFromCache(modules, downloadDir, foldersToDelete, &tx);
    if (!r.ok) {
        tx.rollback();
        g->restoreRegistrySnapshot(regSnapshot);
        m_installProgress = std::function<void(const QString &, int)>();
        return r;
    }
    g->saveRegistry();
    tx.commit();
    m_installProgress = std::function<void(const QString &, int)>();
    return r;
}

InstallResult CKan::uninstall(const QString &identifier)
{
    ModuleInstaller installer(&m_instance);
    installer.setProxyUrl(m_config.proxyUrl);
    return installer.uninstall(identifier);
}

void CKan::cancelInstall()
{
    QMutexLocker locker(&m_installerMutex);
    if (m_installer)
        m_installer->cancel();
}

void CKan::releaseInstaller()
{
    QMutexLocker locker(&m_installerMutex);
    delete m_installer;
    m_installer = nullptr;
}

QString CKan::safeCacheFileName(const QString &s)
{
    return ModuleInstaller::safeCacheFileName(s);
}

GameVersion CKan::detectVersionFromDir(const QString &gameDir)
{
    return GameInstance::detectVersionFromDir(gameDir);
}

} // namespace ckan
