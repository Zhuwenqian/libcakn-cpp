#include "moduleinstalldescriptor.h"

#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QJsonArray>

namespace ckan {

ModuleInstallDescriptor ModuleInstallDescriptor::defaultStanza(const QString &identifier)
{
    ModuleInstallDescriptor d;
    d.find = identifier;
    d.installTo = QStringLiteral("GameData");
    return d;
}

QJsonObject ModuleInstallDescriptor::toJsonObject() const
{
    QJsonObject obj;
    if (!file.isEmpty())       obj.insert(QStringLiteral("file"), file);
    if (!find.isEmpty())       obj.insert(QStringLiteral("find"), find);
    if (!findRegexp.isEmpty()) obj.insert(QStringLiteral("find_regexp"), findRegexp);
    if (!installTo.isEmpty())  obj.insert(QStringLiteral("install_to"), installTo);
    if (!as.isEmpty())         obj.insert(QStringLiteral("as"), as);
    const auto writeList = [&obj](const QString &key, const QStringList &list) {
        if (!list.isEmpty()) obj.insert(key, QJsonArray::fromStringList(list));
    };
    writeList(QStringLiteral("filter"), filter);
    writeList(QStringLiteral("filter_regexp"), filterRegexp);
    writeList(QStringLiteral("include_only"), includeOnly);
    writeList(QStringLiteral("include_only_regexp"), includeOnlyRegexp);
    return obj;
}

bool ModuleInstallDescriptor::fromJsonObject(const QJsonObject &obj,
                                             ModuleInstallDescriptor *out,
                                             QString *error)
{
    ModuleInstallDescriptor d;
    d.file           = obj.value(QStringLiteral("file")).toString();
    d.find           = obj.value(QStringLiteral("find")).toString();
    d.findRegexp     = obj.value(QStringLiteral("find_regexp")).toString();
    d.findMatchesFiles = obj.value(QStringLiteral("find_matches_files")).toBool(false);
    d.installTo      = obj.value(QStringLiteral("install_to")).toString(QStringLiteral("GameData"));
    d.as             = obj.value(QStringLiteral("as")).toString();

    const auto readList = [&obj](const QString &key) {
        QStringList outList;
        const QJsonValue v = obj.value(key);
        if (v.isArray()) {
            for (const QJsonValue &e : v.toArray())
                outList << e.toString();
        } else if (v.isString()) {
            outList << v.toString();
        }
        return outList;
    };
    d.filter          = readList(QStringLiteral("filter"));
    d.filterRegexp    = readList(QStringLiteral("filter_regexp"));
    d.includeOnly     = readList(QStringLiteral("include_only"));
    d.includeOnlyRegexp = readList(QStringLiteral("include_only_regexp"));

    // 校验
    const int setCount = (d.file.isEmpty() ? 0 : 1)
                       + (d.find.isEmpty() ? 0 : 1)
                       + (d.findRegexp.isEmpty() ? 0 : 1);
    if (setCount == 0) {
        if (error) *error = QStringLiteral("install stanza must have file, find, or find_regexp");
        return false;
    }
    if (setCount > 1) {
        if (error) *error = QStringLiteral("install stanza has too many of file/find/find_regexp");
        return false;
    }
    if (!d.filter.isEmpty() && !d.includeOnly.isEmpty()) {
        if (error) *error = QStringLiteral("install stanza cannot have both filter and include_only");
        return false;
    }
    if (!d.filterRegexp.isEmpty() && !d.includeOnlyRegexp.isEmpty()) {
        if (error) *error = QStringLiteral("install stanza cannot have both filter_regexp and include_only_regexp");
        return false;
    }
    if (d.installTo.isEmpty()) {
        if (error) *error = QStringLiteral("install stanza missing install_to");
        return false;
    }
    *out = d;
    return true;
}

namespace {
QRegularExpression buildPattern(const ModuleInstallDescriptor &d)
{
    QString pattern;
    if (!d.file.isEmpty())
        pattern = QStringLiteral("^") + QRegularExpression::escape(d.file) + QStringLiteral("(/|$)");
    else if (!d.find.isEmpty())
        pattern = QStringLiteral("(?:^|/)") + QRegularExpression::escape(d.find) + QStringLiteral("(/|$)");
    else
        pattern = d.findRegexp;
    return QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption);
}
} // namespace

