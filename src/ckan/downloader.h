#ifndef CKAN_DOWNLOADER_H
#define CKAN_DOWNLOADER_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QList>
#include <functional>
#include <atomic>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include "ckan_export.h"

namespace ckan {

// 简单 HTTP 下载器，支持镜像回退、代理、进度、超时与取消。
class CKAN_API Downloader : public QObject
{
    Q_OBJECT
public:
    // 内容校验器：返回 false 表示本次下载内容不符合预期（如非 ZIP 归档），
    // 将被视为失败并尝试下一个镜像。
    using Validator = std::function<bool(const QByteArray &)>;
    // 进度回调：received 已接收字节数，total 总字节数（未知时为 -1）。
    using ProgressCallback = std::function<void(qint64 received, qint64 total)>;

    explicit Downloader(QObject *parent = nullptr);

    // 同步下载 URL 全部内容到内存。成功返回 true。
    // mirrors 为可选镜像列表。默认主 URL 优先；preferMirror=true 时镜像优先。
    // validator 可选：用于校验下载内容，不匹配则继续尝试下一个镜像。
    bool download(const QString &url,
                  const QStringList &mirrors,
                  QByteArray *out,
                  QString *error = nullptr,
                  const Validator &validator = nullptr,
                  bool preferMirror = false);

    // 带进度/超时/取消的下载（在调用线程内驱动事件循环）。
    // 连接超时与传输空闲超时均 30 秒；cancelFlag 置真时立即中止，
    // 返回 false 且 error 为「已取消」。
    // resumeAttempts > 0 时启用断点续传：同 URL 传输中断（如连接被关闭）后，
    // 保留已收字节并用 Range 请求剩余部分，最多续传 resumeAttempts 次；
    // 服务器忽略 Range（返回 200 全量）时自动清空已收、从零重下。
    bool downloadProgressed(const QString &url,
                            const QStringList &mirrors,
                            QByteArray *out,
                            QString *error = nullptr,
                            const Validator &validator = nullptr,
                            const ProgressCallback &onProgress = nullptr,
                            std::atomic_bool *cancelFlag = nullptr,
                            int resumeAttempts = 0,
                            bool preferMirror = false);

    // 异步下载（供 UI 使用），完成后发出 finished/failed 信号
    void downloadAsync(const QString &url, const QStringList &mirrors);

    // 设置网络代理（如 http://127.0.0.1:7890），空则直连。
    // 实例级配置，替代原全局静态，避免库内部可变的全局状态。
    void setProxyUrl(const QString &proxyUrl);
    QString proxyUrl() const { return m_proxyUrl; }

    // 设置单链接下载限速（字节/秒，0=不限速）。负数按 0 处理。
    // 每次 download*() 调用前设置；对本下载器的（当前及后续）连接有效。
    void setDownloadRate(qint64 bytesPerSecond)
    {
        m_bytesPerSecond = bytesPerSecond > 0 ? bytesPerSecond : 0;
    }
    qint64 downloadRate() const { return m_bytesPerSecond; }

signals:
    void finished(const QByteArray &data, const QString &url);
    void failed(const QString &url, const QString &error);

private:
    QNetworkAccessManager m_nam;
    QByteArray m_data;
    QStringList m_mirrors;
    QString m_proxyUrl;
    int m_attempt = 0;
    QString m_currentUrl;
    bool m_async = false;
    qint64 m_bytesPerSecond = 0; // 下载限速（字节/秒，0=不限速）

    void startAttempt();
};

} // namespace ckan

#endif // CKAN_DOWNLOADER_H