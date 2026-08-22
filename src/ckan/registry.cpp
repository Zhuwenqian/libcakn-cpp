#include "registry.h"

#include <QJsonDocument>
#include <QJsonArray>

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
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("invalid registry.json: %1").arg(err.errorString());
        return reg;
    }
    const QJsonObject root = doc.object();
    reg.registryVersion = root.value(QStringLiteral("registry_version")).toInt(reg.registryVersion);
    if (reg.registryVersion > LATEST_REGISTRY_VERSION) {
        if (error) *error = QStringLiteral("registry_version %1 not supported").arg(reg.registryVersion);
        return reg;
    }

    // sorted_repositories
    const QJsonObject repos = root.value(QStringLiteral("sorted_repositories")).toObject();
    for (auto it = repos.constBegin(); it != repos.constEnd(); ++it) {
        const QJsonObject ro = it.value().toObject();
        Repository r;
        r.name = it.key();
        r.uri  = ro.value(QStringLiteral("uri")).toString();
        r.priority = ro.value(QStringLiteral("priority")).toInt();
        r.mirror   = ro.value(QStringLiteral("x_mirror")).toBool(false);
        r.comment  = ro.value(QStringLiteral("x_comment")).toString();
        if (r.isValid()) reg.repositories[it.key()] = r;
    }

    // installed_modules
    const QJsonObject mods = root.value(QStringLiteral("installed_modules")).toObject();
    for (auto it = mods.constBegin(); it != mods.constEnd(); ++it) {
        InstalledModule im = InstalledModule::fromJsonObject(it.value().toObject());
        if (im.isValid() && im.identifier == it.key())
            reg.installedModules[it.key()] = im;
    }

    // installed_files
    const QJsonObject files = root.value(QStringLiteral("installed_files")).toObject();
    for (auto it = files.constBegin(); it != files.constEnd(); ++it)
        reg.installedFiles[normalizeRelPath(it.key())] = it.value().toString();

    // installed_dlls
    const QJsonObject dlls = root.value(QStringLiteral("installed_dlls")).toObject();
    for (auto it = dlls.constBegin(); it != dlls.constEnd(); ++it)
        reg.installedDlls[it.key()] = it.value().toString();

    return reg;
}

QByteArray Registry::toJson() const
{
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
    repositories = repos;
}

InstalledModule *Registry::installed(const QString &identifier)
{
    auto it = installedModules.find(identifier);
    return it == installedModules.end() ? nullptr : &it.value();
}

const InstalledModule *Registry::installed(const QString &identifier) const
{
    auto it = installedModules.constFind(identifier);
    return it == installedModules.constEnd() ? nullptr : &it.value();
}

QString Registry::installedVersion(const QString &identifier) const
{
    const InstalledModule *im = installed(identifier);
    return im ? im->module.version : QString();
}

bool Registry::isInstalled(const QString &identifier) const
{
    return installedModules.contains(identifier);
}

QString Registry::fileOwner(const QString &relativePath) const
{
    return installedFiles.value(normalizeRelPath(relativePath));
}

void Registry::registerModule(const InstalledModule &im)
{
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
    auto it = installedModules.find(identifier);
    if (it == installedModules.end()) return;
    for (const QString &f : it->files)
        installedFiles.remove(normalizeRelPath(f));
    installedModules.erase(it);
}

} // namespace ckan