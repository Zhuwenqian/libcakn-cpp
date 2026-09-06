#include "moduleinstaller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QSet>
#include <QMutex>
#include <QCryptographicHash>
#include <QtConcurrent>
#include <QThreadPool>
#include <QFuture>
#include <memory>
#include <cmath>

#include "gameinstance.h"
#include "registry.h"
#include "downloader.h"
#include "moduleinstalldescriptor.h"

#include "miniz.h"

namespace ckan {

ModuleInstaller::ModuleInstaller(GameInstance *instance, QObject *parent)
    : QObject(parent), m_instance(instance)
{
}

void ModuleInstaller::setProxyUrl(const QString &proxyUrl)
{
    m_proxyUrl = proxyUrl;
}

bool ModuleInstaller::listZipEntries(const QString &zipPath, QStringList *entries, QString *error)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath.toUtf8().constData(), 0)) {
        if (error) *error = QStringLiteral("failed to open zip: %1").arg(zipPath);
        return false;
    }
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (mz_zip_reader_file_stat(&zip, i, &stat))
            entries->append(QString::fromUtf8(stat.m_filename));
    }
    mz_zip_reader_end(&zip);
    return true;
}

namespace {
// 从 zip 提取单文件到 out 内存
bool extractToMem(mz_zip_archive &zip, const char *name, QByteArray *out, QString *error)
{
    const int idx = mz_zip_reader_locate_file(&zip, name, nullptr, 0);
    if (idx < 0) {
        if (error) *error = QStringLiteral("file not found in zip: %1").arg(QString::fromUtf8(name));
        return false;
    }
    size_t size = 0;
    void *mem = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(idx), &size, 0);
    if (!mem) {
        if (error) *error = QStringLiteral("extract failed: %1").arg(QString::fromUtf8(name));
        return false;
    }
    out->resize(static_cast<qsizetype>(size));
    memcpy(out->data(), mem, size);
    mz_free(mem);
    return true;
}

// 判断数据是否具备 ZIP 魔数（PK）
bool looksLikeZip(const QByteArray &data)
{
    if (data.size() < 2) return false;
    const uchar *d = reinterpret_cast<const uchar *>(data.constData());
    return d[0] == 'P' && d[1] == 'K';
}

// 校验缓存文件是否是一个可打开的 ZIP 归档；若提供了期望的 SHA256，则同时校验哈希一致。
bool isValidZipFile(const QString &path, const QString &expectedSha256 = QString())
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = f.readAll();
    if (!looksLikeZip(data)) return false;
    if (!expectedSha256.isEmpty()) {
        const QString actual = QString::fromLatin1(
            QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
        if (actual.compare(expectedSha256, Qt::CaseInsensitive) != 0) return false;
    }
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    const bool ok = mz_zip_reader_init_mem(&zip, data.constData(),
                                           static_cast<size_t>(data.size()), 0);
    if (ok) mz_zip_reader_end(&zip);
    return ok;
}

// 某模块提供的所有名称（标识符 + 虚拟包）
QSet<QString> providedNamesOf(const CkanModule &m)
{
    QSet<QString> s;
    s.insert(m.identifier);
    for (const Relationship &r : m.provides)
        if (!r.name.isEmpty()) s.insert(r.name);
    return s;
}

// 判断 dependent 是否依赖 provider 提供的任一名称
bool moduleDependsOn(const CkanModule &dependent, const QSet<QString> &providerNames)
{
    for (const Relationship &rel : dependent.depends)
        if (providerNames.contains(rel.name)) return true;
    return false;
}

// 递归收集反向依赖链并给出卸载顺序（先依赖者、最后 target）。
// 例如 C 依赖 B、B 依赖 A，卸载 A 时顺序为 C,B,A。visited 防止循环依赖。
// 返回 false 表示 target 未安装。
bool collectReverseDeps(const Registry *reg, const QString &target,
                        QSet<QString> &visited, QStringList &order)
{
    QMutexLocker locker(reg->mutex()); // 跨线程读 installedModules，与扫描/安装写互斥（递归）
    if (visited.contains(target)) return true;
    const InstalledModule *targetIm = reg->installed(target);
    if (!targetIm) return false;
    const QSet<QString> providerNames = providedNamesOf(targetIm->module);
    for (auto it = reg->installedModules.constBegin();
         it != reg->installedModules.constEnd(); ++it) {
        const InstalledModule &im = it.value();
        if (im.identifier == target) continue;
        if (moduleDependsOn(im.module, providerNames)
            && !collectReverseDeps(reg, im.identifier, visited, order))
            return false;
    }
    visited.insert(target);
    order.append(target);
    return true;
}
} // namespace

