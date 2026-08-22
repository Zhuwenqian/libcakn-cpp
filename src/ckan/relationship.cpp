#include "relationship.h"
#include "version.h"

namespace ckan {

QJsonObject Relationship::toJsonObject() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("name"), name);
    // 重建版本约束字符串（尽量还原原始 version；否则由 min/max 推导）
    QString v = version;
    if (v.isEmpty()) {
        if (!minVersion.isEmpty() && minVersion == maxVersion && minInclusive && maxInclusive) {
            v = minVersion;
        } else if (!minVersion.isEmpty() || !maxVersion.isEmpty()) {
            const QString minPfx = minInclusive ? QStringLiteral(">=") : QStringLiteral(">");
            const QString maxPfx = maxInclusive ? QStringLiteral("<=") : QStringLiteral("<");
            if (!minVersion.isEmpty() && maxVersion.isEmpty())
                v = minPfx + minVersion;
            else if (minVersion.isEmpty() && !maxVersion.isEmpty())
                v = maxPfx + maxVersion;
            else
                v = minPfx + minVersion + QLatin1Char(' ') + maxPfx + maxVersion;
        }
    }
    if (!v.isEmpty())
        obj.insert(QStringLiteral("version"), v);
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