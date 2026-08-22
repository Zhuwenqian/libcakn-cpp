#include <QtTest/QtTest>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QFileInfo>
#include <QUrl>
#include <QCryptographicHash>

#include "miniz.h"

#include "ckan/version.h"
#include "ckan/relationship.h"
#include "ckan/ckanmodule.h"
#include "ckan/moduleinstalldescriptor.h"
#include "ckan/moduleinstaller.h"
#include "ckan/installedmodule.h"
#include "ckan/registry.h"
#include "ckan/gameinstance.h"
#include "ckan/relationshipresolver.h"
#include "ckan/downloader.h"
#include "ckan/repoindex.h"
#include "ckan/txfilemanager.h"

using namespace ckan;

static Relationship rel(const QString &name, const QString &minVer = QString(),
                        const QString &maxVer = QString(), bool minIncl = true, bool maxIncl = true)
{
    Relationship r;
    r.name = name;
    r.minVersion = minVer;
    r.maxVersion = maxVer;
    r.minInclusive = minIncl;
    r.maxInclusive = maxIncl;
    return r;
}

static Relationship dep(const QString &name, const QString &minVer = QString())
{
    Relationship r = rel(name, minVer);
    r.type = Relationship::Type::Depends;
    return r;
}

static Relationship prov(const QString &name)
{
    Relationship r;
    r.type = Relationship::Type::Provides;
    r.name = name;
    return r;
}

static Relationship sug(const QString &name, const QString &minVer = QString())
{
    Relationship r = rel(name, minVer);
    r.type = Relationship::Type::Suggests;
    return r;
}

static CkanModule makeModule(const QString &id, const QString &version,
                             const QVector<Relationship> &depends = {},
                             const QVector<Relationship> &recommends = {},
                             const QVector<Relationship> &conflicts = {},
                             const QVector<Relationship> &provides = {},
                             const QVector<Relationship> &suggests = {})
{
    CkanModule m;
    m.identifier = id;
    m.name = id;
    m.version = version;
    m.depends = depends;
    m.recommends = recommends;
    m.conflicts = conflicts;
    m.provides = provides;
    m.suggests = suggests;
    return m;
}

static QMap<QString, QVector<CkanModule>> makeIndex(const QVector<CkanModule> &mods)
{
    QMap<QString, QVector<CkanModule>> idx;
    for (const CkanModule &m : mods)
        idx[m.identifier].append(m);
    return idx;
}

// 用 miniz 构造一个内存 zip（供安装测试使用）
static QByteArray makeZip(const QList<QPair<QString, QByteArray>> &files)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    mz_zip_writer_init_heap(&zip, 0, 0);
    for (const auto &f : files)
        mz_zip_writer_add_mem(&zip, f.first.toUtf8().constData(),
                              f.second.constData(), f.second.size(), 0);
    size_t size = 0;
    void *buf = nullptr;
    mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    QByteArray out(static_cast<const char *>(buf), static_cast<int>(size));
    mz_free(buf);
    return out;
}

// 构造单个 tar 512 字节文件头 + 数据块（供 repoindex 解析测试使用）
static QByteArray makeTarEntry(const QString &name, const QByteArray &data)
{
    QByteArray block(512, '\0');
    const QByteArray nameBytes = name.toUtf8();
    memcpy(block.data(), nameBytes.constData(), qMin(nameBytes.size(), 100));
    memcpy(block.data() + 100, "0000644\0", 8);      // mode
    memcpy(block.data() + 108, "0000000\0", 8);      // uid
    memcpy(block.data() + 116, "0000000\0", 8);      // gid
    memcpy(block.data() + 124,
           QByteArray::number(data.size(), 8).rightJustified(11, '0').constData(), 11); // size
    memcpy(block.data() + 136, "00000000000\0", 12); // mtime
    block[156] = '0';                                // typeflag: regular file
    memcpy(block.data() + 257, "ustar\0", 6);        // magic
    memcpy(block.data() + 263, "00", 2);             // version
    // checksum（先置空格再求和，再回填 6 位八进制 + NUL + 空格）
    memset(block.data() + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; ++i)
        sum += static_cast<unsigned char>(block.at(i));
    const QByteArray chk = QByteArray::number(sum, 8).rightJustified(6, '0') + '\0' + ' ';
    memcpy(block.data() + 148, chk.constData(), 8);

    QByteArray out = block;
    out.append(data);
    const int padded = ((data.size() + 511) / 512) * 512;
    out.append(QByteArray(padded - data.size(), '\0'));
    return out;
}

// 用 miniz raw deflate 构造 gzip（仓库归档测试使用；尾部 CRC/ISIZE 以 0 填充，解析器不校验）
static QByteArray makeTarGz(const QList<QPair<QString, QByteArray>> &files)
{
    QByteArray tar;
    for (const auto &f : files)
        tar.append(makeTarEntry(f.first, f.second));
    tar.append(QByteArray(1024, '\0')); // 两个全零块 = EOF

    size_t compLen = 0;
    void *comp = tdefl_compress_mem_to_heap(tar.constData(), static_cast<size_t>(tar.size()),
                                            &compLen, TDEFL_DEFAULT_MAX_PROBES);
    QByteArray gz;
    gz.append(char(0x1f)); gz.append(char(0x8b)); gz.append(char(0x08)); // magic + deflate
    gz.append(char(0));                                                  // flags
    gz.append(QByteArray(4, '\0'));                                      // mtime
    gz.append(char(0));                                                  // XFL
    gz.append(char(0xff));                                               // OS
    gz.append(QByteArray(static_cast<const char *>(comp), static_cast<int>(compLen)));
    mz_free(comp);
    gz.append(QByteArray(8, '\0')); // CRC32 + ISIZE（解析器不校验）
    return gz;
}

// ---------------------------------------------------------------------------
// 版本
// ---------------------------------------------------------------------------
class TestModuleVersion : public QObject
{
    Q_OBJECT
private slots:
    void basicOrdering()
    {
        QVERIFY(ModuleVersion(QStringLiteral("1.0")) < ModuleVersion(QStringLiteral("2.0")));
        QVERIFY(ModuleVersion(QStringLiteral("1.10")) > ModuleVersion(QStringLiteral("1.9")));
        QVERIFY(ModuleVersion(QStringLiteral("1.2.3")) == ModuleVersion(QStringLiteral("1.2.3")));
        QVERIFY(ModuleVersion(QStringLiteral("1.0.0")) > ModuleVersion(QStringLiteral("1.0")));
    }
    void epoch()
    {
        QVERIFY(ModuleVersion(QStringLiteral("1:1.0")) > ModuleVersion(QStringLiteral("0:5.0")));
        QCOMPARE(ModuleVersion(QStringLiteral("2:1.0")).epoch(), 2);
    }
    void validity()
    {
        QVERIFY(ModuleVersion(QStringLiteral("1.0")).isValid());
    }
};

