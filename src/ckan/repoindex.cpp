#include "repoindex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QSaveFile>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <functional>
#include <algorithm>

#include "downloader.h"
#include "version.h"

// miniz 原始 deflate 解压头
#include "miniz.h"

namespace ckan {

namespace {

// 将仓库名转换为安全的缓存文件名
QString safeRepoName(const QString &name)
{
    QString s = name;
    s.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
    return s.isEmpty() ? QStringLiteral("default") : s;
}

// 按仓库自身 URL 生成镜像地址：仅 GitHub 托管的仓库适用（前缀 + 仓库 URL）。
// 非 GitHub 仓库返回空，避免下载失败时错误回退到其他仓库（如官方 KSP-CKAN）的内容。
QStringList repoMirrorUrls(const Repository &repo, const QStringList &prefixes)
{
    QStringList out;
    if (!repo.uri.startsWith(QStringLiteral("https://github.com/"))
        && !repo.uri.startsWith(QStringLiteral("http://github.com/")))
        return out;
    for (const QString &p : prefixes) {
        if (p.isEmpty()) continue;
        out << p + repo.uri;
    }
    return out;
}

// 解压输出上限：仓库索引解压后不应超过 256MB（官方 CKAN-meta 约数十 MB），
// 防止恶意/异常归档膨胀内存（解压炸弹）。
static constexpr qint64 kMaxUncompressedBytes = 256LL * 1024 * 1024;
// 压缩包本身大小上限：64MB（配合下载侧超时，防止异常大文件灌入）。
static constexpr qint64 kMaxCompressedBytes = 64LL * 1024 * 1024;

// 流式解压输出接收器：累计输出字节，超过上限即置 overflow 并中止解压。
struct GunzipSink {
    QByteArray data;
    qint64 limit = kMaxUncompressedBytes;
    bool overflow = false;
};

int gunzipPut(const void *buf, int len, void *user)
{
    GunzipSink *s = static_cast<GunzipSink *>(user);
    if (s->data.size() + len > s->limit) {
        s->overflow = true;
        return 0; // 请求中止解压
    }
    s->data.append(static_cast<const char *>(buf), len);
    return len;
}

// 从 gzip 格式内存数据解压为原始 tar 字节。
// 带大小上限（防解压炸弹）与 ISIZE 完整性校验（防截断/损坏）。
bool gunzip(const QByteArray &gz, QByteArray *out, QString *error)
{
    const quint8 *src = reinterpret_cast<const quint8 *>(gz.constData());
    const size_t len = static_cast<size_t>(gz.size());
    if (len < 18) {
        if (error) *error = QStringLiteral("仓库归档过短（不是有效的 gzip）");
        return false;
    }
    if (gz.size() > kMaxCompressedBytes) {
        if (error) *error = QStringLiteral("仓库压缩包过大（%1 字节）").arg(gz.size());
        return false;
    }
    if (src[0] != 0x1f || src[1] != 0x8b || src[2] != 8) {
        if (error) *error = QStringLiteral("不是有效的 gzip 归档");
        return false;
    }

    size_t off = 10;
    const quint8 flags = src[3];
    if (flags & 0x04) { // FEXTRA
        if (off + 2 > len) return false;
        const size_t xlen = src[off] | (src[off + 1] << 8);
        off += 2 + xlen;
    }
    if (flags & 0x08) { while (off < len && src[off] != 0) ++off; ++off; }
    if (flags & 0x10) { while (off < len && src[off] != 0) ++off; ++off; }
    if (flags & 0x02) off += 2;
    if (off >= len) return false;

    // 流式解压（带输出上限），raw deflate（跳过 gzip 头）。
    GunzipSink sink;
    size_t inBytes = len - off;
    const int ok = tinfl_decompress_mem_to_callback(src + off, &inBytes, &gunzipPut, &sink, 0);
    if (sink.overflow) {
        if (error) *error = QStringLiteral("仓库索引解压超过大小上限，已拒绝");
        return false;
    }
    if (!ok) {
        if (error) *error = QStringLiteral("仓库归档解压失败（已损坏）");
        return false;
    }

    // 完整性校验：gzip 尾部 ISIZE（未压缩大小 mod 2^32，小端序）应与实际解压大小一致。
    // 不一致说明下载被截断/损坏（官方 CKAN 索引为单成员 gzip，ISIZE 即总大小）。
    const quint64 isize = static_cast<quint64>(src[len - 4])
                        | (static_cast<quint64>(src[len - 3]) << 8)
                        | (static_cast<quint64>(src[len - 2]) << 16)
                        | (static_cast<quint64>(src[len - 1]) << 24);
    if (isize != static_cast<quint64>(sink.data.size())) {
        if (error) *error = QStringLiteral("仓库归档被截断或损坏（大小校验失败）");
        return false;
    }

    if (out) *out = sink.data;
    return true;
}

// 解析 tar 归档，对每个普通文件调用回调 (路径, 内容)。errno 无关。
void parseTar(const QByteArray &tar,
              const std::function<void(const QString &, const QByteArray &)> &callback)
{
    size_t off = 0;
    const size_t size = static_cast<size_t>(tar.size());
    while (off + 512 <= size) {
        const quint8 *block = reinterpret_cast<const quint8 *>(tar.constData()) + off;
        bool allZero = true;
        for (int i = 0; i < 512; ++i) { if (block[i] != 0) { allZero = false; break; } }
        if (allZero) break;

        // 文件名（100 字节）
        char name[101] = {0};
        int nameLen = 0;
        for (; nameLen < 100 && block[nameLen] != 0; ++nameLen)
            name[nameLen] = static_cast<char>(block[nameLen]);
        const QString entryName = QString::fromUtf8(name);

        // 类型标志（offset 156）
        const char typeflag = static_cast<char>(block[156]);

        // 文件大小（octal，offset 124，12 字节）
        quint64 fileSize = 0;
        for (int i = 124; i < 136 && block[i] != 0 && block[i] != ' '; ++i)
            if (block[i] >= '0' && block[i] <= '7')
                fileSize = fileSize * 8 + static_cast<quint64>(block[i] - '0');

        // 数据区（按 512 对齐）
        const size_t dataOff = off + 512;
        const size_t padded = static_cast<size_t>((fileSize + 511) / 512) * 512;
        if (dataOff + fileSize > size) break;

        if (typeflag == '0' || typeflag == '\0') {
            const QByteArray data(tar.constData() + dataOff, static_cast<int>(fileSize));
            callback(entryName, data);
        }
        off = dataOff + padded;
    }
}

} // namespace

bool RepoIndex::parseTarGz(const QByteArray &tarGz, QMap<QString, QVector<CkanModule>> *index,
                           QMap<QString, int> *downloadCounts, QString *error)
{
    QByteArray tar;
    if (!gunzip(tarGz, &tar, error)) {
        if (error && error->isEmpty())
            *error = QStringLiteral("failed to gunzip repository archive");
        return false;
    }
    if (index) index->clear();
    if (downloadCounts) downloadCounts->clear();
    parseTar(tar, [&](const QString &entryName, const QByteArray &data) {
        if (entryName.endsWith(QStringLiteral(".ckan"), Qt::CaseInsensitive)) {
            if (!index) return;
            QString err;
            const CkanModule m = CkanModule::fromJson(data, &err);
            if (m.isValid())
                (*index)[m.identifier].append(m);
        } else if (downloadCounts
                   && entryName.endsWith(QStringLiteral("download_counts.json"), Qt::CaseInsensitive)) {
            const QJsonDocument doc = QJsonDocument::fromJson(data);
            if (!doc.isObject()) return;
            const QJsonObject obj = doc.object();
            for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
                const int n = it.value().toInt(-1);
                if (n >= 0)
                    (*downloadCounts)[it.key()] = n;
            }
        }
    });
    return true;
}

