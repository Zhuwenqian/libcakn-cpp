#ifndef CKAN_REGISTRY_H
#define CKAN_REGISTRY_H

#include <QString>
#include <QMap>
#include <QHash>
#include <QJsonObject>
#include <QRecursiveMutex>
#include <memory>

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

    Registry() : m_lock(std::make_shared<QRecursiveMutex>()) {}

    // 从 registry.json 内容加载（返回按值构建的拷贝，供测试/临时使用；
    // 生产主实例请用 loadFromJson 原地装载以保持锁稳定）
    static Registry fromJson(const QByteArray &json, QString *error = nullptr);
    // 原地装载 registry.json 内容（清空旧数据后装入，跨线程安全）。
    // 解析失败返回 false 并清空为默认空注册表。
    bool loadFromJson(const QByteArray &json, QString *error = nullptr);
    // 清空为默认空注册表（跨线程安全）
    void clear();
    // 序列化为 registry.json 内容（跨线程安全）
    QByteArray toJson() const;

    // 返回供外部复合读/写循环使用的共享锁（递归）。跨线程访问统一经此锁互斥。
    QRecursiveMutex *mutex() const { return m_lock.get(); }

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

private:
    // 跨线程共享锁（递归）。Registry 需保持可拷贝（fromJson/测试按值使用），
    // 故用 shared_ptr：任何拷贝共享同一把锁，生产主实例的锁保持不变。
    mutable std::shared_ptr<QRecursiveMutex> m_lock;
};

} // namespace ckan

#endif // CKAN_REGISTRY_H