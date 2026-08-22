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
    // 无效的 GameVersion 表示该侧无界（对应官方 GameVersion.Any）。
    if (lower.isValid()) {
        m_lowerSet = true;
        m_lower = lower;
    }
    if (upper.isValid()) {
        m_upperSet = true;
        m_upper = upper;
    }
    // 兼容判定只使用等值/无界区间，两侧边界恒为包含
    (void)lowerInclusive;
    (void)upperInclusive;
}

bool GameVersionRange::contains(const GameVersion &value) const
{
    if (m_lowerSet && value < m_lower) return false;
    if (m_upperSet && value > m_upper) return false;
    return true;
}

bool GameVersionRange::intersects(const GameVersionRange &other) const
{
    // 交集下界 = 两者下界较大者；交集上界 = 两者上界较小者。
    // 当下界与上界都存在且 下界 > 上界 时无交集。
    const bool hasLower = lowerSet() || other.lowerSet();
    GameVersion lo;
    if (hasLower) {
        if (lowerSet() && other.lowerSet()) lo = lower() > other.lower() ? lower() : other.lower();
        else lo = lowerSet() ? lower() : other.lower();
    }
    const bool hasUpper = upperSet() || other.upperSet();
    GameVersion hi;
    if (hasUpper) {
        if (upperSet() && other.upperSet()) hi = upper() < other.upper() ? upper() : other.upper();
        else hi = upperSet() ? upper() : other.upper();
    }
    if (hasLower && hasUpper && lo > hi) return false;
    return true;
}

// ---------------------------------------------------------------------------
// GameVersion
// ---------------------------------------------------------------------------

GameVersion::GameVersion(const QString &versionString)
{
    // 形如 "1.12.3" 或 "1.12.3.1234"
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
    m_valid = true;
}

GameVersion::GameVersion(int major, int minor, int patch, int build)
    : m_valid(!(major < 0 || minor < 0 || patch < 0 || build < 0)),
      m_major(major), m_minor(minor), m_patch(patch), m_build(build)
{
}

QString GameVersion::toString() const
{
    if (!m_valid) return QString();
    if (m_build > 0)
        return QStringLiteral("%1.%2.%3.%4").arg(m_major).arg(m_minor).arg(m_patch).arg(m_build);
    if (m_patch > 0)
        return QStringLiteral("%1.%2.%3").arg(m_major).arg(m_minor).arg(m_patch);
    return QStringLiteral("%1.%2").arg(m_major).arg(m_minor);
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

} // namespace ckan