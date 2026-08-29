#include "gameinstance.h"

#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>
#include <QRegularExpression>
#include <QHash>
#include <QVector>
#include <algorithm>

namespace ckan {

namespace {

// KSP build id → 版本 映射表。数据源自官方 CKAN 的 builds-ksp.json
// （KSP 已停止更新，映射表基本稳定，静态内置，无需联网刷新）。
const QHash<QString, QString> &kBuildMap()
{
    static const QHash<QString, QString> map = {
        { QStringLiteral("395"),  QStringLiteral("0.23.0.395") },
        { QStringLiteral("464"),  QStringLiteral("0.23.5.464") },
        { QStringLiteral("559"),  QStringLiteral("0.24.2.559") },
        { QStringLiteral("642"),  QStringLiteral("0.25.0.642") },
        { QStringLiteral("705"),  QStringLiteral("0.90.0.705") },
        { QStringLiteral("830"),  QStringLiteral("1.0.0.830") },
        { QStringLiteral("840"),  QStringLiteral("1.0.1.840") },
        { QStringLiteral("842"),  QStringLiteral("1.0.2.842") },
        { QStringLiteral("859"),  QStringLiteral("1.0.3.859") },
        { QStringLiteral("861"),  QStringLiteral("1.0.4.861") },
        { QStringLiteral("1024"), QStringLiteral("1.0.5.1024") },
        { QStringLiteral("1028"), QStringLiteral("1.0.5.1028") },
        { QStringLiteral("1172"), QStringLiteral("1.1.0.1172") },
        { QStringLiteral("1174"), QStringLiteral("1.1.0.1174") },
        { QStringLiteral("1180"), QStringLiteral("1.1.0.1180") },
        { QStringLiteral("1183"), QStringLiteral("1.1.0.1183") },
        { QStringLiteral("1196"), QStringLiteral("1.1.0.1196") },
        { QStringLiteral("1203"), QStringLiteral("1.1.0.1203") },
        { QStringLiteral("1209"), QStringLiteral("1.1.0.1209") },
        { QStringLiteral("1215"), QStringLiteral("1.1.0.1215") },
        { QStringLiteral("1224"), QStringLiteral("1.1.0.1224") },
        { QStringLiteral("1228"), QStringLiteral("1.1.0.1228") },
        { QStringLiteral("1230"), QStringLiteral("1.1.0.1230") },
        { QStringLiteral("1250"), QStringLiteral("1.1.1.1250") },
        { QStringLiteral("1260"), QStringLiteral("1.1.2.1260") },
        { QStringLiteral("1289"), QStringLiteral("1.1.3.1289") },
        { QStringLiteral("1473"), QStringLiteral("1.2.0.1473") },
        { QStringLiteral("1479"), QStringLiteral("1.2.0.1479") },
        { QStringLiteral("1485"), QStringLiteral("1.2.0.1485") },
        { QStringLiteral("1486"), QStringLiteral("1.2.0.1486") },
        { QStringLiteral("1489"), QStringLiteral("1.2.0.1489") },
        { QStringLiteral("1494"), QStringLiteral("1.2.0.1494") },
        { QStringLiteral("1499"), QStringLiteral("1.2.0.1499") },
        { QStringLiteral("1500"), QStringLiteral("1.2.0.1500") },
        { QStringLiteral("1509"), QStringLiteral("1.2.0.1509") },
        { QStringLiteral("1517"), QStringLiteral("1.2.0.1517") },
        { QStringLiteral("1520"), QStringLiteral("1.2.0.1520") },
        { QStringLiteral("1523"), QStringLiteral("1.2.0.1523") },
        { QStringLiteral("1532"), QStringLiteral("1.2.0.1532") },
        { QStringLiteral("1539"), QStringLiteral("1.2.0.1539") },
        { QStringLiteral("1540"), QStringLiteral("1.2.0.1540") },
        { QStringLiteral("1546"), QStringLiteral("1.2.0.1546") },
        { QStringLiteral("1548"), QStringLiteral("1.2.0.1548") },
        { QStringLiteral("1553"), QStringLiteral("1.2.0.1553") },
        { QStringLiteral("1563"), QStringLiteral("1.2.0.1563") },
        { QStringLiteral("1564"), QStringLiteral("1.2.0.1564") },
        { QStringLiteral("1569"), QStringLiteral("1.2.0.1569") },
        { QStringLiteral("1574"), QStringLiteral("1.2.0.1574") },
        { QStringLiteral("1576"), QStringLiteral("1.2.0.1576") },
        { QStringLiteral("1583"), QStringLiteral("1.2.0.1583") },
        { QStringLiteral("1584"), QStringLiteral("1.2.0.1584") },
        { QStringLiteral("1586"), QStringLiteral("1.2.0.1586") },
        { QStringLiteral("1604"), QStringLiteral("1.2.1.1604") },
        { QStringLiteral("1622"), QStringLiteral("1.2.2.1622") },
        { QStringLiteral("1727"), QStringLiteral("1.2.9.1727") },
        { QStringLiteral("1730"), QStringLiteral("1.2.9.1730") },
        { QStringLiteral("1737"), QStringLiteral("1.2.9.1737") },
        { QStringLiteral("1738"), QStringLiteral("1.2.9.1738") },
        { QStringLiteral("1743"), QStringLiteral("1.2.9.1743") },
        { QStringLiteral("1750"), QStringLiteral("1.2.9.1750") },
        { QStringLiteral("1758"), QStringLiteral("1.2.9.1758") },
        { QStringLiteral("1764"), QStringLiteral("1.2.9.1764") },
        { QStringLiteral("1773"), QStringLiteral("1.2.9.1773") },
        { QStringLiteral("1781"), QStringLiteral("1.2.9.1781") },
        { QStringLiteral("1790"), QStringLiteral("1.2.9.1790") },
        { QStringLiteral("1796"), QStringLiteral("1.2.9.1796") },
        { QStringLiteral("1800"), QStringLiteral("1.2.9.1800") },
        { QStringLiteral("1804"), QStringLiteral("1.3.0.1804") },
        { QStringLiteral("1836"), QStringLiteral("1.3.1.1836") },
        { QStringLiteral("1847"), QStringLiteral("1.3.1.1847") },
        { QStringLiteral("1855"), QStringLiteral("1.3.1.1855") },
        { QStringLiteral("1863"), QStringLiteral("1.3.1.1863") },
        { QStringLiteral("1891"), QStringLiteral("1.3.1.1891") },
        { QStringLiteral("2077"), QStringLiteral("1.4.0.2077") },
        { QStringLiteral("2089"), QStringLiteral("1.4.1.2089") },
        { QStringLiteral("2110"), QStringLiteral("1.4.2.2110") },
        { QStringLiteral("2152"), QStringLiteral("1.4.3.2152") },
        { QStringLiteral("2215"), QStringLiteral("1.4.4.2215") },
        { QStringLiteral("2243"), QStringLiteral("1.4.5.2243") },
        { QStringLiteral("2256"), QStringLiteral("1.4.5.2256") },
        { QStringLiteral("2332"), QStringLiteral("1.5.0.2332") },
        { QStringLiteral("2335"), QStringLiteral("1.5.1.2335") },
        { QStringLiteral("2395"), QStringLiteral("1.6.0.2395") },
        { QStringLiteral("2401"), QStringLiteral("1.6.1.2401") },
        { QStringLiteral("2483"), QStringLiteral("1.7.0.2483") },
        { QStringLiteral("2539"), QStringLiteral("1.7.1.2539") },
        { QStringLiteral("2555"), QStringLiteral("1.7.2.2555") },
        { QStringLiteral("2556"), QStringLiteral("1.7.2.2556") },
        { QStringLiteral("2594"), QStringLiteral("1.7.3.2594") },
        { QStringLiteral("2686"), QStringLiteral("1.8.0.2686") },
        { QStringLiteral("2694"), QStringLiteral("1.8.1.2694") },
        { QStringLiteral("2781"), QStringLiteral("1.9.0.2781") },
        { QStringLiteral("2788"), QStringLiteral("1.9.1.2788") },
        { QStringLiteral("2917"), QStringLiteral("1.10.0.2917") },
        { QStringLiteral("2939"), QStringLiteral("1.10.1.2939") },
        { QStringLiteral("3045"), QStringLiteral("1.11.0.3045") },
        { QStringLiteral("3066"), QStringLiteral("1.11.1.3066") },
        { QStringLiteral("3077"), QStringLiteral("1.11.2.3077") },
        { QStringLiteral("3140"), QStringLiteral("1.12.0.3140") },
        { QStringLiteral("3142"), QStringLiteral("1.12.1.3142") },
        { QStringLiteral("3167"), QStringLiteral("1.12.2.3167") },
        { QStringLiteral("3173"), QStringLiteral("1.12.3.3173") },
        { QStringLiteral("3187"), QStringLiteral("1.12.4.3187") },
        { QStringLiteral("3190"), QStringLiteral("1.12.5.3190") },
    };
    return map;
}

// 从 buildID 文件内容解析 build id（行匹配官方格式 "build id = 0*NNNN"，
// 忽略前导 0；也兼容整个文件就是纯数字），经映射表换算成 GameVersion。
// 命中返回 true 且 *out 有效，未命中返回 false。
bool buildIdToVersion(const QString &content, GameVersion *out)
{
    static const QRegularExpression lineRe(
        QStringLiteral("^build\\s+id\\s*=\\s*0*(\\d+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression plainRe(QStringLiteral("^0*(\\d+)\\s*$"));

    const QStringList lines = content.split(QRegularExpression(QStringLiteral("\\r?\\n")));
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) continue;
        QString buildId;
        QRegularExpressionMatch m = lineRe.match(line);
        if (m.hasMatch()) {
            buildId = m.captured(1);
        } else {
            m = plainRe.match(line);
            if (m.hasMatch()) buildId = m.captured(1);
        }
        if (buildId.isEmpty()) continue;
        const GameVersion gv(kBuildMap().value(buildId));
        if (gv.isValid()) {
            if (out) *out = gv;
            return true;
        }
    }
    return false;
}

} // namespace

static QString normalized(const QString &p)
{
    QString n = QDir::cleanPath(p);
    n.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return n;
}

GameInstance::GameInstance(const QString &gameDir, const QString &name)
    : m_gameDir(normalized(gameDir)), m_name(name)
{
    setupCkanDirectories();
}

void GameInstance::setupCkanDirectories()
{
    QDir().mkpath(ckanDir());
    QDir().mkpath(historyDir());
    if (!m_customDownloadDir.isEmpty())
        QDir().mkpath(m_customDownloadDir);
    // 兼容原 CKAN：默认 downloads 存于 CKAN/downloads（用户可覆盖为启动器 downloads）
    QDir().mkpath(downloadDir());
}

QString GameInstance::toRelativeGameDir(const QString &abs) const
{
    const QString a = normalized(abs);
    const QString g = normalized(m_gameDir);
    if (a.startsWith(g + QLatin1Char('/')))
        return a.mid(g.size() + 1);
    return a;
}

QString GameInstance::toAbsoluteGameDir(const QString &rel) const
{
    QString r = normalized(rel);
    while (r.startsWith(QLatin1Char('/'))) r = r.mid(1);
    // 防 Zip Slip：规范化（折叠 ..）后必须仍位于游戏目录内，
    // 否则视为非法路径（返回空，调用方应拒绝写入/删除）。
    const QString base = normalized(m_gameDir);
    const QString abs = normalized(m_gameDir + (r.isEmpty() ? QString() : QStringLiteral("/") + r));
    if (abs != base && !abs.startsWith(base + QLatin1Char('/')))
        return QString();
    return abs;
}

QMap<QString, QString> GameInstance::scanUnmanagedDlls() const
{
    QMap<QString, QString> dlls;
    const QString gameData = m_gameDir + QStringLiteral("/GameData");
    if (!QDir(gameData).exists()) return dlls;

    // KSP 官方目录（对应 KerbalSpaceProgram.cs 的 StockFolders，仅 GameData 内部分）
    static const QSet<QString> stockFolders = {
        QStringLiteral("GameData/Squad"),
        QStringLiteral("GameData/SquadExpansion"),
    };

    QDirIterator it(gameData, { QStringLiteral("*.dll"), QStringLiteral("*.DLL") },
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString abs = it.next();
        const QString rel = toRelativeGameDir(abs);
        bool stock = false;
        for (const QString &sf : stockFolders)
            if (rel.startsWith(sf + QLatin1Char('/'))) { stock = true; break; }
        if (stock) continue;
        // 标识符 = DLL 文件名第一个 '.' 之前的部分（与官方 DllPathToIdentifier 一致）
        const QString base = QFileInfo(abs).completeBaseName();
        const QString identifier = base.section(QLatin1Char('.'), 0, 0).trimmed();
        if (identifier.isEmpty()) continue;
        if (!dlls.contains(identifier))
            dlls.insert(identifier, rel);
    }
    return dlls;
}

QStringList GameInstance::manualGameDataFolders() const
{
    QStringList manual;
    const QString gameData = m_gameDir + QStringLiteral("/GameData");
    QDir d(gameData);
    if (!d.exists()) return manual;

    static const QSet<QString> officialFolders = {
        QStringLiteral("Squad"), QStringLiteral("SquadExpansion")
    };
    const Registry *reg = registry();
    const QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &folder : entries) {
        if (officialFolders.contains(folder)) continue;
        // 该文件夹是否属于某个已登记安装模组（其任一文件落在此文件夹下）
        const QString prefix = QStringLiteral("GameData/") + folder + QLatin1Char('/');
        bool owned = false;
        for (auto it = reg->installedFiles.constBegin();
             it != reg->installedFiles.constEnd(); ++it) {
            if (it.key().startsWith(prefix)) { owned = true; break; }
        }
        if (!owned) manual << QStringLiteral("GameData/") + folder;
    }
    return manual;
}