bool ModuleInstallDescriptor::isWanted(const QString &path, int *matchIndex) const
{
    QString normalized = path;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));

    const QRegularExpression pat = buildPattern(*this);
    const QRegularExpressionMatch match = pat.match(normalized);
    if (!match.hasMatch())
        return false;
    if (matchIndex && match.capturedStart() != -1 && *matchIndex >= 0 && match.capturedStart() != *matchIndex)
        return false;

    // 路径分段过滤
    const QStringList segments = normalized.toLower().split(QLatin1Char('/'));
    auto listContains = [&segments](const QStringList &list) {
        for (const QString &s : list)
            if (segments.contains(s.toLower()))
                return true;
        return false;
    };
    if (!filter.isEmpty() && listContains(filter))
        return false;
    if (!filterRegexp.isEmpty()) {
        for (const QString &re : filterRegexp)
            if (QRegularExpression(re).match(normalized).hasMatch())
                return false;
    }
    if (!includeOnly.isEmpty() && listContains(includeOnly))
        return true;
    if (!includeOnlyRegexp.isEmpty()) {
        for (const QString &re : includeOnlyRegexp)
            if (QRegularExpression(re).match(normalized).hasMatch())
                return true;
    }
    return includeOnly.isEmpty() && includeOnlyRegexp.isEmpty();
}

QString ModuleInstallDescriptor::resolveInstallBaseDir(const QString &primaryModDir) const
{
    QString to = installTo;
    to.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (to == primaryModDir || to.startsWith(primaryModDir + QLatin1Char('/'))) {
        QString sub = to.mid(primaryModDir.size());
        if (sub.startsWith(QLatin1Char('/'))) sub = sub.mid(1);
        if (sub.isEmpty()) return primaryModDir;
        return primaryModDir + QLatin1Char('/') + sub;
    }
    if (to == QStringLiteral("GameRoot"))
        return QString(); // 根目录
    // Ships/Scenarios/Missions 等
    return to;
}

QString ModuleInstallDescriptor::transformOutputName(const QString &outputName,
                                                     const QString &installDirInGame) const
{
    QString normalized = outputName;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // 去掉匹配前缀目录
    const QRegularExpression pat = buildPattern(*this);
    QString current = normalized;
    QString shortest = normalized;
    int slash = current.lastIndexOf(QLatin1Char('/'));
    while (slash >= 0) {
        current = current.left(slash);
        if (pat.match(current).hasMatch())
            shortest = current;
        else
            break;
        slash = current.lastIndexOf(QLatin1Char('/'));
    }
    const int dirPos = shortest.lastIndexOf(QLatin1Char('/'));
    QString leading = dirPos >= 0 ? shortest.left(dirPos) : QString();
    QString result = normalized;
    if (!leading.isEmpty())
        result = result.mid(leading.size() + 1);

    // 应用 as（重命名第一级目录）
    if (!as.isEmpty()) {
        int firstSlash = result.indexOf(QLatin1Char('/'));
        if (firstSlash >= 0)
            result = as + result.mid(firstSlash);
        else
            result = as;
    } else {
        // 若匹配到保留目录前缀，去掉
        static const QStringList reserved = {
            QStringLiteral("GameData"), QStringLiteral("Ships"), QStringLiteral("Missions")
        };
        for (const QString &r : reserved) {
            if (result.startsWith(r + QLatin1Char('/'), Qt::CaseInsensitive)) {
                result = result.mid(r.size() + 1);
                break;
            }
        }
    }

    if (installDirInGame.isEmpty())
        return result;
    return installDirInGame + QLatin1Char('/') + result;
}

QVector<InstallableFile> ModuleInstallDescriptor::findInstallableFiles(
    const QStringList &zipEntries, const QString &primaryModDir, QString *error) const
{
    QVector<InstallableFile> files;
    const QString baseDir = resolveInstallBaseDir(primaryModDir);

    // find 模式需要找到最短匹配索引
    int findMatchIndex = -1;
    if (!find.isEmpty()) {
        const QRegularExpression pat = buildPattern(*this);
        int shortest = -1;
        for (const QString &e : zipEntries) {
            QString n = e; n.replace(QLatin1Char('\\'), QLatin1Char('/'));
            const auto m = pat.match(n);
            if (m.hasMatch()) {
                const int idx = m.capturedStart();
                if (shortest == -1 || idx < shortest) shortest = idx;
            }
        }
        findMatchIndex = shortest;
    }

    for (const QString &rawEntry : zipEntries) {
        QString entry = rawEntry;
        entry.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (entry.endsWith(QLatin1Char('/')))
            continue; // 仅目录条目跳过
        if (!isWanted(entry, &findMatchIndex))
            continue;
        // 跳过内部 .ckan 文件
        if (entry.endsWith(QStringLiteral(".ckan"), Qt::CaseInsensitive))
            continue;
        InstallableFile f;
        f.sourceName    = rawEntry;
        f.destination   = transformOutputName(entry, baseDir);
        f.makeDir       = false;
        files.append(f);
    }

    if (files.isEmpty()) {
        if (error)
            *error = QStringLiteral("install stanza matched no files: file=\"%1\" find=\"%2\" find_regexp=\"%3\"")
                         .arg(file, find, findRegexp);
    }
    return files;
}

} // namespace ckan