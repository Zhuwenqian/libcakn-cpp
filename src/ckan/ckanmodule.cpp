#include "ckanmodule.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>

namespace ckan {

namespace {
QVector<Relationship> parseRelationships(const QJsonObject &obj, const QString &key, Relationship::Type type)
{
    QVector<Relationship> out;
    const QJsonValue v = obj.value(key);
    if (!v.isArray()) return out;
    for (const QJsonValue &item : v.toArray()) {
        if (item.isString()) {
            Relationship r;
            r.type = type;
            r.name = item.toString();
            out.append(r);
        } else if (item.isObject()) {
            const QJsonObject rel = item.toObject();
            Relationship r;
            r.type = type;
            r.name = rel.value(QStringLiteral("name")).toString();
            const QString ver = rel.value(QStringLiteral("version")).toString();
            r.version = ver;
            // 解析版本约束，如 ">=1.2", "1.2.3", "<=3.0"
            if (!ver.isEmpty()) {
                if (ver.startsWith(QStringLiteral(">="))) {
                    r.minVersion = ver.mid(2); r.minInclusive = true;
                } else if (ver.startsWith(QStringLiteral("<="))) {
                    r.maxVersion = ver.mid(2); r.maxInclusive = true;
                } else if (ver.startsWith(QLatin1Char('>'))) {
                    r.minVersion = ver.mid(1); r.minInclusive = false;
                } else if (ver.startsWith(QLatin1Char('<'))) {
                    r.maxVersion = ver.mid(1); r.maxInclusive = false;
                } else if (ver.startsWith(QLatin1Char('='))) {
                    r.minVersion = ver.mid(1); r.minInclusive = true;
                    r.maxVersion = ver.mid(1); r.maxInclusive = true;
                } else {
                    r.minVersion = ver; r.minInclusive = true;
                    r.maxVersion = ver; r.maxInclusive = true;
                }
            }
            out.append(r);
        }
    }
    return out;
}
} // namespace

CkanModule CkanModule::fromJsonObject(const QJsonObject &obj, QString *error)
{
    CkanModule m;
    m.identifier = obj.value(QStringLiteral("identifier")).toString();
    if (m.identifier.isEmpty()) {
        if (error) *error = QStringLiteral("missing required field: identifier");
        return m;
    }
    m.name         = obj.value(QStringLiteral("name")).toString().isEmpty()
                       ? m.identifier
                       : obj.value(QStringLiteral("name")).toString();
    m.version      = obj.value(QStringLiteral("version")).toString();
    if (m.version.isEmpty()) {
        if (error) *error = QStringLiteral("missing required field: version");
        return m;
    }
    m.abstract     = obj.value(QStringLiteral("abstract")).toString();
    m.description  = obj.value(QStringLiteral("description")).toString();
    m.comment      = obj.value(QStringLiteral("comment")).toString();
    m.specVersion  = obj.value(QStringLiteral("spec_version")).toString(QStringLiteral("1"));
    m.releaseDate  = obj.value(QStringLiteral("release_date")).toString();
    m.downloadSize = obj.value(QStringLiteral("download_size")).toVariant().toLongLong();
    m.installSize  = obj.value(QStringLiteral("install_size")).toVariant().toLongLong();
    m.downloadContentType = obj.value(QStringLiteral("download_content_type")).toString();

    const QString kindStr = obj.value(QStringLiteral("kind")).toString();
    if (kindStr == QStringLiteral("metapackage")) m.kind = ModuleKind::Metapackage;
    else if (kindStr == QStringLiteral("dlc")) m.kind = ModuleKind::Dlc;

    const auto readStrList = [&obj](const QString &key) {
        QStringList out;
        const QJsonValue v = obj.value(key);
        if (v.isArray()) for (const QJsonValue &e : v.toArray()) out << e.toString();
        else if (v.isString()) out << v.toString();
        return out;
    };
    m.author = readStrList(QStringLiteral("author"));
    m.license = readStrList(QStringLiteral("license"));
    { QSet<QString> tagSet; const QStringList tagList = readStrList(QStringLiteral("tags")); for (const QString &t : tagList) tagSet.insert(t); m.tags = tagSet.values(); }
    m.localizations = readStrList(QStringLiteral("localizations"));

    // download 可能是字符串或数组
    const QJsonValue dl = obj.value(QStringLiteral("download"));
    if (dl.isArray()) for (const QJsonValue &e : dl.toArray()) m.downloadUrls << e.toString();
    else if (dl.isString() && !dl.toString().isEmpty()) m.downloadUrls << dl.toString();

    // download_hash
    const QJsonObject hash = obj.value(QStringLiteral("download_hash")).toObject();
    m.downloadHash.sha1   = hash.value(QStringLiteral("sha1")).toString();
    m.downloadHash.sha256 = hash.value(QStringLiteral("sha256")).toString();

    // resources
    const QJsonObject res = obj.value(QStringLiteral("resources")).toObject();
    m.resources.homepage   = res.value(QStringLiteral("homepage")).toString();
    m.resources.repository = res.value(QStringLiteral("repository")).toString();
    m.resources.bugtracker = res.value(QStringLiteral("bugtracker")).toString();
    m.resources.license    = res.value(QStringLiteral("license")).toString();
    m.resources.manual     = res.value(QStringLiteral("manual")).toString();
    m.resources.spacedock  = res.value(QStringLiteral("spacedock")).toString();
    m.resources.curseforge = res.value(QStringLiteral("curseforge")).toString();
    m.resources.github     = res.value(QStringLiteral("github")).toString();

    m.kspVersion       = obj.value(QStringLiteral("ksp_version")).toString();
    m.kspVersionMin    = obj.value(QStringLiteral("ksp_version_min")).toString();
    m.kspVersionMax    = obj.value(QStringLiteral("ksp_version_max")).toString();
    m.kspVersionStrict = obj.value(QStringLiteral("ksp_version_strict")).toBool(false);

    m.depends    = parseRelationships(obj, QStringLiteral("depends"), Relationship::Type::Depends);
    m.recommends = parseRelationships(obj, QStringLiteral("recommends"), Relationship::Type::Recommends);
    m.suggests   = parseRelationships(obj, QStringLiteral("suggests"), Relationship::Type::Suggests);
    m.supports   = parseRelationships(obj, QStringLiteral("supports"), Relationship::Type::Supports);
    m.conflicts  = parseRelationships(obj, QStringLiteral("conflicts"), Relationship::Type::Conflicts);
    m.provides   = parseRelationships(obj, QStringLiteral("provides"), Relationship::Type::Provides);

    // install
    const QJsonValue inst = obj.value(QStringLiteral("install"));
    if (inst.isArray()) {
        for (const QJsonValue &stanza : inst.toArray()) {
            ModuleInstallDescriptor d;
            QString err;
            if (ModuleInstallDescriptor::fromJsonObject(stanza.toObject(), &d, &err))
                m.install.append(d);
        }
    }
    return m;
}

CkanModule CkanModule::fromJson(const QByteArray &json, QString *error)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("invalid JSON: %1").arg(err.errorString());
        return CkanModule();
    }
    return fromJsonObject(doc.object(), error);
}

