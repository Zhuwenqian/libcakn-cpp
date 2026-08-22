#ifndef CKAN_MODULE_H
#define CKAN_MODULE_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>

#include "ckan_export.h"
#include "version.h"
#include "relationship.h"
#include "moduleinstalldescriptor.h"

namespace ckan {

// 模块类型
enum class ModuleKind {
    Package,      // 普通包
    Metapackage,  // 元包（无实际文件）
    Dlc,          // 官方 DLC
};

// 下载哈希
struct CKAN_API DownloadHash {
    QString sha1;
    QString sha256;
};

// 下载资源链接
struct CKAN_API ResourceLinks {
    QString homepage;
    QString repository;
    QString bugtracker;
    QString license;
    QString manual;
    QString spacedock;
    QString curseforge;
    QString github;
};

// CKAN 模块元数据。对应 CKAN-master/Core/Types/CkanModule.cs
class CKAN_API CkanModule
{
public:
    // 从 .ckan JSON 解析；失败时返回 false 并填充 error
    static CkanModule fromJson(const QByteArray &json, QString *error = nullptr);
    // 从已解析的 JSON 对象构造
    static CkanModule fromJsonObject(const QJsonObject &obj, QString *error = nullptr);

    bool isValid() const { return !identifier.isEmpty() && !version.isEmpty(); }

    QString toString() const { return name + " " + version; }
    bool isMetapackage() const { return kind == ModuleKind::Metapackage; }
    bool isDlc() const { return kind == ModuleKind::Dlc; }

    // 是否兼容某个 KSP 版本
    bool isCompatible(const GameVersion &kspVersion) const;

    // 默认安装规则（无 install 字段时）：find=identifier, install_to=GameData
    QVector<ModuleInstallDescriptor> effectiveInstallStanzas() const;

    // 此模块提供的所有虚拟包（含自身 identifier）
    QStringList providesList() const;

    // ---- 字段 ----
    QString   identifier;
    QString   name;
    QString   version;
    QString   abstract;
    QString   description;
    QString   comment;
    QString   specVersion;
    QString   downloadContentType;
    QString   releaseDate;
    long long downloadSize = 0;
    long long installSize  = 0;

    ModuleKind kind = ModuleKind::Package;
    int releaseStatus = 0; // 0=stable

    QStringList      author;
    QStringList      license;
    QStringList      tags;
    QStringList      localizations;
    QStringList      downloadUrls;   // download 数组（可能多个）
    DownloadHash     downloadHash;
    ResourceLinks    resources;

    QString          kspVersion;
    QString          kspVersionMin;
    QString          kspVersionMax;
    bool             kspVersionStrict = false;

    QVector<Relationship> depends;
    QVector<Relationship> recommends;
    QVector<Relationship> suggests;
    QVector<Relationship> supports;
    QVector<Relationship> conflicts;
    QVector<Relationship> provides;

    QVector<ModuleInstallDescriptor> install;

    // 序列化为 .ckan JSON（用于 registry/history 导出）
    QJsonObject toJsonObject() const;
    QByteArray  toJson() const;
};

} // namespace ckan

#endif // CKAN_MODULE_H