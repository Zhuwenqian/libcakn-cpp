#ifndef CKAN_INSTALLEDMODULE_H
#define CKAN_INSTALLEDMODULE_H

#include <QString>
#include <QStringList>
#include <QJsonObject>

#include "ckan_export.h"
#include "ckanmodule.h"

namespace ckan {

// 已安装模块记录，对应 CKAN 的 InstalledModule。
// registry.json 中 installed_modules 的每个值。
class CKAN_API InstalledModule
{
public:
    QString    identifier;
    CkanModule module;
    QStringList files;          // 相对 GameRoot 的文件路径
    bool       autoInstalled = false;
    QString    installTime;    // ISO 8601 UTC

    bool isValid() const { return !identifier.isEmpty() && module.isValid(); }

    QJsonObject toJsonObject() const;
    static InstalledModule fromJsonObject(const QJsonObject &obj);
};

} // namespace ckan

#endif // CKAN_INSTALLEDMODULE_H