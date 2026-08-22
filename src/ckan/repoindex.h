#ifndef CKAN_REPOINDEX_H
#define CKAN_REPOINDEX_H

#include <QByteArray>
#include <QString>
#include <QMap>
#include <QVector>
#include <functional>
#include <atomic>

#include "ckan_export.h"
#include "ckanmodule.h"
#include "repository.h"

namespace ckan {

// 仓库索引：下载 CKAN-meta tar.gz，解压并建立 identifier -> 模块列表 索引。
// 支持将下载的 tar.gz 落盘缓存，避免每次启动都重新下载。
class CKAN_API RepoIndex
{
public:
    // 索引缓存默认有效期（秒）
    static constexpr qint64 kDefaultCacheAgeSecs = 6 * 60 * 60;

    // 从 tar.gz 内存数据解析所有 .ckan 文件（及可选的 download_counts.json）。
    // downloadCounts 非空时解析 identifier -> 下载次数；为空则忽略该文件。
    static bool parseTarGz(const QByteArray &tarGz, QMap<QString, QVector<CkanModule>> *index,
                           QMap<QString, int> *downloadCounts = nullptr,
                           QString *error = nullptr);

    // 从仓库下载并建立索引（每次都会下载）。
    // onProgress：下载进度回调(repoName, received, total)，cancelFlag 置真则中止并返回失败。
    // mirrors 为镜像前缀列表，仅对 GitHub 托管的仓库生效（前缀 + 仓库自身 URL）；
    // 非 GitHub 仓库忽略镜像，避免错误回退到其他仓库的内容。
    // preferMirror=true 时镜像优先（否则官方优先）。
    // proxyUrl 为该次下载使用的代理（空=直连），替代原全局静态配置。
    static bool build(const Repository &repo, const QStringList &mirrors,
                      QMap<QString, QVector<CkanModule>> *index,
                      QMap<QString, int> *downloadCounts = nullptr, QString *error = nullptr,
                      const std::function<void(const QString &, qint64, qint64)> &onProgress = {},
                      std::atomic_bool *cancelFlag = nullptr,
                      bool preferMirror = false,
                      const QString &proxyUrl = QString());

    // 带缓存的构建：优先使用缓存（fresh 且未强制刷新时），否则下载并写入缓存。
    // 下载失败时回退到旧缓存（即使已过期），避免单仓库故障导致整体失败。
    // mirrors 语义同 build()：镜像前缀列表，仅对 GitHub 托管的仓库生效。
    // maxAgeSecs 为缓存有效期（秒），默认 6 小时。
    // cacheDir 为索引缓存目录（空=不落盘缓存）；proxyUrl 为该次下载使用的代理（空=直连）。
    static bool buildCached(const Repository &repo, const QStringList &mirrors,
                            QMap<QString, QVector<CkanModule>> *index,
                            QMap<QString, int> *downloadCounts = nullptr, QString *error = nullptr,
                            bool forceRefresh = false, qint64 maxAgeSecs = kDefaultCacheAgeSecs,
                            const std::function<void(const QString &, qint64, qint64)> &onProgress = {},
                            std::atomic_bool *cancelFlag = nullptr,
                            bool preferMirror = false,
                            const QString &cacheDir = QString(),
                            const QString &proxyUrl = QString());

    // 多仓库构建：依次处理每个仓库（按 priority 升序，值越小优先级越高，先处理者获胜），
    // 将各仓库的模块版本与下载次数合并。同 identifier+version 冲突时高优先级仓库优先；
    // 单个仓库失败时回退其旧缓存；至少一个仓库成功即整体成功。
    // 部分仓库失败时，error 会带上失败仓库列表（供 UI 提示），但不影响整体成功。
    // mirrors 语义同 build()：镜像前缀列表，仅对 GitHub 托管的仓库生效。
    // onProgress 额外携带仓库名，便于区分当前下载的仓库。
    // cacheDir 为索引缓存目录（空=不落盘缓存）；proxyUrl 为该次下载使用的代理（空=直连）。
    static bool buildManyCached(const QVector<Repository> &repos, const QStringList &mirrors,
                                QMap<QString, QVector<CkanModule>> *index,
                                QMap<QString, int> *downloadCounts = nullptr,
                                QString *error = nullptr,
                                bool forceRefresh = false,
                                qint64 maxAgeSecs = kDefaultCacheAgeSecs,
                                const std::function<void(const QString &, qint64, qint64)> &onProgress = {},
                                std::atomic_bool *cancelFlag = nullptr,
                                bool preferMirror = false,
                                const QString &cacheDir = QString(),
                                const QString &proxyUrl = QString());

    // 便捷：取某 identifier 的全部版本（按版本降序）
    static QVector<CkanModule> versionsFor(const QMap<QString, QVector<CkanModule>> &index,
                                           const QString &identifier);
    // 取某 identifier 的最新版本
    static CkanModule latestFor(const QMap<QString, QVector<CkanModule>> &index,
                                const QString &identifier);

    // 将各仓库解析结果按给定顺序合并（数组前面的仓库优先级更高，冲突时先到先得）。
    // 同 identifier+version 只保留首个仓库的模块；同 identifier 的下载计数只取首个仓库。
    // subIndexes / subCounts 与仓库顺序一一对应。
    static void mergeSubIndexes(const QVector<QMap<QString, QVector<CkanModule>>> &subIndexes,
                                const QVector<QMap<QString, int>> &subCounts,
                                QMap<QString, QVector<CkanModule>> *index,
                                QMap<QString, int> *downloadCounts = nullptr);
};

} // namespace ckan

#endif // CKAN_REPOINDEX_H