GameVersion GameInstance::detectVersion() const
{
    return detectVersionFromDir(m_gameDir);
}

GameVersion GameInstance::detectVersionFromDir(const QString &gameDir)
{
    // 1) buildID 文件：解析 "build id = NNNN"，经 build 映射表换算成版本。
    //    buildID64 / buildID 两个文件都读取，去重后取版本最大值（对齐官方 KspBuildIdVersionProvider）。
    QVector<GameVersion> found;
    const QStringList buildFiles = { QStringLiteral("buildID64.txt"), QStringLiteral("buildID.txt") };
    for (const QString &bf : buildFiles) {
        QFile f(gameDir + QLatin1Char('/') + bf);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString content = QString::fromUtf8(f.readAll());
            f.close();
            GameVersion v;
            if (buildIdToVersion(content, &v) && !found.contains(v))
                found.append(v);
        }
    }
    if (!found.isEmpty()) {
        std::sort(found.begin(), found.end());
        return found.last();
    }

    // 2) readme.txt 中的版本行（buildID 缺失/未命中映射时的兜底）
    QFile rf(gameDir + QStringLiteral("/readme.txt"));
    if (rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString content = QString::fromUtf8(rf.readAll());
        rf.close();
        static const QRegularExpression re(QStringLiteral("Version\\s*\\n?\\s*(\\d+\\.\\d+(\\.\\d+)?)"),
                                           QRegularExpression::CaseInsensitiveOption);
        const auto m = re.match(content);
        if (m.hasMatch())
            return GameVersion(m.captured(1));
    }
    return GameVersion();
}

