# libckan API 文档（中文）

> 本文档描述 **libckan** 的公开 C++ API —— 用 C++17 + Qt6 重写的 CKAN 核心库。
> 所有公开类位于 `ckan` 命名空间，并通过 `CKAN_API` 宏全量导出。
> 头文件从 `src/ckan` 引入；建议只通过 `ckan::CKan` 门面 + `CKanConfig` 调用。
>
> English version: [API.md](API.md)。

---

## 目录

1. [编译与链接](#编译与链接)
2. [快速上手](#快速上手)
3. [门面类：`CKan`](#1-门面类ckan)
4. [运行配置：`CKanConfig`](#2-运行配置ckanconfig)
5. [版本：`ModuleVersion` / `GameVersion` / `GameVersionRange`](#3-版本)
6. [模组元数据：`CkanModule` 及配套类型](#4-模组元数据)
7. [依赖关系：`Relationship`](#5-依赖关系)
8. [安装规则：`ModuleInstallDescriptor`](#6-安装规则)
9. [注册表：`Registry` / `InstalledModule`](#7-注册表)
10. [仓库：`Repository`](#8-仓库)
11. [游戏实例：`GameInstance`](#9-游戏实例)
12. [依赖解析：`RelationshipResolver`](#10-依赖解析)
13. [安装器：`ModuleInstaller` / `InstallResult`](#11-安装器)
14. [仓库索引：`RepoIndex`](#12-仓库索引)
15. [下载器：`Downloader`](#13-下载器)
16. [事务文件管理器：`TxFileManager`](#14-事务文件管理器)
17. [跨进程文件锁：`FileLock`](#15-跨进程文件锁)
18. [整合包导入：`modpackio.h`](#16-整合包导入)
19. [线程安全与错误约定](#17-线程安全与错误约定)

---

## 编译与链接

库以共享库形式构建（`libckan.dll` / `libckan.so` / `libckan.dylib`）。

- **消费方**链接 `libckan` 并定义 `CKAN_BUILD_SHARED`（Windows 上启用 `dllimport`）。
- 库自身以 `CKAN_BUILD_SHARED + CKAN_BUILDING_LIB` 编译（启用 `dllexport`）。

```cmake
target_link_libraries(your_app PRIVATE libckan)
target_compile_definitions(your_app PRIVATE CKAN_BUILD_SHARED)
```

引入门面头文件：

```cpp
#include <ckan.h>
using namespace ckan;
```

---

## 快速上手

```cpp
#include <ckan.h>
using namespace ckan;

// 1. 配置：索引缓存目录、代理、镜像前缀、并发数…
CKanConfig cfg;
cfg.indexCacheDir = "cache/index";
cfg.proxyUrl = "";                                  // 空 = 直连
cfg.downloadConcurrency = 3;                        // 并行下载数 1~8

// 2. 打开游戏实例（必要时按需创建 CKAN/ 目录结构）
CKan ckan("D:/Steam/steamapps/common/Kerbal Space Program", "KSP 1.12.5",
          cfg);                                     // 全部配置经构造传入

// 3. 刷新仓库索引并搜索
QString err;
QVector<Repository> repos = { Repository::defaultKspRepo() };
if (!ckan.refreshIndex(repos, &err)) { /* 处理 err */ }
auto mods = ckan.search("MechJeb");

// 4. 解析完整安装集（含依赖）
ResolutionResult plan = ckan.resolveInstall(mods.first());
if (plan.conflicted || plan.missing) { /* 处理 */ }

// 5. 分两阶段：先下载、再安装（在后台线程调用）
QStringList conflicts;
if (!ckan.downloadModules(plan.modulesToInstall, "cache/downloads",
                          /*preferModuleMirrors=*/false, 3, &conflicts, &err)) { /* 处理 */ }
InstallResult r = ckan.installFromCache(plan.modulesToInstall, "cache/downloads", conflicts);
// r.ok / r.error / r.cancelled / r.installedIdentifiers
```

> **注意**：`CKan` 没有 `init()` 方法——配置完全通过构造函数传入
> （游戏目录、实例名、`CKanConfig`）。

---

## 1. 门面类：`CKan`

`src/ckan/ckan.h` —— 提供给启动器的唯一公开入口。封装仓库索引、注册表、依赖解析、
下载/安装（含冲突检测与事务回滚）。内部实现（`GameInstance` 等）一律不透出。

```cpp
CKAN_API class CKan
{
public:
    explicit CKan(const QString &gameDir, const QString &instanceName,
                  const CKanConfig &config = {});
    ~CKan();
    ...
};
```

### 实例基本信息

| 签名 | 说明 |
|---|---|
| `QString gameDir() const` | 该实例的游戏目录。 |
| `QString historyDir() const` | 安装历史目录（`<gameDir>/CKAN/history`）。 |
| `GameVersion detectedVersion() const` | 实例实际检测到的 KSP 版本；检测失败返回无效版本。 |
| `void reloadRegistry()` | 重新从 `registry.json` 加载已安装数据。 |
| `bool tryAcquireRegistryLock()` | 尝试获取 `registry.locked` 写锁。返回 `false` 表示被其他进程（官方 CKAN / 另一启动器）占用；用于加载索引前门控等待。 |

### 仓库索引 / 模组列表

| 签名 | 说明 |
|---|---|
| `bool refreshIndex(const QVector<Repository> &repos, QString *error = nullptr, bool force = false, qint64 maxAgeSecs = RepoIndex::kDefaultCacheAgeSecs, bool preferMirror = false, const std::function<void(const QString &, qint64, qint64)> &onProgress = {}, std::atomic_bool *cancelFlag = nullptr)` | 从多个仓库下载并按优先级合并建立索引（`priority` 值越小优先级越高）。默认使用缓存，`force=true` 强制重下；单仓库失败回退其旧缓存。`onProgress(repoName, received, total)`；`cancelFlag` 置真则中止。镜像前缀 / 缓存目录 / 代理取构造传入的 `CKanConfig`（镜像仅对 GitHub 托管的仓库生效）。 |
| `QVector<CkanModule> search(const QString &query) const` | 按名称 / 标识符搜索模组。 |
| `QVector<CkanModule> versionsOf(const QString &identifier) const` | 某标识符的全部版本。 |
| `CkanModule latestOf(const QString &identifier) const` | 某标识符的最新版本（不存在时返回无效模块）。 |
| `bool indexReady() const` | 索引是否已加载。 |
| `int indexSize() const` | 索引中的标识符数量。 |
| `QStringList allIdentifiers() const` | 索引中全部标识符（用于精确清理下载缓存）。 |
| `int downloadCount(const QString &identifier) const` | 该标识符的下载次数（来自仓库 `download_counts.json`）；无数据返回 `-1`。 |
| `bool hasDownloadCount(const QString &identifier) const` | 该标识符是否有下载次数数据。 |

### 已安装查询

| 签名 | 说明 |
|---|---|
| `QString installedVersion(const QString &identifier) const` | 已安装版本字符串；未安装为空。 |
| `bool isInstalled(const QString &identifier) const` | 该标识符是否已安装。 |
| `QVector<InstalledModule> installedModules() const` | 全部已安装模组。 |
| `QStringList installedGameDataEntries(const QString &identifier) const` | 该标识符已安装模组的 GameData 顶层条目（相对游戏目录，如 `GameData/SomeMod` 或直接放根的单文件 `GameData/single.dll`）。覆盖注册表文件归属与手动安装（AD，DLL 扫描）模组的 DLL 路径。 |

### 整合包导出

| 签名 | 说明 |
|---|---|
| `QByteArray exportModpackCkan(QString *error = nullptr)` | 生成官方 CKAN 元包（metapackage）JSON：`depends` 列出已安装模组（无版本号），排除 DLC / 自动安装 / 手动安装（AD）模组；索引已加载时同样排除索引中不存在的模组。依赖拓扑序。无可导出模组时返回空并填充 `error`。检测不到 KSP 版本时省略 `ksp_version_min/max`（不算失败）。 |
| `bool writeHistorySnapshot(QString *error = nullptr)` | 写安装历史快照到 `historyDir()`（`已安装-{实例}-{时间戳}.ckan`），只保留最近 `kMaxHistoryCount` 条。尽力而为，失败填充 `error`。 |

### 导入单模组文件

| 签名 | 说明 |
|---|---|
| `CkanModule importModuleFile(const QString &path, bool *isMetapackage = nullptr, QString *error = nullptr)` | 从本地 `.zip` 或 `.ckan` 解析出一个 `CkanModule`。`.ckan`：按 JSON 解析，`kind=metapackage` 或无 install 规则但含 `depends` 时置 `*isMetapackage=true`。`.zip`：先扫压缩包内嵌 `*.ckan` 元数据，再用文件 SHA256 匹配仓库已知下载哈希；仍不匹配则失败并填充 `error`（对齐官方“无元数据拒绝”行为）。解析失败返回无效模块。 |
| `QString importStoreCache(const CkanModule &mod, const QString &sourcePath, const QString &downloadDir, QString *error = nullptr)` | 把导入的本地文件复制进缓存目录（命名 `{id}_{safeVersion}.zip`，使 `installFromCache`/`findCacheZip` 能命中）。返回写入的缓存路径；失败返回空并填充 `error`。 |

### 手动安装模组（DLL 扫描，AD）

| 签名 | 说明 |
|---|---|
| `void scanUnmanagedDlls()` | 扫描 GameData 下 `.dll` 写入 `registry.installedDlls` 并保存。结果缓存，重复调用直接返回。 |
| `bool dllsScanned() const` | 本次 DLL 扫描是否已完成（用于“正在扫描/已就绪”提示）。 |
| `bool isAutoDetected(const QString &identifier) const` | 该标识符是否为手动安装（AD）模组。 |
| `QString autoDetectedVersion(const QString &identifier) const` | AD 模组的已装版本（尽力推导，可能为空）：(1) 按官方 DllScanner 语义从 DLL 文件名推导（标识符为点前部分，其后即版本，如 `ModuleManager.4.2.3.dll` → `4.2.3`）；(2) 回退读取 DLL 内部文件版本资源（仅 Windows，PE 版本信息）；(3) 仍失败返回空。 |

### 依赖解析

| 签名 | 说明 |
|---|---|
| `ResolutionResult resolveInstall(const CkanModule &mod, bool autoInstallRecommends = true, bool withSuggests = false)` | 解析安装该模组所需的完整集合（含依赖）。 |
| `ResolutionResult resolveInstallMany(const QVector<CkanModule> &mods, bool autoInstallRecommends = true, bool withSuggests = false, const GameVersionRange &extraRange = GameVersionRange(), bool collectRecommends = false)` | 一次性解析多个模组的完整安装集（含相互依赖）。`extraRange`：用户勾选的额外兼容区间（无效表示未启用）；候选兼容当前实例版本或兼容该区间即算兼容。`collectRecommends`：把本批的推荐模组（Recommends）收集到 `result.recommendedModules` 而非自动安装（对齐官方安装对话框的推荐勾选；收集模式下 `autoInstallRecommends` 失效）。 |

### 安装流程（分两阶段，供后台线程调用）

| 签名 | 说明 |
|---|---|
| `bool downloadModules(const QVector<CkanModule> &modules, const QString &downloadDir, bool preferModuleMirrors, int concurrency = 3, QStringList *conflicts = nullptr, QString *error = nullptr, const std::function<void(const QString &, qint64, qint64, qint64)> &onByteProgress = {}, std::atomic_bool *cancelFlag = nullptr)` | **阶段一**。把全部 zip 下载到 `downloadDir`，并按 zip 实际内容计算与手动占用文件夹的冲突。`concurrency` 为 1~8。`conflicts`（已排序去重）输出冲突的 GameData 顶层文件夹（相对 GameDir，如 `GameData/SomeMod`）。`onByteProgress(identifier, doneBytes, totalBytes, speedBps)`；经 `cancelFlag` 或 `cancelInstall()` 中止。 |
| `InstallResult installFromCache(const QVector<CkanModule> &modules, const QString &downloadDir, const QStringList &foldersToDelete = {}, const QStringList &preUninstall = {}, const std::function<void(const QString &, int)> &onInstallProgress = {})` | **阶段二**。单事务从缓存安装：先卸载 `preUninstall` 中的旧版，再写入。`foldersToDelete` 为相对 GameData 的顶层文件夹（写入前先递归删除旧文件夹）。任一步失败（含用户取消）整体回滚：恢复被删旧文件、删除新写入文件、还原注册表。`onInstallProgress(identifier, percent 0~100)` 在后台线程回调。 |
| `InstallResult uninstall(const QString &identifier)` | 卸载单个模组（单事务，失败整体回滚）。走共享安装器，可被 `cancelInstall()` 中止；中止时已回滚，返回 `cancelled=true`。 |
| `InstallResult uninstallMany(const QStringList &identifiers)` | 整批卸载多个标识符：共享同一事务实现整批原子，成功统一提交、失败/取消整体回滚。 |
| `QStringList uninstallPlan(const QStringList &identifiers)` | 只读：一次性卸载 `identifiers` 的级联顺序（含目标，依赖者在前）。任一未安装返回空。 |
| `void cancelInstall()` | 请求中止当前安装/下载任务（线程安全）。 |
| `void releaseInstaller()` | 安装流程结束后释放内部安装器（下载/安装均完成后调用）。 |

### 下载缓存辅助（静态）

| 签名 | 说明 |
|---|---|
| `static QString safeCacheFileName(const QString &s)` | 清洗缓存文件名中的非法字符（Windows 不含 `: \ / ? * < > | "`）—— 如 `1:3.4.0` → `1_3.4.0`，防止带 epoch 的版本经 NTFS ADS 破坏缓存。 |
| `static QString officialCacheFileName(const QString &identifier, const QString &version, const QString &downloadUrl = QString())` | 官方 CKAN 缓存文件名：`{SHA1(URL)[:8]}-{identifier}-{version}.zip`；URL 为空时退化为 `{identifier}-{version}.zip`。 |
| `static QString findCacheZip(const QString &downloadDir, const CkanModule &mod)` | 查找缓存目录中该模组实际存在的有效缓存文件：官方格式优先，其次手动下载格式，最后本工程格式；均不存在或无效返回空。 |
| `static qint64 estimateRequiredBytes(const QVector<CkanModule> &modules, double bufferFactor = 1.15)` | 估算所需磁盘空间（字节）：非元包模组 `downloadSize` 之和 × `bufferFactor`。 |

### 静态工具

| 签名 | 说明 |
|---|---|
| `static GameVersion detectVersionFromDir(const QString &gameDir)` | 从游戏目录检测 KSP 版本，无需构造实例（供实例发现时显示版本号）；失败返回无效版本。 |

---

## 2. 运行配置：`CKanConfig`

`src/ckan/ckanconfig.h` —— 一次性传入 `CKan` 构造函数的运行配置。替代原先的全局静态配置
（`Downloader::setProxyUrl` / `RepoIndex::setCacheDir` 等），避免库内部可变的全局状态。

| 字段 | 类型 | 说明 |
|---|---|---|
| `indexCacheDir` | `QString` | 索引缓存目录（空 = 不落盘缓存）。 |
| `proxyUrl` | `QString` | 网络代理，如 `http://127.0.0.1:7890`（空 = 直连）。 |
| `indexMirrorPrefixes` | `QStringList` | 索引下载镜像前缀（仅 GitHub 托管仓库生效）。 |
| `moduleMirrorPrefixes` | `QStringList` | 模组下载镜像前缀。 |
| `downloadConcurrency` | `int` | 模组并行下载数，默认 `3`。 |
| `downloadRateLimitBps` | `qint64` | 单链接下载限速（字节/秒），`0` = 不限速。 |

静态辅助：

```cpp
static inline QStringList defaultIndexMirrorPrefixes();   // { "https://gh-proxy.com/", "https://ghfast.top/" }
static inline QStringList defaultModuleMirrorPrefixes();  // { "https://gh-proxy.com/", "https://ghfast.top/" }
```

---

## 3. 版本

`src/ckan/version.h` —— 与官方 CKAN 完全兼容的语义化版本。

### `ModuleVersion`

格式 `[epoch:]version`，比较规则与官方 CKAN 一致（epoch、build 忽略）。

```cpp
class CKAN_API ModuleVersion {
public:
    explicit ModuleVersion(const QString &versionString);
    bool isValid() const;
    int  epoch() const;
    QString versionPart() const;
    QString toString() const;
    int compareTo(const ModuleVersion &other) const;
    bool equals(const ModuleVersion &other) const;
    // == != < > <= >= 运算符
};
```

### `GameVersion`

KSP 版本 `major.minor.patch[.build]`。与官方 `GameVersion` 一致：记录每个分量是否显式声明
（未声明分量数值为 0，但其 `isXxxDefined()` 为 `false`）。

```cpp
class CKAN_API GameVersion {
public:
    explicit GameVersion(const QString &versionString);
    GameVersion(int major, int minor, int patch = 0, int build = 0);
    GameVersion();                              // 无效
    bool isValid() const;
    int major() const; int minor() const; int patch() const; int build() const;
    bool isMajorDefined() const; bool isMinorDefined() const;
    bool isPatchDefined() const; bool isBuildDefined() const;
    GameVersion withoutBuild() const;
    QString toString() const;
    GameVersionRange toVersionRange() const;    // 展开为半开区间
    int compareWithoutBuild(const GameVersion &other) const;
    int compareTo(const GameVersion &other) const;
    // < > <= >= == 运算符（比较时忽略 build）
};
```

### `GameVersionRange`

版本区间，边界可能开可能闭（未设置一侧为无界）。

```cpp
class CKAN_API GameVersionRange {
public:
    GameVersionRange();
    GameVersionRange(const GameVersion &lower, bool lowerInclusive,
                     const GameVersion &upper, bool upperInclusive);
    bool contains(const GameVersion &value) const;        // 官方 Contains：与 value.toVersionRange() 求交
    bool intersects(const GameVersionRange &other) const; // 官方 Intersects
    bool lowerSet() const; bool upperSet() const;
    bool lowerInclusive() const; bool upperInclusive() const;
    GameVersion lower() const; GameVersion upper() const;
};

// 由勾选的版本线集合（如 {"1.9","1.10","1.11","1.12"}）构造连续兼容区间
// （取最小版本线展开下界 ~ 最大版本线展开上界，如 [1.9.0.0, 1.13.0.0)）。
// 空集合或全部无效时返回无效区间（调用方应回退为仅按当前实例版本判断）。
CKAN_API GameVersionRange versionLinesToRange(const QStringList &versionLines);
```

**示例**

```cpp
ModuleVersion a("1.2.3"), b("1:1.2.3");   // 1:1.2.3 > 1.2.3（epoch=1）
bool newer = b > a;                        // true

GameVersion v("1.12.5");
GameVersionRange r = versionLinesToRange({"1.9", "1.10", "1.11", "1.12"});
bool ok = r.contains(v);                   // true（1.9.0.0 <= 1.12.5 < 1.13.0.0）
```

---

## 4. 模组元数据

`src/ckan/ckanmodule.h`

### 枚举与结构体

```cpp
enum class ModuleKind { Package, Metapackage, Dlc };   // 普通包 / 元包 / 官方 DLC

enum class ReleaseStatus {      // stable < testing < development
    Stable      = 0,            // JSON: "stable" / 缺省
    Testing     = 1,            // JSON: "testing" / "beta"
    Development = 2,            // JSON: "development" / "alpha"
};

struct CKAN_API DownloadHash { QString sha1; QString sha256; };

struct CKAN_API ResourceLinks {
    QString homepage, repository, bugtracker, license, manual, spacedock, curseforge, github;
};
```

### `CkanModule`

```cpp
class CKAN_API CkanModule {
public:
    static CkanModule fromJson(const QByteArray &json, QString *error = nullptr);
    static CkanModule fromJsonObject(const QJsonObject &obj, QString *error = nullptr);

    bool isValid() const;                      // !identifier.isEmpty() && !version.isEmpty()
    QString toString() const;                  // name + " " + version
    bool isMetapackage() const; bool isDlc() const;

    bool isCompatible(const GameVersion &kspVersion) const;
    bool isCompatible(const GameVersionRange &range) const;   // 兼容范围与该区间相交即兼容
    GameVersionRange compatibilityRange() const;
    QVector<ModuleInstallDescriptor> effectiveInstallStanzas() const;  // 默认：find=identifier, install_to=GameData
    QStringList providesList() const;          // 提供的所有虚拟包（含自身 identifier）

    // ---- 字段 ----
    QString identifier, name, version, abstract, description, comment;
    QString specVersion, downloadContentType, releaseDate;
    long long downloadSize = 0, installSize = 0;
    ModuleKind kind = ModuleKind::Package;
    ReleaseStatus releaseStatus = ReleaseStatus::Stable;
    QStringList author, license, tags, localizations, downloadUrls;
    DownloadHash downloadHash;
    ResourceLinks resources;
    QString kspVersion, kspVersionMin, kspVersionMax;
    bool kspVersionStrict = false;
    QVector<Relationship> depends, recommends, suggests, supports, conflicts, provides;
    QVector<ModuleInstallDescriptor> install;

    QJsonObject toJsonObject() const;          // 与 fromJsonObject 完整往返
    QByteArray  toJson() const;
};
```

**示例**

```cpp
QByteArray json = R"({
  "spec_version": "v1.4",
  "identifier": "ModuleManager",
  "name": "Module Manager",
  "version": "4.2.3",
  "download": "https://github.com/sarbian/ModuleManager/releases/download/v4.2.3/ModuleManager.4.2.3.zip",
  "download_hash": { "sha256": "..." },
  "ksp_version": "1.12"
})";
QString err;
CkanModule mod = CkanModule::fromJson(json, &err);
if (mod.isValid() && mod.isCompatible(GameVersion("1.12.5"))) { /* 可安装 */ }
```

---

## 5. 依赖关系

`src/ckan/relationship.h`

```cpp
class CKAN_API Relationship {
public:
    enum class Type { Depends, Recommends, Suggests, Supports, Conflicts, Provides };

    Type type = Type::Depends;
    QString name;            // 目标模块 identifier 或虚拟包名
    QString version;         // 可选版本约束，如 ">=1.2"，空表示任意
    QString minVersion, maxVersion;
    bool minInclusive = false, maxInclusive = false;
    QVector<Relationship> anyOf;   // any_of 关系：任一子依赖满足即可（子关系继承 type）

    bool isVirtual() const;                       // 由 provides 列表在解析时判断
    bool versionSatisfies(const QString &installedVersion) const;
    QJsonObject toJsonObject() const;
};
```

---

## 6. 安装规则

`src/ckan/moduleinstalldescriptor.h`

```cpp
struct CKAN_API InstallableFile {
    QString sourceName;      // zip 内原始路径
    QString destination;     // 转换后的目标相对路径，如 GameData/foo/bar.dll
    bool    makeDir = false;
};

class CKAN_API ModuleInstallDescriptor {
public:
    static ModuleInstallDescriptor defaultStanza(const QString &identifier);
    static bool fromJsonObject(const QJsonObject &obj, ModuleInstallDescriptor *out, QString *error);

    bool isWanted(const QString &path, int *matchIndex) const;      // zip 内路径是否被此规则选中
    QString transformOutputName(const QString &outputName, const QString &installDirInGame) const;
    QJsonObject toJsonObject() const;
    QString resolveInstallBaseDir(const QString &primaryModDir) const;  // 安装基准目录（install_to 解析后）
    QVector<InstallableFile> findInstallableFiles(const QStringList &zipEntries,
                                                  const QString &primaryModDir,
                                                  QString *error) const; // 遍历 zip 条目筛选

    // ---- 字段 ----
    QString file;            // 精确路径
    QString find;            // 目录名
    QString findRegexp;      // 正则
    bool    findMatchesFiles = false;
    QString installTo;       // GameData / GameRoot / Ships 等
    QString as;              // 重命名第一级目录
    QStringList filter, filterRegexp, includeOnly, includeOnlyRegexp;

    bool isValid() const;    // file/find/findRegexp 任一非空
};
```

---

## 7. 注册表

`src/ckan/registry.h` / `src/ckan/installedmodule.h`

### `Registry`

读写 `registry.json`，`installed_modules` / `installed_files` / `sorted_repositories` 与官方 CKAN 完全兼容。

```cpp
class CKAN_API Registry {
public:
    static const int LATEST_REGISTRY_VERSION = 3;

    static Registry fromJson(const QByteArray &json, QString *error = nullptr);
    bool loadFromJson(const QByteArray &json, QString *error = nullptr);   // 原地装载，跨线程安全
    void clear();                                                          // 清空为默认空注册表，跨线程安全
    QByteArray toJson() const;                                             // 跨线程安全
    QRecursiveMutex *mutex() const;                                        // 供复合读/写循环使用的共享锁（递归）

    QMap<QString, Repository> repositories;
    void setRepositories(const QMap<QString, Repository> &repos);
    QMap<QString, InstalledModule> installedModules;   // identifier -> InstalledModule
    QHash<QString, QString> installedFiles;            // 文件相对路径 -> identifier（文件归属）
    QMap<QString, QString> installedDlls;              // 手动安装的 dll：identifier -> 相对路径
    int registryVersion = LATEST_REGISTRY_VERSION;
    bool isValid() const;

    InstalledModule *installed(const QString &identifier);
    const InstalledModule *installed(const QString &identifier) const;
    QString installedVersion(const QString &identifier) const;
    bool isInstalled(const QString &identifier) const;
    QString fileOwner(const QString &relativePath) const;

    void registerModule(const InstalledModule &im);     // 注册（同时更新 installedFiles）
    void unregisterModule(const QString &identifier);   // 卸载（删除其文件归属）
};
```

> `Registry` 的所有拷贝共享同一把 `QRecursiveMutex`（经 `shared_ptr` 持有），按值传的生产主实例
> 锁保持稳定。复合读/写循环请持 `mutex()`。

### `InstalledModule`

`registry.json` 中 `installed_modules` 的每条记录。

```cpp
class CKAN_API InstalledModule {
public:
    QString identifier;
    CkanModule module;
    QStringList files;             // 相对 GameRoot 的文件路径
    bool autoInstalled = false;
    QString installTime;           // ISO 8601 UTC
    bool isValid() const;
    QJsonObject toJsonObject() const;
    static InstalledModule fromJsonObject(const QJsonObject &obj);
};
```

---

## 8. 仓库

`src/ckan/repository.h`

```cpp
struct CKAN_API Repository {
    QString name;
    QString uri;
    int     priority = 0;      // 值越小优先级越高
    bool    mirror   = false;
    QString comment;
    bool isValid() const;      // !name.isEmpty() && !uri.isEmpty()

    static Repository defaultKspRepo();
    static QString defaultRepoUrl();
    static QString repositoryListUrl();

    // 预设备用仓库（供设置页“添加预设”）
    static Repository presetKspCkanBackup();  // KSP-CKAN 备用（GitLab 归档）
    static Repository presetSol();            // Sol / RSS-Reborn
    static Repository presetMechJeb2Dev();    // MechJeb2-dev（CI 构建）
};
```

默认仓库为 `KSP-CKAN/CKAN-meta` 的 master tar.gz。

---

## 9. 游戏实例

`src/ckan/gameinstance.h` —— 管理单个 KSP 游戏目录下的 CKAN 数据目录。常规使用走 `CKan` 门面，
仅在需要内部能力时直接使用本类。

```cpp
class CKAN_API GameInstance {
public:
    GameInstance();
    GameInstance(const QString &gameDir, const QString &name);
    void setupCkanDirectories();                      // 确保 CKAN/ 目录结构存在

    QString gameDir() const; QString name() const;
    QString ckanDir() const;                          // <gameDir>/CKAN
    QString downloadDir() const;                      // <gameDir>/CKAN/downloads
    QString historyDir() const;                       // <gameDir>/CKAN/history
    QString registryPath() const;                     // <gameDir>/CKAN/registry.json
    QString compatibleVersionsPath() const;           // <gameDir>/CKAN/compatible_ksp_versions.json

    QString toRelativeGameDir(const QString &abs) const;
    QString toAbsoluteGameDir(const QString &rel) const;

    GameVersion detectVersion() const;                // 优先 buildID 文件经 build 映射表，其次 readme 兜底
    static GameVersion detectVersionFromDir(const QString &gameDir);   // 只读版本检测，无副作用

    static QStringList detectInstallKindTags(const QString &gameDir, bool *corrupted = nullptr);
    static QString suggestedInstanceName(const QString &gameDir);

    QMap<QString, QString> scanUnmanagedDlls() const; // identifier -> 相对 GameDir 路径
    QStringList manualGameDataFolders() const;        // 手动占用的 GameData 顶层文件夹

    Registry *registry();
    const Registry *registry() const;
    void loadRegistry();
    bool saveRegistry() const;                        // 拿不到锁时跳过写入并返回 false
    bool registryLockHeld() const;
    bool engageRegistryLock();                        // 尝试获取写锁（其他进程占用返回 false）
    void restoreRegistrySnapshot(const QByteArray &json);   // 事务回滚：还原内存并写回 registry.json

    bool isValid() const;                             // 存在游戏文件

    void setCustomDownloadDir(const QString &dir);
    QString customDownloadDir() const;
};
```

**版本检测**：优先读 `buildID64.txt` / `buildID.txt`（`build id = NNNN`，忽略前导 0），经内置
build ID → 版本映射表（数据源自官方 `builds-ksp.json`，覆盖 0.23.0.395 ~ 1.12.5.3190）换算；
双文件去重取版本最大值。未命中回退解析 `readme.txt` 版本行；全部失败返回无效版本。

---

## 10. 依赖解析

`src/ckan/relationshipresolver.h`

### 结果类型

```cpp
struct CKAN_API ProviderChoice {
    QString provides;                // 虚拟包名
    QStringList requiredBy;          // 依赖它的模块标识符（可能多个）
    QString requirement;             // 约束描述（如版本范围，可能为空）
    QVector<CkanModule> candidates;  // 满足约束且 KSP 兼容的候选提供者（按版本降序）
};

struct CKAN_API ResolutionResult {
    QVector<CkanModule> modulesToInstall;    // 按依赖顺序（依赖在前）
    QVector<CkanModule> recommendedModules;  // 收集的推荐安装模组（Recommends；仅 collectRecommends=true 时收集，
                                             // 不随 modulesToInstall 自动安装，由 UI 层弹窗勾选）
    QVector<CkanModule> suggestedModules;    // 级联建议的可选模组（仅收集，不自动安装）
    QVector<ProviderChoice> providerChoices; // 需用户选择的多提供者（非空时 UI 应先弹窗处理）
    QStringList notFound;                    // 无法满足的依赖
    QStringList conflicts;                   // 冲突描述
    bool conflicted = false;
    bool missing    = false;
};
```

### `RelationshipResolver`

```cpp
class CKAN_API RelationshipResolver {
public:
    explicit RelationshipResolver(const QMap<QString, QVector<CkanModule>> &index);

    ResolutionResult resolve(const QVector<CkanModule> &modulesToInstall,
                             const Registry &registry,
                             bool autoInstallRecommends = true,
                             bool withSuggests = false,
                             const GameVersion &kspVersion = GameVersion(),
                             const GameVersionRange &extraRange = GameVersionRange(),
                             bool collectRecommends = false);
};
```

解析规则：

- BFS 依赖展开；虚拟包（`provides`）建立索引并解析。
- 每个依赖选**最高**满足约束且 KSP 兼容的版本（兼容 `kspVersion` 或 `extraRange` 任一；无效版本视为不过滤）。
- 已安装模组满足版本约束才算已满足；否则选新版升级。
- 冲突做**双向**检测（新模块声明的 + 已选模块声明的，含版本约束）。
- 推荐/建议模组与已选集合冲突时静默跳过；硬依赖冲突计入 `conflicts`。
- `collectRecommends=true` 时把推荐模组（Recommends）收集到 `recommendedModules` 而非自动安装，
  且做级联收集（推荐模组自身的推荐也继续收集）；该模式下 `autoInstallRecommends` 失效。候选去重、按版本降序。
- `providerChoices` 非空时，UI 应让用户选择后重新解析。

---

## 11. 安装器

`src/ckan/moduleinstaller.h` —— `QObject` 派生安装器：下载 zip → miniz 解压 → 按 install 规则
复制到 GameData → 更新注册表。

```cpp
struct CKAN_API InstallResult {
    bool ok = false;
    QString error;
    bool cancelled = false;            // 用户主动取消（卸载场景，已回滚）
    QStringList installedIdentifiers;
};

class CKAN_API ModuleInstaller : public QObject {
    Q_OBJECT
public:
    explicit ModuleInstaller(GameInstance *instance, QObject *parent = nullptr);

    void setProxyUrl(const QString &proxyUrl);   QString proxyUrl() const;
    void setDownloadRateLimitBps(qint64 bps);    qint64 downloadRateLimitBps() const;

    InstallResult install(const QVector<CkanModule> &modules, const QString &downloadDir,
                          const QStringList &foldersToDelete = {},
                          const QStringList &mirrorPrefixes = {},
                          bool preferModuleMirrors = false);

    bool downloadModules(const QVector<CkanModule> &modules, const QString &downloadDir,
                         const QStringList &mirrorPrefixes, bool preferModuleMirrors,
                         QString *error, int maxConcurrent = 3,
                         std::atomic_bool *cancelFlag = nullptr);

    InstallResult installFromCache(const QVector<CkanModule> &modules, const QString &downloadDir,
                                   const QStringList &foldersToDelete = {},
                                   TxFileManager *tx = nullptr);

    static QStringList actualGameDataFolders(const QString &zipPath, const CkanModule &mod,
                                             QString *error = nullptr);

    InstallResult uninstall(const QString &identifier, TxFileManager *tx = nullptr);
    InstallResult uninstallMany(const QStringList &identifiers, TxFileManager *tx = nullptr);
    QStringList uninstallPlan(const QStringList &identifiers);

    void cancel();          // 线程安全中止
    void resetCancel();     // 新一轮操作入口处清空取消标志

    static bool listZipEntries(const QString &zipPath, QStringList *entries, QString *error);
    static QString safeCacheFileName(const QString &s);
    static QString officialCacheFileName(const QString &identifier, const QString &version,
                                         const QString &downloadUrl = QString());
    static QString findCacheZip(const QString &downloadDir, const CkanModule &mod);
    static qint64 estimateRequiredBytes(const QVector<CkanModule> &modules, double bufferFactor = 1.15);

signals:
    void installProgress(const QString &identifier, int percent);
    void moduleInstalled(const QString &identifier);
    void byteProgress(const QString &identifier, qint64 doneBytes, qint64 totalBytes, qint64 speedBps);
};
```

**事务语义**（`installFromCache` / `uninstall` / `uninstallMany`）：

- `tx` 为空时自动创建内部事务：任一步失败（含取消）整体回滚（恢复被覆盖/删除的文件、删除本批
  新写入文件、还原注册表），成功则提交。
- 传入外部 `tx` 时（如升级 = 卸载旧版 + 安装新版合并为单事务），文件操作计入该事务；本方法
  不保存/回滚，由调用方决定 `commit()` / `rollback()`。

---

## 12. 仓库索引

`src/ckan/repoindex.h` —— 下载 `CKAN-meta` tar.gz、解压并建立 `identifier → 版本列表` 索引，
支持落盘缓存。

```cpp
class CKAN_API RepoIndex {
public:
    static constexpr qint64 kDefaultCacheAgeSecs = 6 * 60 * 60;

    static bool parseTarGz(const QByteArray &tarGz,
                           QMap<QString, QVector<CkanModule>> *index,
                           QMap<QString, int> *downloadCounts = nullptr,
                           QString *error = nullptr);

    static bool build(const Repository &repo, const QStringList &mirrors,
                      QMap<QString, QVector<CkanModule>> *index,
                      QMap<QString, int> *downloadCounts = nullptr, QString *error = nullptr,
                      const std::function<void(const QString &, qint64, qint64)> &onProgress = {},
                      std::atomic_bool *cancelFlag = nullptr,
                      bool preferMirror = false, const QString &proxyUrl = QString(),
                      qint64 rateLimitBps = 0);

    static bool buildCached(const Repository &repo, const QStringList &mirrors,
                            QMap<QString, QVector<CkanModule>> *index,
                            QMap<QString, int> *downloadCounts = nullptr, QString *error = nullptr,
                            bool forceRefresh = false, qint64 maxAgeSecs = kDefaultCacheAgeSecs,
                            const std::function<void(const QString &, qint64, qint64)> &onProgress = {},
                            std::atomic_bool *cancelFlag = nullptr,
                            bool preferMirror = false, const QString &cacheDir = QString(),
                            const QString &proxyUrl = QString(), qint64 rateLimitBps = 0);

    static bool buildManyCached(const QVector<Repository> &repos, const QStringList &mirrors,
                                QMap<QString, QVector<CkanModule>> *index,
                                QMap<QString, int> *downloadCounts = nullptr,
                                QString *error = nullptr,
                                bool forceRefresh = false,
                                qint64 maxAgeSecs = kDefaultCacheAgeSecs,
                                const std::function<void(const QString &, qint64, qint64)> &onProgress = {},
                                std::atomic_bool *cancelFlag = nullptr,
                                bool preferMirror = false, const QString &cacheDir = QString(),
                                const QString &proxyUrl = QString(), qint64 rateLimitBps = 0);

    static QVector<CkanModule> versionsFor(const QMap<QString, QVector<CkanModule>> &index,
                                           const QString &identifier);
    static CkanModule latestFor(const QMap<QString, QVector<CkanModule>> &index,
                                const QString &identifier);

    static void mergeSubIndexes(const QVector<QMap<QString, QVector<CkanModule>>> &subIndexes,
                                const QVector<QMap<QString, int>> &subCounts,
                                QMap<QString, QVector<CkanModule>> *index,
                                QMap<QString, int> *downloadCounts = nullptr);
};
```

多仓库行为：

- 仓库按 `priority` 升序处理（值越小优先级越高，先处理者获胜）。
- 同 `identifier+version` 冲突时高优先级仓库优先；下载计数取首个命中。
- 单个仓库失败时回退其旧缓存；至少一个仓库成功即整体成功。
- 镜像前缀仅对 GitHub 托管仓库生效（前缀 + 仓库自身 URL）；非 GitHub 仓库忽略镜像，避免错误回退到其他仓库内容。

---

## 13. 下载器

`src/ckan/downloader.h` —— 简单 HTTP 下载器，支持镜像回退、代理、进度、超时与取消。

```cpp
class CKAN_API Downloader : public QObject {
    Q_OBJECT
public:
    using Validator = std::function<bool(const QByteArray &)>;          // 返回 false → 尝试下一镜像
    using ProgressCallback = std::function<void(qint64 received, qint64 total)>;

    explicit Downloader(QObject *parent = nullptr);

    bool download(const QString &url, const QStringList &mirrors, QByteArray *out,
                  QString *error = nullptr, const Validator &validator = nullptr,
                  bool preferMirror = false);

    bool downloadProgressed(const QString &url, const QStringList &mirrors, QByteArray *out,
                            QString *error = nullptr, const Validator &validator = nullptr,
                            const ProgressCallback &onProgress = nullptr,
                            std::atomic_bool *cancelFlag = nullptr,
                            int resumeAttempts = 0, bool preferMirror = false);

    void downloadAsync(const QString &url, const QStringList &mirrors); // 完成后发信号

    void setProxyUrl(const QString &proxyUrl);  QString proxyUrl() const;
    void setDownloadRate(qint64 bytesPerSecond); qint64 downloadRate() const;

signals:
    void finished(const QByteArray &data, const QString &url);
    void failed(const QString &url, const QString &error);
};
```

要点：

- `downloadProgressed` 在调用线程内驱动事件循环；连接超时与传输空闲超时均 30 秒；`cancelFlag`
  置真立即中止（返回 `false`，error 为「已取消」）。
- `resumeAttempts > 0` 启用断点续传：传输中断（如连接被关闭）后保留已收字节，用 `Range` 请求
  剩余部分；服务器忽略 `Range`（返回 200 全量）时自动清空已收、从零重下。

---

## 14. 事务文件管理器

`src/ckan/txfilemanager.h` —— 让文件操作可回滚（对应官方 `ChinhDo.Transactions.TxFileManager`）。

所有写/删/覆盖操作先把原文件/目录备份到事务子目录；`commit()` 提交（丢弃备份）；
`rollback()` 回滚（恢复所有原始内容）。未显式提交/回滚即析构时自动回滚作为安全网。

```cpp
class CKAN_API TxFileManager {
public:
    explicit TxFileManager(const QString &txBaseDir);   // 备份存于 <txBaseDir>/<唯一子目录>
    ~TxFileManager();

    bool snapshot(const QString &absPath);   // 记录文件（不存在则仅记录）
    bool deleteFile(const QString &absPath);
    bool deleteDir(const QString &absPath);
    bool copyFile(const QString &src, const QString &absDest);
    bool writeFile(const QString &absPath, const QByteArray &content);
    bool makePath(const QString &absDir);

    void commit();
    void rollback();
    QString txDir() const;
    bool finished() const;
};
```

回滚语义：

- 被覆盖/删除的文件 → 从备份恢复原内容。
- 本事务新建的文件 → 删除。
- 被整目录删除的目录 → 恢复备份（保持事务开始前的精确状态）。
- 本事务 `makePath` 新建的目录 → 从最深到最浅删除空目录。

---

## 15. 跨进程文件锁

`src/ckan/filelock.h` —— 通过独占创建锁文件（`registry.locked`）实现互斥，支持陈旧锁（PID）
检测与接管。不可拷贝。

```cpp
class CKAN_API FileLock {
public:
    FileLock() = default;
    ~FileLock();                                  // 释放锁
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

    bool acquire(const QString &lockPath);   // 成功或已持有返回 true；被其他活跃进程占用返回 false
    void release();                          // 删除锁文件；未持有则无操作
    bool held() const;
    QString path() const;
};
```

陈旧锁（PID 不可用或对应进程已退出）自动检测清除，并重试一次。

---

## 16. 整合包导入

`src/ckan/modpackio.h` —— 「导入整合包」的文件系统 + zip/JSON 处理辅助（不含 UI，便于单测）。
均为 `ckan` 命名空间下的自由函数。

```cpp
// 整合包元数据条目（zip 根目录下的固定文件名，与 GameData 同层）。导出时写入：
// launcherVersion=启动器版本(导入仅显示不校验)、gameVersion=游戏版本(精确到 patch，如 "1.12.5")、
// name=整合包名、description=描述(可空)。
inline constexpr const char *kModpackMetaFileName = "hkspl_package.json";

// 读取 zip 根目录下整合包元数据文件 hkspl_package.json 的原始 JSON 字节。
// 返回状态：Ok=成功且 json 写回；NotFound=zip 中无该条目；ReadError=存在但打开/解压失败。
enum class ModpackMetaStatus { NotFound, ReadError, Ok };
CKAN_API ModpackMetaStatus modpackReadPackageMeta(const QString &zipPath,
                                                  QByteArray *json, QString *error);

// 整合包游戏版本与当前实例版本的 minor 级兼容判定：major 与 minor 都相同即视为兼容（patch 差异
// 不影响，如 1.12.4 vs 1.12.5 兼容）。任一侧版本无效时返回 false（无法比较）。
CKAN_API bool modpackVersionCompatible(const GameVersion &pkgVersion,
                                       const GameVersion &currentVersion);

CKAN_API bool modpackZipGameDataPrefix(const QString &zipPath, QString *prefix, QString *error);
    // 在 zip 中探测顶层 GameData 目录，返回解压前缀（如 "GameData/" 或 "包名/GameData/"）。
    // 找不到任何 GameData 目录时返回 false 并填充 error。

CKAN_API QStringList modpackCkanDepends(const QByteArray &json, QString *error);
    // 解析 .ckan 元包 JSON 的 depends 标识符列表（取顶层 depends[].name）。

CKAN_API bool modpackClearGameData(const QString &gameDir, QString *error);
    // 清空实例 GameData（保留 Squad/SquadExpansion），并删除实例 CKAN 注册表。

CKAN_API bool modpackImportGameData(const QString &zipPath, const QString &gameDir,
                                    const std::function<void(int)> &progress,
                                    std::atomic_bool *cancelRequested, QString *error);
    // 把 zip 中 GameData 内容导入实例 GameData（先清空现有文件，再解压）。
    // progress 取 0..1000（按解压字节比例）；cancelRequested 为 true 时安全提前返回 false。
```

---

## 17. 线程安全与错误约定

- **`CKan`** —— `refreshIndex`、`downloadModules`、`installFromCache` 设计在后台线程执行，
  进度回调在该线程回调；只读查询（`search`、`latestOf`、`installedVersion` 等）可与索引更新
  并发地从 UI 线程调用（门面内部加锁）。
- **`CKan::cancelInstall()`** 与 `ModuleInstaller::cancel()` 线程安全，可从任意线程调用中止
  当前下载/安装。
- **`Registry`** —— 所有拷贝共享一把 `QRecursiveMutex`；复合读-改-写循环应持 `registry->mutex()`。
  `loadFromJson` / `clear` / `toJson` 各自线程安全。
- **错误约定** —— 大多数可能失败的方法接受 `QString *error`；失败时返回默认值（空 / `false` /
  无效对象）并填充 `error`，**不抛异常**。
- **取消** —— `std::atomic_bool *cancelFlag` 参数支持外部取消；新一轮操作入口必须调用
  `ModuleInstaller::resetCancel()`，避免上一轮取消残留影响本轮。
- **事务** —— 卸载/安装默认原子执行；只有需要把多个操作合并为单事务（如升级）时才传入外部
  `TxFileManager`。

---

*本文档根据 `src/ckan/` 下公开头文件整理。若代码与本文档不一致，以头文件为准。*
