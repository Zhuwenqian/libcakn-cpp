#ifndef CKAN_CKANCONFIG_H
#define CKAN_CKANCONFIG_H

#include <QString>
#include <QStringList>

#include "ckan_export.h"

namespace ckan {

// libckan 运行配置：替代原先的全局静态配置
// （Downloader::setProxyUrl / RepoIndex::setCacheDir）与镜像前缀散落问题，
// 由启动器在构造 CKan 时一次性传入，避免库内部可变的全局状态与配置双向渗透。
// 镜像前缀：拼接在官方 URL 前（gh 代理，可代理任意 GitHub 资源），空列表表示不使用镜像。
struct CKAN_API CKanConfig {
    QString indexCacheDir;             // 索引缓存目录（空=不做落盘缓存）
    QString proxyUrl;                  // 网络代理（如 http://127.0.0.1:7890，空=直连）
    QStringList indexMirrorPrefixes;   // 索引下载镜像前缀（仅 GitHub 托管仓库生效）
    QStringList moduleMirrorPrefixes;  // 模组下载镜像前缀
    int downloadConcurrency = 3;       // 模组并行下载数
    qint64 downloadRateLimitBps = 0;   // 单链接下载限速（字节/秒，0=不限速）

    // 内置默认镜像前缀（官方优先，镜像回退）
    static inline QStringList defaultIndexMirrorPrefixes()
    {
        return { QStringLiteral("https://gh-proxy.com/"),
                 QStringLiteral("https://ghfast.top/") };
    }
    static inline QStringList defaultModuleMirrorPrefixes()
    {
        return { QStringLiteral("https://gh-proxy.com/"),
                 QStringLiteral("https://ghfast.top/") };
    }
};

} // namespace ckan

#endif // CKAN_CKANCONFIG_H