QString ModuleInstaller::safeCacheFileName(const QString &s)
{
    QString r = s;
    r.replace(QLatin1Char(':'), QLatin1Char('_'));
    r.replace(QLatin1Char('\\'), QLatin1Char('_'));
    r.replace(QLatin1Char('/'), QLatin1Char('_'));
    r.replace(QLatin1Char('?'), QLatin1Char('_'));
    r.replace(QLatin1Char('*'), QLatin1Char('_'));
    r.replace(QLatin1Char('<'), QLatin1Char('_'));
    r.replace(QLatin1Char('>'), QLatin1Char('_'));
    r.replace(QLatin1Char('|'), QLatin1Char('_'));
    r.replace(QLatin1Char('"'), QLatin1Char('_'));
    return r;
}

QString ModuleInstaller::officialCacheFileName(const QString &identifier, const QString &version,
                                               const QString &downloadUrl)
{
    // 官方 CkanModule.StandardName：{identifier}-{version}.zip，
    // version 中不在 [A-Za-z0-9_.-] 集合内的字符全部替换为 '-'
    QString v = version;
    for (int i = 0; i < v.size(); ++i) {
        const QChar c = v.at(i);
        const bool keep = (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
                       || (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                       || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                       || c == QLatin1Char('.') || c == QLatin1Char('_') || c == QLatin1Char('-');
        if (!keep)
            v[i] = QLatin1Char('-');
    }
    // 官方 NetFileCache.CacheKey：下载 URL 的 SHA1 前 8 位十六进制大写。
    // 形如 6F5B077A-FreeIva-0.2.20.2.zip；URL 为空时退化为无前缀形式。
    QString prefix;
    if (!downloadUrl.isEmpty()) {
        const QByteArray sha1 = QCryptographicHash::hash(
            downloadUrl.toUtf8(), QCryptographicHash::Sha1);
        prefix = QString::fromLatin1(sha1.toHex()).left(8).toUpper() + QLatin1Char('-');
    }
    return prefix + identifier + QLatin1Char('-') + v + QStringLiteral(".zip");
}

QString ModuleInstaller::findCacheZip(const QString &downloadDir, const CkanModule &mod)
{
    const QString url = mod.downloadUrls.isEmpty() ? QString() : mod.downloadUrls.first();
    // 官方格式优先（带 SHA1 URL 哈希前缀，对应 D:\CKAN Downloads 等官方缓存目录）
    const QString official = downloadDir + QLatin1Char('/')
                           + officialCacheFileName(mod.identifier, mod.version, url);
    if (QFileInfo::exists(official) && isValidZipFile(official, mod.downloadHash.sha256))
        return official;
    // 手动下载的无前缀 {identifier}-{version}.zip 兜底
    const QString plain = downloadDir + QLatin1Char('/')
                        + officialCacheFileName(mod.identifier, mod.version);
    if (QFileInfo::exists(plain) && isValidZipFile(plain, mod.downloadHash.sha256))
        return plain;
    // 本启动器格式兜底
    const QString launcher = downloadDir + QLatin1Char('/') + mod.identifier + QLatin1Char('_')
                           + safeCacheFileName(mod.version) + QStringLiteral(".zip");
    if (QFileInfo::exists(launcher) && isValidZipFile(launcher, mod.downloadHash.sha256))
        return launcher;
    return QString();
}

qint64 ModuleInstaller::estimateRequiredBytes(const QVector<CkanModule> &modules, double bufferFactor)
{
    qint64 total = 0;
    for (const CkanModule &m : modules) {
        if (m.isMetapackage()) continue;
        total += m.downloadSize > 0 ? m.downloadSize : 1;
    }
    return static_cast<qint64>(std::ceil(total * bufferFactor));
}

QStringList ModuleInstaller::actualGameDataFolders(const QString &zipPath, const CkanModule &mod,
                                                   QString *error)
{
    // 以 zip 实际内容为准：读入内存后套用 install 规则，推导真实写入的 GameData 顶层文件夹。
    // 用 init_mem 而非 init_file，规避 Windows fopen 按 ANSI 代码页解析非 ASCII 路径的问题。
    QFile zf(zipPath);
    if (!zf.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open zip: %1").arg(zipPath);
        return {};
    }
    const QByteArray data = zf.readAll();
    zf.close();
    if (!looksLikeZip(data)) {
        if (error) *error = QStringLiteral("not a zip: %1").arg(zipPath);
        return {};
    }
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data.constData(),
                                static_cast<size_t>(data.size()), 0)) {
        if (error) *error = QStringLiteral("corrupt zip: %1").arg(zipPath);
        return {};
    }
    QStringList entries;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, i, &st))
            entries.append(QString::fromUtf8(st.m_filename));
    }
    mz_zip_reader_end(&zip);

    QSet<QString> folders;
    const QString gameData = QStringLiteral("GameData");
    for (const ModuleInstallDescriptor &stanza : mod.effectiveInstallStanzas()) {
        QString err;
        const QVector<InstallableFile> files = stanza.findInstallableFiles(entries, gameData, &err);
        for (const InstallableFile &f : files) {
            QString rel = f.destination;
            rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
            if (rel.startsWith(QStringLiteral("GameData/"))) {
                const QString top = rel.mid(9).section(QLatin1Char('/'), 0, 0);
                if (!top.isEmpty()) folders.insert(top);
            }
        }
    }
    QStringList out = folders.values();
    std::sort(out.begin(), out.end());
    return out;
}