bool RepoIndex::build(const Repository &repo, const QStringList &mirrors,
                      QMap<QString, QVector<CkanModule>> *index,
                      QMap<QString, int> *downloadCounts, QString *error,
                      const std::function<void(const QString &, qint64, qint64)> &onProgress,
                      std::atomic_bool *cancelFlag, bool preferMirror,
                      const QString &proxyUrl)
{
    Downloader dl;
    dl.setProxyUrl(proxyUrl);
    QByteArray data;
    const auto progress = [&onProgress, &repo](qint64 received, qint64 total) {
        if (onProgress) onProgress(repo.name, received, total);
    };
    const QStringList mirrorUrls = repoMirrorUrls(repo, mirrors);
    if (!dl.downloadProgressed(repo.uri, mirrorUrls, &data, error, nullptr, progress,
                               cancelFlag, 0, preferMirror))
        return false;
    return parseTarGz(data, index, downloadCounts, error);
}

bool RepoIndex::buildCached(const Repository &repo, const QStringList &mirrors,
                            QMap<QString, QVector<CkanModule>> *index,
                            QMap<QString, int> *downloadCounts, QString *error,
                            bool forceRefresh, qint64 maxAgeSecs,
                            const std::function<void(const QString &, qint64, qint64)> &onProgress,
                            std::atomic_bool *cancelFlag, bool preferMirror,
                            const QString &cacheDir, const QString &proxyUrl)
{
    // 未配置缓存目录：退回每次下载
    if (cacheDir.isEmpty())
        return build(repo, mirrors, index, downloadCounts, error, onProgress,
                     cancelFlag, preferMirror, proxyUrl);

    QDir().mkpath(cacheDir);
    const QString cacheFile = QDir(cacheDir).filePath(safeRepoName(repo.name) + QStringLiteral(".tar.gz"));

    // 尝试使用新鲜缓存
    if (!forceRefresh) {
        const QFileInfo fi(cacheFile);
        if (fi.exists()) {
            const qint64 age = fi.lastModified().secsTo(QDateTime::currentDateTime());
            if (age >= 0 && age < maxAgeSecs) {
                QFile f(cacheFile);
                if (f.open(QIODevice::ReadOnly)) {
                    const QByteArray data = f.readAll();
                    if (parseTarGz(data, index, downloadCounts, error))
                        return true;
                }
            }
        }
    }

    // 下载并写入缓存
    Downloader dl;
    dl.setProxyUrl(proxyUrl);
    QByteArray data;
    const auto progress = [&onProgress, &repo](qint64 received, qint64 total) {
        if (onProgress) onProgress(repo.name, received, total);
    };
    const QStringList mirrorUrls = repoMirrorUrls(repo, mirrors);
    if (dl.downloadProgressed(repo.uri, mirrorUrls, &data, error, nullptr, progress,
                              cancelFlag, 0, preferMirror)) {
        QSaveFile sf(cacheFile);
        if (sf.open(QIODevice::WriteOnly)) {
            sf.write(data);
            sf.commit();
        }
        return parseTarGz(data, index, downloadCounts, error);
    }

    // 下载失败：回退到旧缓存（即使已过期），避免仓库故障导致索引整体不可用
    QFile stale(cacheFile);
    if (stale.exists() && stale.open(QIODevice::ReadOnly)) {
        const QByteArray data = stale.readAll();
        if (parseTarGz(data, index, downloadCounts, error))
            return true;
    }
    return false;
}