class TestGameVersion : public QObject
{
    Q_OBJECT
private slots:
    void parse()
    {
        const GameVersion v(QStringLiteral("1.12.3"));
        QVERIFY(v.isValid());
        QCOMPARE(v.major(), 1);
        QCOMPARE(v.minor(), 12);
        QCOMPARE(v.patch(), 3);
        QCOMPARE(v.build(), 0);
        QCOMPARE(v.toString(), QStringLiteral("1.12.3"));
    }
    void parseBuild()
    {
        const GameVersion v(QStringLiteral("1.12.3.1234"));
        QVERIFY(v.isValid());
        QCOMPARE(v.build(), 1234);
    }
    void invalid()
    {
        QVERIFY(!GameVersion(QStringLiteral("abc")).isValid());
        QVERIFY(!GameVersion(QString()).isValid());
    }
    void ordering()
    {
        QVERIFY(GameVersion(QStringLiteral("1.12.3")) < GameVersion(QStringLiteral("1.12.4")));
        QVERIFY(GameVersion(QStringLiteral("1.12.3")) < GameVersion(QStringLiteral("1.13.0")));
        QVERIFY(GameVersion(QStringLiteral("1.12.3")) == GameVersion(QStringLiteral("1.12.3.0")));
    }
    void rangeContains()
    {
        const GameVersionRange range(GameVersion(QStringLiteral("1.10.0")), true,
                                     GameVersion(QStringLiteral("1.12.0")), true);
        QVERIFY(range.contains(GameVersion(QStringLiteral("1.10.0"))));
        QVERIFY(range.contains(GameVersion(QStringLiteral("1.11.5"))));
        QVERIFY(range.contains(GameVersion(QStringLiteral("1.12.0"))));
        QVERIFY(!range.contains(GameVersion(QStringLiteral("1.9.9"))));
        QVERIFY(!range.contains(GameVersion(QStringLiteral("1.12.1"))));
    }
    void rangeUnbounded()
    {
        // 仅下界：[lower, +inf)
        const GameVersionRange lowerOnly(GameVersion(QStringLiteral("1.10.0")), true,
                                         GameVersion(), true);
        QVERIFY(lowerOnly.contains(GameVersion(QStringLiteral("1.10.0"))));
        QVERIFY(lowerOnly.contains(GameVersion(QStringLiteral("2.0.0"))));
        QVERIFY(!lowerOnly.contains(GameVersion(QStringLiteral("1.9.9"))));
        // 仅上界：(-inf, upper]
        const GameVersionRange upperOnly(GameVersion(), true,
                                         GameVersion(QStringLiteral("1.12.0")), true);
        QVERIFY(upperOnly.contains(GameVersion(QStringLiteral("1.0.0"))));
        QVERIFY(upperOnly.contains(GameVersion(QStringLiteral("1.12.0"))));
        QVERIFY(!upperOnly.contains(GameVersion(QStringLiteral("1.12.1"))));
    }
    void rangeIntersects()
    {
        const GameVersionRange a(GameVersion(QStringLiteral("1.10.0")), true,
                                 GameVersion(QStringLiteral("1.12.0")), true);
        const GameVersionRange b(GameVersion(QStringLiteral("1.11.0")), true,
                                 GameVersion(QStringLiteral("1.13.0")), true);
        QVERIFY(a.intersects(b));
        const GameVersionRange c(GameVersion(QStringLiteral("1.13.0")), true,
                                 GameVersion(QStringLiteral("1.14.0")), true);
        QVERIFY(!a.intersects(c));
        // 无界区间与任何区间相交
        const GameVersionRange any;
        QVERIFY(any.intersects(a));
        QVERIFY(a.intersects(any));
    }
};

// ---------------------------------------------------------------------------
// 关系约束
// ---------------------------------------------------------------------------
class TestRelationship : public QObject
{
    Q_OBJECT
private slots:
    void unconstrained()
    {
        Relationship r;
        r.name = QStringLiteral("foo");
        QVERIFY(r.versionSatisfies(QStringLiteral("any.thing")));
    }
    void minInclusive()
    {
        Relationship r = rel(QStringLiteral("foo"), QStringLiteral("1.0"));
        QVERIFY(r.versionSatisfies(QStringLiteral("1.0")));
        QVERIFY(r.versionSatisfies(QStringLiteral("2.0")));
        QVERIFY(!r.versionSatisfies(QStringLiteral("0.9")));
    }
    void minExclusive()
    {
        Relationship r = rel(QStringLiteral("foo"), QStringLiteral("1.0"), {}, false, true);
        QVERIFY(!r.versionSatisfies(QStringLiteral("1.0")));
        QVERIFY(r.versionSatisfies(QStringLiteral("1.1")));
    }
    void maxInclusive()
    {
        Relationship r = rel(QStringLiteral("foo"), {}, QStringLiteral("2.0"), true, true);
        QVERIFY(r.versionSatisfies(QStringLiteral("2.0")));
        QVERIFY(!r.versionSatisfies(QStringLiteral("2.1")));
    }
};

// ---------------------------------------------------------------------------
// 模块元数据
// ---------------------------------------------------------------------------
class TestCkanModule : public QObject
{
    Q_OBJECT
private slots:
    void fromJson()
    {
        const QByteArray json =
            "{\"identifier\":\"ModA\",\"name\":\"Mod A\",\"version\":\"1.2.3\","
            "\"ksp_version\":\"1.12.3\",\"depends\":[\"DepMod\"],"
            "\"provides\":[\"virtual-a\"]}";
        QString err;
        const CkanModule m = CkanModule::fromJson(json, &err);
        QVERIFY(m.isValid());
        QCOMPARE(m.identifier, QStringLiteral("ModA"));
        QCOMPARE(m.version, QStringLiteral("1.2.3"));
        QCOMPARE(m.depends.size(), 1);
        QCOMPARE(m.depends.at(0).name, QStringLiteral("DepMod"));
        QVERIFY(m.providesList().contains(QStringLiteral("virtual-a")));
        QVERIFY(m.providesList().contains(QStringLiteral("ModA")));
    }
    void fromJsonMissingIdentifier()
    {
        QString err;
        const CkanModule m = CkanModule::fromJson("{\"version\":\"1.0\"}", &err);
        QVERIFY(!m.isValid());
        QVERIFY(!err.isEmpty());
    }
    void fromJsonMissingVersion()
    {
        QString err;
        const CkanModule m = CkanModule::fromJson("{\"identifier\":\"ModA\"}", &err);
        QVERIFY(!m.isValid());
        QVERIFY(!err.isEmpty());
    }
    void compatible()
    {
        // 规则（官方 StrictGameComparator）：
        //   ksp_version 且非 strict -> 下界 [ksp_version, +inf)
        //   ksp_version + strict -> 等值 [ksp_version, ksp_version]
        //   ksp_version_min + ksp_version_max -> [min, max]
        //   游戏版本检测失败（无效）视为兼容
        CkanModule m = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        const GameVersion ksp(GameVersion(QStringLiteral("1.12.3")));

        // 1) ksp_version 非 strict：兼容实际游戏版本及更高版本
        m.kspVersion = QStringLiteral("1.12.3");
        QVERIFY(m.isCompatible(ksp));
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("1.13.0"))));
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("2.0.0"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.11.0"))));

        // 2) ksp_version strict：等值匹配
        m.kspVersion = QStringLiteral("1.12.3");
        m.kspVersionStrict = true;
        QVERIFY(m.isCompatible(ksp));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.12.4"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("2.0.0"))));

        // 3) ksp_version_min + ksp_version_max：区间
        m.kspVersionStrict = false;
        m.kspVersion.clear();
        m.kspVersionMin = QStringLiteral("1.10.0");
        m.kspVersionMax = QStringLiteral("1.12.0");
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("1.10.0"))));
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("1.12.0"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.12.1"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.9.9"))));

        // 4) 仅 ksp_version_min：下界
        m.kspVersionMax.clear();
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("1.10.0"))));
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("2.0.0"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.9.9"))));

        // 5) 仅 ksp_version_max：上界
        m.kspVersionMin.clear();
        m.kspVersionMax = QStringLiteral("1.12.0");
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("1.0.0"))));
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("1.12.0"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.12.1"))));

        // 6) 未声明任何 ksp_version 信息 -> 兼容一切
        m.kspVersion.clear();
        m.kspVersionMin.clear();
        m.kspVersionMax.clear();
        QVERIFY(m.isCompatible(ksp));
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("1.0.0"))));
        QVERIFY(m.isCompatible(GameVersion()));

        // 7) 游戏版本检测失败（无效）视为兼容
        m.kspVersion = QStringLiteral("1.12.3");
        QVERIFY(m.isCompatible(GameVersion()));

        // 8) 非法 ksp_version 字符串 -> 无效版本 -> 视为兼容（因为无合法区间约束）
        m.kspVersion = QStringLiteral("not-a-version");
        QVERIFY(m.isCompatible(ksp));
    }
    void effectiveInstallDefault()
    {
        const CkanModule m = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        const QVector<ModuleInstallDescriptor> stanzas = m.effectiveInstallStanzas();
        QCOMPARE(stanzas.size(), 1);
        QCOMPARE(stanzas.at(0).find, QStringLiteral("ModA"));
        QCOMPARE(stanzas.at(0).installTo, QStringLiteral("GameData"));
    }
};

