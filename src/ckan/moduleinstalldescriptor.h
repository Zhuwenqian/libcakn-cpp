#ifndef CKAN_MODULEINSTALLDESCRIPTOR_H
#define CKAN_MODULEINSTALLDESCRIPTOR_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <functional>

#include "ckan_export.h"

namespace ckan {

// 一个待安装的文件（zip 内条目 -> 目标相对路径）
struct CKAN_API InstallableFile {
    QString sourceName;     // zip 内原始路径
    QString destination;    // 转换后的目标相对路径（如 GameData/foo/bar.dll）
    bool    makeDir = false;
};

// 安装规则，对应 CKAN-master/Core/Types/ModuleInstallDescriptor.cs
class CKAN_API ModuleInstallDescriptor
{
public:
    static ModuleInstallDescriptor defaultStanza(const QString &identifier);

    // 从 JSON 解析
    static bool fromJsonObject(const QJsonObject &obj, ModuleInstallDescriptor *out, QString *error);

    // 判断 zip 内某路径是否被此规则选中
    bool isWanted(const QString &path, int *matchIndex) const;

    // 将 zip 内路径转换为目标相对路径（去掉匹配前缀、应用 as/保留规则）
    QString transformOutputName(const QString &outputName, const QString &installDirInGame) const;

    // 序列化为 JSON 对象（与 fromJsonObject 对应）
    QJsonObject toJsonObject() const;

    // 计算安装基准目录（install_to 解析后的 GameRoot 相对目录）
    QString resolveInstallBaseDir(const QString &primaryModDir) const;

    // 遍历 zip 内所有条目，筛选出本规则要安装的文件。
    // zipEntries: 所有 zip 内文件路径；返回安装文件列表。
    QVector<InstallableFile> findInstallableFiles(const QStringList &zipEntries,
                                                  const QString &primaryModDir,
                                                  QString *error) const;

    // ---- 字段 ----
    QString file;           // 精确路径
    QString find;           // 目录名
    QString findRegexp;     // 正则
    bool    findMatchesFiles = false;
    QString installTo;      // GameData / GameRoot / Ships 等
    QString as;             // 重命名第一级目录
    QStringList filter;
    QStringList filterRegexp;
    QStringList includeOnly;
    QStringList includeOnlyRegexp;

    bool isValid() const { return !file.isEmpty() || !find.isEmpty() || !findRegexp.isEmpty(); }
};

} // namespace ckan

#endif // CKAN_MODULEINSTALLDESCRIPTOR_H