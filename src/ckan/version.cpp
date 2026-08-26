#include "version.h"

#include <QRegularExpression>
#include <QChar>

namespace ckan {

// ---------------------------------------------------------------------------
// ModuleVersion
// ---------------------------------------------------------------------------

ModuleVersion::ModuleVersion(const QString &versionString)
    : m_string(versionString)
{
    // 格式: [epoch:]version，epoch 为可选的非负整数
    static const QRegularExpression epochRe(QStringLiteral("^([0-9]+):"));
    const QRegularExpressionMatch m = epochRe.match(versionString);
    if (m.hasMatch()) {
        bool ok = false;
        m_epoch  = m.captured(1).toInt(&ok);
        if (!ok) return;
        m_version = versionString.mid(m.capturedLength());
    } else {
        m_version = versionString;
    }
    m_valid = true;
}

namespace {
// 一个字符串/数字比较步骤的结果
struct StepResult {
    int     comparison = 0;   // 若为非零则代表已分出胜负
    QString firstRemainder;   // v1 剩余部分
    QString secondRemainder;  // v2 剩余部分
};

// 比较两个版本的字符串部分，找出数字起始点
StepResult stringCompare(const QString &v1, const QString &v2)
{
    StepResult r;
    // 找到第一个数字的位置
    int i1 = 0;
    for (; i1 < v1.size(); ++i1)
        if (v1.at(i1).isDigit()) break;
    int i2 = 0;
    for (; i2 < v2.size(); ++i2)
        if (v2.at(i2).isDigit()) break;

    QString str1 = v1.left(i1);
    QString str2 = v2.left(i2);
    r.firstRemainder  = v1.mid(i1);
    r.secondRemainder = v2.mid(i2);

    if (!str1.isEmpty() && !str2.isEmpty()) {
        const QChar first1 = str1.at(0);
        const QChar first2 = str2.at(0);
        if (first1 != QLatin1Char('.') && first2 == QLatin1Char('.')) {
            r.comparison = -1;
        } else if (first1 == QLatin1Char('.') && first2 != QLatin1Char('.')) {
            r.comparison = 1;
        } else if (first1 == QLatin1Char('.') && first2 == QLatin1Char('.')) {
            if (str1.size() == 1 && str2.size() > 1) r.comparison = 1;
            else if (str1.size() > 1 && str2.size() == 1) r.comparison = -1;
            else r.comparison = QString::compare(str1, str2, Qt::CaseSensitive);
        } else {
            r.comparison = QString::compare(str1, str2, Qt::CaseSensitive);
        }
    } else {
        r.comparison = QString::compare(str1, str2, Qt::CaseSensitive);
    }
    return r;
}

// 比较数字部分，找出非数字起始点
StepResult numberCompare(const QString &v1, const QString &v2)
{
    StepResult r;
    int len1 = 0;
    for (; len1 < v1.size(); ++len1)
        if (!v1.at(len1).isDigit()) break;
    int len2 = 0;
    for (; len2 < v2.size(); ++len2)
        if (!v2.at(len2).isDigit()) break;

    r.firstRemainder  = v1.mid(len1);
    r.secondRemainder = v2.mid(len2);

    bool ok1 = false, ok2 = false;
    const long long n1 = v1.left(len1).toLongLong(&ok1);
    const long long n2 = v2.left(len2).toLongLong(&ok2);
    if (!ok1) { (void)n1; }
    if (!ok2) { (void)n2; }
    const long long num1 = ok1 ? n1 : 0;
    const long long num2 = ok2 ? n2 : 0;
    r.comparison = (num1 < num2) ? -1 : (num1 > num2) ? 1 : 0;
    return r;
}
} // namespace

int ModuleVersion::compareTo(const ModuleVersion &other) const
{
    if (!m_valid || !other.m_valid) return 0;
    if (m_epoch != other.m_epoch)
        return m_epoch > other.m_epoch ? 1 : -1;

    QString firstRemainder  = m_version;
    QString secondRemainder = other.m_version;

    while (!firstRemainder.isEmpty() && !secondRemainder.isEmpty()) {
        // 先比较字符串部分
        StepResult step = stringCompare(firstRemainder, secondRemainder);
        if (step.comparison != 0) return step.comparison;
        firstRemainder  = step.firstRemainder;
        secondRemainder = step.secondRemainder;

        // 再比较数字部分
        step = numberCompare(firstRemainder, secondRemainder);
        if (step.comparison != 0) return step.comparison;
        firstRemainder  = step.firstRemainder;
        secondRemainder = step.secondRemainder;
    }

    if (firstRemainder.isEmpty() && secondRemainder.isEmpty()) return 0;
    return firstRemainder.isEmpty() ? -1 : 1;
}

bool ModuleVersion::equals(const ModuleVersion &other) const
{
    return m_valid && other.m_valid && m_epoch == other.m_epoch && m_version == other.m_version;
}

// ---------------------------------------------------------------------------
// GameVersionRange
// ---------------------------------------------------------------------------

GameVersionRange::GameVersionRange()
{
}

GameVersionRange::GameVersionRange(const GameVersion &lower, bool lowerInclusive,
                                   const GameVersion &upper, bool upperInclusive)
{
    // 官方 GameVersionRange(lower, upper) 语义：每个边界先经 ToVersionRange 半开展开，
    // 下界取展开区间的 Lower（包含），上界取展开区间的 Upper（开闭视原版本是否完整而定）。
    // 显式传入的开闭标记在此被展开语义取代（现有调用均为 true,true）。
    (void)lowerInclusive;
    (void)upperInclusive;
    if (lower.isValid()) {
        const GameVersionRange lr = lower.toVersionRange();
        setLower(lr.lower(), lr.lowerInclusive());
    }
    if (upper.isValid()) {
        const GameVersionRange ur = upper.toVersionRange();
        setUpper(ur.upper(), ur.upperInclusive());
    }
}

void GameVersionRange::setLower(const GameVersion &v, bool inclusive)
{
    if (!v.isValid()) return;
    m_lower = v;
    m_lowerSet = true;
    m_lowerInclusive = inclusive;
}

void GameVersionRange::setUpper(const GameVersion &v, bool inclusive)
{
    if (!v.isValid()) return;
    m_upper = v;
    m_upperSet = true;
    m_upperInclusive = inclusive;
}

namespace {
// 区间边界（含开闭与是否设置），对应官方 GameVersionBound。
struct VersionBound {
    GameVersion value;
    bool set = false;
    bool inclusive = false;
};

// 两个下界取较高者；相等时取包含侧（更宽松）。
VersionBound highestBound(const VersionBound &a, const VersionBound &b)
{
    if (!a.set) return b;
    if (!b.set) return a;
    if (a.value > b.value) return a;
    if (b.value > a.value) return b;
    return a.inclusive ? a : b;
}

// 两个上界取较低者；相等时取排除侧（更严格）。
VersionBound lowestBound(const VersionBound &a, const VersionBound &b)
{
    if (!a.set) return b;
    if (!b.set) return a;
    if (a.value < b.value) return a;
    if (b.value < a.value) return b;
    return a.inclusive ? b : a;
}

// 官方 IsEmpty：上界 < 下界，或相等但两侧不都包含。
bool isEmptyRange(const VersionBound &lo, const VersionBound &hi)
{
    if (!lo.set || !hi.set) return false;
    if (hi.value < lo.value) return true;
    if (hi.value == lo.value && (!lo.inclusive || !hi.inclusive)) return true;
    return false;
}
} // namespace

bool GameVersionRange::contains(const GameVersion &value) const
{
    // 官方 Contains：与 value.ToVersionRange() 求交；无效（Any）版本视为兼容
    if (!value.isValid()) return true;
    return intersects(value.toVersionRange());
}

bool GameVersionRange::intersects(const GameVersionRange &other) const
{
    const VersionBound loA{ m_lower, m_lowerSet, m_lowerInclusive };
    const VersionBound loB{ other.m_lower, other.m_lowerSet, other.m_lowerInclusive };
    const VersionBound hiA{ m_upper, m_upperSet, m_upperInclusive };
    const VersionBound hiB{ other.m_upper, other.m_upperSet, other.m_upperInclusive };
    const VersionBound lo = highestBound(loA, loB);
    const VersionBound hi = lowestBound(hiA, hiB);
    return !isEmptyRange(lo, hi);
}

// ---------------------------------------------------------------------------
// GameVersion
// ---------------------------------------------------------------------------

GameVersion::GameVersion(const QString &versionString)
{
    // 形如 "1.12.3" 或 "1.12.3.1234"；记录每个分量是否显式声明
    const QStringList parts = versionString.split(QLatin1Char('.'));
    if (parts.isEmpty() || parts.size() > 4) return;

    QVector<int> nums;
    for (const QString &p : parts) {
        bool ok = false;
        int v = p.toInt(&ok);
        if (!ok) return;
        nums.append(v);
    }
    m_major = nums.value(0, 0);
    m_minor = nums.value(1, 0);
    m_patch = nums.value(2, 0);
    m_build = nums.value(3, 0);
    m_majorDefined = parts.size() >= 1;
    m_minorDefined = parts.size() >= 2;
    m_patchDefined = parts.size() >= 3;
    m_buildDefined = parts.size() >= 4;
    m_valid = true;
}

GameVersion::GameVersion(int major, int minor, int patch, int build)
    : m_valid(!(major < 0 || minor < 0 || patch < 0 || build < 0)),
      m_majorDefined(m_valid), m_minorDefined(m_valid),
      m_patchDefined(m_valid), m_buildDefined(m_valid),
      m_major(major), m_minor(minor), m_patch(patch), m_build(build)
{
}

QString GameVersion::toString() const
{
    if (!m_valid) return QString();
    if (m_buildDefined)
        return QStringLiteral("%1.%2.%3.%4").arg(m_major).arg(m_minor).arg(m_patch).arg(m_build);
    if (m_patchDefined)
        return QStringLiteral("%1.%2.%3").arg(m_major).arg(m_minor).arg(m_patch);
    if (m_minorDefined)
        return QStringLiteral("%1.%2").arg(m_major).arg(m_minor);
    return QStringLiteral("%1").arg(m_major);
}

GameVersionRange GameVersion::toVersionRange() const
{
    // 官方 GameVersion.ToVersionRange 语义：
    //   完整版本（含 build）        -> 点区间 [v, v]
    //   仅到 patch（无 build）      -> [major.minor.patch.0, major.minor.(patch+1).0)
    //   仅到 minor（无 patch）      -> [major.minor.0.0, major.(minor+1).0.0)
    //   仅到 major（无 minor）      -> [major.0.0.0, (major+1).0.0.0)
    //   无效（Any）                -> 无界区间
    GameVersionRange r;
    if (!m_valid) return r;
    if (m_buildDefined) {
        r.setLower(*this, true);
        r.setUpper(*this, true);
    } else if (m_patchDefined) {
        r.setLower(GameVersion(m_major, m_minor, m_patch, 0), true);
        r.setUpper(GameVersion(m_major, m_minor, m_patch + 1, 0), false);
    } else if (m_minorDefined) {
        r.setLower(GameVersion(m_major, m_minor, 0, 0), true);
        r.setUpper(GameVersion(m_major, m_minor + 1, 0, 0), false);
    } else if (m_majorDefined) {
        r.setLower(GameVersion(m_major, 0, 0, 0), true);
        r.setUpper(GameVersion(m_major + 1, 0, 0, 0), false);
    }
    return r;
}

int GameVersion::compareWithoutBuild(const GameVersion &other) const
{
    if (m_major != other.m_major) return m_major - other.m_major;
    if (m_minor != other.m_minor) return m_minor - other.m_minor;
    return m_patch - other.m_patch;
}

int GameVersion::compareTo(const GameVersion &other) const
{
    const int c = compareWithoutBuild(other);
    if (c != 0) return c;
    return m_build - other.m_build;
}

GameVersionRange versionLinesToRange(const QStringList &versionLines)
{
    // 勾选的版本线集合（如 1.9 / 1.10 / 1.11 / 1.12）按「连续区间」语义处理：
    // 取最小版本线的展开下界 ~ 最大版本线的展开上界。
    GameVersion minV, maxV;
    for (const QString &line : versionLines) {
        const GameVersion v(line);
        if (!v.isValid()) continue;
        if (!minV.isValid() || v < minV) minV = v;
        if (!maxV.isValid() || v > maxV) maxV = v;
    }
    if (!minV.isValid() || !maxV.isValid()) return GameVersionRange();

    // GameVersionRange(lower, true, upper, true) 构造时对边界做半开展开：
    // 下界取 minV 展开区间的 Lower（含），上界取 maxV 展开区间的 Upper（不含）。
    // 例如 1.9 + 1.12 -> [1.9.0.0, 1.13.0.0)。
    return GameVersionRange(minV, true, maxV, true);
}

} // namespace ckan