// ---------------------------------------------------------------------------
// 安装规则
// ---------------------------------------------------------------------------
class TestModuleInstallDescriptor : public QObject
{
    Q_OBJECT
private slots:
    void defaultStanza()
    {
        const ModuleInstallDescriptor d = ModuleInstallDescriptor::defaultStanza(QStringLiteral("ModA"));
        QCOMPARE(d.find, QStringLiteral("ModA"));
        QCOMPARE(d.installTo, QStringLiteral("GameData"));
    }
    void fromJsonValid()
    {
        ModuleInstallDescriptor d;
        QString err;
        QVERIFY(ModuleInstallDescriptor::fromJsonObject(
            QJsonObject{{QStringLiteral("find"), QStringLiteral("ModA")}},
            &d, &err));
        QCOMPARE(d.find, QStringLiteral("ModA"));
    }
    void fromJsonConflict()
    {
        ModuleInstallDescriptor d;
        QString err;
        QVERIFY(!ModuleInstallDescriptor::fromJsonObject(
            QJsonObject{{QStringLiteral("file"), QStringLiteral("a")},
                        {QStringLiteral("find"), QStringLiteral("b")}},
            &d, &err));
        QVERIFY(!err.isEmpty());
    }
    void findInstallable()
    {
        ModuleInstallDescriptor d;
        QString err;
        QVERIFY(ModuleInstallDescriptor::fromJsonObject(
            QJsonObject{{QStringLiteral("find"), QStringLiteral("ModA")}},
            &d, &err));
        const QStringList entries = {QStringLiteral("ModA/a.dll"),
                                     QStringLiteral("ModA/ModA.ckan"),
                                     QStringLiteral("Other/x.txt")};
        const QVector<InstallableFile> files = d.findInstallableFiles(entries,
                                                                      QStringLiteral("GameData"),
                                                                      &err);
        // 只匹配 ModA/a.dll，跳过 .ckan 与无关文件
        QCOMPARE(files.size(), 1);
        QCOMPARE(files.at(0).sourceName, QStringLiteral("ModA/a.dll"));
        QVERIFY(files.at(0).destination.startsWith(QStringLiteral("GameData/")));
    }
    void safeCacheFileName()
    {
        // version 带 epoch（如 "1:3.4.0"）：冒号在 Windows 上会导致 ADS 读写错位，
        // 清洗后应得到一个可安全用作文件名的小写安全串。
        QCOMPARE(ModuleInstaller::safeCacheFileName(QStringLiteral("1:3.4.0")),
                 QStringLiteral("1_3.4.0"));
        QCOMPARE(ModuleInstaller::safeCacheFileName(QStringLiteral("2:1.0")),
                 QStringLiteral("2_1.0"));
        // 普通版本号原样保留
        QCOMPARE(ModuleInstaller::safeCacheFileName(QStringLiteral("2.21.0.4")),
                 QStringLiteral("2.21.0.4"));
        // 其余非法字符一并清洗
        QCOMPARE(ModuleInstaller::safeCacheFileName(QStringLiteral("a:b\\c/d?e*f<g>h|i\"j")),
                 QStringLiteral("a_b_c_d_e_f_g_h_i_j"));
    }
    void actualGameDataFolders_data()
    {
        QTest::addColumn<QByteArray>("zip");
        QTest::addColumn<QJsonArray>("install");
        QTest::addColumn<QStringList>("expected");

        // 默认规则：find=identifier, install_to=GameData -> 顶层文件夹 = 标识符
        QTest::newRow("default") << makeZip({{QStringLiteral("SomeMod/a.dll"), QByteArray("dll")}})
                                 << QJsonArray{}
                                 << QStringList({QStringLiteral("SomeMod")});

        // as 重命名优先：zip 里是 Source/foo，目标是 GameData/Renamed/foo
        QTest::newRow("asRename") << makeZip({{QStringLiteral("Source/foo.txt"), QByteArray("x")}})
                                  << QJsonArray{QJsonObject{{QStringLiteral("find"), QStringLiteral("Source")},
                                                            {QStringLiteral("as"),     QStringLiteral("Renamed")}}}
                                  << QStringList({QStringLiteral("Renamed")});

        // 嵌套 install_to：GameData/Sub -> 顶层文件夹 = Sub
        QTest::newRow("nested") << makeZip({{QStringLiteral("Plugin/AB_Data/a.dll"), QByteArray("dll")}})
                                << QJsonArray{QJsonObject{{QStringLiteral("file"), QStringLiteral("Plugin/AB_Data")},
                                                          {QStringLiteral("install_to"), QStringLiteral("GameData/Sub")}}}
                                << QStringList({QStringLiteral("Sub")});

        // 多规则：同时写入两个不同顶层文件夹
        QTest::newRow("multiFolders") << makeZip({{QStringLiteral("A/a.dll"), QByteArray("dll")},
                                                  {QStringLiteral("B/b.dll"), QByteArray("dll")}})
                                      << QJsonArray{QJsonObject{{QStringLiteral("find"), QStringLiteral("A")}},
                                                    QJsonObject{{QStringLiteral("find"), QStringLiteral("B")}}}
                                      << QStringList({QStringLiteral("A"), QStringLiteral("B")});

        // 非 GameData 目标忽略
        QTest::newRow("nonGameData") << makeZip({{QStringLiteral("Ships/MyShip/ship.craft"), QByteArray("x")}})
                                     << QJsonArray{QJsonObject{{QStringLiteral("file"), QStringLiteral("Ships/MyShip")},
                                                               {QStringLiteral("install_to"), QStringLiteral("Ships")}}}
                                     << QStringList{};
    }
    void actualGameDataFolders()
    {
        QFETCH(QByteArray, zip);
        QFETCH(QJsonArray, install);
        QFETCH(QStringList, expected);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString zipPath = dir.filePath(QStringLiteral("mod.zip"));
        QFile zipFile(zipPath);
        QVERIFY(zipFile.open(QIODevice::WriteOnly));
        zipFile.write(zip);
        zipFile.close();

        CkanModule mod = makeModule(QStringLiteral("SomeMod"), QStringLiteral("1.0"));
        if (!install.isEmpty()) {
            QVector<ModuleInstallDescriptor> stanzas;
            for (const QJsonValue &v : install) {
                ModuleInstallDescriptor d;
                QString err;
                QVERIFY2(ModuleInstallDescriptor::fromJsonObject(v.toObject(), &d, &err),
                         qPrintable(err));
                stanzas.append(d);
            }
            mod.install = stanzas;
        }

        QString err;
        const QStringList folders = ModuleInstaller::actualGameDataFolders(zipPath, mod, &err);
        QCOMPARE(err, QString());
        QCOMPARE(folders, expected);
    }
};

// ---------------------------------------------------------------------------
// 注册表
// ---------------------------------------------------------------------------
class TestRegistry : public QObject
{
    Q_OBJECT
private slots:
    void roundtrip()
    {
        Registry reg;
        const CkanModule m = makeModule(QStringLiteral("ModA"), QStringLiteral("1.2.3"));
        InstalledModule im;
        im.identifier = QStringLiteral("ModA");
        im.module = m;
        im.files = {QStringLiteral("GameData/ModA/a.dll"),
                    QStringLiteral("GameData/ModA/b.dll")};
        reg.registerModule(im);

        const QByteArray json = reg.toJson();
        Registry reg2 = Registry::fromJson(json);
        QVERIFY(reg2.isValid());
        QVERIFY(reg2.isInstalled(QStringLiteral("ModA")));
        QCOMPARE(reg2.installedVersion(QStringLiteral("ModA")), QStringLiteral("1.2.3"));
        QCOMPARE(reg2.fileOwner(QStringLiteral("GameData/ModA/a.dll")),
                 QStringLiteral("ModA"));
    }
    void unregister()
    {
        Registry reg;
        const CkanModule m = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        InstalledModule im;
        im.identifier = QStringLiteral("ModA");
        im.module = m;
        im.files = {QStringLiteral("GameData/ModA/a.dll")};
        reg.registerModule(im);
        reg.unregisterModule(QStringLiteral("ModA"));
        QVERIFY(!reg.isInstalled(QStringLiteral("ModA")));
        QVERIFY(reg.fileOwner(QStringLiteral("GameData/ModA/a.dll")).isEmpty());
    }
    void invalidJson()
    {
        QString err;
        const Registry reg = Registry::fromJson("not json", &err);
        QVERIFY(!err.isEmpty());
    }
    void licenseRoundtrip()
    {
        // 注册表序列化必须保留 license 必填字段，供官方 CKAN 校验
        CkanModule m = makeModule(QStringLiteral("ModA"), QStringLiteral("1.2.3"));
        m.license = {QStringLiteral("MIT"), QStringLiteral("GPL-3.0")};
        m.author = {QStringLiteral("Alice")};
        m.kspVersion = QStringLiteral("1.12.3");
        InstalledModule im;
        im.identifier = QStringLiteral("ModA");
        im.module = m;
        im.files = {QStringLiteral("GameData/ModA/a.dll")};
        Registry reg;
        reg.registerModule(im);

        const QByteArray json = reg.toJson();
        QVERIFY(json.contains("\"license\""));
        QVERIFY(json.contains("\"MIT\""));

        const Registry reg2 = Registry::fromJson(json);
        QVERIFY(reg2.isValid());
        const InstalledModule *im2 = reg2.installed(QStringLiteral("ModA"));
        QVERIFY(im2);
        QCOMPARE(im2->module.license.size(), 2);
        QCOMPARE(im2->module.license[0], QStringLiteral("MIT"));
        QCOMPARE(im2->module.license[1], QStringLiteral("GPL-3.0"));
        QCOMPARE(im2->module.author.size(), 1);
        QCOMPARE(im2->module.author[0], QStringLiteral("Alice"));
        QCOMPARE(im2->module.kspVersion, QStringLiteral("1.12.3"));
    }
};