bool ModuleInstaller::downloadModules(const QVector<CkanModule> &modules,
                                      const QString &downloadDir,
                                      const QStringList &mirrorPrefixes,
                                      bool preferModuleMirrors,
                                      QString *error,
                                      int maxConcurrent,
                                      std::atomic_bool *cancelFlag)
{
    m_cancelRequested.store(false);
    QDir().mkpath(downloadDir);

    // 前置拦截 DLC：官方付费内容不可经 CKAN 下载/安装（对应 ModuleIsDLCKraken），
    // 在任何下载发生之前直接失败，避免浪费流量。
    for (const CkanModule &mod : modules) {
        if (mod.isDlc()) {
            if (error) *error = QStringLiteral("DLC 不可直接安装：%1（官方付费内容，请通过 Steam 购买安装）")
                                    .arg(mod.identifier);
            return false;
        }
    }

    // 前置校验：非元包必须有下载地址
    for (const CkanModule &mod : modules) {
        if (mod.isMetapackage()) continue;
        if (mod.downloadUrls.isEmpty()) {
            if (error) *error = QStringLiteral("%1 has no download URL").arg(mod.identifier);
            return false;
        }
    }

    // 批量总字节数（downloadSize 未知时按 1 计，避免除零）；
    // preDone 为无需下载（元包/命中有效缓存）模块的字节数，作为进度起点。
    qint64 totalBytes = 0;
    qint64 preDone = 0;
    QVector<DownloadTask> tasks;
    for (const CkanModule &mod : modules) {
        const qint64 size = mod.downloadSize > 0 ? mod.downloadSize : 1;
        totalBytes += size;
        if (mod.isMetapackage()) {
            preDone += size;
            continue;
        }

        // 复用已存在且有效的缓存（官方格式或本启动器格式，含 SHA256 校验），只补缺失/损坏的
        // （兼容 D:\CKAN Downloads 等官方缓存目录的 {identifier}-{version}.zip 命名）
        if (!findCacheZip(downloadDir, mod).isEmpty()) {
            preDone += size;
            continue;
        }
        const QString zipPath = downloadDir + QLatin1Char('/') + mod.identifier + QLatin1Char('_')
                              + safeCacheFileName(mod.version) + QStringLiteral(".zip");
        if (QFile::exists(zipPath))
            QFile::remove(zipPath); // 清理损坏/无效的本启动器格式缓存后再重新下载
        // 同时清理损坏/无效的官方格式缓存（带哈希前缀与无前缀两种），避免残留旧文件
        const QString staleUrl = mod.downloadUrls.isEmpty() ? QString() : mod.downloadUrls.first();
        const QString staleOfficial = downloadDir + QLatin1Char('/')
                                    + officialCacheFileName(mod.identifier, mod.version, staleUrl);
        if (QFile::exists(staleOfficial))
            QFile::remove(staleOfficial);
        const QString stalePlain = downloadDir + QLatin1Char('/')
                                 + officialCacheFileName(mod.identifier, mod.version);
        if (QFile::exists(stalePlain))
            QFile::remove(stalePlain);

        // 该模组下载镜像：每个前缀拼接官方 URL，作为回退（或镜像优先）
        QStringList modMirrors;
        const QString officialUrl = mod.downloadUrls.first();
        for (const QString &p : mirrorPrefixes)
            if (!p.isEmpty()) modMirrors << p + officialUrl;

        DownloadTask t;
        t.mod = mod;
        t.zipPath = zipPath;
        t.mirrors = modMirrors;
        t.size = size;
        tasks.append(t);
    }

    if (tasks.isEmpty()) {
        // 全部命中有效缓存（或本批无实体模块）
        if (!modules.isEmpty())
            emit byteProgress(modules.first().identifier, totalBytes, totalBytes, 0);
        return true;
    }

    // 跨线程累计的已下载字节数，起点为已完成（缓存/元包）的字节
    std::atomic<qint64> doneBytes{preDone};
    QThreadPool pool;
    pool.setMaxThreadCount(std::max(1, maxConcurrent));

    QVector<QFuture<DownloadOutcome>> futures;
    futures.reserve(tasks.size());
    for (const DownloadTask &t : tasks) {
        futures.append(QtConcurrent::run(&pool,
            [this, t, &doneBytes, totalBytes, preferModuleMirrors, cancelFlag]() {
                return downloadOneTask(t, doneBytes, totalBytes, preferModuleMirrors, cancelFlag);
            }));
    }
    for (QFuture<DownloadOutcome> &f : futures)
        f.waitForFinished();

    DownloadOutcome firstError;
    for (const QFuture<DownloadOutcome> &f : futures) {
        const DownloadOutcome o = f.result();
        if (!o.ok && firstError.error.isEmpty()) firstError = o;
    }
    if (!firstError.error.isEmpty()) {
        if (error) *error = firstError.error;
        return false;
    }
    return true;
}

