#include "installedmodule.h"

#include <QJsonDocument>
#include <QJsonArray>

namespace ckan {

QJsonObject InstalledModule::toJsonObject() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("install_time"), installTime);
    obj.insert(QStringLiteral("source_module"), module.toJsonObject());
    obj.insert(QStringLiteral("auto_installed"), autoInstalled);
    QJsonObject filesObj;
    for (const QString &f : files)
        filesObj.insert(f, QJsonObject());
    obj.insert(QStringLiteral("installed_files"), filesObj);
    return obj;
}

InstalledModule InstalledModule::fromJsonObject(const QJsonObject &obj)
{
    InstalledModule im;
    im.installTime    = obj.value(QStringLiteral("install_time")).toString();
    im.autoInstalled  = obj.value(QStringLiteral("auto_installed")).toBool(false);

    const QJsonValue src = obj.value(QStringLiteral("source_module"));
    if (src.isObject()) {
        im.module = CkanModule::fromJsonObject(src.toObject());
        im.identifier = im.module.identifier;
    }

    const QJsonValue filesV = obj.value(QStringLiteral("installed_files"));
    if (filesV.isObject()) {
        const QJsonObject fo = filesV.toObject();
        for (auto it = fo.constBegin(); it != fo.constEnd(); ++it)
            im.files << it.key();
    }
    return im;
}

} // namespace ckan