// ---------------------------------------------------------------------------
// 依赖解析
// ---------------------------------------------------------------------------
class TestRelationshipResolver : public QObject
{
    Q_OBJECT
private slots:
    void simpleDep()
    {
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("B"))});
        const CkanModule B = makeModule(QStringLiteral("B"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, B});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(!r.missing);
        QVERIFY(!r.conflicted);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("A")));
        QVERIFY(ids.contains(QStringLiteral("B")));
    }
    void virtualDep()
    {
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("virtual-lib"))});
        const CkanModule P = makeModule(QStringLiteral("P"), QStringLiteral("1.0"),
                                        {}, {}, {}, {prov(QStringLiteral("virtual-lib"))});
        const auto idx = makeIndex({A, P});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(!r.missing);
        QVERIFY(!r.conflicted);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("A")));
        QVERIFY(ids.contains(QStringLiteral("P")));
    }
    void missingDep()
    {
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("Nope"))});
        const auto idx = makeIndex({A});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(r.missing);
        QVERIFY(r.notFound.contains(QStringLiteral("Nope")));
    }
    void conflict()
    {
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"));
        const CkanModule B = makeModule(QStringLiteral("B"), QStringLiteral("1.0"),
                                        {}, {}, {dep(QStringLiteral("A"))});
        const auto idx = makeIndex({A, B});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A, B}, reg, false);
        QVERIFY(r.conflicted);
        QVERIFY(!r.conflicts.isEmpty());
    }
    void unionSharedDependency()
    {
        // 批量安装：两个待装模组共享同一依赖，应只安装一份依赖
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("S"))});
        const CkanModule B = makeModule(QStringLiteral("B"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("S"))});
        const CkanModule S = makeModule(QStringLiteral("S"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, B, S});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A, B}, reg, false);
        QVERIFY(!r.missing);
        QVERIFY(!r.conflicted);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("A")));
        QVERIFY(ids.contains(QStringLiteral("B")));
        QVERIFY(ids.contains(QStringLiteral("S")));
    }
    void recommendsAutoInstalled()
    {
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {}, {dep(QStringLiteral("R"))});
        const CkanModule R = makeModule(QStringLiteral("R"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, R});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, true);
        QVERIFY(!r.missing);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("R")));
    }
    void recommendsSkippedWithoutAuto()
    {
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {}, {dep(QStringLiteral("R"))});
        const CkanModule R = makeModule(QStringLiteral("R"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, R});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(!ids.contains(QStringLiteral("R")));
    }
    void versionConstraintPicksBest()
    {
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("B"), QStringLiteral("2.0"))});
        QMap<QString, QVector<CkanModule>> idx;
        idx[QStringLiteral("A")] = {A};
        idx[QStringLiteral("B")] = {makeModule(QStringLiteral("B"), QStringLiteral("1.0")),
                                    makeModule(QStringLiteral("B"), QStringLiteral("2.0")),
                                    makeModule(QStringLiteral("B"), QStringLiteral("3.0"))};
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(!r.missing);
        bool found300 = false;
        for (const CkanModule &m : r.modulesToInstall)
            if (m.identifier == QStringLiteral("B") && m.version == QStringLiteral("3.0"))
                found300 = true;
        QVERIFY(found300);
    }
    void adDependencySatisfied()
    {
        // A 依赖 FooLib，而 FooLib 是手动安装（DLL 扫描）的 AD 模组：
        // 应视为已满足，不下载 FooLib，也不报缺失。
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("FooLib"))});
        const CkanModule FooLib = makeModule(QStringLiteral("FooLib"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, FooLib});
        RelationshipResolver resolver(idx);
        Registry reg;
        reg.installedDlls[QStringLiteral("FooLib")] = QStringLiteral("GameData/FooLib/FooLib.dll");
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(!r.missing);
        QVERIFY(!r.conflicted);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("A")));
        QVERIFY(!ids.contains(QStringLiteral("FooLib"))); // 依赖已被 AD 满足，不下载
    }
    void nonAdDependencyStillDownloaded()
    {
        // 依赖不是 AD 模组时，仍应进入待安装集合
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("B"))});
        const CkanModule B = makeModule(QStringLiteral("B"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, B});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(!r.missing);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("B")));
    }
    void suggestsCollectedWhenEnabled()
    {
        // A 建议 S：withSuggests=true 时 S 出现在 suggestedModules，但不自动安装
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {}, {}, {}, {}, {sug(QStringLiteral("S"))});
        const CkanModule S = makeModule(QStringLiteral("S"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, S});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false, true);
        QVERIFY(!r.missing);
        QVERIFY(!r.conflicted);
        QSet<QString> inst;
        for (const CkanModule &m : r.modulesToInstall) inst.insert(m.identifier);
        QVERIFY(inst.contains(QStringLiteral("A")));
        QVERIFY(!inst.contains(QStringLiteral("S"))); // 建议不自动进入安装集
        QSet<QString> sugSet;
        for (const CkanModule &m : r.suggestedModules) sugSet.insert(m.identifier);
        QVERIFY(sugSet.contains(QStringLiteral("S")));
    }
    void suggestsIgnoredWhenDisabled()
    {
        // withSuggests=false 时不收集建议
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {}, {}, {}, {}, {sug(QStringLiteral("S"))});
        const CkanModule S = makeModule(QStringLiteral("S"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, S});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false, false);
        QVERIFY(r.suggestedModules.isEmpty());
    }
    void suggestsCascade()
    {
        // A 建议 B，B 建议 C：级联收集 B 和 C
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {}, {}, {}, {}, {sug(QStringLiteral("B"))});
        const CkanModule B = makeModule(QStringLiteral("B"), QStringLiteral("1.0"),
                                        {}, {}, {}, {}, {sug(QStringLiteral("C"))});
        const CkanModule C = makeModule(QStringLiteral("C"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, B, C});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false, true);
        QSet<QString> sugSet;
        for (const CkanModule &m : r.suggestedModules) sugSet.insert(m.identifier);
        QVERIFY(sugSet.contains(QStringLiteral("B")));
        QVERIFY(sugSet.contains(QStringLiteral("C")));
    }
    void suggestsExcludeInstalled()
    {
        // A 建议 S，但 S 已安装：不再出现在建议中
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {}, {}, {}, {}, {sug(QStringLiteral("S"))});
        const CkanModule S = makeModule(QStringLiteral("S"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, S});
        RelationshipResolver resolver(idx);
        Registry reg;
        InstalledModule im;
        im.identifier = QStringLiteral("S");
        im.module = S;
        reg.installedModules[QStringLiteral("S")] = im;
        const ResolutionResult r = resolver.resolve({A}, reg, false, true);
        QVERIFY(r.suggestedModules.isEmpty());
    }
    void suggestsCycleTerminates()
    {
        // A 建议 B，B 建议 A：级联去重，不成环不重复
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {}, {}, {}, {}, {sug(QStringLiteral("B"))});
        const CkanModule B = makeModule(QStringLiteral("B"), QStringLiteral("1.0"),
                                        {}, {}, {}, {}, {sug(QStringLiteral("A"))});
        const auto idx = makeIndex({A, B});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false, true);
        QSet<QString> sugSet;
        for (const CkanModule &m : r.suggestedModules) sugSet.insert(m.identifier);
        QVERIFY(sugSet.contains(QStringLiteral("B")));
        QVERIFY(!sugSet.contains(QStringLiteral("A"))); // A 在安装集，不作为建议
    }
    void suggestsSelectedThenResolvedWithDeps()
    {
        // 模拟"弹窗选中建议后重新解析"：选中 S 后，S 的依赖 D 也进入安装集
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {}, {}, {}, {}, {sug(QStringLiteral("S"))});
        const CkanModule S = makeModule(QStringLiteral("S"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("D"))});
        const CkanModule D = makeModule(QStringLiteral("D"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, S, D});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r1 = resolver.resolve({A}, reg, false, true);
        QSet<QString> sugSet;
        for (const CkanModule &m : r1.suggestedModules) sugSet.insert(m.identifier);
        QVERIFY(sugSet.contains(QStringLiteral("S")));

        // 用户勾选 S，重新解析
        QVector<CkanModule> selected;
        for (const CkanModule &m : r1.suggestedModules)
            if (m.identifier == QStringLiteral("S")) selected.append(m);
        QVector<CkanModule> combined = {A};
        combined += selected;
        const ResolutionResult r2 = resolver.resolve(combined, reg, false, false);
        QVERIFY(!r2.missing);
        QVERIFY(!r2.conflicted);
        QSet<QString> inst;
        for (const CkanModule &m : r2.modulesToInstall) inst.insert(m.identifier);
        QVERIFY(inst.contains(QStringLiteral("A")));
        QVERIFY(inst.contains(QStringLiteral("S")));
        QVERIFY(inst.contains(QStringLiteral("D"))); // S 的依赖被解析
    }
};