bool RepoIndex::buildManyCached(const QVector<Repository> &repos, const QStringList &mirrors,
                                QMap<QString, QVector<CkanModule>> *index,
                                QMap<QString, int> *downloadCounts, QString *error,
                                bool forceRefresh, qint64 maxAgeSecs,
                                const std::function<void(const QString &, qint64, qint64)> &onProgress,
                                std::atomic_bool *cancelFlag, bool preferMirror,
                                const QString &cacheDir, const QString &proxyUrl)
{
    if (index) index->clear();
    if (downloadCounts) downloadCounts->clear();

    // 按优先级升序处理：priority 值越小优先级越高，先处理者在其版本/计数冲突时获胜
    QVector<Repository> ordered = repos;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Repository &a, const Repository &b) { return a.priority < b.priority; });

    if (ordered.isEmpty()) {
        if (error) *error = QStringLiteral("仓库列表为空，无法建立索引");
        return false;
    }

    QStringList failed;
    int okCount = 0;
    QVector<QMap<QString, QVector<CkanModule>>> subIndexes;
    QVector<QMap<QString, int>> subCounts;

    for (const Repository &repo : ordered) {
        QMap<QString, QVector<CkanModule>> subIndex;
        QMap<QString, int> subCount;
        QString repoErr;
        const bool ok = buildCached(repo, mirrors, &subIndex, &subCount, &repoErr,
                                    forceRefresh, maxAgeSecs,
                                    onProgress,
                                    cancelFlag, preferMirror, cacheDir, proxyUrl);
        if (!ok) {
            failed << QStringLiteral("%1: %2").arg(repo.name, repoErr);
            continue;
        }
        ++okCount;
        subIndexes.append(subIndex);
        subCounts.append(subCount);
    }

    if (okCount == 0) {
        if (error) *error = failed.join(QStringLiteral("；"));
        return false;
    }
    // 部分仓库失败：仍视为整体成功，但把失败仓库透出，便于 UI 提示用户
    if (!failed.isEmpty() && error)
        *error = failed.join(QStringLiteral("；"));
    mergeSubIndexes(subIndexes, subCounts, index, downloadCounts);
    return true;
}

