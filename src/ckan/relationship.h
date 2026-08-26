#ifndef CKAN_RELATIONSHIP_H
#define CKAN_RELATIONSHIP_H

#include <QString>
#include <QVector>
#include <QJsonObject>
#include "ckan_export.h"

namespace ckan {

// 依赖/推荐/冲突等关系描述符。
// 对应 CKAN 的 RelationshipDescriptor / ModuleRelationshipDescriptor。
// name 可以是模块 identifier 或虚拟包（provides 提供的名称）。
class CKAN_API Relationship
{
public:
    enum class Type {
        Depends,      // 硬性依赖
        Recommends,   // 推荐（一般自动安装）
        Suggests,     // 建议（可选）
        Supports,     // 支持
        Conflicts,    // 冲突
        Provides,     // 提供虚拟包
    };

    Type    type = Type::Depends;
    QString name;         // 目标模块 identifier 或虚拟包名
    QString version;      // 可选版本约束，如 ">=1.2", "1.2.3"，空表示任意
    QString minVersion;   // 解析出的最小版本（可选）
    QString maxVersion;   // 解析出的最大版本（可选）
    bool    minInclusive = false;
    bool    maxInclusive = false;
    QVector<Relationship> anyOf; // any_of 关系：任一子依赖满足即可（子关系继承 type）

    bool isVirtual() const;  // 由 ProvidesList 在解析时判断

    // 简单的约束是否满足某版本
    bool versionSatisfies(const QString &installedVersion) const;

    // 序列化为 JSON 对象（name + 可选 version 约束）
    QJsonObject toJsonObject() const;
};

} // namespace ckan

#endif // CKAN_RELATIONSHIP_H