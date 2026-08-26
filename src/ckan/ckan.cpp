#include "ckan.h"

#include <QSet>
#include <QDateTime>
#include <QRegularExpression>
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

namespace {

// 官方 Identifier.Sanitize：先去除非字母数字前缀，再把非 [A-Za-z0-9-] 字符替换为 '-'
QString sanitizeCkanIdentifier(const QString &name)
{
    static const QRegularExpression prefixRe(QStringLiteral("^[^A-Za-z0-9]+"));
    static const QRegularExpression invalidRe(QStringLiteral("[^A-Za-z0-9-]"));
    QString out = name;
    out.remove(prefixRe);
    out.replace(invalidRe, QStringLiteral("-"));
    return out;
}

// 依赖优先的拓扑排序（对应官方"Sort dependencies before dependers"）。
// 依据 depends（含 any_of 子关系）建图；虚拟包经 provides 解析到提供者；
// 自依赖/未解析的依赖忽略；成环时未排序的模组按原顺序追加到末尾。
QVector<CkanModule> sortModsByDependencies(const QVector<CkanModule> &mods)
{
    const int n = mods.size();
    QMap<QString, int> idIndex;
    for (int i = 0; i < n; ++i)
        idIndex.insert(mods[i].identifier, i);

    // 虚拟包名 -> 提供者下标（首个提供者胜出，与解析器 providedToOwner 一致）
    QMap<QString, int> providedToIndex;
    for (int i = 0; i < n; ++i)
        for (const QString &p : mods[i].providesList())
            if (!providedToIndex.contains(p))
                providedToIndex.insert(p, i);

    QVector<int> indegree(n, 0);
    QVector<QVector<int>> dependents(n);
    for (int i = 0; i < n; ++i) {
        QSet<int> seen;
        const auto addRel = [&](const Relationship &r) {
            int depIdx = idIndex.value(r.name, -1);
            if (depIdx < 0)
                depIdx = providedToIndex.value(r.name, -1);
            if (depIdx == i || depIdx < 0 || seen.contains(depIdx))
                return;
            seen.insert(depIdx);
            ++indegree[i];
            dependents[depIdx].append(i);
        };
        for (const Relationship &r : mods[i].depends) {
            if (!r.anyOf.isEmpty())
                for (const Relationship &sub : r.anyOf) addRel(sub);
            else
                addRel(r);
        }
    }

    // Kahn 算法：始终取最小下标的就绪节点，保证输出确定
    QVector<int> ready;
    for (int i = 0; i < n; ++i)
        if (indegree[i] == 0) ready.append(i);
    QVector<CkanModule> sorted;
    while (!ready.isEmpty()) {
        const int i = ready.takeFirst();
        sorted.append(mods[i]);
        for (const int d : dependents[i]) {
            if (--indegree[d] == 0) {
                auto it = std::lower_bound(ready.begin(), ready.end(), d);
                ready.insert(it, d);
            }
        }
    }
    // 成环的模组追加到末尾（保持原相对顺序）
    if (sorted.size() < n) {
        QSet<int> sortedSet;
        for (const CkanModule &m : sorted) sortedSet.insert(idIndex.value(m.identifier));
        for (int i = 0; i < n; ++i)
            if (!sortedSet.contains(i)) sorted.append(mods[i]);
    }
    return sorted;
}

} // namespace

QByteArray CKan::exportModpackCkan(QString *error)
{
    // 先重载注册表，确保拿到最新的已安装数据
    reloadRegistry();

    // 收集待导出模组：排除 DLC / 自动安装 / 手动安装（AD）模组；
    // 索引已加载时同样排除索引中不存在的模组（无法从仓库重新安装，参照官方 IsAvailable）
    const bool indexLoaded = m_indexReady && !m_index.isEmpty();
    QVector<CkanModule> mods;
    const QVector<InstalledModule> installed = installedModules();
    for (const InstalledModule &im : installed) {
        if (im.module.isDlc() || im.autoInstalled || isAutoDetected(im.identifier))
            continue;
        if (indexLoaded && !m_index.contains(im.identifier))
            continue;
        mods.append(im.module);
    }
    if (mods.isEmpty()) {
        if (error) *error = QStringLiteral("没有可导出的已安装模组。");
        return QByteArray();
    }

    // 依赖优先排序
    const QVector<CkanModule> sorted = sortModsByDependencies(mods);

    // 构建元包（参照官方 RegistryManager.GenerateModpack）
    const QString instanceName = m_instance.name();
    CkanModule meta;
    meta.kind = ModuleKind::Metapackage;
    meta.specVersion = QStringLiteral("1");
    meta.name = QStringLiteral("已安装-%1").arg(instanceName);
    meta.identifier = sanitizeCkanIdentifier(meta.name);
    meta.abstract = QStringLiteral("已安装在 KSP 实例 %1 的模组列表").arg(instanceName);
    meta.author = QStringList{QString::fromLocal8Bit(qgetenv("USERNAME"))};
    meta.license = QStringList{QStringLiteral("unknown")};
    meta.version = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy.MM.dd.hh.mm.ss"));
    meta.releaseDate = QDateTime::currentDateTime().toString(Qt::ISODate);

    // ksp_version_min/max 写检测到的游戏版本线（去掉 build，如 1.12.5.3190 -> 1.12.5）
    const GameVersion detected = detectedVersion();
    if (detected.isValid()) {
        QStringList parts = detected.toString().split(QLatin1Char('.'));
        while (parts.size() > 3) parts.removeLast();
        const QString versionLine = parts.join(QLatin1Char('.'));
        meta.kspVersionMin = versionLine;
        meta.kspVersionMax = versionLine;
    }

    for (const CkanModule &m : sorted) {
        Relationship r;
        r.type = Relationship::Type::Depends;
        r.name = m.identifier;
        meta.depends.append(r);
    }
    return meta.toJson();
}

void CKan::scanUnmanagedDlls()
{
    Registry *reg = m_instance.registry();
    reg->installedDlls = m_instance.scanUnmanagedDlls();
    m_instance.saveRegistry();
    m_dllsScanned = true;
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
                                          bool withSuggests,
                                          const GameVersionRange &extraRange)
{
    RelationshipResolver resolver(m_index);
    // 传入当前实例检测到的 KSP 版本，候选按兼容性过滤（无效版本视为不过滤）；
    // extraRange 为用户勾选的额外兼容区间（无效表示未启用）。
    return resolver.resolve(mods, *m_instance.registry(), autoInstallRecommends, withSuggests,
                            m_instance.detectVersion(), extraRange);
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
        // 优先官方格式缓存，其次本启动器格式（兼容 D:\CKAN Downloads 等官方缓存目录）
        const QString zipPath = ModuleInstaller::findCacheZip(downloadDir, m);
        if (zipPath.isEmpty())
            continue; // 缓存缺失（未下载/下载失败），冲突计算跳过
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

QString CKan::officialCacheFileName(const QString &identifier, const QString &version,
                                    const QString &downloadUrl)
{
    return ModuleInstaller::officialCacheFileName(identifier, version, downloadUrl);
}

QString CKan::findCacheZip(const QString &downloadDir, const CkanModule &mod)
{
    return ModuleInstaller::findCacheZip(downloadDir, mod);
}

qint64 CKan::estimateRequiredBytes(const QVector<CkanModule> &modules, double bufferFactor)
{
    return ModuleInstaller::estimateRequiredBytes(modules, bufferFactor);
}

GameVersion CKan::detectVersionFromDir(const QString &gameDir)
{
    return GameInstance::detectVersionFromDir(gameDir);
}

} // namespace ckan