QStringList GameInstance::detectInstallKindTags(const QString &gameDir, bool *corrupted)
{
    QStringList tags;
    if (corrupted) *corrupted = false;

    QDir gd(gameDir + QStringLiteral("/GameData"));
    if (!gd.exists()) {
        if (corrupted) *corrupted = true;
        return tags;
    }

    const QStringList dirs = gd.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    bool hasSquad = false;   // Squad 为游戏运行必需目录
    bool hasOther = false;   // 除官方目录与已知模组外的其它第三方目录
    bool rss = false, sol = false, ro = false, rp1 = false;
    for (const QString &d : dirs) {
        const QString low = d.toLower();   // 目录匹配不区分大小写
        if (low == QStringLiteral("squad"))           { hasSquad = true; continue; }
        if (low == QStringLiteral("squadexpansion"))  continue;            // 官方扩展目录
        if (low == QStringLiteral("realsolarsystem")) { rss = true; continue; }
        if (low == QStringLiteral("sol-configs"))     { sol = true; continue; }
        if (low == QStringLiteral("realismoverhaul")) { ro  = true; continue; }
        if (low == QStringLiteral("rp-1"))            { rp1 = true; continue; }
        hasOther = true;
    }

    // 缺少必需目录视为游戏安装损坏
    if (!hasSquad) {
        if (corrupted) *corrupted = true;
        return tags;
    }

    // 固定顺序：RSS → Sol → RO → RP-1
    if (rss) tags << QStringLiteral("RSS");
    if (sol) tags << QStringLiteral("Sol");
    if (ro)  tags << QStringLiteral("RO");
    if (rp1) tags << QStringLiteral("RP-1");

    // 仅含官方目录（Squad / SquadExpansion）、无任何已知模组或其它第三方目录 → 纯净
    if (tags.isEmpty() && !hasOther)
        tags << QStringLiteral("Clean Stock");

    return tags;
}

