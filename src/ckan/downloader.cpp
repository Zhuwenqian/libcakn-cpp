#include "downloader.h"

#include <QNetworkProxy>
#include <QUrl>
#include <QEventLoop>
#include <QTimer>

namespace ckan {

// 传输超时（毫秒）：Qt 的 transferTimeout 统一覆盖连接建立与传输空闲。
static constexpr int kTransferTimeoutMs = 30000;

Downloader::Downloader(QObject *parent)
    : QObject(parent)
{
}

void Downloader::setProxyUrl(const QString &proxyUrl)
{
    m_proxyUrl = proxyUrl;
    if (proxyUrl.isEmpty()) {
        m_nam.setProxy(QNetworkProxy::NoProxy);
    } else {
        m_nam.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy,
                                     QUrl(proxyUrl).host(),
                                     static_cast<quint16>(QUrl(proxyUrl).port(0))));
    }
}

void Downloader::startAttempt()
{
    if (m_attempt >= m_mirrors.size()) {
        if (m_async)
            emit failed(m_currentUrl, QStringLiteral("all download attempts failed"));
        return;
    }
    m_currentUrl = m_mirrors.at(m_attempt++);
    QNetworkRequest req{QUrl(m_currentUrl)};
    req.setRawHeader("User-Agent", "HelloKSPLauncher/1.0");
    m_nam.get(req);
}

bool Downloader::download(const QString &url, const QStringList &mirrors,
                          QByteArray *out, QString *error,
                          const Validator &validator, bool preferMirror)
{
    m_mirrors.clear();
    if (preferMirror) m_mirrors << mirrors << url;
    else              m_mirrors << url << mirrors;
    m_attempt = 0;
    bool ok = false;
    QString lastError;

    while (m_attempt < m_mirrors.size()) {
        m_currentUrl = m_mirrors.at(m_attempt++);
        QNetworkRequest req{QUrl(m_currentUrl)};
        req.setRawHeader("User-Agent", "HelloKSPLauncher/1.0");
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

        QNetworkReply *reply = m_nam.get(req);
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        const QNetworkReply::NetworkError netErr = reply->error();
        const QString errStr = reply->errorString();
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        if (netErr == QNetworkReply::NoError && (!validator || validator(data))) {
            *out = data;
            ok = true;
            break;
        }
        if (netErr != QNetworkReply::NoError) {
            lastError = QStringLiteral("网络错误: %1").arg(errStr);
        } else {
            lastError = QStringLiteral("内容校验失败（%1 字节，非预期格式，可能返回了错误页面或被截断）")
                            .arg(data.size());
        }
    }
    if (!ok && error && !lastError.isEmpty())
        *error = lastError;
    return ok;
}

bool Downloader::downloadProgressed(const QString &url, const QStringList &mirrors,
                                    QByteArray *out, QString *error,
                                    const Validator &validator,
                                    const ProgressCallback &onProgress,
                                    std::atomic_bool *cancelFlag,
                                    int resumeAttempts, bool preferMirror)
{
    m_mirrors.clear();
    if (preferMirror) m_mirrors << mirrors << url;
    else              m_mirrors << url << mirrors;
    m_attempt = 0;
    bool ok = false;
    QString lastError;

    while (m_attempt < m_mirrors.size()) {
        m_currentUrl = m_mirrors.at(m_attempt++);
        QByteArray partial;         // 同一 URL 已收字节（跨续传累积）
        int retriesLeft = resumeAttempts;

        // 内层循环：同一 URL 支持断点续传
        for (;;) {
            QNetworkRequest req{QUrl(m_currentUrl)};
            req.setRawHeader("User-Agent", "HelloKSPLauncher/1.0");
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
            // Qt 的 transferTimeout 同时覆盖连接建立与传输空闲（每次收到数据都会重置），
            // 因此 30s 的连接超时与 30s 的空闲超时由它统一承担。
            req.setTransferTimeout(kTransferTimeoutMs);

            if (!partial.isEmpty())
                req.setRawHeader("Range", QByteArrayLiteral("bytes=")
                                 + QByteArray::number(partial.size()) + QByteArrayLiteral("-"));

            QNetworkReply *reply = m_nam.get(req);
            QEventLoop loop;
            // 周期轮询取消标志：置真则中止并退出事件循环
            QTimer cancelTimer;
            cancelTimer.setInterval(200);
            connect(&cancelTimer, &QTimer::timeout, &loop, [&loop, reply, cancelFlag]() {
                if (cancelFlag && cancelFlag->load()) {
                    reply->abort();
                    loop.quit();
                }
            });
            // 进度回调：累计已收字节（partial + 本次连接已收）
            connect(reply, &QNetworkReply::downloadProgress, &loop,
                    [onProgress, &partial](qint64 received, qint64 total) {
                        if (onProgress) {
                            const qint64 cumReceived = static_cast<qint64>(partial.size()) + received;
                            const qint64 cumTotal = total > 0
                                ? static_cast<qint64>(partial.size()) + total
                                : -1;
                            onProgress(cumReceived, cumTotal);
                        }
                    });
            connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            cancelTimer.start();
            loop.exec();
            cancelTimer.stop();

            const QNetworkReply::NetworkError netErr = reply->error();
            const QString errStr = reply->errorString();
            const QByteArray chunk = reply->readAll();
            const int statusCode =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            reply->deleteLater();

            if (cancelFlag && cancelFlag->load()) {
                lastError = QStringLiteral("已取消");
                break;
            }

            if (netErr == QNetworkReply::NoError) {
                // 成功：将本次收到的数据合并到 partial
                if (statusCode == 206 && !partial.isEmpty()) {
                    partial.append(chunk);       // 续传成功，追加
                } else if (statusCode == 200 && !partial.isEmpty()) {
                    partial = chunk;             // 服务器不支持 Range，全量重下
                } else {
                    partial = chunk;             // 首次下载或 206 但无已收
                }

                if (!validator || validator(partial)) {
                    *out = partial;
                    ok = true;
                    break;
                }
                lastError = QStringLiteral("内容校验失败（%1 字节，非预期格式，可能返回了错误页面或被截断）")
                                .arg(partial.size());
                break;
            }

            // 网络错误（如 connection closed）：保留已收字节，尝试续传
            partial.append(chunk);
            if (retriesLeft > 0) {
                --retriesLeft;
                continue; // 同一 URL 续传
            }
            lastError = QStringLiteral("网络错误: %1").arg(errStr);
            break;
        }
        if (ok) break;
    }
    if (!ok && error && !lastError.isEmpty())
        *error = lastError;
    return ok;
}

void Downloader::downloadAsync(const QString &url, const QStringList &mirrors)
{
    m_async = true;
    m_mirrors.clear();
    m_mirrors << url << mirrors;
    m_attempt = 0;
    m_data.clear();

    connect(&m_nam, &QNetworkAccessManager::finished, this, [this](QNetworkReply *reply) {
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QByteArray data = reply->readAll();
        reply->deleteLater();
        if (ok) {
            emit finished(data, reply->url().toString());
        } else if (m_attempt < m_mirrors.size()) {
            startAttempt();
        } else {
            emit failed(m_currentUrl, tr("下载失败:%1").arg(reply->errorString()));
        }
    });

    startAttempt();
}

} // namespace ckan