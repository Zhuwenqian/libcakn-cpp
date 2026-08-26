#include "relationship.h"
#include "version.h"

#include <QJsonArray>
#include <QJsonObject>

namespace ckan {

QJsonObject Relationship::toJsonObject() const
{
    QJsonObject obj;
    // any_of：写为 {"any_of": [...]}，子关系各自序列化（官方 CKAN 格式）
    if (!anyOf.isEmpty()) {
        QJsonArray arr;
        for (const Relationship &s : anyOf) arr.append(s.toJsonObject());
        obj.insert(QStringLiteral("any_of"), arr);
        return obj;
    }
    obj.insert(QStringLiteral("name"), name);
    if (!version.isEmpty()) {
        // 原始 version 键保留（含 ">=1.2" 这类前缀写法）
        obj.insert(QStringLiteral("version"), version);
    } else if (!minVersion.isEmpty() || !maxVersion.isEmpty()) {
        // 独立键形式（官方 ModuleRelationshipDescriptor）：
        // min_version / max_version 单独写出，inclusive 非默认值（true）时才写出。
        if (!minVersion.isEmpty()) {
            obj.insert(QStringLiteral("min_version"), minVersion);
            if (!minInclusive)
                obj.insert(QStringLiteral("min_version_inclusive"), false);
        }
        if (!maxVersion.isEmpty()) {
            obj.insert(QStringLiteral("max_version"), maxVersion);
            if (!maxInclusive)
                obj.insert(QStringLiteral("max_version_inclusive"), false);
        }
    }
    return obj;
}

bool Relationship::isVirtual() const
{
    // 虚拟包无法从名称上直接判断，需由解析器结合 provides 判断。
    // 这里仅做占位，实际判断在 RelationshipResolver 中完成。
    return false;
}

bool Relationship::versionSatisfies(const QString &installedVersion) const
{
    if (version.isEmpty() && minVersion.isEmpty() && maxVersion.isEmpty())
        return true;

    const ModuleVersion inst(installedVersion);
    if (!inst.isValid()) return false;

    if (!minVersion.isEmpty()) {
        const ModuleVersion minVer(minVersion);
        if (minVer.isValid()) {
            const int cmp = inst.compareTo(minVer);
            if (minInclusive ? cmp < 0 : cmp <= 0) return false;
        }
    }
    if (!maxVersion.isEmpty()) {
        const ModuleVersion maxVer(maxVersion);
        if (maxVer.isValid()) {
            const int cmp = inst.compareTo(maxVer);
            if (maxInclusive ? cmp > 0 : cmp >= 0) return false;
        }
    }
    return true;
}

} // namespace ckan