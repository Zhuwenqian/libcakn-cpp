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
#include "ckan/ckan.h"
#include "ckan/moduleinstalldescriptor.h"
#include "ckan/moduleinstaller.h"
#include "ckan/installedmodule.h"
#include "ckan/registry.h"
#include "ckan/gameinstance.h"
#include "ckan/relationshipresolver.h"
#include "ckan/filelock.h"
#include "ckan/downloader.h"
#include "ckan/repoindex.h"
#include "ckan/txfilemanager.h"
#include "ckan/modpackio.h"

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

// any_of 关系：任一子依赖满足即可（子关系通常为 depends 类型）
static Relationship anyOfDep(const QVector<Relationship> &subs)
{
    Relationship r;
    r.type = Relationship::Type::Depends;
    r.anyOf = subs;
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

// 用 miniz raw deflate 构造 gzip（仓库归档测试使用；尾部写入真实 CRC32 与 ISIZE，
// 因有界 gunzip 会校验 ISIZE 以检测截断/损坏）
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

    // gzip 尾部：CRC32 + ISIZE（未压缩大小 mod 2^32，小端序）。
    // 有界 gunzip 会校验 ISIZE 以检测截断/损坏，测试助手须写入真实值。
    const mz_ulong crc = mz_crc32(0, reinterpret_cast<const unsigned char *>(tar.constData()),
                                  static_cast<size_t>(tar.size()));
    const quint32 isize = static_cast<quint32>(tar.size());
    for (int i = 0; i < 4; ++i)
        gz.append(char((crc >> (8 * i)) & 0xff));
    for (int i = 0; i < 4; ++i)
        gz.append(char((isize >> (8 * i)) & 0xff));
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
    void toVersionRange()
    {
        // 官方 GameVersion.ToVersionRange 半开展开
        const GameVersionRange r1 = GameVersion(QStringLiteral("1.12.5")).toVersionRange();
        QVERIFY(r1.lowerSet());
        QVERIFY(r1.upperSet());
        QVERIFY(r1.lowerInclusive());
        QVERIFY(!r1.upperInclusive());
        QCOMPARE(r1.lower().toString(), QStringLiteral("1.12.5.0"));
        QCOMPARE(r1.upper().toString(), QStringLiteral("1.12.6.0"));
        // 完整版本（含 build）为点区间
        const GameVersionRange r2 = GameVersion(QStringLiteral("1.12.5.3190")).toVersionRange();
        QVERIFY(r2.lowerInclusive());
        QVERIFY(r2.upperInclusive());
        QVERIFY(r2.lower() == r2.upper());
    }
    void rangeHalfOpenUpper()
    {
        // kRPC 场景：ksp_version_min 1.12.3 / ksp_version_max 1.12.5，
        // 未显式声明 build 的上界为半开区间，应兼容 1.12.5.3190
        const GameVersionRange range(GameVersion(QStringLiteral("1.12.3")), true,
                                     GameVersion(QStringLiteral("1.12.5")), true);
        QVERIFY(range.contains(GameVersion(QStringLiteral("1.12.5.3190"))));
        QVERIFY(range.contains(GameVersion(QStringLiteral("1.12.3.0"))));
        QVERIFY(range.contains(GameVersion(QStringLiteral("1.12.4.9999"))));
        QVERIFY(!range.contains(GameVersion(QStringLiteral("1.12.6"))));
        QVERIFY(!range.contains(GameVersion(QStringLiteral("1.12.6.0"))));
        QVERIFY(!range.contains(GameVersion(QStringLiteral("1.12.2"))));
    }
    void rangePointWithBuild()
    {
        // 显式声明 build 的上界为点区间（如 ksp_version_max: 1.12.5.3190）
        const GameVersionRange range(GameVersion(QStringLiteral("1.12.5.3190")), true,
                                     GameVersion(QStringLiteral("1.12.5.3190")), true);
        QVERIFY(range.contains(GameVersion(QStringLiteral("1.12.5.3190"))));
        QVERIFY(!range.contains(GameVersion(QStringLiteral("1.12.5.3191"))));
        QVERIFY(!range.contains(GameVersion(QStringLiteral("1.12.6.0"))));
    }
    void versionLinesToRange()
    {
        // 默认勾选 1.9~1.12 -> 连续区间 [1.9.0.0, 1.13.0.0)
        const GameVersionRange r = ckan::versionLinesToRange(
            {QStringLiteral("1.9"), QStringLiteral("1.10"),
             QStringLiteral("1.11"), QStringLiteral("1.12")});
        QVERIFY(r.lowerSet());
        QVERIFY(r.upperSet());
        QVERIFY(r.lowerInclusive());
        QVERIFY(!r.upperInclusive());
        QCOMPARE(r.lower().toString(), QStringLiteral("1.9.0.0"));
        QCOMPARE(r.upper().toString(), QStringLiteral("1.13.0.0"));
        // 区间内：1.9 首行、1.12.5.3190 等；区间外：1.8 与 1.13
        QVERIFY(r.contains(GameVersion(QStringLiteral("1.9.0.0"))));
        QVERIFY(r.contains(GameVersion(QStringLiteral("1.12.5.3190"))));
        QVERIFY(!r.contains(GameVersion(QStringLiteral("1.8.9"))));
        QVERIFY(!r.contains(GameVersion(QStringLiteral("1.13.0"))));
        // 单个版本线 {"1.12"} -> [1.12.0.0, 1.13.0.0)
        const GameVersionRange single = ckan::versionLinesToRange({QStringLiteral("1.12")});
        QVERIFY(single.contains(GameVersion(QStringLiteral("1.12.0"))));
        QVERIFY(single.contains(GameVersion(QStringLiteral("1.12.5.3190"))));
        QVERIFY(!single.contains(GameVersion(QStringLiteral("1.11.9"))));
        // 空集合/全部无效 -> 无效区间（调用方回退为仅按当前实例版本判断）
        QVERIFY(!ckan::versionLinesToRange({}).lowerSet());
        QVERIFY(!ckan::versionLinesToRange({QStringLiteral("abc")}).lowerSet());
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

        // 1) ksp_version 非 strict：仅兼容该版本线（官方 StrictGameComparator），
        //    1.12.3 -> [1.12.3.0, 1.12.4.0)，不再按「1.12.3 及以上」放宽为无界上界
        m.kspVersion = QStringLiteral("1.12.3");
        QVERIFY(m.isCompatible(ksp));
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("1.12.3.0"))));
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("1.12.3.9999"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.13.0"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("2.0.0"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.11.0"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.12.4.0"))));

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

        // 9) kRPC 场景：ksp_version_min/max 未显式声明 build -> 上界为半开区间，
        //    兼容该 patch 线的所有 build（如 KSP 1.12.5.3190）
        m.kspVersion.clear();
        m.kspVersionMin = QStringLiteral("1.12.3");
        m.kspVersionMax = QStringLiteral("1.12.5");
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("1.12.5.3190"))));
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("1.12.3.0"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.12.6.0"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.12.2.0"))));
    }
    void compatibleWithRange()
    {
        // isCompatible(GameVersionRange)：模组兼容区间与用户勾选区间相交即兼容
        CkanModule m = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        // 默认勾选 1.9~1.12 -> [1.9.0.0, 1.13.0.0)
        const GameVersionRange defaultRange = versionLinesToRange(
            {QStringLiteral("1.9"), QStringLiteral("1.10"),
             QStringLiteral("1.11"), QStringLiteral("1.12")});

        // 1) 上界 1.12.5 的模组（kRPC 风格）：半开上界 1.12.6.0 落在默认区间内 -> 兼容
        m.kspVersionMin = QStringLiteral("1.12.3");
        m.kspVersionMax = QStringLiteral("1.12.5");
        QVERIFY(m.isCompatible(defaultRange));

        // 2) 只兼容 1.8 及以下的模组：与 [1.9,1.13) 不相交 -> 不兼容
        m.kspVersionMin.clear();
        m.kspVersionMax = QStringLiteral("1.8.0");
        QVERIFY(!m.isCompatible(defaultRange));

        // 3) 无任何版本信息 -> 无界区间，与任何区间兼容
        m.kspVersionMax.clear();
        QVERIFY(m.isCompatible(defaultRange));

        // 4) 只兼容 1.14 及以上的模组：超出默认区间上界 -> 不兼容
        m.kspVersionMin = QStringLiteral("1.14.0");
        QVERIFY(!m.isCompatible(defaultRange));

        // 5) DCK FutureTech 回归（仅声明 ksp_version=1.3.1，无 min/max）：
        //    版本线 [1.3.1.0, 1.4.0.0) 与默认区间 [1.9,1.13) 及实例 1.12.5.3190 均不相交 -> 不兼容
        m.kspVersionMin.clear();
        m.kspVersionMax.clear();
        m.kspVersion = QStringLiteral("1.3.1");
        QVERIFY(!m.isCompatible(defaultRange));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.12.5.3190"))));
        QVERIFY(m.isCompatible(GameVersion(QStringLiteral("1.3.1"))));
        QVERIFY(!m.isCompatible(GameVersion(QStringLiteral("1.9.0"))));
    }
    void effectiveInstallDefault()
    {
        const CkanModule m = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        const QVector<ModuleInstallDescriptor> stanzas = m.effectiveInstallStanzas();
        QCOMPARE(stanzas.size(), 1);
        QCOMPARE(stanzas.at(0).find, QStringLiteral("ModA"));
        QCOMPARE(stanzas.at(0).installTo, QStringLiteral("GameData"));
    }
    void releaseStatusParsing()
    {
        // 缺省 → Stable；testing/beta → Testing；development/alpha → Development；序列化往返
        QString err;
        const CkanModule dev = CkanModule::fromJson(
            "{\"identifier\":\"ModA\",\"version\":\"1.0\","
            "\"release_status\":\"development\"}", &err);
        QVERIFY(dev.isValid());
        QCOMPARE(static_cast<int>(dev.releaseStatus),
                 static_cast<int>(ReleaseStatus::Development));

        // beta 兼容映射为 Testing
        const CkanModule beta = CkanModule::fromJson(
            "{\"identifier\":\"ModB\",\"version\":\"1.0\","
            "\"release_status\":\"beta\"}", &err);
        QVERIFY(beta.isValid());
        QCOMPARE(static_cast<int>(beta.releaseStatus),
                 static_cast<int>(ReleaseStatus::Testing));

        // 缺省 → Stable
        const CkanModule stable = CkanModule::fromJson(
            "{\"identifier\":\"ModC\",\"version\":\"1.0\"}", &err);
        QVERIFY(stable.isValid());
        QCOMPARE(static_cast<int>(stable.releaseStatus),
                 static_cast<int>(ReleaseStatus::Stable));

        // 非 stable 序列化写出并往返
        const QByteArray jsonDev = dev.toJson();
        QVERIFY(jsonDev.contains("release_status"));
        const CkanModule back = CkanModule::fromJson(jsonDev, &err);
        QCOMPARE(static_cast<int>(back.releaseStatus),
                 static_cast<int>(ReleaseStatus::Development));

        // stable 不写出（默认值，与官方 DefaultValue 一致）
        const QByteArray jsonStable = stable.toJson();
        QVERIFY(!jsonStable.contains("release_status"));
    }
    void dlcKindParsing()
    {
        QString err;
        const CkanModule dlc = CkanModule::fromJson(
            "{\"identifier\":\"MakingHistory\",\"version\":\"1.8.1\",\"kind\":\"dlc\"}", &err);
        QVERIFY(dlc.isValid());
        QVERIFY(dlc.isDlc());
        const QByteArray json = dlc.toJson();
        QVERIFY(json.contains("kind"));
        const CkanModule back = CkanModule::fromJson(json, &err);
        QVERIFY(back.isDlc());
    }
    void minMaxVersionKeysParsing()
    {
        // 官方 ModuleRelationshipDescriptor 独立键：min_version / max_version（含 inclusive）
        QString err;
        const CkanModule m = CkanModule::fromJson(
            "{\"identifier\":\"ModA\",\"version\":\"1.0\",\"depends\":["
            "{\"name\":\"Dep\",\"min_version\":\"1.5\",\"max_version\":\"2.0\"}]}", &err);
        QVERIFY2(m.isValid(), qPrintable(err));
        QCOMPARE(m.depends.size(), 1);
        const Relationship &d = m.depends.at(0);
        QCOMPARE(d.name, QStringLiteral("Dep"));
        QCOMPARE(d.minVersion, QStringLiteral("1.5"));
        QCOMPARE(d.maxVersion, QStringLiteral("2.0"));
        QVERIFY(d.minInclusive); // inclusive 缺省为 true
        QVERIFY(d.maxInclusive);
        // 约束语义
        QVERIFY(d.versionSatisfies(QStringLiteral("1.5")));
        QVERIFY(d.versionSatisfies(QStringLiteral("1.9")));
        QVERIFY(d.versionSatisfies(QStringLiteral("2.0")));
        QVERIFY(!d.versionSatisfies(QStringLiteral("1.4")));
        QVERIFY(!d.versionSatisfies(QStringLiteral("2.1")));

        // 仅 min_version（下界）
        const CkanModule lo = CkanModule::fromJson(
            "{\"identifier\":\"ModB\",\"version\":\"1.0\",\"recommends\":["
            "{\"name\":\"Dep\",\"min_version\":\"3.0\"}]}", &err);
        QVERIFY(lo.isValid());
        QVERIFY(lo.recommends.at(0).versionSatisfies(QStringLiteral("3.0")));
        QVERIFY(!lo.recommends.at(0).versionSatisfies(QStringLiteral("2.9")));

        // 非默认 inclusive=false 也解析
        const CkanModule excl = CkanModule::fromJson(
            "{\"identifier\":\"ModC\",\"version\":\"1.0\",\"conflicts\":["
            "{\"name\":\"Other\",\"min_version\":\"2.0\",\"min_version_inclusive\":false}]}", &err);
        QVERIFY(excl.isValid());
        QVERIFY(!excl.conflicts.at(0).minInclusive);

        // 序列化往返：独立键原样保留
        const QByteArray json = m.toJson();
        QVERIFY(json.contains("min_version"));
        QVERIFY(json.contains("max_version"));
        const CkanModule back = CkanModule::fromJson(json, &err);
        QVERIFY(back.isValid());
        QCOMPARE(back.depends.size(), 1);
        QCOMPARE(back.depends.at(0).minVersion, QStringLiteral("1.5"));
        QCOMPARE(back.depends.at(0).maxVersion, QStringLiteral("2.0"));
    }
    void parseAnyOf()
    {
        // any_of：任一子依赖满足即可（官方 CKAN 格式 {"any_of":[{...},{...}]}）
        QString err;
        const CkanModule m = CkanModule::fromJson(
            "{\"identifier\":\"Reviva\",\"version\":\"1.0\",\"depends\":["
            "{\"name\":\"ModuleManager\"},"
            "{\"any_of\":[{\"name\":\"RasterPropMonitor-Core\"},{\"name\":\"AvionicsSystems\"}]}]}",
            &err);
        QVERIFY2(m.isValid(), qPrintable(err));
        QCOMPARE(m.depends.size(), 2);
        const Relationship &normal = m.depends.at(0);
        QVERIFY(normal.anyOf.isEmpty());
        QCOMPARE(normal.name, QStringLiteral("ModuleManager"));
        const Relationship &any = m.depends.at(1);
        QCOMPARE(any.anyOf.size(), 2);
        QCOMPARE(any.anyOf.at(0).name, QStringLiteral("RasterPropMonitor-Core"));
        QCOMPARE(any.anyOf.at(1).name, QStringLiteral("AvionicsSystems"));
        // 子关系继承 depends 类型
        QVERIFY(any.anyOf.at(0).type == Relationship::Type::Depends);

        // 序列化往返：any_of 原样保留
        const QByteArray json = m.toJson();
        QVERIFY(json.contains("any_of"));
        const CkanModule back = CkanModule::fromJson(json, &err);
        QVERIFY(back.isValid());
        QCOMPARE(back.depends.size(), 2);
        QCOMPARE(back.depends.at(1).anyOf.size(), 2);
        QCOMPARE(back.depends.at(1).anyOf.at(1).name, QStringLiteral("AvionicsSystems"));
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
    void extraRangeCandidateFilter()
    {
        // 实例实际版本 1.12.5.3190，但用户勾选了 1.9~1.12（默认区间）。
        // A 依赖 B；B 仅兼容到 1.12.4（上界 [1.12.4.0,1.12.5.0)）：
        // 不兼容实例版本，但兼容勾选区间 -> 启用 extraRange 时可安装。
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("B"))});
        CkanModule B = makeModule(QStringLiteral("B"), QStringLiteral("1.0"));
        B.kspVersionMax = QStringLiteral("1.12.4");
        const auto idx = makeIndex({A, B});
        RelationshipResolver resolver(idx);
        Registry reg;
        const GameVersion ksp(QStringLiteral("1.12.5.3190"));
        const GameVersionRange extraRange = versionLinesToRange(
            {QStringLiteral("1.9"), QStringLiteral("1.12")});

        // 启用 extraRange：B 兼容勾选区间 -> 解析成功且安装 B
        const ResolutionResult r = resolver.resolve({A}, reg, false, false, ksp, extraRange);
        QVERIFY(!r.missing);
        QVERIFY(!r.conflicted);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("B")));

        // 未启用 extraRange（默认无效区间）：仅按实例版本判断 -> B 不兼容，依赖缺失
        const ResolutionResult r2 = resolver.resolve({A}, reg, false, false, ksp);
        QVERIFY(r2.missing);
        QVERIFY(r2.notFound.contains(QStringLiteral("B")));
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
    void installedVersionUpgrade()
    {
        // 已安装 B 1.0，A 依赖 B>=2.0：已安装版本不满足约束 → 应升级到满足约束的新版
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("B"), QStringLiteral("2.0"))});
        QMap<QString, QVector<CkanModule>> idx;
        idx[QStringLiteral("A")] = {A};
        idx[QStringLiteral("B")] = {makeModule(QStringLiteral("B"), QStringLiteral("1.0")),
                                    makeModule(QStringLiteral("B"), QStringLiteral("2.0")),
                                    makeModule(QStringLiteral("B"), QStringLiteral("3.0"))};
        RelationshipResolver resolver(idx);
        Registry reg;
        InstalledModule im;
        im.identifier = QStringLiteral("B");
        im.module = makeModule(QStringLiteral("B"), QStringLiteral("1.0"));
        reg.installedModules[QStringLiteral("B")] = im;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(!r.missing);
        QVERIFY(!r.conflicted);
        bool found300 = false;
        for (const CkanModule &m : r.modulesToInstall)
            if (m.identifier == QStringLiteral("B") && m.version == QStringLiteral("3.0"))
                found300 = true;
        QVERIFY(found300); // 升级到满足 >=2.0 的最高版本
    }
    void kspCompatibilityFiltersCandidates()
    {
        // A 依赖 B>=1.5。B 2.0 需要 KSP 1.13+（与 1.12.5 不兼容），B 1.6 兼容：
        // 应选兼容的 1.6，而非不兼容的 2.0（不报缺失，不选不兼容候选）
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("B"), QStringLiteral("1.5"))});
        CkanModule b20 = makeModule(QStringLiteral("B"), QStringLiteral("2.0"));
        b20.kspVersion = QStringLiteral("1.13.0"); // 需要 1.13+：与 1.12.5 不兼容
        CkanModule b16 = makeModule(QStringLiteral("B"), QStringLiteral("1.6"));
        b16.kspVersionMin = QStringLiteral("1.10.0"); // 区间 [1.10.0, 1.12.5]：与 1.12.5 兼容
        b16.kspVersionMax = QStringLiteral("1.12.5");
        QMap<QString, QVector<CkanModule>> idx;
        idx[QStringLiteral("A")] = {A};
        idx[QStringLiteral("B")] = {b20, b16};
        RelationshipResolver resolver(idx);
        Registry reg;
        const GameVersion ksp(QStringLiteral("1.12.5"));
        const ResolutionResult r = resolver.resolve({A}, reg, false, false, ksp);
        QVERIFY(!r.missing);
        QVERIFY(!r.conflicted);
        bool found160 = false;
        for (const CkanModule &m : r.modulesToInstall)
            if (m.identifier == QStringLiteral("B") && m.version == QStringLiteral("1.6"))
                found160 = true;
        QVERIFY(found160);
    }
    void noCompatibleCandidateIsMissing()
    {
        // 唯一候选与游戏版本不兼容 → 依赖缺失（而非选择不兼容版本）
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("B"), QStringLiteral("1.0"))});
        CkanModule b10 = makeModule(QStringLiteral("B"), QStringLiteral("1.0"));
        b10.kspVersion = QStringLiteral("1.13.0"); // 需要 1.13+：与 1.12.5 不兼容
        QMap<QString, QVector<CkanModule>> idx;
        idx[QStringLiteral("A")] = {A};
        idx[QStringLiteral("B")] = {b10};
        RelationshipResolver resolver(idx);
        Registry reg;
        const GameVersion ksp(QStringLiteral("1.12.5"));
        const ResolutionResult r = resolver.resolve({A}, reg, false, false, ksp);
        QVERIFY(r.missing);
        QVERIFY(r.notFound.contains(QStringLiteral("B")));
    }
    void reverseConflictFromInstalled()
    {
        // 已安装 B 声明 conflicts A；新安装 A 应被裁决为冲突（反向冲突检测）
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"));
        const CkanModule B = makeModule(QStringLiteral("B"), QStringLiteral("1.0"),
                                        {}, {}, {dep(QStringLiteral("A"))});
        const auto idx = makeIndex({A, B});
        RelationshipResolver resolver(idx);
        Registry reg;
        InstalledModule im;
        im.identifier = QStringLiteral("B");
        im.module = B;
        reg.installedModules[QStringLiteral("B")] = im;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(r.conflicted);
        QVERIFY(!r.conflicts.isEmpty());
    }
    void virtualProviderConflict()
    {
        // 新模块 P 提供虚拟包 virt-lib，而已选 A 声明 conflicts virt-lib → 冲突
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {}, {}, {dep(QStringLiteral("virt-lib"))});
        const CkanModule P = makeModule(QStringLiteral("P"), QStringLiteral("1.0"),
                                        {}, {}, {}, {prov(QStringLiteral("virt-lib"))});
        const auto idx = makeIndex({A, P});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A, P}, reg, false);
        QVERIFY(r.conflicted);
    }
    void recommendsCascade()
    {
        // A 推荐 R，R 推荐 R2：autoInstallRecommends=true 时两级推荐级联安装
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {}, {dep(QStringLiteral("R"))});
        const CkanModule R = makeModule(QStringLiteral("R"), QStringLiteral("1.0"),
                                        {}, {dep(QStringLiteral("R2"))});
        const CkanModule R2 = makeModule(QStringLiteral("R2"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, R, R2});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, true);
        QVERIFY(!r.missing);
        QVERIFY(!r.conflicted);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("R")));
        QVERIFY(ids.contains(QStringLiteral("R2")));
    }
    void recommendConflictSkipped()
    {
        // 推荐模组 R 与硬依赖 B 冲突 → 静默跳过 R，不产生硬冲突
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("B"))}, {dep(QStringLiteral("R"))});
        const CkanModule B = makeModule(QStringLiteral("B"), QStringLiteral("1.0"));
        const CkanModule R = makeModule(QStringLiteral("R"), QStringLiteral("1.0"),
                                        {}, {}, {dep(QStringLiteral("B"))});
        const auto idx = makeIndex({A, B, R});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, true);
        QVERIFY(!r.conflicted);
        QVERIFY(r.conflicts.isEmpty());
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("B")));
        QVERIFY(!ids.contains(QStringLiteral("R"))); // 冲突的推荐被跳过
    }
    void multiProviderCollectsChoices()
    {
        // A 依赖虚拟包 virt；P1、P2 同时提供 virt → 不自动选最高版本，
        // 收集到 providerChoices 交由 UI 弹窗（对应官方 TooManyModsProvideKraken）
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("virt"))});
        const CkanModule P1 = makeModule(QStringLiteral("P1"), QStringLiteral("2.0"),
                                         {}, {}, {}, {prov(QStringLiteral("virt"))});
        const CkanModule P2 = makeModule(QStringLiteral("P2"), QStringLiteral("1.0"),
                                         {}, {}, {}, {prov(QStringLiteral("virt"))});
        const auto idx = makeIndex({A, P1, P2});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(!r.missing);
        QVERIFY(!r.conflicted);
        QCOMPARE(r.providerChoices.size(), 1);
        QCOMPARE(r.providerChoices.at(0).provides, QStringLiteral("virt"));
        QCOMPARE(r.providerChoices.at(0).candidates.size(), 2);
        // 未自动选择任何提供者
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("A")));
        QVERIFY(!ids.contains(QStringLiteral("P1")));
        QVERIFY(!ids.contains(QStringLiteral("P2")));

        // 模拟用户选择 P1 后重新解析：virt 被满足，P1 进入安装集且不再有待选
        QVector<CkanModule> combined = {A, P1};
        const ResolutionResult r2 = resolver.resolve(combined, reg, false);
        QVERIFY(r2.providerChoices.isEmpty());
        QVERIFY(!r2.missing);
        QVERIFY(!r2.conflicted);
        QSet<QString> ids2;
        for (const CkanModule &m : r2.modulesToInstall) ids2.insert(m.identifier);
        QVERIFY(ids2.contains(QStringLiteral("P1")));
    }
    void multiProviderSingleCandidateAutoSelected()
    {
        // 虚拟包只有一个提供者 → 自动选择（不产生 providerChoices）
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("virt"))});
        const CkanModule P1 = makeModule(QStringLiteral("P1"), QStringLiteral("2.0"),
                                         {}, {}, {}, {prov(QStringLiteral("virt"))});
        const auto idx = makeIndex({A, P1});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(r.providerChoices.isEmpty());
        QVERIFY(!r.missing);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("P1")));
    }
    void multiProviderKspFiltersCandidates()
    {
        // 虚拟包 virt 有两个提供者，但 P2 与当前 KSP 不兼容：
        // 候选只剩 P1 → 自动选择 P1，不产生 providerChoices
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("virt"))});
        CkanModule P1 = makeModule(QStringLiteral("P1"), QStringLiteral("1.0"),
                                   {}, {}, {}, {prov(QStringLiteral("virt"))});
        P1.kspVersionMin = QStringLiteral("1.10.0"); // 区间 [1.10.0, 1.12.5]：兼容 1.12.5
        P1.kspVersionMax = QStringLiteral("1.12.5");
        CkanModule P2 = makeModule(QStringLiteral("P2"), QStringLiteral("2.0"),
                                   {}, {}, {}, {prov(QStringLiteral("virt"))});
        P2.kspVersion = QStringLiteral("1.13.0"); // 与 1.12.5 不兼容
        const auto idx = makeIndex({A, P1, P2});
        RelationshipResolver resolver(idx);
        Registry reg;
        const GameVersion ksp(QStringLiteral("1.12.5"));
        const ResolutionResult r = resolver.resolve({A}, reg, false, false, ksp);
        QVERIFY(r.providerChoices.isEmpty()); // 兼容提供者只剩 1 个，无需选择
        QVERIFY(!r.missing);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("P1")));
        QVERIFY(!ids.contains(QStringLiteral("P2")));
    }
    void multiProviderVersionConstraintFiltersCandidates()
    {
        // virt 有 P1(1.0)、P2(2.0) 两个提供者，但依赖要求 virt>=2.0：
        // 满足约束的只剩 P2 → 自动选择，无需用户选择
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {dep(QStringLiteral("virt"), QStringLiteral("2.0"))});
        const CkanModule P1 = makeModule(QStringLiteral("P1"), QStringLiteral("1.0"),
                                         {}, {}, {}, {prov(QStringLiteral("virt"))});
        const CkanModule P2 = makeModule(QStringLiteral("P2"), QStringLiteral("2.0"),
                                         {}, {}, {}, {prov(QStringLiteral("virt"))});
        const auto idx = makeIndex({A, P1, P2});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(r.providerChoices.isEmpty());
        QVERIFY(!r.missing);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("P2")));
    }
    void anyOfSatisfiedByInstalled()
    {
        // A 依赖 {any_of: [B, C]}；B 已安装 → 整个 any_of 视为满足，不强制装 C
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {anyOfDep({dep(QStringLiteral("B")), dep(QStringLiteral("C"))})});
        const CkanModule B = makeModule(QStringLiteral("B"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, B});
        RelationshipResolver resolver(idx);
        Registry reg;
        InstalledModule im;
        im.identifier = QStringLiteral("B");
        im.module = B;
        reg.installedModules[QStringLiteral("B")] = im;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(!r.missing);
        QVERIFY(!r.conflicted);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("A")));
        QVERIFY(!ids.contains(QStringLiteral("C"))); // 任一子依赖满足即可
    }
    void anyOfMissingMessage()
    {
        // A 依赖 {any_of: [X, Y]}，X/Y 均不在索引 → 缺失，notFound 显示 "X 或 Y"（而非空串）
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {anyOfDep({dep(QStringLiteral("X")), dep(QStringLiteral("Y"))})});
        const auto idx = makeIndex({A});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(r.missing);
        QVERIFY(r.notFound.contains(QStringLiteral("X 或 Y")));
    }
    void anyOfConflictFallback()
    {
        // A 依赖 {any_of: [B, C]}；最高版本 B 2.0 与 A 冲突 → 回退选次高且不冲突的 C 1.0
        const CkanModule A = makeModule(QStringLiteral("A"), QStringLiteral("1.0"),
                                        {anyOfDep({dep(QStringLiteral("B")), dep(QStringLiteral("C"))})},
                                        {}, {dep(QStringLiteral("B"))});
        const CkanModule B = makeModule(QStringLiteral("B"), QStringLiteral("2.0"));
        const CkanModule C = makeModule(QStringLiteral("C"), QStringLiteral("1.0"));
        const auto idx = makeIndex({A, B, C});
        RelationshipResolver resolver(idx);
        Registry reg;
        const ResolutionResult r = resolver.resolve({A}, reg, false);
        QVERIFY(!r.missing);
        QVERIFY(!r.conflicted);
        QSet<QString> ids;
        for (const CkanModule &m : r.modulesToInstall) ids.insert(m.identifier);
        QVERIFY(ids.contains(QStringLiteral("C")));
        QVERIFY(!ids.contains(QStringLiteral("B")));
    }
};