ModuleInstaller::DownloadOutcome ModuleInstaller::downloadOneTask(
    const DownloadTask &task, std::atomic<qint64> &doneBytes,
    qint64 totalBytes, bool preferModuleMirrors,
    std::atomic_bool *cancelFlag)
{
    DownloadOutcome out;
    out.identifier = task.mod.identifier;
    if (m_cancelRequested.load() || (cancelFlag && cancelFlag->load())) {
        out.error = QStringLiteral("已取消");
        return out;
    }

    Downloader dl;
    dl.setDownloadRate(m_downloadRateLimitBps);
    QByteArray data;
    QString err;
    const QString expectedSha256 = task.mod.downloadHash.sha256;
    // 内容校验器：必须为 ZIP 归档，且（若声明了哈希）SHA256 与仓库元数据一致
    const Downloader::Validator zipValidator = [&](const QByteArray &d) {
        if (!looksLikeZip(d)) return false;
        if (!expectedSha256.isEmpty()) {
            const QString actual = QString::fromLatin1(
                QCryptographicHash::hash(d, QCryptographicHash::Sha256).toHex());
            if (actual.compare(expectedSha256, Qt::CaseInsensitive) != 0) return false;
        }
        return true;
    };

    // 该模组下载进度与实时速度；增量累加进跨线程 doneBytes
    QElapsedTimer spd;          spd.start();
    qint64 lastRecv = 0;
    const auto onProgress = [&](qint64 received, qint64) {
        const qint64 now = spd.elapsed();
        const qint64 delta = received - lastRecv;
        lastRecv = received;
        if (delta > 0) doneBytes.fetch_add(delta);
        qint64 speed = 0;
        if (now > 0) speed = delta * 1000 / now;
        spd.restart();
        emit byteProgress(task.mod.identifier, doneBytes.load(), totalBytes, speed);
    };

    const QString officialUrl = task.mod.downloadUrls.first();
    if (!dl.downloadProgressed(officialUrl, task.mirrors, &data, &err,
                               zipValidator, onProgress, &m_cancelRequested,
                               /*resumeAttempts=*/5, preferModuleMirrors)) {
        out.error = m_cancelRequested.load()
            ? QStringLiteral("已取消：%1").arg(task.mod.identifier)
            : QStringLiteral("failed to download %1: %2").arg(task.mod.identifier, err);
        return out;
    }
    QFile zf(task.zipPath);
    if (!zf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.error = QStringLiteral("cannot write download cache: %1").arg(task.zipPath);
        return out;
    }
    zf.write(data);
    zf.close();

    // 实际字节与声明大小存在差异时按声明大小补齐，使总进度贴近 totalBytes
    if (data.size() < task.size)
        doneBytes.fetch_add(task.size - data.size());
    emit byteProgress(task.mod.identifier, doneBytes.load(), totalBytes, 0);
    out.ok = true;
    return out;
}