// ---------------------------------------------------------------------------
// GameData DLL 扫描
// ---------------------------------------------------------------------------
class TestGameInstance : public QObject
{
    Q_OBJECT
private slots:
    void scanIgnoresStockAndDeduplicates()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        auto mkfile = [&](const QString &rel, const QByteArray &content) {
            const QString abs = dir.filePath(rel);
            QDir().mkpath(QFileInfo(abs).absolutePath());
            QFile f(abs);
            QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(rel));
            f.write(content);
            f.close();
        };

        // KSP 官方目录（应排除）
        mkfile(QStringLiteral("GameData/Squad/StockDll.dll"), "x");
        mkfile(QStringLiteral("GameData/SquadExpansion/EasterEggs/egg.dll"), "x");

        // 手动模组 DLL
        mkfile(QStringLiteral("GameData/ModA/ModA.dll"), "x");
        mkfile(QStringLiteral("GameData/SomePath/MyMod.Core.dll"), "x"); // 标识符取文件名第一个点之前
        mkfile(QStringLiteral("GameData/MyMod/MyMod.Core-KSP.dll"), "x"); // 与上面应去重为一个 MyMod

        GameInstance gi(dir.path(), QStringLiteral("test"));
        const QMap<QString, QString> dlls = gi.scanUnmanagedDlls();

        QCOMPARE(dlls.size(), 2);
        QVERIFY(dlls.contains(QStringLiteral("ModA")));
        QCOMPARE(dlls.value(QStringLiteral("ModA")),
                 QStringLiteral("GameData/ModA/ModA.dll"));
        QVERIFY(dlls.contains(QStringLiteral("MyMod")));
        // 官方目录的 DLL 不应出现
        QVERIFY(!dlls.contains(QStringLiteral("StockDll")));
        QVERIFY(!dlls.contains(QStringLiteral("egg")));
    }

    void scanMissingGameDataReturnsEmpty()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        QVERIFY(gi.scanUnmanagedDlls().isEmpty());
    }

    void uninstallCascade()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));

        // 依赖链：C 依赖 B，B 依赖 A，外加独立的 D
        auto registerMod = [&](const CkanModule &m) {
            InstalledModule im;
            im.identifier = m.identifier;
            im.module = m;
            im.files = {QStringLiteral("GameData/%1/x.dll").arg(m.identifier)};
            gi.registry()->registerModule(im);
        };
        registerMod(makeModule(QStringLiteral("A"), QStringLiteral("1.0")));
        registerMod(makeModule(QStringLiteral("B"), QStringLiteral("1.0"), {dep(QStringLiteral("A"))}));
        registerMod(makeModule(QStringLiteral("C"), QStringLiteral("1.0"), {dep(QStringLiteral("B"))}));
        registerMod(makeModule(QStringLiteral("D"), QStringLiteral("1.0")));

        ModuleInstaller installer(&gi);
        const InstallResult r = installer.uninstall(QStringLiteral("A"));
        QVERIFY(r.ok);
        // 卸载顺序自外向内：先 C、B，最后 A
        QCOMPARE(r.installedIdentifiers,
                 QStringList({QStringLiteral("C"), QStringLiteral("B"), QStringLiteral("A")}));
        QVERIFY(!gi.registry()->isInstalled(QStringLiteral("A")));
        QVERIFY(!gi.registry()->isInstalled(QStringLiteral("B")));
        QVERIFY(!gi.registry()->isInstalled(QStringLiteral("C")));
        // 不依赖 A 的模组保留
        QVERIFY(gi.registry()->isInstalled(QStringLiteral("D")));
    }

    void uninstallNoDependentsOnlyTarget()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        InstalledModule im;
        im.identifier = QStringLiteral("D");
        im.module = makeModule(QStringLiteral("D"), QStringLiteral("1.0"));
        im.files = {QStringLiteral("GameData/D/x.dll")};
        gi.registry()->registerModule(im);

        ModuleInstaller installer(&gi);
        const InstallResult r = installer.uninstall(QStringLiteral("D"));
        QVERIFY(r.ok);
        QCOMPARE(r.installedIdentifiers, QStringList({QStringLiteral("D")}));
        QVERIFY(!gi.registry()->isInstalled(QStringLiteral("D")));
    }

    void manualGameDataFoldersDetected()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto mkfile = [&](const QString &rel, const QByteArray &content) {
            const QString abs = dir.filePath(rel);
            QDir().mkpath(QFileInfo(abs).absolutePath());
            QFile f(abs);
            QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(rel));
            f.write(content);
            f.close();
        };
        // 官方目录（排除）
        mkfile(QStringLiteral("GameData/Squad/x.dll"), "x");
        // 手动占用（无 DLL 的纯配置/纹理模组）
        mkfile(QStringLiteral("GameData/ManualCfg/Config.cfg"), "x");
        // 已登记安装模组的文件夹（应排除）
        mkfile(QStringLiteral("GameData/Tracked/x.dll"), "x");

        GameInstance gi(dir.path(), QStringLiteral("test"));
        InstalledModule im;
        im.identifier = QStringLiteral("Tracked");
        im.module = makeModule(QStringLiteral("Tracked"), QStringLiteral("1.0"));
        im.files = {QStringLiteral("GameData/Tracked/x.dll")};
        gi.registry()->registerModule(im);

        const QStringList manual = gi.manualGameDataFolders();
        QCOMPARE(manual, QStringList({QStringLiteral("GameData/ManualCfg")}));
    }

    void installDeletesOldFolder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));

        // 已有手动占用的 GameData/ModA 文件夹（含一个额外的、新模组不提供的文件）
        const QString oldExtra = dir.filePath(QStringLiteral("GameData/ModA/legacy.cfg"));
        QDir().mkpath(QFileInfo(oldExtra).absolutePath());
        QFile fe(oldExtra);
        QVERIFY(fe.open(QIODevice::WriteOnly));
        fe.write("legacy");
        fe.close();

        // 构造 zip：ModA/a.dll
        const QByteArray zip = makeZip({qMakePair(QStringLiteral("ModA/a.dll"), QByteArray("dll"))});
        const QString dl = dir.filePath(QStringLiteral("dl"));
        QDir().mkpath(dl);
        QFile zf(dl + QStringLiteral("/ModA_1.0.zip"));
        QVERIFY(zf.open(QIODevice::WriteOnly));
        zf.write(zip);
        zf.close();

        CkanModule mod = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        mod.downloadUrls = QStringList{QStringLiteral("file:///dummy/mod.zip")}; // 缓存已预置，此 URL 不会真正使用
        ModuleInstaller installer(&gi);
        // foldersToDelete = {"ModA"}：写入前应删除整个 GameData/ModA
        const InstallResult r = installer.install({mod}, dl, {QStringLiteral("ModA")});
        QVERIFY2(r.ok, qPrintable(r.error));
        // 旧的手动文件被删除
        QVERIFY(!QFileInfo::exists(oldExtra));
        // 新文件已安装
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("GameData/ModA/a.dll"))));
        // 已登记为安装
        QVERIFY(gi.registry()->isInstalled(QStringLiteral("ModA")));
    }

    void detectVersionFromBuildIDLine()
    {
        // 官方 buildID 格式："build id = 0*NNNN"，前导 0 忽略，经映射表换算成版本
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto mkfile = [&](const QString &name, const QByteArray &content) {
            QFile f(dir.filePath(name));
            QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(name));
            f.write(content);
            f.close();
        };
        mkfile(QStringLiteral("buildID64.txt"),
               "Kerbal Space Program\n\nbuild id = 03190\n\nsteam build id = 0xCA7\n");
        GameInstance gi(dir.path(), QStringLiteral("test"));
        const GameVersion v = gi.detectVersion();
        QVERIFY(v.isValid());
        QCOMPARE(v.toString(), QStringLiteral("1.12.5.3190"));
        QCOMPARE(v.major(), 1);
        QCOMPARE(v.minor(), 12);
        QCOMPARE(v.build(), 3190);
    }

    void detectVersionFromPlainNumericBuildID()
    {
        // 兼容整个文件就是纯数字的情况
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile f(dir.filePath(QStringLiteral("buildID.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("3190");
        f.close();
        GameInstance gi(dir.path(), QStringLiteral("test"));
        QCOMPARE(gi.detectVersion().toString(), QStringLiteral("1.12.5.3190"));
    }

    void detectVersionPicksMaxOfBothFiles()
    {
        // 同时存在 buildID64.txt / buildID.txt，取其中版本最大值（对齐官方 KspBuildIdVersionProvider）
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto mkfile = [&](const QString &name, const QByteArray &content) {
            QFile f(dir.filePath(name));
            QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(name));
            f.write(content);
            f.close();
        };
        mkfile(QStringLiteral("buildID64.txt"), "build id = 03173\n"); // 1.12.3.3173
        mkfile(QStringLiteral("buildID.txt"),   "build id = 03190\n"); // 1.12.5.3190
        GameInstance gi(dir.path(), QStringLiteral("test"));
        QCOMPARE(gi.detectVersion().toString(), QStringLiteral("1.12.5.3190"));
    }

    void detectVersionFallsBackToReadmeWhenBuildIDUnknown()
    {
        // buildID 存在但 id 未命中映射表（如 99999）→ 回退 readme.txt 中的版本行
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto mkfile = [&](const QString &name, const QByteArray &content) {
            QFile f(dir.filePath(name));
            QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(name));
            f.write(content);
            f.close();
        };
        mkfile(QStringLiteral("buildID.txt"), "build id = 099999\n");
        mkfile(QStringLiteral("readme.txt"),
               "Kerbal Space Program\n\nVersion 1.12.3\n\nThanks for playing!\n");
        GameInstance gi(dir.path(), QStringLiteral("test"));
        QCOMPARE(gi.detectVersion().toString(), QStringLiteral("1.12.3"));
    }

    void detectVersionNoInfoReturnsInvalid()
    {
        // 既无 buildID 文件也无 readme → 无效版本
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        QVERIFY(!gi.detectVersion().isValid());
    }
};