QString GameInstance::suggestedInstanceName(const QString &gameDir)
{
    QString name = QStringLiteral("KSP");
    const GameVersion v = detectVersionFromDir(gameDir);
    if (v.isValid())
        name += QLatin1Char(' ') + v.toString();
    const QStringList tags = detectInstallKindTags(gameDir);
    for (const QString &t : tags)
        name += QLatin1Char(' ') + t;
    return name;
}

Registry *GameInstance::registry()
{
    if (!m_registryLoaded)
        loadRegistry();
    return &m_registry;
}

bool GameInstance::acquireRegistryLock() const
{
    if (m_registryLocked) return true;
    if (m_registryLock.acquire(ckanDir() + QStringLiteral("/registry.locked"))) {
        m_registryLocked = true;
        return true;
    }
    return false; // 另一进程持有注册表锁
}

void GameInstance::loadRegistry()
{
    // 获取跨进程锁（最佳努力）：拿不到锁（另一进程占用）时仍只读加载，
    // 但后续 saveRegistry 会因无锁而拒绝写入，避免并发写坏 registry.json。
    acquireRegistryLock();

    QFile f(registryPath());
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        QString err;
        m_registry = Registry::fromJson(f.readAll(), &err);
        f.close();
    } else {
        // 注册表文件不存在（含被删除的整合包导入场景）：内存态必须重置为空，
        // 否则被清空的暂存数据仍滞留内存，导致后续安装与文件归属冲突判断失真。
        m_registry = Registry();
    }
    m_registryLoaded = true;
}

bool GameInstance::saveRegistry() const
{
    // 未持有锁则先尝试获取；仍失败说明另一进程正在操作该注册表，跳过写入以防损坏。
    if (!m_registryLocked && !acquireRegistryLock())
        return false;

    QDir().mkpath(ckanDir());
    QFile f(registryPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(m_registry.toJson());
        f.close();
        return true;
    }
    return false;
}

void GameInstance::restoreRegistrySnapshot(const QByteArray &json)
{
    QString err;
    if (json.isEmpty())
        m_registry = Registry();
    else
        m_registry = Registry::fromJson(json, &err);
    m_registryLoaded = true;
    saveRegistry(); // 同步写回磁盘，保证内存与 registry.json 一致（回滚场景拿不到锁时尽力而为）
}

bool GameInstance::isValid() const
{
    const bool hasExec = QFileInfo::exists(m_gameDir + QStringLiteral("/KSP_x64.exe"))
                      || QFileInfo::exists(m_gameDir + QStringLiteral("/KSP.exe"))
                      || QFileInfo::exists(m_gameDir + QStringLiteral("/KSP.x86_64"))
                      || QDir(m_gameDir).exists(QStringLiteral("GameData"));
    return hasExec;
}

} // namespace ckan