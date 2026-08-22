#ifndef CKAN_VERSION_H
#define CKAN_VERSION_H

#include <QString>
#include <QVector>
#include "ckan_export.h"

namespace ckan {

// 模块版本，格式 [epoch:]version，排序规则与官方 CKAN 一致。
// 参考 CKAN-master/Core/Versioning/ModuleVersion.cs
class CKAN_API ModuleVersion
{
public:
    explicit ModuleVersion(const QString &versionString);

    bool isValid() const { return m_valid; }
    int  epoch() const { return m_epoch; }
    QString versionPart() const { return m_version; }
    QString toString() const { return m_string; }

    int compareTo(const ModuleVersion &other) const;
    bool equals(const ModuleVersion &other) const;

    bool operator==(const ModuleVersion &o) const { return equals(o); }
    bool operator!=(const ModuleVersion &o) const { return !equals(o); }
    bool operator<(const ModuleVersion &o) const { return compareTo(o) < 0; }
    bool operator>(const ModuleVersion &o) const { return compareTo(o) > 0; }
    bool operator<=(const ModuleVersion &o) const { return compareTo(o) <= 0; }
    bool operator>=(const ModuleVersion &o) const { return compareTo(o) >= 0; }

private:
    bool    m_valid = false;
    int     m_epoch = 0;
    QString m_version;
    QString m_string;
};

// KSP 游戏版本，major.minor.patch[.build]，与官方 CKAN 的 GameVersion 对应。
class CKAN_API GameVersion
{
public:
    explicit GameVersion(const QString &versionString);
    GameVersion(int major, int minor, int patch = 0, int build = 0);
    GameVersion() : GameVersion(0, 0, 0, 0) { m_valid = false; }

    bool isValid() const { return m_valid; }
    int major() const { return m_major; }
    int minor() const { return m_minor; }
    int patch() const { return m_patch; }
    int build() const { return m_build; }

    GameVersion withoutBuild() const { return GameVersion(m_major, m_minor, m_patch, 0); }
    QString toString() const;

    // 比较时忽略 build 部分（与 CKAN GameVersion.CompareTo 一致，build 仅用于区分）
    int compareWithoutBuild(const GameVersion &other) const;
    int compareTo(const GameVersion &other) const;

    bool operator<(const GameVersion &o) const { return compareTo(o) < 0; }
    bool operator>(const GameVersion &o) const { return compareTo(o) > 0; }
    bool operator<=(const GameVersion &o) const { return compareTo(o) <= 0; }
    bool operator>=(const GameVersion &o) const { return compareTo(o) >= 0; }
    bool operator==(const GameVersion &o) const { return compareTo(o) == 0; }

private:
    bool m_valid = false;
    int  m_major = 0, m_minor = 0, m_patch = 0, m_build = 0;
};

// 游戏版本区间 [lower, upper]，可开可闭（未设置一侧为无界）。
// 对应官方 CKAN 的 GameVersionRange（CKAN-master/Core/Versioning/GameVersionRange.cs）。
class CKAN_API GameVersionRange
{
public:
    GameVersionRange();
    GameVersionRange(const GameVersion &lower, bool lowerInclusive,
                     const GameVersion &upper, bool upperInclusive);
    // 区间判定：value 是否落在 [lower, upper] 内（未设置一侧视为无界）
    bool contains(const GameVersion &value) const;
    // 与另一区间是否相交（官方 IntersectWith：不相交返回空）
    bool intersects(const GameVersionRange &other) const;

    bool lowerSet() const { return m_lowerSet; }
    bool upperSet() const { return m_upperSet; }
    GameVersion lower() const { return m_lower; }
    GameVersion upper() const { return m_upper; }

private:
    bool        m_lowerSet = false;
    bool        m_upperSet = false;
    GameVersion m_lower;
    GameVersion m_upper;
};

} // namespace ckan

#endif // CKAN_VERSION_H