// ---------------------------------------------------------------------------
// 下载器：进度回调 / 超时 / 取消
// ---------------------------------------------------------------------------
class TestDownloader : public QObject
{
    Q_OBJECT
private slots:
    void fileDownloadProgress()
    {
        // 用本地文件验证 downloadProgressed 的成功路径与进度回调
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("payload.bin"));
        QByteArray payload;
        payload.resize(256 * 1024);
        for (int i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>(i & 0xff);
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(payload);
        f.close();

        Downloader dl;
        QByteArray out;
        QString err;
        qint64 lastTotal = -1;
        std::atomic_bool cancel{false};
        const bool ok = dl.downloadProgressed(
            QUrl::fromLocalFile(path).toString(), {}, &out, &err, nullptr,
            [&](qint64, qint64 total) { lastTotal = total; }, &cancel);
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(out, payload);
        QCOMPARE(lastTotal, static_cast<qint64>(payload.size()));
    }

    void cancelFlagAbortsDownload()
    {
        // 预置取消标志：函数应返回失败并报告「已取消」
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("payload.bin"));
        QByteArray payload(1024 * 1024, 'x');
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(payload);
        f.close();

        Downloader dl;
        QByteArray out;
        QString err;
        std::atomic_bool cancel{true};
        const bool ok = dl.downloadProgressed(
            QUrl::fromLocalFile(path).toString(), {}, &out, &err, nullptr, nullptr, &cancel);
        QVERIFY(!ok);
        QCOMPARE(err, QStringLiteral("已取消"));
    }

    void resumeAfterConnectionClosed()
    {
        // 本地 HTTP 服务器：首次请求只发一半就断开（模拟 connection closed），
        // 后续 Range 请求返回 206 剩余部分。验证断点续传能拼出完整内容。
        class RangeServer : public QTcpServer
        {
        public:
            QByteArray payload;
            int fullRequests = 0;
            int rangeRequests = 0;
            void startListening() { listen(QHostAddress::LocalHost, 0); }
            quint16 port() const { return serverPort(); }

        protected:
            void incomingConnection(qintptr socketDescriptor) override
            {
                QTcpSocket *s = new QTcpSocket(this);
                s->setSocketDescriptor(socketDescriptor);
                connect(s, &QTcpSocket::readyRead, this, [this, s]() {
                    const QByteArray req = s->readAll();
                    if (!req.contains("\r\n\r\n"))
                        return; // 头未收全，等待
                    const int rangePos = req.indexOf("Range: bytes=");
                    if (rangePos >= 0) {
                        ++rangeRequests;
                        const int start = rangePos + 13;
                        int end = req.indexOf('\r', start);
                        QByteArray rr = req.mid(start, end - start);
                        // Qt 的 toLongLong 无法解析带尾随 '-' 的值（如 "153600-"），需先截取数字部分
                        const int dashIdx = rr.indexOf('-');
                        if (dashIdx >= 0)
                            rr.truncate(dashIdx);
                        qint64 offset = rr.toLongLong();
                        const QByteArray body = payload.mid(static_cast<int>(offset));
                        QByteArray resp;
                        resp += "HTTP/1.1 206 Partial Content\r\n";
                        resp += "Content-Range: bytes "
                              + QByteArray::number(offset) + "-"
                              + QByteArray::number(payload.size() - 1) + "/"
                              + QByteArray::number(payload.size()) + "\r\n";
                        resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
                        resp += "Connection: close\r\n\r\n";
                        resp += body;
                        s->write(resp);
                        s->flush();
                        s->disconnectFromHost();
                    } else {
                        ++fullRequests;
                        const int half = payload.size() / 2;
                        QByteArray resp;
                        resp += "HTTP/1.1 200 OK\r\n";
                        resp += "Content-Length: " + QByteArray::number(payload.size()) + "\r\n";
                        resp += "Connection: close\r\n\r\n";
                        resp += payload.left(half);
                        s->write(resp);
                        s->flush();
                        s->disconnectFromHost(); // 发送一半后断开 → connection closed
                    }
                });
                connect(s, &QTcpSocket::disconnected, s, &QTcpSocket::deleteLater);
            }
        };

        RangeServer server;
        server.payload.resize(300 * 1024);
        for (int i = 0; i < server.payload.size(); ++i)
            server.payload[i] = static_cast<char>(i & 0xff);
        server.startListening();
        QVERIFY(server.isListening());

        Downloader dl;
        QByteArray out;
        QString err;
        const QString url = QStringLiteral("http://127.0.0.1:%1/mod.zip").arg(server.port());
        const bool ok = dl.downloadProgressed(url, {}, &out, &err, nullptr,
                                              nullptr, nullptr, /*resumeAttempts=*/5);
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(out, server.payload);
        QCOMPARE(server.fullRequests, 1);  // 首次全量请求
        QCOMPARE(server.rangeRequests, 1); // 续传一次取回剩余部分
    }
};

// ---------------------------------------------------------------------------
// 模组下载：SHA256 校验与多线程并行
// ---------------------------------------------------------------------------
class TestModuleDownload : public QObject
{
    Q_OBJECT
private slots:
    void hashVerificationAcceptsMatch()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        ModuleInstaller installer(&gi);

        // 构造合法 zip 并计算其 SHA256
        const QByteArray zip = makeZip({{QStringLiteral("GameData/ModA/a.dll"), QByteArray("dll")}});
        const QString sha256 = QString::fromLatin1(
            QCryptographicHash::hash(zip, QCryptographicHash::Sha256).toHex());

        const QString srcPath = dir.filePath(QStringLiteral("src.zip"));
        QFile sf(srcPath);
        QVERIFY(sf.open(QIODevice::WriteOnly));
        sf.write(zip);
        sf.close();

        CkanModule mod = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        mod.downloadUrls = {QUrl::fromLocalFile(srcPath).toString()};
        mod.downloadSize = zip.size();
        mod.downloadHash.sha256 = sha256;

