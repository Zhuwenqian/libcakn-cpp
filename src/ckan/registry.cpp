#include "registry.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QMutex>

namespace ckan {

static QString normalizeRelPath(const QString &p)
{
    QString n = p;
    n.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (n.startsWith(QLatin1Char('/'))) n = n.mid(1);
    return n;
}

Registry Registry::fromJson(const QByteArray &json, QString *error)
{
    Registry reg;
    reg.loadFromJson(json, error);
    return reg;
}

bool Registry::loadFromJson(const QByteArray &json, QString *error)
{
    // 先在局部解析，缩短持锁区间；解析失败清空为默认注册表。
    QHash<QString, QString> fileOwners;
    QMap<QString, InstalledModule> mods;
    QMap<QString, Repository> repos;
    QMap<QString, QString> dlls;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject root = doc.object();
        const int rv = root.value(QStringLiteral("registry_version")).toInt(LATEST_REGISTRY_VERSION);
        if (rv <= LATEST_REGISTRY_VERSION) {
            const QJsonObject reposObj = root.value(QStringLiteral("sorted_repositories")).toObject();
            for (auto it = reposObj.constBegin(); it != reposObj.constEnd(); ++it) {
                const QJsonObject ro = it.value().toObject();
                Repository r;
                r.name = it.key();
                r.uri  = ro.value(QStringLiteral("uri")).toString();
                r.priority = ro.value(QStringLiteral("priority")).toInt();
                r.mirror   = ro.value(QStringLiteral("x_mirror")).toBool(false);
                r.comment  = ro.value(QStringLiteral("x_comment")).toString();
                if (r.isValid()) repos[it.key()] = r;
            }
            const QJsonObject modsObj = root.value(QStringLiteral("installed_modules")).toObject();
            for (auto it = modsObj.constBegin(); it != modsObj.constEnd(); ++it) {
                InstalledModule im = InstalledModule::fromJsonObject(it.value().toObject());
                if (im.isValid() && im.identifier == it.key())
                    mods[it.key()] = im;
            }
            const QJsonObject filesObj = root.value(QStringLiteral("installed_files")).toObject();
            for (auto it = filesObj.constBegin(); it != filesObj.constEnd(); ++it)
                fileOwners[normalizeRelPath(it.key())] = it.value().toString();
            const QJsonObject dllsObj = root.value(QStringLiteral("installed_dlls")).toObject();
            for (auto it = dllsObj.constBegin(); it != dllsObj.constEnd(); ++it)
                dlls[it.key()] = it.value().toString();

            QMutexLocker locker(m_lock.get());
            repositories = repos;
            installedModules = mods;
            installedFiles = fileOwners;
            installedDlls = dlls;
            registryVersion = rv;
            return true;
        } else if (error) {
            *error = QStringLiteral("registry_version %1 not supported").arg(rv);
        }
    } else if (err.error != QJsonParseError::NoError) {
        if (error) *error = QStringLiteral("invalid registry.json: %1").arg(err.errorString());
    }

    // 解析失败/版本不支持：清空为默认空注册表（与旧行为一致）
    QMutexLocker locker(m_lock.get());
    repositories.clear();
    installedModules.clear();
    installedFiles.clear();
    installedDlls.clear();
    registryVersion = LATEST_REGISTRY_VERSION;
    return false;
}

void Registry::clear()
{
    QMutexLocker locker(m_lock.get());
    repositories.clear();
    installedModules.clear();
    installedFiles.clear();
    installedDlls.clear();
    registryVersion = LATEST_REGISTRY_VERSION;
}

QByteArray Registry::toJson() const
{
    QMutexLocker locker(m_lock.get());
    QJsonObject root;
    root.insert(QStringLiteral("registry_version"), registryVersion);

    QJsonObject repos;
    for (auto it = repositories.constBegin(); it != repositories.constEnd(); ++it) {
        QJsonObject ro;
        ro.insert(QStringLiteral("name"), it.value().name);
        ro.insert(QStringLiteral("uri"), it.value().uri);
        ro.insert(QStringLiteral("priority"), it.value().priority);
        repos.insert(it.key(), ro);
    }
    root.insert(QStringLiteral("sorted_repositories"), repos);

    QJsonObject dlls;
    for (auto it = installedDlls.constBegin(); it != installedDlls.constEnd(); ++it)
        dlls.insert(it.key(), it.value());
    root.insert(QStringLiteral("installed_dlls"), dlls);

    QJsonObject mods;
    for (auto it = installedModules.constBegin(); it != installedModules.constEnd(); ++it)
        mods.insert(it.key(), it.value().toJsonObject());
    root.insert(QStringLiteral("installed_modules"), mods);

    QJsonObject files;
    for (auto it = installedFiles.constBegin(); it != installedFiles.constEnd(); ++it)
        files.insert(it.key(), it.value());
    root.insert(QStringLiteral("installed_files"), files);

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

void Registry::setRepositories(const QMap<QString, Repository> &repos)
{
    QMutexLocker locker(m_lock.get());
    repositories = repos;
}

InstalledModule *Registry::installed(const QString &identifier)
{
    QMutexLocker locker(m_lock.get());
    const auto it = installedModules.find(identifier);
    return it == installedModules.end() ? nullptr : &it.value();
}

const InstalledModule *Registry::installed(const QString &identifier) const
{
    QMutexLocker locker(m_lock.get());
    const auto it = installedModules.constFind(identifier);
    return it == installedModules.constEnd() ? nullptr : &it.value();
}

QString Registry::installedVersion(const QString &identifier) const
{
    QMutexLocker locker(m_lock.get());
    const auto it = installedModules.constFind(identifier);
    return it == installedModules.constEnd()
        ? QString() : it.value().module.version;
}

bool Registry::isInstalled(const QString &identifier) const
{
    QMutexLocker locker(m_lock.get());
    return installedModules.contains(identifier);
}

QString Registry::fileOwner(const QString &relativePath) const
{
    QMutexLocker locker(m_lock.get());
    return installedFiles.value(normalizeRelPath(relativePath));
}

void Registry::registerModule(const InstalledModule &im)
{
    QMutexLocker locker(m_lock.get());
    // 先移除旧文件归属
    if (installedModules.contains(im.identifier)) {
        const InstalledModule &old = installedModules[im.identifier];
        for (const QString &f : old.files)
            installedFiles.remove(normalizeRelPath(f));
    }
    installedModules[im.identifier] = im;
    for (const QString &f : im.files)
        installedFiles[normalizeRelPath(f)] = im.identifier;
}

void Registry::unregisterModule(const QString &identifier)
{
    QMutexLocker locker(m_lock.get());
    const auto it = installedModules.find(identifier);
    if (it == installedModules.end()) return;
    for (const QString &f : it->files)
        installedFiles.remove(normalizeRelPath(f));
    installedModules.erase(it);
}

} // namespace ckan