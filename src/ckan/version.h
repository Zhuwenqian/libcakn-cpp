#ifndef CKAN_VERSION_H
#define CKAN_VERSION_H

#include <QString>
#include <QStringList>
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

class CKAN_API GameVersionRange;

// KSP 游戏版本，major.minor.patch[.build]，与官方 CKAN 的 GameVersion 对应。
// 与官方一致：记录每个分量是否显式声明（缺省分量数值为 0，但 isXxxDefined 为 false）。
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

    bool isMajorDefined() const { return m_majorDefined; }
    bool isMinorDefined() const { return m_minorDefined; }
    bool isPatchDefined() const { return m_patchDefined; }
    bool isBuildDefined() const { return m_buildDefined; }

    GameVersion withoutBuild() const { return GameVersion(m_major, m_minor, m_patch, 0); }
    QString toString() const;

    // 将该版本展开为半开区间（官方 GameVersion.ToVersionRange 语义）。
    // 例如 "1.12.5" -> [1.12.5.0, 1.12.6.0)，"1.12.5.3190" -> [1.12.5.3190, 1.12.5.3190]。
    GameVersionRange toVersionRange() const;

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
    bool m_majorDefined = false, m_minorDefined = false, m_patchDefined = false, m_buildDefined = false;
    int  m_major = 0, m_minor = 0, m_patch = 0, m_build = 0;
};

// 游戏版本区间，边界可能开可能闭（未设置一侧为无界）。
// 对应官方 CKAN 的 GameVersionRange（CKAN-master/Core/Versioning/GameVersionRange.cs）。
// 构造时把 lower/upper 通过 toVersionRange() 展开为半开区间后取边界。
class CKAN_API GameVersionRange
{
public:
    GameVersionRange();
    GameVersionRange(const GameVersion &lower, bool lowerInclusive,
                     const GameVersion &upper, bool upperInclusive);
    // 区间判定：value 是否落在区间内（官方 Contains：与 value.toVersionRange() 求交）
    bool contains(const GameVersion &value) const;
    // 与另一区间是否相交（官方 Intersects：不相交返回 false）
    bool intersects(const GameVersionRange &other) const;

    bool lowerSet() const { return m_lowerSet; }
    bool upperSet() const { return m_upperSet; }
    bool lowerInclusive() const { return m_lowerInclusive; }
    bool upperInclusive() const { return m_upperInclusive; }
    GameVersion lower() const { return m_lower; }
    GameVersion upper() const { return m_upper; }

private:
    // 允许 GameVersion::toVersionRange 直接构造展开后的边界
    friend GameVersionRange GameVersion::toVersionRange() const;
    // 直接设置边界（不经过半开展开），供 GameVersion::toVersionRange 内部使用
    void setLower(const GameVersion &v, bool inclusive);
    void setUpper(const GameVersion &v, bool inclusive);

    bool        m_lowerSet = false, m_upperSet = false;
    bool        m_lowerInclusive = false, m_upperInclusive = false;
    GameVersion m_lower;
    GameVersion m_upper;
};

// 由勾选的版本线集合（如 {"1.9","1.10","1.11","1.12"}）构造连续兼容区间：
// 取最小版本线的展开下界 ~ 最大版本线的展开上界（如 [1.9.0.0, 1.13.0.0)）。
// 空集合或全部无效时返回无效区间（调用方应回退为仅按当前实例版本判断）。
CKAN_API GameVersionRange versionLinesToRange(const QStringList &versionLines);

} // namespace ckan

#endif // CKAN_VERSION_H