        QString err;
        const QString downloadDir = dir.filePath(QStringLiteral("downloads"));
        QVERIFY2(installer.downloadModules({mod}, downloadDir, {}, false, &err, 1),
                 qPrintable(err));
        const QString cached = downloadDir + QLatin1Char('/') + QStringLiteral("ModA_1.0.zip");
        QVERIFY(QFile::exists(cached));
        QFile cf(cached);
        QVERIFY(cf.open(QIODevice::ReadOnly));
        QCOMPARE(cf.readAll(), zip);
    }

    void hashVerificationRejectsMismatch()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        ModuleInstaller installer(&gi);

        const QByteArray zip = makeZip({{QStringLiteral("GameData/ModA/a.dll"), QByteArray("dll")}});
        const QString srcPath = dir.filePath(QStringLiteral("src.zip"));
        QFile sf(srcPath);
        QVERIFY(sf.open(QIODevice::WriteOnly));
        sf.write(zip);
        sf.close();

        CkanModule mod = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        mod.downloadUrls = {QUrl::fromLocalFile(srcPath).toString()};
        mod.downloadSize = zip.size();
        mod.downloadHash.sha256 = QStringLiteral("0000000000000000000000000000000000000000");

        QString err;
        const QString downloadDir = dir.filePath(QStringLiteral("downloads"));
        QVERIFY(!installer.downloadModules({mod}, downloadDir, {}, false, &err, 1));
        QVERIFY(!err.isEmpty());
        // 校验失败的下载不应写入缓存
        const QString cached = downloadDir + QLatin1Char('/') + QStringLiteral("ModA_1.0.zip");
        QVERIFY(!QFile::exists(cached));
    }

    void parallelDownloads()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        ModuleInstaller installer(&gi);

        const QString srcDir = dir.filePath(QStringLiteral("src"));
        QDir().mkpath(srcDir);
        QVector<CkanModule> mods;
        for (const QString &id : {QStringLiteral("ModA"), QStringLiteral("ModB")}) {
            const QByteArray zip =
                makeZip({{QStringLiteral("GameData/%1/x.dll").arg(id), QByteArray("dll")}});
            const QString srcPath = srcDir + QLatin1Char('/') + id + QStringLiteral(".zip");
            QFile sf(srcPath);
            QVERIFY(sf.open(QIODevice::WriteOnly));
            sf.write(zip);
            sf.close();
            CkanModule mod = makeModule(id, QStringLiteral("1.0"));
            mod.downloadUrls = {QUrl::fromLocalFile(srcPath).toString()};
            mod.downloadSize = zip.size();
            mod.downloadHash.sha256 = QString::fromLatin1(
                QCryptographicHash::hash(zip, QCryptographicHash::Sha256).toHex());
            mods.append(mod);
        }

        QString err;
        const QString downloadDir = dir.filePath(QStringLiteral("downloads"));
        QVERIFY2(installer.downloadModules(mods, downloadDir, {}, false, &err, 2),
                 qPrintable(err));
        for (const CkanModule &mod : mods) {
            const QString cached = downloadDir + QLatin1Char('/')
                                 + mod.identifier + QStringLiteral("_1.0.zip");
            QVERIFY(QFile::exists(cached));
        }
    }
};

// ---------------------------------------------------------------------------
// 仓库索引：版本排序与最新版本选取（search() 依赖的逻辑）
// ---------------------------------------------------------------------------
class TestRepoIndex : public QObject
{
    Q_OBJECT
private slots:
    void latestPicksNewestNotAlphabeticalFirst()
    {
        // 回归：Kerbal Konstructs 这类含 v 前缀的版本，必须按版本取最新 v1.12.2.0，
        // 而不能像旧逻辑那样取 m_index 首个（tar 字母序 0.5.1b）版本。
        // 构造顺序故意模拟 tar 字母序：0.5.1b 排在最前。
        auto idx = makeIndex({
            makeModule(QStringLiteral("KerbalKonstructs"), QStringLiteral("0.5.1b")),
            makeModule(QStringLiteral("KerbalKonstructs"), QStringLiteral("1.8.1.15")),
            makeModule(QStringLiteral("KerbalKonstructs"), QStringLiteral("v1.12.2.0")),
        });
        const QVector<CkanModule> sorted =
            RepoIndex::versionsFor(idx, QStringLiteral("KerbalKonstructs"));
        QCOMPARE(sorted.size(), 3);
        QCOMPARE(sorted.first().version, QStringLiteral("v1.12.2.0"));
        QCOMPARE(RepoIndex::latestFor(idx, QStringLiteral("KerbalKonstructs")).version,
                 QStringLiteral("v1.12.2.0"));
    }

    void latestEmptyIndex()
    {
        const QMap<QString, QVector<CkanModule>> idx;
        QVERIFY(!RepoIndex::latestFor(idx, QStringLiteral("Missing")).isValid());
    }

    void mergePrioritizesFirstRepo()
    {
        // 仓库1（优先级高）：A 有 1.0/2.0，B 有 1.0；计数 A=100、B=50
        QMap<QString, QVector<CkanModule>> r1 = makeIndex({
            makeModule(QStringLiteral("A"), QStringLiteral("1.0")),
            makeModule(QStringLiteral("A"), QStringLiteral("2.0")),
            makeModule(QStringLiteral("B"), QStringLiteral("1.0")),
        });
        QMap<QString, int> c1;
        c1[QStringLiteral("A")] = 100;
        c1[QStringLiteral("B")] = 50;

        // 仓库2（优先级低）：A 有 2.0（与仓库1重复）/3.0；计数 A=999 应被仓库1覆盖
        QMap<QString, QVector<CkanModule>> r2 = makeIndex({
            makeModule(QStringLiteral("A"), QStringLiteral("2.0")),
            makeModule(QStringLiteral("A"), QStringLiteral("3.0")),
        });
        QMap<QString, int> c2;
        c2[QStringLiteral("A")] = 999;

        QMap<QString, QVector<CkanModule>> index;
        QMap<QString, int> counts;
        RepoIndex::mergeSubIndexes({r1, r2}, {c1, c2}, &index, &counts);

        // A 版本去重：2.0 只保留一次
        const QVector<CkanModule> aVersions = index.value(QStringLiteral("A"));
        QCOMPARE(aVersions.size(), 3);
        QSet<QString> versions;
        for (const CkanModule &m : aVersions) versions.insert(m.version);
        QVERIFY(versions.contains(QStringLiteral("1.0")));
        QVERIFY(versions.contains(QStringLiteral("2.0")));
        QVERIFY(versions.contains(QStringLiteral("3.0")));
        // B 只来自仓库1
        QCOMPARE(index.value(QStringLiteral("B")).size(), 1);
        // 计数：高优先级仓库优先
        QCOMPARE(counts.value(QStringLiteral("A")), 100);
        QCOMPARE(counts.value(QStringLiteral("B")), 50);
    }

    void parseTarGzReadsDownloadCounts()
    {
        const QByteArray tarGz = makeTarGz({
            {QStringLiteral("CKAN-meta-master/ModA/ModA-1.2.3.ckan"),
             QByteArrayLiteral("{\"identifier\":\"ModA\",\"name\":\"Mod A\",\"version\":\"1.2.3\"}")},
            {QStringLiteral("CKAN-meta-master/download_counts.json"),
             QByteArrayLiteral("{\"ModA\": 12345, \"ModB\": -1}")},
        });
        QMap<QString, QVector<CkanModule>> index;
        QMap<QString, int> counts;
        QString err;
        QVERIFY(RepoIndex::parseTarGz(tarGz, &index, &counts, &err));
        QCOMPARE(index.size(), 1);
        QCOMPARE(index.value(QStringLiteral("ModA")).size(), 1);
        QCOMPARE(index.value(QStringLiteral("ModA")).at(0).version, QStringLiteral("1.2.3"));
        QCOMPARE(counts.value(QStringLiteral("ModA")), 12345);
        QVERIFY(!counts.contains(QStringLiteral("ModB"))); // 负数不计入
    }
};

// ---------------------------------------------------------------------------
// 事务回滚：安装/卸载/升级原子执行，失败（含用户取消）整体回滚不残留文件
// ---------------------------------------------------------------------------
class TestTransactionRollback : public QObject
{
    Q_OBJECT
private slots:
    void txFileManagerRollbackRestoresEverything()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString txBase = dir.filePath(QStringLiteral("tx"));
        TxFileManager tx(txBase);

        const QString a = dir.filePath(QStringLiteral("a.txt"));
        const QString b = dir.filePath(QStringLiteral("b.txt"));
        QFile fa(a);
        QVERIFY(fa.open(QIODevice::WriteOnly)); fa.write("orig-a"); fa.close();
        QFile fb(b);
        QVERIFY(fb.open(QIODevice::WriteOnly)); fb.write("orig-b"); fb.close();