bool CkanModule::isCompatible(const GameVersion &ksp) const
{
    // 官方 StrictGameComparator 语义（CKAN-master/Core/Types/GameComparator/StrictGameComparator.cs）：
    // 1) 无任何 ksp_version 信息 -> 兼容一切
    // 2) 安装了 ksp_version（非 strict） -> 视为下界 [ksp_version, +inf)
    // 3) ksp_version + strict -> 等值区间 [ksp_version, ksp_version]
    // 4) ksp_version_min / ksp_version_max -> 相应单侧区间
    // 5) 两者都声明 -> [min, max]
    // 游戏版本检测失败（ksp 无效）时按兼容处理。
    if (!ksp.isValid())
        return true;

    if (kspVersion.isEmpty() && kspVersionMin.isEmpty() && kspVersionMax.isEmpty())
        return true;

    const GameVersion kspVer(kspVersion);
    const GameVersion minVer(kspVersionMin);
    const GameVersion maxVer(kspVersionMax);

    GameVersion lower;   // 无效值代表无界
    GameVersion upper;
    if (!kspVersion.isEmpty()) {
        if (kspVersionStrict) {
            lower = kspVer;
            upper = kspVer;
        } else {
            lower = kspVer; // 非 strict：作为最低兼容版本
        }
    }
    if (!kspVersionMin.isEmpty())
        lower = minVer; // 显式 min 优先于 ksp_version 推导出的下界
    if (!kspVersionMax.isEmpty())
        upper = maxVer; // 显式 max 优先于 ksp_version 推导出的上界

    // 区间有效性：min > max 视为不兼容
    if (lower.isValid() && upper.isValid() && lower > upper)
        return false;

    const GameVersionRange range(lower, true, upper, true);
    return range.contains(ksp);
}

QVector<ModuleInstallDescriptor> CkanModule::effectiveInstallStanzas() const
{
    if (!install.isEmpty()) return install;
    QVector<ModuleInstallDescriptor> v;
    v.append(ModuleInstallDescriptor::defaultStanza(identifier));
    return v;
}

QStringList CkanModule::providesList() const
{
    QStringList out;
    out << identifier;
    for (const Relationship &r : provides)
        if (!r.name.isEmpty()) out << r.name;
    out.removeDuplicates();
    return out;
}