// ---------------------------------------------------------------------------
// 注册表文件锁（registry.locked）
// ---------------------------------------------------------------------------
class TestFileLock : public QObject
{
    Q_OBJECT
private slots:
    void acquireAndRelease()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString lockPath = dir.filePath(QStringLiteral("registry.locked"));
        FileLock lock;
        QVERIFY(lock.acquire(lockPath));
        QVERIFY(QFile::exists(lockPath));
        lock.release();
        QVERIFY(!QFile::exists(lockPath));
    }
    void corruptLockCleared()
    {
        // 内容损坏（崩溃时未写入完整 PID）的锁视为陈旧，可清除后获取
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString lockPath = dir.filePath(QStringLiteral("registry.locked"));
        QFile f(lockPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not-a-pid");
        f.close();
        FileLock lock;
        QVERIFY(lock.acquire(lockPath));
        lock.release();
    }
    void deadPidLockCleared()
    {
        // 持有者进程已退出（PID 不存在）的锁视为陈旧，可清除后获取
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString lockPath = dir.filePath(QStringLiteral("registry.locked"));
        QFile f(lockPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::number(qint64(999999999))); // 几乎肯定不存在的 PID
        f.close();
        FileLock lock;
        QVERIFY(lock.acquire(lockPath));
        lock.release();
    }
    void ownPidLockCleared()
    {
        // 本进程残留的锁（同进程内重复加载）视为陈旧，可清除后重新获取
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString lockPath = dir.filePath(QStringLiteral("registry.locked"));
        QFile f(lockPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::number(QCoreApplication::applicationPid()));
        f.close();
        FileLock lock;
        QVERIFY(lock.acquire(lockPath));
        lock.release();
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

    // 回归：BDArmory.dll 被原版 BDArmory / BDAc / BDAP（BDArmoryForRunwayProject）
    // 三个继任者共用，DLL 扫描必须按实际 KSP 版本消歧，否则会把 BDAP 的 DLL 误判成
    // 原版 BDArmory（而 BDAP 的 conflicts 声明了 BDArmory），导致重装 BDAP 误报不兼容。
    void scanDisambiguatesSharedDllByKspVersion()
    {
        const auto mkfile = [](const QString &root, const QString &rel, const QByteArray &content) {
            const QString abs = root + QLatin1Char('/') + rel;
            QDir().mkpath(QFileInfo(abs).absolutePath());
            QFile f(abs);
            QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(rel));
            f.write(content);
            f.close();
        };
        // 返回某标识符扫描到的 DLL 相对路径；未命中时为空串（路径必非空，可区分）
        const auto scannedKey = [](const QString &root, const QString &key) {
            GameInstance gi(root, QStringLiteral("test"));
            return gi.scanUnmanagedDlls().value(key);
        };
        const QString relDll = QStringLiteral("GameData/BDArmory/BDArmory.dll");

        // 1) 现代 KSP 1.12.5 → BDAP（持续维护）
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            mkfile(dir.path(), QStringLiteral("buildID64.txt"), QByteArrayLiteral("build id = 3190\n"));
            mkfile(dir.path(), QStringLiteral("GameData/BDArmory/BDArmory.dll"), "x");
            QCOMPARE(scannedKey(dir.path(), QStringLiteral("BDArmoryForRunwayProject")), relDll);
            QVERIFY(scannedKey(dir.path(), QStringLiteral("BDArmory")).isEmpty());
            QVERIFY(scannedKey(dir.path(), QStringLiteral("BDArmoryContinued")).isEmpty());
        }

        // 2) KSP 1.3.0 → BDAc（BDArmoryContinued）
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            mkfile(dir.path(), QStringLiteral("buildID64.txt"), QByteArrayLiteral("build id = 1804\n"));
            mkfile(dir.path(), QStringLiteral("GameData/BDArmory/BDArmory.dll"), "x");
            QCOMPARE(scannedKey(dir.path(), QStringLiteral("BDArmoryContinued")), relDll);
            QVERIFY(scannedKey(dir.path(), QStringLiteral("BDArmory")).isEmpty());
            QVERIFY(scannedKey(dir.path(), QStringLiteral("BDArmoryForRunwayProject")).isEmpty());
        }

        // 3) 老版本 KSP 1.1.0（build 表外，走 readme 兜底）→ 原版 BDArmory
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            mkfile(dir.path(), QStringLiteral("readme.txt"), QByteArrayLiteral("Version 1.1.0\n"));
            mkfile(dir.path(), QStringLiteral("GameData/BDArmory/BDArmory.dll"), "x");
            QCOMPARE(scannedKey(dir.path(), QStringLiteral("BDArmory")), relDll);
            QVERIFY(scannedKey(dir.path(), QStringLiteral("BDArmoryContinued")).isEmpty());
            QVERIFY(scannedKey(dir.path(), QStringLiteral("BDArmoryForRunwayProject")).isEmpty());
        }

        // 4) 版本未知 → 回退持续维护的 BDAP（避免误判为原版 BDArmory 引发假冲突）
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            mkfile(dir.path(), QStringLiteral("GameData/BDArmory/BDArmory.dll"), "x");
            QCOMPARE(scannedKey(dir.path(), QStringLiteral("BDArmoryForRunwayProject")), relDll);
            QVERIFY(scannedKey(dir.path(), QStringLiteral("BDArmory")).isEmpty());
        }

        // 5) 非共享 DLL 不受消歧影响
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            mkfile(dir.path(), QStringLiteral("buildID64.txt"), QByteArrayLiteral("build id = 3190\n"));
            mkfile(dir.path(), QStringLiteral("GameData/ModX/ModX.dll"), "x");
            QCOMPARE(scannedKey(dir.path(), QStringLiteral("ModX")),
                     QStringLiteral("GameData/ModX/ModX.dll"));
        }
    }

    // 回归：注册表文件被删除后（如 .ckan 整合包导入清空），loadRegistry 必须重置内存态，
    // 否则已删除的暂存数据仍滞留内存，污染后续安装与文件归属判断。
    void reloadRegistryResetsWhenFileDeleted()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));

        // 先让内存与磁盘都有已安装数据
        InstalledModule im;
        im.identifier = QStringLiteral("ModA");
        im.module = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        im.files = {QStringLiteral("GameData/ModA/x.dll")};
        gi.registry()->registerModule(im); // 触发首次加载
        QVERIFY2(gi.saveRegistry(), "save registry failed");
        QVERIFY(gi.registry()->isInstalled(QStringLiteral("ModA")));

        // 删除注册表文件（整合包导入清空场景）
        QVERIFY2(QFile::remove(gi.registryPath()), "remove registry failed");

        // 重新加载：文件不存在时内存态必须重置为空
        gi.loadRegistry();
        QVERIFY(!gi.registry()->isInstalled(QStringLiteral("ModA")));
    }

    void scanMissingGameDataReturnsEmpty()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        QVERIFY(gi.scanUnmanagedDlls().isEmpty());
    }

    // 回归（防 Zip Slip）：toAbsoluteGameDir 必须拒绝逃逸出游戏目录的路径，返回空
    void toAbsoluteRejectsEscapingPaths()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));

        // 合法路径：游戏目录内保持原样
        QCOMPARE(gi.toAbsoluteGameDir(QStringLiteral("GameData/ModA/a.dll")),
                 dir.filePath(QStringLiteral("GameData/ModA/a.dll")));
        // 逃逸路径：返回空（调用方据此拒绝写入）
        QVERIFY(gi.toAbsoluteGameDir(QStringLiteral("../evil.txt")).isEmpty());
        QVERIFY(gi.toAbsoluteGameDir(QStringLiteral("GameData/../../evil.txt")).isEmpty());
        // 以 / 开头的绝对路径会被规范化剥离为游戏目录内相对路径（安全，不会逃逸）
        QCOMPARE(gi.toAbsoluteGameDir(QStringLiteral("/absolute/path")),
                 dir.filePath(QStringLiteral("absolute/path")));
        // 相对化后恰好回到游戏目录：合法（安装路径不会使用，但不应报逃逸）
        QCOMPARE(gi.toAbsoluteGameDir(QStringLiteral("GameData/..")), dir.path());
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

    void installKindTagsCleanStockOnlySquad()
    {
        // 仅含 Squad → Clean Stock
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/Squad/Part"))));
        bool corrupted = false;
        QCOMPARE(GameInstance::detectInstallKindTags(dir.path(), &corrupted),
                 QStringList({QStringLiteral("Clean Stock")}));
        QVERIFY(!corrupted);
    }

    void installKindTagsCleanStockWithSquadExpansion()
    {
        // 只有 Squad 和 SquadExpansion → Clean Stock
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/Squad"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/SquadExpansion/EasterEggs"))));
        QCOMPARE(GameInstance::detectInstallKindTags(dir.path()),
                 QStringList({QStringLiteral("Clean Stock")}));
    }

    void installKindTagsROAndRSSAndRP1Order()
    {
        // RealSolarSystem + RealismOverhaul + RP-1 → RSS 在前，RP-1 在 RO 之后
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/Squad"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/RealismOverhaul"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/RealSolarSystem"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/RP-1"))));
        QCOMPARE(GameInstance::detectInstallKindTags(dir.path()),
                 QStringList({QStringLiteral("RSS"), QStringLiteral("RO"), QStringLiteral("RP-1")}));
    }

    void installKindTagsSolPositionBeforeRO()
    {
        // Sol-Configs 位置与 RSS 相同，在 RO 之前
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/Squad"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/Sol-Configs"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/RealismOverhaul"))));
        QCOMPARE(GameInstance::detectInstallKindTags(dir.path()),
                 QStringList({QStringLiteral("Sol"), QStringLiteral("RO")}));
    }

    void installKindTagsCaseInsensitive()
    {
        // 目录匹配不区分大小写
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/squad"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/realismoverhaul"))));
        QCOMPARE(GameInstance::detectInstallKindTags(dir.path()),
                 QStringList({QStringLiteral("RO")}));
    }

    void installKindTagsCorruptedMissingSquad()
    {
        // GameData 存在但缺少必需的 Squad → 判定损坏，无标签
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/CustomMod"))));
        bool corrupted = false;
        QVERIFY(GameInstance::detectInstallKindTags(dir.path(), &corrupted).isEmpty());
        QVERIFY(corrupted);
    }

    void installKindTagsUnknownModsNoTag()
    {
        // 含官方目录 + 其它第三方模组（非已知类型）→ 无标签
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/Squad"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/SomeRandomMod"))));
        QCOMPARE(GameInstance::detectInstallKindTags(dir.path()), QStringList());
    }

    void suggestedNameWithVersionAndTags()
    {
        // KSP + 版本（经 buildID）+ 标签
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile f(dir.filePath(QStringLiteral("buildID64.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("build id = 03190\n");   // 1.12.5.3190
        f.close();
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/Squad"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/RealismOverhaul"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/RealSolarSystem"))));
        QCOMPARE(GameInstance::suggestedInstanceName(dir.path()),
                 QStringLiteral("KSP 1.12.5.3190 RSS RO"));
    }

    void suggestedNameCleanStockNoVersion()
    {
        // 无 buildID/readme → 省略版本号；仅 Squad → Clean Stock
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("GameData/Squad"))));
        QCOMPARE(GameInstance::suggestedInstanceName(dir.path()),
                 QStringLiteral("KSP Clean Stock"));
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
    void dlcDownloadIntercepted()
    {
        // 官方 DLC 不可直接经 CKAN 下载/安装（对应 ModuleIsDLCKraken）
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        ModuleInstaller installer(&gi);
        CkanModule dlc = makeModule(QStringLiteral("MakingHistory"), QStringLiteral("1.8.1"));
        dlc.kind = ModuleKind::Dlc;
        QString err;
        QVERIFY(!installer.downloadModules({dlc}, dir.path(), {}, false, &err, 1));
        QVERIFY(!err.isEmpty());
        // 未写入任何缓存文件
        const QString cached = dir.path() + QLatin1Char('/')
                             + QStringLiteral("MakingHistory_1.8.1.zip");
        QVERIFY(!QFile::exists(cached));
    }

    void officialCacheFileName()
    {
        // 官方 CKAN StandardName：{identifier}-{version}.zip，
        // version 中非 [A-Za-z0-9_.-] 的字符替换为 '-'
        QCOMPARE(ModuleInstaller::officialCacheFileName(QStringLiteral("RealSolarSystem"),
                                                        QStringLiteral("7.3")),
                 QStringLiteral("RealSolarSystem-7.3.zip"));
        // 带 epoch 前缀的版本：冒号替换为 '-'
        QCOMPARE(ModuleInstaller::officialCacheFileName(QStringLiteral("Kopernicus"),
                                                        QStringLiteral("1:release")),
                 QStringLiteral("Kopernicus-1-release.zip"));
        // 含空格/加号等非法字符：逐一替换为 '-'
        QCOMPARE(ModuleInstaller::officialCacheFileName(QStringLiteral("SomeMod"),
                                                        QStringLiteral("2.0 beta+1")),
                 QStringLiteral("SomeMod-2.0-beta-1.zip"));
        // 合法字符（字母数字 ._-）原样保留
        QCOMPARE(ModuleInstaller::officialCacheFileName(QStringLiteral("Module_Manager"),
                                                        QStringLiteral("4.2.1")),
                 QStringLiteral("Module_Manager-4.2.1.zip"));
        // 带下载 URL：官方 NetFileCache 格式 {SHA1(url) 前 8 位大写}-{identifier}-{version}.zip
        // （对应 D:\CKAN Downloads 中形如 2105D4E8-Shaddy-v2.5.zip 的缓存文件）
        {
            const QString url = QStringLiteral("https://github.com/ShaddyKSP/Shaddy/releases/download/v2.5/Shaddy.zip");
            const QString name = ModuleInstaller::officialCacheFileName(
                QStringLiteral("Shaddy"), QStringLiteral("2.5"), url);
            const QString prefix = QString::fromLatin1(
                QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex())
                .left(8).toUpper();
            QCOMPARE(name, prefix + QStringLiteral("-Shaddy-2.5.zip"));
            QCOMPARE(name.size(), 8 + 1 + static_cast<int>(QStringLiteral("Shaddy-2.5.zip").size()));
        }
    }

    void estimateRequiredBytes()
    {
        // 空列表 -> 0
        QCOMPARE(ModuleInstaller::estimateRequiredBytes({}), qint64(0));
        // 单个模块 downloadSize=100，缓冲系数 1.15 -> ceil(115)=115
        CkanModule m = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        m.downloadSize = 100;
        QCOMPARE(ModuleInstaller::estimateRequiredBytes({m}), qint64(115));
        // 元包不计入空间估算
        CkanModule meta = makeModule(QStringLiteral("CommunityTechTree"), QStringLiteral("1.0"));
        meta.kind = ModuleKind::Metapackage;
        meta.downloadSize = 500;
        QCOMPARE(ModuleInstaller::estimateRequiredBytes({meta, m}), qint64(115));
        // downloadSize 未知(0)的模块按 1 字节计，避免误判为零：ceil(1*1.15)=2
        CkanModule unknown = makeModule(QStringLiteral("ModB"), QStringLiteral("1.0"));
        unknown.downloadSize = 0;
        QCOMPARE(ModuleInstaller::estimateRequiredBytes({unknown}), qint64(2));
        // 缓冲系数可配置：1.0 时不加缓冲
        QCOMPARE(ModuleInstaller::estimateRequiredBytes({m}, 1.0), qint64(100));
    }

    void findCacheZipPrefersOfficial()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dl = dir.filePath(QStringLiteral("dl"));
        QDir().mkpath(dl);

        CkanModule mod = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        mod.downloadUrls = QStringList{QStringLiteral("file:///dummy/modA.zip")};

        // 同时存在官方格式与本启动器格式，应优先返回官方格式
        const QByteArray zip = makeZip({qMakePair(QStringLiteral("ModA/a.dll"), QByteArray("dll"))});
        QFile zfOfficial(dl + QStringLiteral("/ModA-1.0.zip"));
        QVERIFY(zfOfficial.open(QIODevice::WriteOnly)); zfOfficial.write(zip); zfOfficial.close();
        QFile zfLauncher(dl + QStringLiteral("/ModA_1.0.zip"));
        QVERIFY(zfLauncher.open(QIODevice::WriteOnly)); zfLauncher.write(zip); zfLauncher.close();
        QCOMPARE(ModuleInstaller::findCacheZip(dl, mod),
                 dl + QStringLiteral("/ModA-1.0.zip"));

        // 仅官方格式存在时也能找到
        QVERIFY(QFile::remove(dl + QStringLiteral("/ModA_1.0.zip")));
        QCOMPARE(ModuleInstaller::findCacheZip(dl, mod),
                 dl + QStringLiteral("/ModA-1.0.zip"));

        // 仅本启动器格式存在时兜底命中
        QVERIFY(QFile::remove(dl + QStringLiteral("/ModA-1.0.zip")));
        QFile zfL2(dl + QStringLiteral("/ModA_1.0.zip"));
        QVERIFY(zfL2.open(QIODevice::WriteOnly)); zfL2.write(zip); zfL2.close();
        QCOMPARE(ModuleInstaller::findCacheZip(dl, mod),
                 dl + QStringLiteral("/ModA_1.0.zip"));

        // 无效 zip（非 ZIP 内容）不被当作有效缓存
        QVERIFY(QFile::remove(dl + QStringLiteral("/ModA_1.0.zip")));
        QFile zfBad(dl + QStringLiteral("/ModA-1.0.zip"));
        QVERIFY(zfBad.open(QIODevice::WriteOnly)); zfBad.write("not a zip"); zfBad.close();
        QVERIFY(ModuleInstaller::findCacheZip(dl, mod).isEmpty());
    }

    void officialCacheReusedForDownload()
    {
        // D:\CKAN Downloads 官方格式缓存应被直接复用，不再重新下载
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dl = dir.filePath(QStringLiteral("dl"));
        QDir().mkpath(dl);
        GameInstance gi(dir.path(), QStringLiteral("test"));
        ModuleInstaller installer(&gi);

        const QByteArray zip = makeZip({qMakePair(QStringLiteral("ModA/a.dll"), QByteArray("dll"))});
        const QString sha256 = QString::fromLatin1(
            QCryptographicHash::hash(zip, QCryptographicHash::Sha256).toHex());
        QFile zf(dl + QStringLiteral("/ModA-1.0.zip"));
        QVERIFY(zf.open(QIODevice::WriteOnly)); zf.write(zip); zf.close();

        CkanModule mod = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        // 指向不存在地址：若未命中缓存必失败，命中缓存则无需下载
        mod.downloadUrls = QStringList{QStringLiteral("file:///nonexistent/modA.zip")};
        mod.downloadSize = zip.size();
        mod.downloadHash.sha256 = sha256;

        QString err;
        QVERIFY2(installer.downloadModules({mod}, dl, {}, false, &err, 1), qPrintable(err));
        // 未生成本启动器格式缓存，官方缓存原样保留
        QVERIFY(!QFile::exists(dl + QStringLiteral("/ModA_1.0.zip")));
        QVERIFY(QFile::exists(dl + QStringLiteral("/ModA-1.0.zip")));
    }

    void officialCacheUsedForInstall()
    {
        // 安装时能正确安装官方 CKAN 格式的缓存文件
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dl = dir.filePath(QStringLiteral("dl"));
        QDir().mkpath(dl);
        GameInstance gi(dir.path(), QStringLiteral("test"));
        ModuleInstaller installer(&gi);

        const QByteArray zip = makeZip({qMakePair(QStringLiteral("ModA/a.dll"), QByteArray("dll"))});
        QFile zf(dl + QStringLiteral("/ModA-1.0.zip"));
        QVERIFY(zf.open(QIODevice::WriteOnly)); zf.write(zip); zf.close();

        CkanModule mod = makeModule(QStringLiteral("ModA"), QStringLiteral("1.0"));
        mod.downloadUrls = QStringList{QStringLiteral("file:///dummy/modA.zip")};

        const InstallResult r = installer.installFromCache({mod}, dl);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("GameData/ModA/a.dll"))));
        QVERIFY(gi.registry()->isInstalled(QStringLiteral("ModA")));
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

    void parseTarGzRejectsTruncated()
    {
        // 回归：截断的 gzip（下载中途断开）必须被拒绝，而不是解析出半成品索引
        const QByteArray gz = makeTarGz({
            {QStringLiteral("CKAN-meta-master/ModA/ModA-1.0.ckan"),
             QByteArrayLiteral("{\"identifier\":\"ModA\",\"name\":\"Mod A\",\"version\":\"1.0\"}")},
        });
        QVERIFY(gz.size() > 30);
        const QByteArray chopped = gz.left(gz.size() - 10); // 去掉尾部（含 ISIZE），deflate 流不完整
        QMap<QString, QVector<CkanModule>> index;
        QString err;
        QVERIFY(!RepoIndex::parseTarGz(chopped, &index, nullptr, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(index.isEmpty());
    }

    void parseTarGzRejectsBadFooter()
    {
        // 回归：ISIZE（尾部 4 字节）与实际解压大小不符 → 判定归档损坏
        const QByteArray gz = makeTarGz({
            {QStringLiteral("CKAN-meta-master/ModA/ModA-1.0.ckan"),
             QByteArrayLiteral("{\"identifier\":\"ModA\",\"name\":\"Mod A\",\"version\":\"1.0\"}")},
        });
        QByteArray bad = gz;
        bad[bad.size() - 1] = char(bad[bad.size() - 1] ^ 0xff); // 翻转 ISIZE 末字节
        QMap<QString, QVector<CkanModule>> index;
        QString err;
        QVERIFY(!RepoIndex::parseTarGz(bad, &index, nullptr, &err));
        QVERIFY(!err.isEmpty());
    }

    void parseTarGzRejectsDecompressionBomb()
    {
        // 回归（安全）：解压炸弹——高度可压缩的大体积数据（300MB 全零，压缩后极小）
        // 超过 256MB 解压上限时必须被拒绝，防止恶意归档膨胀内存。
        const QByteArray big = QByteArray(300 * 1024 * 1024, '\0');
        const QByteArray gz = makeTarGz({
            {QStringLiteral("CKAN-meta-master/pad/pad-1.0.ckan"), big},
        });
        QVERIFY(gz.size() < big.size() / 10); // 压缩后远小于原始体积，验证其"炸弹"特性
        QMap<QString, QVector<CkanModule>> index;
        QString err;
        QVERIFY(!RepoIndex::parseTarGz(gz, &index, nullptr, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(index.isEmpty());
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
    void fileConflictRejected()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("test"));
        const QString dl = dir.filePath(QStringLiteral("dl"));
        QDir().mkpath(dl);

        // ModA 安装并登记文件 GameData/Shared/a.dll（默认规则 find=Shared, install_to=GameData）
        const QByteArray zipA = makeZip({qMakePair(QStringLiteral("Shared/a.dll"), QByteArray("aaa"))});
        QFile zfa(dl + QStringLiteral("/Shared_1.0.zip"));
        QVERIFY(zfa.open(QIODevice::WriteOnly)); zfa.write(zipA); zfa.close();
        CkanModule modA = makeModule(QStringLiteral("Shared"), QStringLiteral("1.0"));
        modA.downloadUrls = QStringList{QStringLiteral("file:///dummy/modA.zip")};
        ModuleInstaller installer(&gi);
        QVERIFY2(installer.installFromCache({modA}, dl).ok, "install ModA failed");
        QCOMPARE(gi.registry()->fileOwner(QStringLiteral("GameData/Shared/a.dll")),
                 QStringLiteral("Shared"));

        // ModB 显式安装到同一目标文件（find=Shared → GameData/Shared/a.dll）：
        // 目标文件已被 ModA 登记归属 → 文件级覆盖冲突，拒绝安装并回滚
        ModuleInstallDescriptor stanza;
        stanza.find = QStringLiteral("Shared");
        stanza.installTo = QStringLiteral("GameData");
        const QByteArray zipB = makeZip({qMakePair(QStringLiteral("Shared/a.dll"), QByteArray("bbb"))});
        QFile zfb(dl + QStringLiteral("/ModB_1.0.zip"));
        QVERIFY(zfb.open(QIODevice::WriteOnly)); zfb.write(zipB); zfb.close();
        CkanModule modB = makeModule(QStringLiteral("ModB"), QStringLiteral("1.0"));
        modB.install = {stanza};
        modB.downloadUrls = QStringList{QStringLiteral("file:///dummy/modB.zip")};
        const InstallResult rb = installer.installFromCache({modB}, dl);
        QVERIFY(!rb.ok); // 冲突导致失败
        QVERIFY(rb.error.contains(QStringLiteral("文件冲突")));
        // 原文件未被覆盖，ModB 未登记安装
        QFile f(dir.path() + QStringLiteral("/GameData/Shared/a.dll"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("aaa"));
        QVERIFY(!gi.registry()->isInstalled(QStringLiteral("ModB")));
        QCOMPARE(gi.registry()->fileOwner(QStringLiteral("GameData/Shared/a.dll")),
                 QStringLiteral("Shared"));
    }
};

class TestCkanExport : public QObject
{
    Q_OBJECT
private slots:
    void exportGeneratesMetapackage()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // 写 readme.txt 以便版本检测成功（1.12.3）
        QFile rf(dir.filePath(QStringLiteral("readme.txt")));
        QVERIFY(rf.open(QIODevice::WriteOnly));
        rf.write("Version 1.12.3\n");
        rf.close();

        GameInstance gi(dir.path(), QStringLiteral("Test Instance"));
        auto registerMod = [&](const CkanModule &m, bool autoInstalled = false) {
            InstalledModule im;
            im.identifier = m.identifier;
            im.module = m;
            im.autoInstalled = autoInstalled;
            im.files = {QStringLiteral("GameData/%1/x.dll").arg(m.identifier)};
            gi.registry()->registerModule(im);
        };
        // 依赖链：C 依赖 B，B 依赖 A
        registerMod(makeModule(QStringLiteral("A"), QStringLiteral("1.0")));
        registerMod(makeModule(QStringLiteral("B"), QStringLiteral("1.0"), {dep(QStringLiteral("A"))}));
        registerMod(makeModule(QStringLiteral("C"), QStringLiteral("1.0"), {dep(QStringLiteral("B"))}));
        // DLC（应排除）
        CkanModule dlc = makeModule(QStringLiteral("DlcFoo"), QStringLiteral("1.0"));
        dlc.kind = ModuleKind::Dlc;
        registerMod(dlc);
        // 自动安装（应排除）
        registerMod(makeModule(QStringLiteral("AutoMod"), QStringLiteral("1.0")), true);
        // 手动安装 AD 模组（应排除）
        gi.registry()->installedDlls[QStringLiteral("ManualMod")] =
            QStringLiteral("GameData/ManualMod/ManualMod.dll");
        QVERIFY2(gi.saveRegistry(), "save registry failed");

        CKan ckan(dir.path(), QStringLiteral("Test Instance"));
        QString error;
        const QByteArray json = ckan.exportModpackCkan(&error);
        QVERIFY2(!json.isEmpty(), qPrintable(error));

        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
        QVERIFY2(perr.error == QJsonParseError::NoError, qPrintable(perr.errorString()));
        const QJsonObject obj = doc.object();

        QCOMPARE(obj.value(QStringLiteral("kind")).toString(), QStringLiteral("metapackage"));
        QCOMPARE(obj.value(QStringLiteral("name")).toString(),
                 QStringLiteral("已安装-Test Instance"));
        // 官方 Identifier.Sanitize：去前缀 + 非法字符替换为 '-'
        QCOMPARE(obj.value(QStringLiteral("identifier")).toString(),
                 QStringLiteral("Test-Instance"));
        QVERIFY(!obj.value(QStringLiteral("version")).toString().isEmpty());
        QCOMPARE(obj.value(QStringLiteral("ksp_version_min")).toString(), QStringLiteral("1.12.3"));
        QCOMPARE(obj.value(QStringLiteral("ksp_version_max")).toString(), QStringLiteral("1.12.3"));

        // depends：仅 A/B/C，依赖在前，无版本约束
        const QJsonArray depends = obj.value(QStringLiteral("depends")).toArray();
        QCOMPARE(depends.size(), 3);
        QStringList names;
        for (const QJsonValue &v : depends) {
            const QJsonObject d = v.toObject();
            QVERIFY(d.contains(QStringLiteral("name")));
            QVERIFY(!d.contains(QStringLiteral("version")));
            names << d.value(QStringLiteral("name")).toString();
        }
        QCOMPARE(names, QStringList({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}));
        QVERIFY(!names.contains(QStringLiteral("DlcFoo")));
        QVERIFY(!names.contains(QStringLiteral("AutoMod")));
        QVERIFY(!names.contains(QStringLiteral("ManualMod")));
    }

    void exportVirtualProvidesOrderedBeforeDepender()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile rf(dir.filePath(QStringLiteral("readme.txt")));
        QVERIFY(rf.open(QIODevice::WriteOnly));
        rf.write("Version 1.12.3\n");
        rf.close();

        GameInstance gi(dir.path(), QStringLiteral("Virt"));
        auto registerMod = [&](const CkanModule &m) {
            InstalledModule im;
            im.identifier = m.identifier;
            im.module = m;
            im.files = {QStringLiteral("GameData/%1/x.dll").arg(m.identifier)};
            gi.registry()->registerModule(im);
        };
        // Lib 提供虚拟包 SharedLib；Consumer 依赖 SharedLib
        registerMod(makeModule(QStringLiteral("Lib"), QStringLiteral("1.0"),
                               {}, {}, {}, {prov(QStringLiteral("SharedLib"))}));
        registerMod(makeModule(QStringLiteral("Consumer"), QStringLiteral("1.0"),
                               {dep(QStringLiteral("SharedLib"))}));
        QVERIFY2(gi.saveRegistry(), "save registry failed");

        CKan ckan(dir.path(), QStringLiteral("Virt"));
        const QByteArray json = ckan.exportModpackCkan();
        QVERIFY(!json.isEmpty());
        const QJsonObject obj = QJsonDocument::fromJson(json).object();
        QStringList names;
        for (const QJsonValue &v : obj.value(QStringLiteral("depends")).toArray())
            names << v.toObject().value(QStringLiteral("name")).toString();
        QCOMPARE(names, QStringList({QStringLiteral("Lib"), QStringLiteral("Consumer")}));
    }

    void exportEmptyReportsError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("Empty"));
        QVERIFY2(gi.saveRegistry(), "save registry failed");
        CKan ckan(dir.path(), QStringLiteral("Empty"));
        QString error;
        QVERIFY(ckan.exportModpackCkan(&error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
};

class TestModpackIO : public QObject
{
    Q_OBJECT
private slots:
    void detectPrefix()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray zip = makeZip({
            {QStringLiteral("GameData/ModA/a.dll"), QByteArray("A")},
            {QStringLiteral("GameData/ModB/b.dll"), QByteArray("B")},
        });
        QFile f(dir.filePath(QStringLiteral("p.zip")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(zip);
        f.close();

        QString prefix, error;
        QVERIFY2(modpackZipGameDataPrefix(f.fileName(), &prefix, &error),
                 qPrintable(error));
        QCOMPARE(prefix, QStringLiteral("GameData/"));
    }

    void detectPrefixNestedPack()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray zip = makeZip({
            {QStringLiteral("MyPack/GameData/ModA/a.dll"), QByteArray("A")},
            {QStringLiteral("MyPack/readme.txt"), QByteArray("R")},
        });
        QFile f(dir.filePath(QStringLiteral("p.zip")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(zip);
        f.close();

        QString prefix, error;
        QVERIFY2(modpackZipGameDataPrefix(f.fileName(), &prefix, &error),
                 qPrintable(error));
        QCOMPARE(prefix, QStringLiteral("MyPack/GameData/"));
    }

    void detectMissingReportsError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray zip = makeZip({
            {QStringLiteral("some.txt"), QByteArray("x")},
        });
        QFile f(dir.filePath(QStringLiteral("p.zip")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(zip);
        f.close();

        QString prefix, error;
        QVERIFY(!modpackZipGameDataPrefix(f.fileName(), &prefix, &error));
        QVERIFY(!error.isEmpty());
    }

    void ckanDependsParsing()
    {
        const QByteArray json = R"({"depends":[{"name":"A"},{"name":"B"},
            {"name":"C","version":"1.2"}],"conflicts":[]})";
        QString error;
        const QStringList deps = modpackCkanDepends(json, &error);
        QCOMPARE(deps, QStringList({QStringLiteral("A"), QStringLiteral("B"),
                                    QStringLiteral("C")}));
        QVERIFY(error.isEmpty());

        // 空 depends 报错
        QString error2;
        QVERIFY(modpackCkanDepends(R"({"spec_version":1})", &error2).isEmpty());
        QVERIFY(!error2.isEmpty());
    }

    void clearPreservesOfficialFolders()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString gameData = dir.path() + QStringLiteral("/GameData");
        QVERIFY(QDir().mkpath(gameData + QStringLiteral("/Squad/Part")));
        QVERIFY(QDir().mkpath(gameData + QStringLiteral("/SquadExpansion/X")));
        QVERIFY(QDir().mkpath(gameData + QStringLiteral("/SomeMod")));
        QFile a(gameData + QStringLiteral("/Squad/keep.txt"));
        QVERIFY(a.open(QIODevice::WriteOnly)); a.write("k"); a.close();
        QFile b(gameData + QStringLiteral("/SomeMod/bin.dll"));
        QVERIFY(b.open(QIODevice::WriteOnly)); b.write("x"); b.close();
        // 制造一个注册表文件验证被删除
        QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/CKAN")));
        QFile reg(dir.path() + QStringLiteral("/CKAN/registry.json"));
        QVERIFY(reg.open(QIODevice::WriteOnly)); reg.write("{}"); reg.close();

        QString error;
        QVERIFY2(modpackClearGameData(dir.path(), &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(gameData + QStringLiteral("/Squad/keep.txt")));
        QVERIFY(QFileInfo::exists(gameData + QStringLiteral("/SquadExpansion/X")));
        QVERIFY(!QDir(gameData + QStringLiteral("/SomeMod")).exists());
        QVERIFY(!QFileInfo::exists(dir.path() + QStringLiteral("/CKAN/registry.json")));
    }

    void importFromZipReplacesMods()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString gameData = dir.path() + QStringLiteral("/GameData");
        QVERIFY(QDir().mkpath(gameData + QStringLiteral("/OldMod")));
        QVERIFY(QDir().mkpath(gameData + QStringLiteral("/Squad")));
        QFile old(gameData + QStringLiteral("/OldMod/old.dll"));
        QVERIFY(old.open(QIODevice::WriteOnly)); old.write("old"); old.close();

        // zip：GameData/ModA + GameData/ModB，包根还带 readme（不应解压到 GameData 内）
        const QByteArray zip = makeZip({
            {QStringLiteral("GameData/ModA/a.dll"), QByteArray("A")},
            {QStringLiteral("GameData/ModB/sub/b.dll"), QByteArray("BBB")},
        });
        QFile f(dir.filePath(QStringLiteral("pack.zip")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(zip);
        f.close();

        std::atomic_bool cancel{false};
        int lastProgress = -1;
        QString error;
        const bool ok = modpackImportGameData(
            f.fileName(), dir.path(),
            [&](int p) { lastProgress = p; }, &cancel, &error);
        QVERIFY2(ok, qPrintable(error));
        QCOMPARE(lastProgress, 1000);
        QVERIFY(QFileInfo::exists(gameData + QStringLiteral("/ModA/a.dll")));
        QVERIFY(QFileInfo::exists(gameData + QStringLiteral("/ModB/sub/b.dll")));
        QVERIFY(!QDir(gameData + QStringLiteral("/OldMod")).exists());
        // Squad 保留
        QVERIFY(QDir(gameData + QStringLiteral("/Squad")).exists());
    }

    void importRejectsPathTraversal()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // 恶意 zip：条目借 .. 试图逃逸出 GameData（Zip Slip）
        const QByteArray zip = makeZip({
            {QStringLiteral("GameData/ModA/a.dll"), QByteArray("A")},
            {QStringLiteral("GameData/../../escape.txt"), QByteArray("EVIL")},
        });
        QFile f(dir.filePath(QStringLiteral("evil.zip")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(zip);
        f.close();

        std::atomic_bool cancel{false};
        QString error;
        const bool ok = modpackImportGameData(f.fileName(), dir.path(),
                                              [](int) {}, &cancel, &error);
        QVERIFY(!ok);
        QVERIFY(!error.isEmpty());
        // 越界文件不得被写出
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("escape.txt"))));
        // 整体拒绝：合法条目也不得被部分导入
        QVERIFY(!QFileInfo::exists(dir.path() + QStringLiteral("/GameData/ModA/a.dll")));
    }
};

class TestCkanHistoryImport : public QObject
{
    Q_OBJECT
private slots:
    void historySnapshotWritesMetapackage()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("Hist"));
        auto registerMod = [&](const QString &id, const QString &ver) {
            InstalledModule im;
            im.identifier = id;
            im.module = makeModule(id, ver);
            im.files = {QStringLiteral("GameData/%1/x.dll").arg(id)};
            gi.registry()->registerModule(im);
        };
        registerMod(QStringLiteral("A"), QStringLiteral("1.0.0"));
        registerMod(QStringLiteral("B"), QStringLiteral("2.1"));
        QVERIFY2(gi.saveRegistry(), "save registry failed");

        CKan ckan(dir.path(), QStringLiteral("Hist"));
        QString error;
        QVERIFY2(ckan.writeHistorySnapshot(&error), qPrintable(error));

        QDir hdir(dir.path() + QStringLiteral("/CKAN/history"));
        QVERIFY2(hdir.exists(), "history dir missing");
        const QStringList files = hdir.entryList({QStringLiteral("*.ckan")}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QVERIFY(files.first().startsWith(QStringLiteral("已安装-Hist-")));

        // 校验快照内容：depends 列出 A/B 及各自版本
        QFile f(hdir.filePath(files.first()));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(obj.value(QStringLiteral("kind")).toString(), QStringLiteral("metapackage"));
        QMap<QString, QString> got;
        for (const QJsonValue &v : obj.value(QStringLiteral("depends")).toArray()) {
            const QJsonObject d = v.toObject();
            got[d.value(QStringLiteral("name")).toString()] =
                d.value(QStringLiteral("version")).toString();
        }
        QCOMPARE(got.value(QStringLiteral("A")), QStringLiteral("1.0.0"));
        QCOMPARE(got.value(QStringLiteral("B")), QStringLiteral("2.1"));
    }

    void historySnapshotSkipsEmptyInstance()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("Empty"));
        QVERIFY2(gi.saveRegistry(), "save registry failed");
        CKan ckan(dir.path(), QStringLiteral("Empty"));
        QString error;
        QVERIFY2(ckan.writeHistorySnapshot(&error), qPrintable(error));
        const QStringList files = QDir(dir.path() + QStringLiteral("/CKAN/history"))
                                      .entryList({QStringLiteral("*.ckan")}, QDir::Files);
        QCOMPARE(files.size(), 0);
    }

    void importCkanDirect()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray json = "{"
            "\"spec_version\":\"v1.6\","
            "\"identifier\":\"ImportedMod\","
            "\"name\":\"Imported Mod\","
            "\"version\":\"1.2.3\","
            "\"kind\":\"metapackage\","
            "\"depends\":[{\"name\":\"A\"},{\"name\":\"B\"}]\n}"
        ;
        QFile f(dir.filePath(QStringLiteral("ImportedMod.ckan")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(json);
        f.close();

        CKan ckan(dir.path(), QStringLiteral("T"));
        bool isMeta = false;
        QString error;
        const CkanModule mod = ckan.importModuleFile(f.fileName(), &isMeta, &error);
        QVERIFY2(mod.isValid(), qPrintable(error));
        QCOMPARE(mod.identifier, QStringLiteral("ImportedMod"));
        QCOMPARE(mod.version, QStringLiteral("1.2.3"));
        QVERIFY(isMeta); // 仅 depends 无 install → 判为元包
        QCOMPARE(mod.depends.size(), 2);
    }

    void importZipWithEmbeddedCkan()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray meta = "{"
            "\"spec_version\":\"v1.6\","
            "\"identifier\":\"ZipMod\","
            "\"name\":\"Zip Mod\","
            "\"version\":\"3.0\","
            "\"install\":[{\"file\":\"ZipMod\",\"install_to\":\"GameData\"}]\n}"
        ;
        const QByteArray zip = makeZip({
            {QStringLiteral("ZipMod/CHANGELOG.txt"), QByteArray("change")},
            {QStringLiteral("ZipMod/ZipMod.ckan"), meta},
        });
        QFile f(dir.filePath(QStringLiteral("ZipMod.zip")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(zip);
        f.close();

        CKan ckan(dir.path(), QStringLiteral("T"));
        QString error;
        const CkanModule mod = ckan.importModuleFile(f.fileName(), nullptr, &error);
        QVERIFY2(mod.isValid(), qPrintable(error));
        QCOMPARE(mod.identifier, QStringLiteral("ZipMod"));
        QCOMPARE(mod.version, QStringLiteral("3.0"));
    }

    void importZipRejectsNoMetadata()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // 无 .ckan 且索引匹配不到哈希 → 拒绝（参照官方 ModuleImporter）
        const QByteArray zip = makeZip({{QStringLiteral("SomeMod/x.dll"), QByteArray("x")}});
        QFile f(dir.filePath(QStringLiteral("NoMeta.zip")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(zip);
        f.close();

        CKan ckan(dir.path(), QStringLiteral("T"));
        QString error;
        const CkanModule mod = ckan.importModuleFile(f.fileName(), nullptr, &error);
        QVERIFY(!mod.isValid());
        QVERIFY(!error.isEmpty());
    }
};

class TestCkanInstalledBrowse : public QObject
{
    Q_OBJECT
private slots:
    // 已安装模组（注册表文件归属）+ AD 模组（DLL 扫描）→ GameData 顶层条目
    void installedEntriesCoverRegistryAndAd()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("T"));
        // 注册表已安装：目录模组（含多层子路径）+ 直接放 GameData 根的单文件模组
        InstalledModule im;
        im.identifier = QStringLiteral("DirMod");
        im.module = makeModule(QStringLiteral("DirMod"), QStringLiteral("1.0"));
        im.files = {QStringLiteral("GameData/DirMod/Plugins/x.dll"),
                    QStringLiteral("GameData/DirMod/GameData/extra.cfg")};
        gi.registry()->registerModule(im);
        InstalledModule single;
        single.identifier = QStringLiteral("RootDll");
        single.module = makeModule(QStringLiteral("RootDll"), QStringLiteral("1.0"));
        single.files = {QStringLiteral("GameData/RootDll.dll")};
        gi.registry()->registerModule(single);
        // AD 模组：DLL 扫描路径
        gi.registry()->installedDlls[QStringLiteral("AdMod")] =
            QStringLiteral("GameData/AdMod/AdMod.dll");
        QVERIFY2(gi.saveRegistry(), "save registry failed");

        CKan ckan(dir.path(), QStringLiteral("T"));
        ckan.reloadRegistry(); // CKan 构造不自动加载注册表，需显式从磁盘读入
        QCOMPARE(ckan.installedGameDataEntries(QStringLiteral("DirMod")),
                 QStringList({QStringLiteral("GameData/DirMod")}));
        QCOMPARE(ckan.installedGameDataEntries(QStringLiteral("RootDll")),
                 QStringList({QStringLiteral("GameData/RootDll.dll")}));
        QCOMPARE(ckan.installedGameDataEntries(QStringLiteral("AdMod")),
                 QStringList({QStringLiteral("GameData/AdMod")}));
        // 未安装/未知标识符 → 空
        QVERIFY(ckan.installedGameDataEntries(QStringLiteral("Nope")).isEmpty());
    }

    // AD 模组版本：按 DLL 文件名点号后缀推导（标识符为点前部分，其后即版本）
    void adVersionFromDllFilename()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("T"));
        gi.registry()->installedDlls[QStringLiteral("ModuleManager")] =
            QStringLiteral("GameData/ModuleManager/ModuleManager.4.2.3.dll");
        QVERIFY2(gi.saveRegistry(), "save registry failed");

        CKan ckan(dir.path(), QStringLiteral("T"));
        ckan.reloadRegistry();
        QCOMPARE(ckan.autoDetectedVersion(QStringLiteral("ModuleManager")),
                 QStringLiteral("4.2.3"));
    }

    // AD 模组版本：文件名无版本部分且非有效 PE 文件 → 空（调用方回退标记最新版）
    void adVersionFallsBackEmpty()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        GameInstance gi(dir.path(), QStringLiteral("T"));
        gi.registry()->installedDlls[QStringLiteral("SomeTool")] =
            QStringLiteral("GameData/SomeTool/SomeTool.dll");
        QVERIFY2(gi.saveRegistry(), "save registry failed");

        CKan ckan(dir.path(), QStringLiteral("T"));
        ckan.reloadRegistry();
        QVERIFY(ckan.autoDetectedVersion(QStringLiteral("SomeTool")).isEmpty());
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
    TestFileLock tFileLock;
    TestDownloader tDownloader;
    TestRepoIndex tRepoIndex;
    TestModuleDownload tModDownload;
    TestTransactionRollback tTxRollback;
    TestCkanExport tCkanExport;
    TestModpackIO tModpackIO;
    TestCkanHistoryImport tCkanHistoryImport;
    TestCkanInstalledBrowse tCkanInstalledBrowse;
    failures += runSuite(argc, argv, tModVer);
    failures += runSuite(argc, argv, tGameVer);
    failures += runSuite(argc, argv, tRel);
    failures += runSuite(argc, argv, tMod);
    failures += runSuite(argc, argv, tInstall);
    failures += runSuite(argc, argv, tReg);
    failures += runSuite(argc, argv, tGameInst);
    failures += runSuite(argc, argv, tResolver);
    failures += runSuite(argc, argv, tFileLock);
    failures += runSuite(argc, argv, tDownloader);
    failures += runSuite(argc, argv, tModDownload);
    failures += runSuite(argc, argv, tRepoIndex);
    failures += runSuite(argc, argv, tTxRollback);
    failures += runSuite(argc, argv, tCkanExport);
    failures += runSuite(argc, argv, tModpackIO);
    failures += runSuite(argc, argv, tCkanHistoryImport);
    failures += runSuite(argc, argv, tCkanInstalledBrowse);
    return failures;
}

#include "test_libckan.moc"