InstallResult ModuleInstaller::installFromCache(const QVector<CkanModule> &modules,
                                                const QString &downloadDir,
                                                const QStringList &foldersToDelete,
                                                TxFileManager *tx)
{
    InstallResult result;
    Registry *reg = m_instance->registry();

    // 事务化安装：所有文件操作（删除旧文件夹、写入）都经过事务管理器，任一步失败整体回滚，
    // 保证“安装失败不残留文件”。tx 为空时自动创建内部事务并负责提交/回滚；
    // 外部传入 tx（升级场景）时由调用方统一提交/回滚。
    std::unique_ptr<TxFileManager> autoTx;
    if (!tx) {
        autoTx.reset(new TxFileManager(m_instance->ckanDir() + QStringLiteral("/transactions")));
        tx = autoTx.get();
    }
    const QByteArray regSnapshot = reg->toJson(); // 事务开始时的注册表快照

    const auto fail = [&](const QString &err) {
        if (autoTx) {
            tx->rollback();
            m_instance->restoreRegistrySnapshot(regSnapshot);
        }
        InstallResult r;
        r.error = err;
        return r;
    };

    for (const CkanModule &mod : modules) {
        emit installProgress(mod.identifier, 0);
        if (m_cancelRequested.load())
            return fail(QStringLiteral("已取消"));
        if (mod.isDlc())
            return fail(QStringLiteral("DLC 不可直接安装：%1（官方付费内容，请通过 Steam 购买安装）")
                .arg(mod.identifier));
        if (mod.isMetapackage()) {
            // 元包无文件，仅注册
            InstalledModule im;
            im.identifier = mod.identifier;
            im.module = mod;
            im.installTime = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            reg->registerModule(im);
            emit moduleInstalled(mod.identifier);
            result.installedIdentifiers << mod.identifier;
            continue;
        }
        if (mod.downloadUrls.isEmpty())
            return fail(QStringLiteral("%1 has no download URL").arg(mod.identifier));

        // 定位缓存文件：优先官方格式，其次本启动器格式（兼容 D:\CKAN Downloads 等官方缓存目录）
        const QString zipPath = findCacheZip(downloadDir, mod);
        if (zipPath.isEmpty())
            return fail(QStringLiteral("download cache missing or invalid: %1，请先下载")
                .arg(mod.identifier));

        // 读取 zip 到内存并打开
        //    不用 mz_zip_reader_init_file：其内部用 fopen 按 ANSI 代码页解析路径，
        //    当缓存路径含非 ASCII 字符时打开失败，会被误报为 "corrupt zip"。
        QFile zipFile(zipPath);
        if (!zipFile.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("cannot open cached download: %1").arg(zipPath));
        const QByteArray zipData = zipFile.readAll();
        zipFile.close();
        if (!looksLikeZip(zipData))
            return fail(QStringLiteral("corrupt zip for %1 (size=%2 bytes, 下载内容并非 ZIP 归档)")
                .arg(mod.identifier).arg(zipData.size()));
        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));
        if (!mz_zip_reader_init_mem(&zip, zipData.constData(),
                                    static_cast<size_t>(zipData.size()), 0))
            return fail(QStringLiteral("corrupt zip for %1 (size=%2 bytes)")
                .arg(mod.identifier).arg(zipData.size()));
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        QStringList entries;
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat st;
            if (mz_zip_reader_file_stat(&zip, i, &st))
                entries.append(QString::fromUtf8(st.m_filename));
        }

        // 应用 install 规则，得到目标文件列表
        const QString gameData = QStringLiteral("GameData");
        QVector<InstallableFile> files;
        for (const ModuleInstallDescriptor &stanza : mod.effectiveInstallStanzas()) {
            QString err;
            const QVector<InstallableFile> f = stanza.findInstallableFiles(entries, gameData, &err);
            files.append(f);
        }
        if (files.isEmpty()) {
            mz_zip_reader_end(&zip);
            return fail(QStringLiteral("no files matched install rules for %1").arg(mod.identifier));
        }
        emit installProgress(mod.identifier, 60);

        // 若用户选择“删除旧的保留新的”，先递归删除将写入的顶层 GameData 文件夹（事务化）
        if (!foldersToDelete.isEmpty()) {
            QSet<QString> writeRoots;
            for (const InstallableFile &f : files) {
                QString rel = f.destination;
                rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
                if (rel.startsWith(QStringLiteral("GameData/"))) {
                    const QString top = rel.mid(9).section(QLatin1Char('/'), 0, 0);
                    if (!top.isEmpty()) writeRoots.insert(top);
                }
            }
            for (const QString &top : writeRoots) {
                if (foldersToDelete.contains(top)) {
                    if (!tx->deleteDir(m_instance->toAbsoluteGameDir(
                            QStringLiteral("GameData/") + top))) {
                        mz_zip_reader_end(&zip);
                        return fail(QStringLiteral("cannot delete old folder: %1").arg(top));
                    }
                }
            }
        }

        // 提取并复制文件到 GameData（事务化写入，失败整体回滚）
        QStringList installedRelPaths;
        for (const InstallableFile &f : files) {
            // 文件级覆盖冲突检测（对应官方 Registry.RegisterModule 的 installed_files 一致性检查）：
            // 目标文件已被其他已安装模组登记归属 → 拒绝覆盖并整体回滚。
            // 归属为本模块自身（升级/重装同标识符）则允许覆盖。
            const QString owner = reg->fileOwner(f.destination);
            if (!owner.isEmpty() && owner != mod.identifier) {
                mz_zip_reader_end(&zip);
                return fail(QStringLiteral("文件冲突：%1 %2 将覆盖已安装模组 %3 的文件 %4")
                    .arg(mod.identifier, mod.version, owner, f.destination));
            }
            QByteArray content;
            if (!extractToMem(zip, f.sourceName.toUtf8().constData(), &content, &result.error)) {
                mz_zip_reader_end(&zip);
                return fail(result.error);
            }
            const QString abs = m_instance->toAbsoluteGameDir(f.destination);
            // 防 Zip Slip：目标路径逃逸出游戏目录时拒绝写入并整体回滚
            if (abs.isEmpty()) {
                mz_zip_reader_end(&zip);
                return fail(QStringLiteral("非法安装路径（越出游戏目录）：%1").arg(f.destination));
            }
            if (!tx->writeFile(abs, content)) {
                mz_zip_reader_end(&zip);
                return fail(QStringLiteral("cannot write %1").arg(abs));
            }
            installedRelPaths << f.destination;
        }
        mz_zip_reader_end(&zip);
        emit installProgress(mod.identifier, 90);

        // 更新 registry
        InstalledModule im;
        im.identifier = mod.identifier;
        im.module = mod;
        im.files = installedRelPaths;
        im.installTime = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        reg->registerModule(im);
        emit moduleInstalled(mod.identifier);
        result.installedIdentifiers << mod.identifier;
        emit installProgress(mod.identifier, 100);
    }

    if (autoTx) {
        if (!m_instance->saveRegistry())
            return fail(QStringLiteral("注册表被其他进程占用，无法保存（registry.locked 已被持有）"));
        tx->commit();
    }
    result.ok = true;
    return result;
}

