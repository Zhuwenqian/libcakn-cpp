#ifndef CKAN_REGISTRY_H
#define CKAN_REGISTRY_H

#include <QString>
#include <QMap>
#include <QHash>
#include <QJsonObject>

#include "ckan_export.h"
#include "installedmodule.h"
#include "repository.h"
#include "version.h"

namespace ckan {

// 注册表，对应 CKAN-master/Core/Registry/Registry.cs。
// 读写 registry.json，格式与官方 CKAN 完全兼容。
class CKAN_API Registry
{
public:
    static const int LATEST_REGISTRY_VERSION = 3;

    Registry() = default;

    // 从 registry.json 内容加载
    static Registry fromJson(const QByteArray &json, QString *error = nullptr);
    // 序列化为 registry.json 内容
    QByteArray toJson() const;

    // ---- 仓库 ----
    QMap<QString, Repository> repositories;
    void setRepositories(const QMap<QString, Repository> &repos);

    // ---- 已安装模块 ----
    QMap<QString, InstalledModule> installedModules; // identifier -> InstalledModule

    // file relative path -> identifier（文件归属）
    QHash<QString, QString> installedFiles;

    // 手动安装的 dll：identifier -> relative path
    QMap<QString, QString> installedDlls;

    // ----
    int registryVersion = LATEST_REGISTRY_VERSION;

    bool isValid() const { return registryVersion <= LATEST_REGISTRY_VERSION; }

    // 便捷查询
    InstalledModule *installed(const QString &identifier);
    const InstalledModule *installed(const QString &identifier) const;
    QString installedVersion(const QString &identifier) const;
    bool isInstalled(const QString &identifier) const;
    QString fileOwner(const QString &relativePath) const;

    // 注册一个已安装模块（同时更新 installedFiles）
    void registerModule(const InstalledModule &im);
    // 卸载一个模块（删除其文件归属）
    void unregisterModule(const QString &identifier);
};

} // namespace ckan

#endif // CKAN_REGISTRY_H