        const auto readBytes = [](const QString &p) {
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly)) return QByteArray();
            return f.readAll();
        };

        // 覆盖已存在文件 + 删除文件 + 新建文件
        QVERIFY(tx.writeFile(a, QByteArray("new-a")));
        QVERIFY(tx.deleteFile(b));
        const QString c = dir.filePath(QStringLiteral("c.txt"));
        QVERIFY(tx.writeFile(c, QByteArray("new-c")));

        QCOMPARE(readBytes(a), QByteArray("new-a"));
        QVERIFY(!QFile::exists(b));

        tx.rollback();
        // 被覆盖/删除的文件恢复原内容，新建文件被删除，事务目录清空
        QCOMPARE(readBytes(a), QByteArray("orig-a"));
        QVERIFY(QFile::exists(b));
        QVERIFY(!QFile::exists(c));
        QVERIFY(QDir(txBase).entryList(QDir::NoDotAndDotDot).isEmpty());
    }

    void installBatchFailureLeavesNoResidue()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        const QString dl = dir.filePath(QStringLiteral("dl"));
        QDir().mkpath(dl);

        // 模块 A：合法 zip，安装中会先被成功写入
        const QByteArray zipA = makeZip({qMakePair(QStringLiteral("ModA/a.dll"), QByteArray("dll"))});
        QFile zfa(dl + QStringLiteral("/ModA_1.0.zip"));
        QVERIFY(zfa.open(QIODevice::WriteOnly)); zfa.write(zipA); zfa.close();

        // 模块 B：无缓存 zip，installFromCache 在 A 写入后中途失败
        CkanModule modA = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        modA.downloadUrls = QStringList{QStringLiteral("file:///dummy/modA.zip")};
        CkanModule modB = makeModule(QStringLiteral("ModB"), QStringLiteral("1.0"));
        modB.downloadUrls = QStringList{QStringLiteral("file:///dummy/modB.zip")};

        ModuleInstaller installer(&gi);
        const InstallResult r = installer.installFromCache({modA, modB}, dl);
        QVERIFY(!r.ok); // 整批原子失败
        // A 已写入的文件被回滚，注册表还原为空
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("GameData/ModA/a.dll"))));
        QVERIFY(!gi.registry()->isInstalled(QStringLiteral("ModA")));
        QVERIFY(!gi.registry()->isInstalled(QStringLiteral("ModB")));
    }

    void cancelMidBatchRollsBack()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        const QString dl = dir.filePath(QStringLiteral("dl"));
        QDir().mkpath(dl);

        const QByteArray zipA = makeZip({qMakePair(QStringLiteral("ModA/a.dll"), QByteArray("dll"))});
        QFile zfa(dl + QStringLiteral("/ModA_1.0.zip"));
        QVERIFY(zfa.open(QIODevice::WriteOnly)); zfa.write(zipA); zfa.close();
        const QByteArray zipB = makeZip({qMakePair(QStringLiteral("ModB/b.dll"), QByteArray("dll"))});
        QFile zfb(dl + QStringLiteral("/ModB_1.0.zip"));
        QVERIFY(zfb.open(QIODevice::WriteOnly)); zfb.write(zipB); zfb.close();

        CkanModule modA = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        modA.downloadUrls = QStringList{QStringLiteral("file:///dummy/modA.zip")};
        CkanModule modB = makeModule(QStringLiteral("ModB"), QStringLiteral("1.0"));
        modB.downloadUrls = QStringList{QStringLiteral("file:///dummy/modB.zip")};

        ModuleInstaller installer(&gi);
        // 模块 A 安装完成（100%）时触发取消，使模块 B 在写入前被中止
        QObject::connect(&installer, &ModuleInstaller::installProgress,
                         [&installer](const QString &id, int percent) {
            if (id == QStringLiteral("ModA") && percent == 100)
                installer.cancel();
        });
        const InstallResult r = installer.installFromCache({modA, modB}, dl);
        QVERIFY(!r.ok); // 用户取消视为失败
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("GameData/ModA/a.dll"))));
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("GameData/ModB/b.dll"))));
        QVERIFY(!gi.registry()->isInstalled(QStringLiteral("ModA")));
        QVERIFY(!gi.registry()->isInstalled(QStringLiteral("ModB")));
    }

    void uninstallFailureRollsBack()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        const QString dl = dir.filePath(QStringLiteral("dl"));
        QDir().mkpath(dl);

        const QByteArray zip = makeZip({
            qMakePair(QStringLiteral("ModA/a.dll"), QByteArray("dll")),
            qMakePair(QStringLiteral("ModA/b.dll"), QByteArray("dll")),
        });
        QFile zf(dl + QStringLiteral("/ModA_1.0.zip"));
        QVERIFY(zf.open(QIODevice::WriteOnly)); zf.write(zip); zf.close();

        CkanModule mod = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        mod.downloadUrls = QStringList{QStringLiteral("file:///dummy/modA.zip")};
        ModuleInstaller installer(&gi);
        const InstallResult ir = installer.installFromCache({mod}, dl);
        QVERIFY2(ir.ok, qPrintable(ir.error));

        // 把注册表中列为文件的 b.dll 替换成同名目录，使卸载时删除失败
        const QString bPath = dir.filePath(QStringLiteral("GameData/ModA/b.dll"));
        QVERIFY(QFile::remove(bPath));
        QVERIFY(QDir().mkpath(bPath));

        const InstallResult r = installer.uninstall(QStringLiteral("ModA"));
        QVERIFY(!r.ok); // 卸载失败
        // 已删除的 a.dll 被回滚恢复，注册表保持已安装
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("GameData/ModA/a.dll"))));
        QVERIFY(gi.registry()->isInstalled(QStringLiteral("ModA")));
    }

    void upgradeMergedTransactionRollsBack()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        const QString dl = dir.filePath(QStringLiteral("dl"));
        QDir().mkpath(dl);

        // 安装 ModA v1.0
        const QByteArray zipOld = makeZip({qMakePair(QStringLiteral("ModA/a.dll"), QByteArray("old"))});
        QFile zfo(dl + QStringLiteral("/ModA_1.0.zip"));
        QVERIFY(zfo.open(QIODevice::WriteOnly)); zfo.write(zipOld); zfo.close();
        CkanModule oldMod = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        oldMod.downloadUrls = QStringList{QStringLiteral("file:///dummy/old.zip")};
        ModuleInstaller installer(&gi);
        QVERIFY2(installer.installFromCache({oldMod}, dl).ok, "install old failed");

        // 模拟升级：外部单事务 = 卸载旧版 + 安装新版，新版安装失败 -> 整体回滚
        CkanModule newMod = makeModule(QStringLiteral("ModA"), QStringLiteral("2.0"));
        newMod.downloadUrls = QStringList{QStringLiteral("file:///dummy/new.zip")}; // 无缓存，安装必然失败

        TxFileManager tx(gi.ckanDir() + QStringLiteral("/transactions"));
        const QByteArray regSnapshot = gi.registry()->toJson();
        const InstallResult ru = installer.uninstall(QStringLiteral("ModA"), &tx);
        QVERIFY2(ru.ok, qPrintable(ru.error));
        const InstallResult ri = installer.installFromCache({newMod}, dl, {}, &tx);
        QVERIFY(!ri.ok); // 新版安装失败

        tx.rollback();
        gi.restoreRegistrySnapshot(regSnapshot);

        // 旧版文件被恢复，注册表回到旧版
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("GameData/ModA/a.dll"))));
        QVERIFY(gi.registry()->isInstalled(QStringLiteral("ModA")));
        QCOMPARE(gi.registry()->installedVersion(QStringLiteral("ModA")), QStringLiteral("1.0"));
    }
};

static int runSuite(int argc, char *argv[], QObject &suite)
{
    return QTest::qExec(&suite, argc, argv);
}

int main(int argc, char *argv[])
{
    // 下载器依赖事件循环，需先创建 QCoreApplication
    QCoreApplication app(argc, argv);
    int failures = 0;
    TestModuleVersion tModVer;
    TestGameVersion tGameVer;
    TestRelationship tRel;
    TestCkanModule tMod;
    TestModuleInstallDescriptor tInstall;
    TestRegistry tReg;
    TestGameInstance tGameInst;
    TestRelationshipResolver tResolver;
    TestDownloader tDownloader;
    TestRepoIndex tRepoIndex;
    TestModuleDownload tModDownload;
    TestTransactionRollback tTxRollback;
    failures += runSuite(argc, argv, tModVer);
    failures += runSuite(argc, argv, tGameVer);
    failures += runSuite(argc, argv, tRel);
    failures += runSuite(argc, argv, tMod);
    failures += runSuite(argc, argv, tInstall);
    failures += runSuite(argc, argv, tReg);
    failures += runSuite(argc, argv, tGameInst);
    failures += runSuite(argc, argv, tResolver);
    failures += runSuite(argc, argv, tDownloader);
    failures += runSuite(argc, argv, tModDownload);
    failures += runSuite(argc, argv, tRepoIndex);
    failures += runSuite(argc, argv, tTxRollback);
    return failures;
}

#include "test_libckan.moc"