InstallResult ModuleInstaller::install(const QVector<CkanModule> &modules,
                                       const QString &downloadDir,
                                       const QStringList &foldersToDelete,
                                       const QStringList &mirrorPrefixes,
                                       bool preferModuleMirrors)
{
    QString err;
    if (!downloadModules(modules, downloadDir, mirrorPrefixes, preferModuleMirrors, &err)) {
        InstallResult r;
        r.error = err;
        return r;
    }
    return installFromCache(modules, downloadDir, foldersToDelete);
}

void ModuleInstaller::cancel()
{
    m_cancelRequested.store(true);
}

InstallResult ModuleInstaller::uninstall(const QString &identifier, TxFileManager *tx)
{
    return uninstallMany({identifier}, tx);
}

InstallResult ModuleInstaller::uninstallMany(const QStringList &identifiers, TxFileManager *tx)
{
    InstallResult result;

    Registry *reg = m_instance->registry();
    // 一次性校验：全部目标必须已安装；缺任何一个 → 整批直接失败，尚未发生任何改动。
    for (const QString &id : identifiers) {
        if (!reg->isInstalled(id)) {
            result.error = QStringLiteral("%1 is not installed").arg(id);
            return result;
        }
    }

    // 事务化卸载：删除的文件先备份，任一步失败/取消整体回滚（恢复已删除文件、还原注册表）。
    std::unique_ptr<TxFileManager> autoTx;
    if (!tx) {
        autoTx.reset(new TxFileManager(m_instance->ckanDir() + QStringLiteral("/transactions")));
        tx = autoTx.get();
    }
    const QByteArray regSnapshot = reg->toJson(); // 事务开始时的注册表快照

    const auto fail = [&](const QString &err, bool cancelled) {
        if (autoTx) {
            tx->rollback();
            m_instance->restoreRegistrySnapshot(regSnapshot);
        }
        InstallResult r;
        r.error = err;
        r.cancelled = cancelled;
        return r;
    };

    // 整批原子：跨全部目标共享一个卸载顺序（依赖链自外向内，先卸依赖者最后卸目标）。
    QSet<QString> visited;
    QStringList order;
    for (const QString &id : identifiers) {
        if (!collectReverseDeps(reg, id, visited, order))
            return fail(QStringLiteral("%1 is not installed").arg(id), false);
    }

    for (int i = 0; i < order.size(); ++i) {
        const QString &id = order.at(i);
        // 取消检查点：已删除的文件都在 tx 备份中，回滚即以“卸载前”一切还原。
        if (m_cancelRequested.load())
            return fail(QStringLiteral("uninstall cancelled"), true);
        const InstalledModule *im = reg->installed(id);
        if (!im) continue;
        for (const QString &rel : im->files) {
            const QString abs = m_instance->toAbsoluteGameDir(rel);
            if (!tx->deleteFile(abs))
                return fail(QStringLiteral("cannot delete %1").arg(abs), false);
        }
        reg->unregisterModule(id);
        result.installedIdentifiers << id;
    }

    if (autoTx) {
        if (!m_instance->saveRegistry())
            return fail(QStringLiteral("注册表被其他进程占用，无法保存（registry.locked 已被持有）"), false);
        tx->commit();
    }
    result.ok = true;
    return result;
}

QStringList ModuleInstaller::uninstallPlan(const QStringList &identifiers)
{
    Registry *reg = m_instance->registry();
    for (const QString &id : identifiers)
        if (!reg->isInstalled(id)) return QStringList();
    QSet<QString> visited;
    QStringList order;
    for (const QString &id : identifiers)
        if (!collectReverseDeps(reg, id, visited, order)) return QStringList();
    return order;
}

} // namespace ckan