QJsonObject CkanModule::toJsonObject() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("spec_version"), specVersion.isEmpty() ? QStringLiteral("1") : specVersion);
    obj.insert(QStringLiteral("identifier"), identifier);
    obj.insert(QStringLiteral("name"), name);
    obj.insert(QStringLiteral("version"), version);
    if (!abstract.isEmpty()) obj.insert(QStringLiteral("abstract"), abstract);
    if (!description.isEmpty()) obj.insert(QStringLiteral("description"), description);
    if (!comment.isEmpty()) obj.insert(QStringLiteral("comment"), comment);
    if (!releaseDate.isEmpty()) obj.insert(QStringLiteral("release_date"), releaseDate);
    if (downloadSize) obj.insert(QStringLiteral("download_size"), static_cast<double>(downloadSize));
    if (installSize) obj.insert(QStringLiteral("install_size"), static_cast<double>(installSize));
    if (!downloadContentType.isEmpty()) obj.insert(QStringLiteral("download_content_type"), downloadContentType);
    if (kind == ModuleKind::Metapackage) obj.insert(QStringLiteral("kind"), QStringLiteral("metapackage"));
    else if (kind == ModuleKind::Dlc) obj.insert(QStringLiteral("kind"), QStringLiteral("dlc"));

    const auto writeList = [&obj](const QString &key, const QStringList &list) {
        if (!list.isEmpty()) obj.insert(key, QJsonArray::fromStringList(list));
    };
    writeList(QStringLiteral("author"), author);
    writeList(QStringLiteral("license"), license);
    writeList(QStringLiteral("tags"), tags);
    writeList(QStringLiteral("localizations"), localizations);

    if (!downloadUrls.isEmpty()) {
        if (downloadUrls.size() == 1)
            obj.insert(QStringLiteral("download"), downloadUrls.first());
        else
            obj.insert(QStringLiteral("download"), QJsonArray::fromStringList(downloadUrls));
    }
    if (!downloadHash.sha1.isEmpty() || !downloadHash.sha256.isEmpty()) {
        QJsonObject h;
        if (!downloadHash.sha1.isEmpty()) h.insert(QStringLiteral("sha1"), downloadHash.sha1);
        if (!downloadHash.sha256.isEmpty()) h.insert(QStringLiteral("sha256"), downloadHash.sha256);
        obj.insert(QStringLiteral("download_hash"), h);
    }

    QJsonObject res;
    if (!resources.homepage.isEmpty()) res.insert(QStringLiteral("homepage"), resources.homepage);
    if (!resources.repository.isEmpty()) res.insert(QStringLiteral("repository"), resources.repository);
    if (!resources.bugtracker.isEmpty()) res.insert(QStringLiteral("bugtracker"), resources.bugtracker);
    if (!resources.license.isEmpty()) res.insert(QStringLiteral("license"), resources.license);
    if (!resources.manual.isEmpty()) res.insert(QStringLiteral("manual"), resources.manual);
    if (!resources.spacedock.isEmpty()) res.insert(QStringLiteral("spacedock"), resources.spacedock);
    if (!resources.curseforge.isEmpty()) res.insert(QStringLiteral("curseforge"), resources.curseforge);
    if (!resources.github.isEmpty()) res.insert(QStringLiteral("github"), resources.github);
    if (!res.isEmpty()) obj.insert(QStringLiteral("resources"), res);

    if (!kspVersion.isEmpty()) obj.insert(QStringLiteral("ksp_version"), kspVersion);
    if (!kspVersionMin.isEmpty()) obj.insert(QStringLiteral("ksp_version_min"), kspVersionMin);
    if (!kspVersionMax.isEmpty()) obj.insert(QStringLiteral("ksp_version_max"), kspVersionMax);
    if (kspVersionStrict) obj.insert(QStringLiteral("ksp_version_strict"), true);

    const auto writeRelationships = [&obj](const QString &key, const QVector<Relationship> &rels) {
        if (rels.isEmpty()) return;
        QJsonArray arr;
        for (const Relationship &r : rels) arr.append(r.toJsonObject());
        obj.insert(key, arr);
    };
    writeRelationships(QStringLiteral("depends"), depends);
    writeRelationships(QStringLiteral("recommends"), recommends);
    writeRelationships(QStringLiteral("suggests"), suggests);
    writeRelationships(QStringLiteral("supports"), supports);
    writeRelationships(QStringLiteral("conflicts"), conflicts);

    // provides 为标识符字符串列表（与 CKAN 原格式一致）
    if (!provides.isEmpty()) {
        QJsonArray arr;
        for (const Relationship &r : provides)
            if (!r.name.isEmpty()) arr.append(r.name);
        if (!arr.isEmpty()) obj.insert(QStringLiteral("provides"), arr);
    }

    if (!install.isEmpty()) {
        QJsonArray arr;
        for (const ModuleInstallDescriptor &d : install) arr.append(d.toJsonObject());
        obj.insert(QStringLiteral("install"), arr);
    }
    return obj;
}

QByteArray CkanModule::toJson() const
{
    return QJsonDocument(toJsonObject()).toJson(QJsonDocument::Compact);
}

} // namespace ckan