void RepoIndex::mergeSubIndexes(const QVector<QMap<QString, QVector<CkanModule>>> &subIndexes,
                                const QVector<QMap<QString, int>> &subCounts,
                                QMap<QString, QVector<CkanModule>> *index,
                                QMap<QString, int> *downloadCounts)
{
    if (index) index->clear();
    if (downloadCounts) downloadCounts->clear();

    QSet<QString> seenVersionKeys; // "identifier\x1fversion"，跨仓库去重
    for (int i = 0; i < subIndexes.size(); ++i) {
        if (index) {
            for (auto it = subIndexes.at(i).constBegin(); it != subIndexes.at(i).constEnd(); ++it) {
                const QString &id = it.key();
                QVector<CkanModule> &dest = (*index)[id];
                for (const CkanModule &m : it.value()) {
                    const QString key = id + QChar(0x1f) + m.version;
                    if (seenVersionKeys.contains(key)) continue;
                    seenVersionKeys.insert(key);
                    dest.append(m);
                }
            }
        }
        // 合并下载次数（同 identifier 高优先级先到先得）
        if (downloadCounts && i < subCounts.size()) {
            for (auto it = subCounts.at(i).constBegin(); it != subCounts.at(i).constEnd(); ++it) {
                if (!downloadCounts->contains(it.key()))
                    (*downloadCounts)[it.key()] = it.value();
            }
        }
    }
}

QVector<CkanModule> RepoIndex::versionsFor(const QMap<QString, QVector<CkanModule>> &index,
                                           const QString &identifier)
{
    QVector<CkanModule> v = index.value(identifier);
    std::sort(v.begin(), v.end(), [](const CkanModule &a, const CkanModule &b) {
        return ModuleVersion(a.version) > ModuleVersion(b.version);
    });
    return v;
}

CkanModule RepoIndex::latestFor(const QMap<QString, QVector<CkanModule>> &index,
                                const QString &identifier)
{
    const QVector<CkanModule> v = versionsFor(index, identifier);
    return v.isEmpty() ? CkanModule() : v.first();
}

} // namespace ckan