# libckan API Reference (English)

> This document describes the public C++ API of **libckan** — the C++17 + Qt6 re-implementation of the CKAN core library.
> All public classes live in the `ckan` namespace and are exported via the `CKAN_API` macro.
> Headers are included from `src/ckan`; the suggested entry point is the `ckan::CKan` facade plus `CKanConfig`.
>
> 中文版见 [API.zh-CN.md](API.zh-CN.md)。

---

## Table of contents

1. [Build & Link](#build--link)
2. [Quick Start](#quick-start)
3. [Facade: `CKan`](#1-facade-ckan)
4. [Config: `CKanConfig`](#2-config-ckanconfig)
5. [Versioning: `ModuleVersion` / `GameVersion` / `GameVersionRange`](#3-versioning)
6. [Module metadata: `CkanModule` and friends](#4-module-metadata)
7. [Relationships: `Relationship`](#5-relationships)
8. [Install rules: `ModuleInstallDescriptor`](#6-install-rules)
9. [Registry: `Registry` / `InstalledModule`](#7-registry)
10. [Repositories: `Repository`](#8-repositories)
11. [Game instance: `GameInstance`](#9-game-instance)
12. [Dependency resolution: `RelationshipResolver`](#10-dependency-resolution)
13. [Installer: `ModuleInstaller` / `InstallResult`](#11-installer)
14. [Repository index: `RepoIndex`](#12-repository-index)
15. [Downloader: `Downloader`](#13-downloader)
16. [Transactional file manager: `TxFileManager`](#14-transactional-file-manager)
17. [Cross-process file lock: `FileLock`](#15-cross-process-file-lock)
18. [Mod pack import: `modpackio.h`](#16-mod-pack-import)
19. [Thread-safety & error conventions](#17-thread-safety--error-conventions)

---

## Build & Link

The library is built as a shared library (`libckan.dll` / `libckan.so` / `libckan.dylib`).

- **Consumers** link `libckan` and define `CKAN_BUILD_SHARED` (this selects `dllimport` on Windows).
- The library itself is compiled with `CKAN_BUILD_SHARED + CKAN_BUILDING_LIB` (selects `dllexport`).

```cmake
target_link_libraries(your_app PRIVATE libckan)
target_compile_definitions(your_app PRIVATE CKAN_BUILD_SHARED)
```

Include the facade:

```cpp
#include <ckan.h>
using namespace ckan;
```

---

## Quick Start

```cpp
#include <ckan.h>
using namespace ckan;

// 1. Config: cache dir, proxy, mirror prefixes, concurrency...
CKanConfig cfg;
cfg.indexCacheDir = "cache/index";
cfg.proxyUrl = "";                                  // empty = direct connection
cfg.downloadConcurrency = 3;                        // 1..8 parallel downloads

// 2. Open a game instance (creates the CKAN/ directory structure on demand)
CKan ckan("D:/Steam/steamapps/common/Kerbal Space Program", "KSP 1.12.5");
ckan.init(cfg);                                     // (illustrative; see note below)

// 3. Refresh the repository index and search
QString err;
QVector<Repository> repos = { Repository::defaultKspRepo() };
if (!ckan.refreshIndex(repos, &err)) { /* handle err */ }
auto mods = ckan.search("MechJeb");

// 4. Resolve the full install set (dependencies included)
ResolutionResult plan = ckan.resolveInstall(mods.first());
if (plan.conflicted || plan.missing) { /* handle */ }

// 5. Download, then install (two phases, call from a worker thread)
QStringList conflicts;
if (!ckan.downloadModules(plan.modulesToInstall, "cache/downloads",
                          /*preferModuleMirrors=*/false, 3, &conflicts, &err)) { /* handle */ }
InstallResult r = ckan.installFromCache(plan.modulesToInstall, "cache/downloads", conflicts);
// r.ok / r.error / r.cancelled / r.installedIdentifiers
```

> **Note:** `CKan` has no `init()` method in the current public API — it is configured entirely
> through its constructor, which takes the game directory, an instance name and a `CKanConfig`.

---

## 1. Facade: `CKan`

`src/ckan/ckan.h` — The single public entry point for launchers. It wraps the repository index,
registry, dependency resolver, downloader and installer (with conflict detection and transaction
rollback). Internals (`GameInstance` etc.) are never exposed.

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

### Instance basics

| Signature | Description |
|---|---|
| `QString gameDir() const` | The game directory of this instance. |
| `QString historyDir() const` | Install-history directory (`<gameDir>/CKAN/history`). |
| `GameVersion detectedVersion() const` | KSP version actually detected for the instance; an invalid `GameVersion` if detection failed. |
| `void reloadRegistry()` | Reload installed-module data from `registry.json`. |
| `bool tryAcquireRegistryLock()` | Try to take the `registry.locked` write lock. Returns `false` if another process (official CKAN / another launcher) holds it. Use it to gate index loading. |

### Repository index / mod list

| Signature | Description |
|---|---|
| `bool refreshIndex(const QVector<Repository> &repos, QString *error = nullptr, bool force = false, qint64 maxAgeSecs = RepoIndex::kDefaultCacheAgeSecs, bool preferMirror = false, const std::function<void(const QString &, qint64, qint64)> &onProgress = {}, std::atomic_bool *cancelFlag = nullptr)` | Download and merge indexes from multiple repositories (smaller `priority` = higher precedence). Uses cache unless `force`; falls back to stale cache if a single repo fails. `onProgress(repoName, received, total)`; set `cancelFlag` to abort. Mirror prefixes / cache dir / proxy come from the `CKanConfig` passed to the constructor (mirrors apply to GitHub-hosted repos only). |
| `QVector<CkanModule> search(const QString &query) const` | Search mods by name / identifier. |
| `QVector<CkanModule> versionsOf(const QString &identifier) const` | All versions of an identifier. |
| `CkanModule latestOf(const QString &identifier) const` | Latest version of an identifier (invalid module if absent). |
| `bool indexReady() const` | Whether the index is currently loaded. |
| `int indexSize() const` | Number of identifiers in the index. |
| `QStringList allIdentifiers() const` | Every identifier in the index (useful to clean the download cache precisely). |
| `int downloadCount(const QString &identifier) const` | Download count from `download_counts.json`; `-1` if no data. |
| `bool hasDownloadCount(const QString &identifier) const` | Whether a download count is available for the identifier. |

### Installed-module queries

| Signature | Description |
|---|---|
| `QString installedVersion(const QString &identifier) const` | Installed version string; empty if not installed. |
| `bool isInstalled(const QString &identifier) const` | Whether the identifier is installed. |
| `QVector<InstalledModule> installedModules() const` | All installed modules. |
| `QStringList installedGameDataEntries(const QString &identifier) const` | Top-level GameData entries owned by the identifier (relative to the game dir, e.g. `GameData/SomeMod` or a root single file `GameData/single.dll`). Covers both registry file attribution and auto-detected (AD) DLL paths. |

### Mod pack export

| Signature | Description |
|---|---|
| `QByteArray exportModpackCkan(QString *error = nullptr)` | Generate an official CKAN metapackage JSON: `depends` lists installed mods (no versions), excluding DLC / auto-installed / AD mods and (when the index is loaded) mods absent from the index. Dependency-topological order. Returns empty and fills `error` when there is nothing to export. `ksp_version_min/max` are omitted when the KSP version cannot be detected (not an error). |
| `bool writeHistorySnapshot(QString *error = nullptr)` | Write an install-history snapshot to `historyDir()` (`已安装-{instance}-{timestamp}.ckan`), keeping only the most recent `kMaxHistoryCount` entries. Best-effort; fills `error` on failure. |

### Single-file import

| Signature | Description |
|---|---|
| `CkanModule importModuleFile(const QString &path, bool *isMetapackage = nullptr, QString *error = nullptr)` | Parse a local `.zip` or `.ckan` into a `CkanModule`. `.ckan`: parsed as JSON; `kind=metapackage`, or no install stanzas but has `depends`, sets `*isMetapackage=true`. `.zip`: looks for an embedded `*.ckan` metadata file first, then matches the file SHA256 against known download hashes; otherwise fails with `error` (mirrors official “no metadata → reject”). Returns an invalid module on failure. |
| `QString importStoreCache(const CkanModule &mod, const QString &sourcePath, const QString &downloadDir, QString *error = nullptr)` | Copy the imported local file into the cache dir as `{id}_{safeVersion}.zip` so `installFromCache`/`findCacheZip` can hit it. Returns the written cache path; empty + `error` on failure. |

### Auto-detected (AD) mods — DLL scan

| Signature | Description |
|---|---|
| `void scanUnmanagedDlls()` | Scan `GameData` for `.dll` files and write them into `registry.installedDlls`, then save. Result is cached; repeated calls return immediately. |
| `bool dllsScanned() const` | Whether the DLL scan has finished (for “scanning… / ready” UI states). |
| `bool isAutoDetected(const QString &identifier) const` | Whether the identifier was auto-detected (AD). |
| `QString autoDetectedVersion(const QString &identifier) const` | Best-effort installed version of an AD mod: (1) derive from the DLL filename (identifier = part before the first `.`, the rest is the version, e.g. `ModuleManager.4.2.3.dll` → `4.2.3`); (2) fall back to the DLL's embedded PE file-version resource (Windows only); (3) empty if both fail. |

### Dependency resolution

| Signature | Description |
|---|---|
| `ResolutionResult resolveInstall(const CkanModule &mod, bool autoInstallRecommends = true, bool withSuggests = false)` | Resolve the full set (dependencies included) needed to install `mod`. |
| `ResolutionResult resolveInstallMany(const QVector<CkanModule> &mods, bool autoInstallRecommends = true, bool withSuggests = false, const GameVersionRange &extraRange = GameVersionRange())` | Resolve a batch of modules as one install set (mutual dependencies handled). `extraRange`: user-selected extra compatibility range (invalid = disabled); a candidate is compatible if it matches the current instance version **or** that range. |

### Install flow (two phases — call from a worker thread)

| Signature | Description |
|---|---|
| `bool downloadModules(const QVector<CkanModule> &modules, const QString &downloadDir, bool preferModuleMirrors, int concurrency = 3, QStringList *conflicts = nullptr, QString *error = nullptr, const std::function<void(const QString &, qint64, qint64, qint64)> &onByteProgress = {}, std::atomic_bool *cancelFlag = nullptr)` | **Phase 1.** Download all zips into `downloadDir` and compute conflicts against manually-occupied folders based on the zips' real contents. `concurrency` 1..8. `conflicts` (sorted, de-duplicated) receives conflicting top-level GameData folders (relative to GameDir, e.g. `GameData/SomeMod`). `onByteProgress(identifier, doneBytes, totalBytes, speedBps)`; abort via `cancelFlag` or `cancelInstall()`. |
| `InstallResult installFromCache(const QVector<CkanModule> &modules, const QString &downloadDir, const QStringList &foldersToDelete = {}, const QStringList &preUninstall = {}, const std::function<void(const QString &, int)> &onInstallProgress = {})` | **Phase 2.** Install from cache in a single transaction: first uninstall `preUninstall` old versions, then write. `foldersToDelete` are top-level GameData folders removed recursively before writing. Any failure (including user cancel) rolls back everything: restored old files, removed new files, restored registry. `onInstallProgress(identifier, percent 0..100)` runs on the worker thread. |
| `InstallResult uninstall(const QString &identifier)` | Uninstall one mod (single transaction; whole operation rolls back on failure). Uses the shared installer, so it can be aborted via `cancelInstall()`; an abort returns `cancelled=true` after rollback. |
| `InstallResult uninstallMany(const QStringList &identifiers)` | Uninstall a batch atomically in one shared transaction (all-or-nothing). |
| `QStringList uninstallPlan(const QStringList &identifiers)` | Read-only cascade order for uninstalling `identifiers` (targets included, dependents first). Returns empty if any identifier is not installed. |
| `void cancelInstall()` | Request cancellation of the current install/download task (thread-safe). |
| `void releaseInstaller()` | Release the internal installer after the install flow is finished (call after download + install are done). |

### Download-cache helpers (static)

| Signature | Description |
|---|---|
| `static QString safeCacheFileName(const QString &s)` | Strip illegal Windows filename characters (`: \ / ? * < > | "`) — e.g. `1:3.4.0` → `1_3.4.0` — so epoch versions cannot corrupt the cache via NTFS ADS. |
| `static QString officialCacheFileName(const QString &identifier, const QString &version, const QString &downloadUrl = QString())` | Official CKAN cache filename: `{SHA1(url)[:8]}-{identifier}-{version}.zip`; falls back to `{identifier}-{version}.zip` when the URL is empty. |
| `static QString findCacheZip(const QString &downloadDir, const CkanModule &mod)` | Find the actual valid cache zip for a mod: official format first, then manual-download format, then this project's format; empty if none exists or all are invalid. |
| `static qint64 estimateRequiredBytes(const QVector<CkanModule> &modules, double bufferFactor = 1.15)` | Estimate required disk space (bytes): sum of `downloadSize` for non-metapackage mods × `bufferFactor`. |

### Static utilities

| Signature | Description |
|---|---|
| `static GameVersion detectVersionFromDir(const QString &gameDir)` | Detect the KSP version from a game directory without constructing an instance (for instance discovery; invalid version on failure). |

---

## 2. Config: `CKanConfig`

`src/ckan/ckanconfig.h` — Immutable runtime configuration passed once to the `CKan` constructor.
Replaces the former global static configuration; keeps the library free of mutable global state.

| Field | Type | Description |
|---|---|---|
| `indexCacheDir` | `QString` | Index cache directory (empty = no on-disk cache). |
| `proxyUrl` | `QString` | Network proxy, e.g. `http://127.0.0.1:7890` (empty = direct). |
| `indexMirrorPrefixes` | `QStringList` | Mirror prefixes for index downloads (GitHub-hosted repos only). |
| `moduleMirrorPrefixes` | `QStringList` | Mirror prefixes for module downloads. |
| `downloadConcurrency` | `int` | Parallel module downloads, default `3`. |
| `downloadRateLimitBps` | `qint64` | Per-connection download rate limit (bytes/s), `0` = unlimited. |

Static helpers:

```cpp
static inline QStringList defaultIndexMirrorPrefixes();   // { "https://gh-proxy.com/", "https://ghfast.top/" }
static inline QStringList defaultModuleMirrorPrefixes();  // { "https://gh-proxy.com/", "https://ghfast.top/" }
```

---

## 3. Versioning

`src/ckan/version.h` — Semantic versioning fully compatible with official CKAN.

### `ModuleVersion`

Format `[epoch:]version`. Comparison follows official CKAN rules (epoch, build-ignore).

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
    // == != < > <= >= operators
};
```

### `GameVersion`

KSP version `major.minor.patch[.build]`. Matches official `GameVersion`: each component records
whether it was explicitly declared (an undeclared component is `0` numerically but its `isXxxDefined()`
is `false`).

```cpp
class CKAN_API GameVersion {
public:
    explicit GameVersion(const QString &versionString);
    GameVersion(int major, int minor, int patch = 0, int build = 0);
    GameVersion();                              // invalid
    bool isValid() const;
    int major() const; int minor() const; int patch() const; int build() const;
    bool isMajorDefined() const; bool isMinorDefined() const;
    bool isPatchDefined() const; bool isBuildDefined() const;
    GameVersion withoutBuild() const;
    QString toString() const;
    GameVersionRange toVersionRange() const;    // expand to a half-open range
    int compareWithoutBuild(const GameVersion &other) const;
    int compareTo(const GameVersion &other) const;
    // < > <= >= == operators (compare with build ignored)
};
```

### `GameVersionRange`

Version interval; each bound may be open or closed (an unset side is unbounded).

```cpp
class CKAN_API GameVersionRange {
public:
    GameVersionRange();
    GameVersionRange(const GameVersion &lower, bool lowerInclusive,
                     const GameVersion &upper, bool upperInclusive);
    bool contains(const GameVersion &value) const;
    bool intersects(const GameVersionRange &other) const;
    bool lowerSet() const; bool upperSet() const;
    bool lowerInclusive() const; bool upperInclusive() const;
    GameVersion lower() const; GameVersion upper() const;
};

// Build a continuous range from a set of checked version lines
// (e.g. {"1.9","1.10","1.11","1.12"} → [1.9.0.0, 1.13.0.0)).
// Empty or all-invalid input returns an invalid range.
CKAN_API GameVersionRange versionLinesToRange(const QStringList &versionLines);
```

**Example**

```cpp
ModuleVersion a("1.2.3"), b("1:1.2.3");   // 1:1.2.3 > 1.2.3 (epoch 1)
bool newer = b > a;                        // true

GameVersion v("1.12.5");
GameVersionRange r = versionLinesToRange({"1.9", "1.10", "1.11", "1.12"});
bool ok = r.contains(v);                   // true (1.9.0.0 <= 1.12.5 < 1.13.0.0)
```

---

## 4. Module metadata

`src/ckan/ckanmodule.h`

### Enums & structs

```cpp
enum class ModuleKind { Package, Metapackage, Dlc };

enum class ReleaseStatus {      // stable < testing < development
    Stable      = 0,            // JSON: "stable" / absent
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
    bool isCompatible(const GameVersionRange &range) const;   // intersects
    GameVersionRange compatibilityRange() const;
    QVector<ModuleInstallDescriptor> effectiveInstallStanzas() const;  // default: find=identifier, GameData
    QStringList providesList() const;          // virtual packages incl. itself

    // ---- fields ----
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

    QJsonObject toJsonObject() const;          // round-trips with fromJsonObject
    QByteArray  toJson() const;
};
```

**Example**

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
if (mod.isValid() && mod.isCompatible(GameVersion("1.12.5"))) { /* installable */ }
```

---

## 5. Relationships

`src/ckan/relationship.h`

```cpp
class CKAN_API Relationship {
public:
    enum class Type { Depends, Recommends, Suggests, Supports, Conflicts, Provides };

    Type type = Type::Depends;
    QString name;            // target identifier or virtual package name
    QString version;         // optional version constraint, e.g. ">=1.2", empty = any
    QString minVersion, maxVersion;
    bool minInclusive = false, maxInclusive = false;
    QVector<Relationship> anyOf;   // any_of: any single child satisfying is enough (children inherit type)

    bool isVirtual() const;                       // decided by the provides list during resolution
    bool versionSatisfies(const QString &installedVersion) const;
    QJsonObject toJsonObject() const;
};
```

---

## 6. Install rules

`src/ckan/moduleinstalldescriptor.h`

```cpp
struct CKAN_API InstallableFile {
    QString sourceName;      // original path inside the zip
    QString destination;     // converted target relative path, e.g. GameData/foo/bar.dll
    bool    makeDir = false;
};

class CKAN_API ModuleInstallDescriptor {
public:
    static ModuleInstallDescriptor defaultStanza(const QString &identifier);
    static bool fromJsonObject(const QJsonObject &obj, ModuleInstallDescriptor *out, QString *error);

    bool isWanted(const QString &path, int *matchIndex) const;
    QString transformOutputName(const QString &outputName, const QString &installDirInGame) const;
    QJsonObject toJsonObject() const;
    QString resolveInstallBaseDir(const QString &primaryModDir) const;
    QVector<InstallableFile> findInstallableFiles(const QStringList &zipEntries,
                                                  const QString &primaryModDir,
                                                  QString *error) const;

    // ---- fields ----
    QString file;            // exact path
    QString find;            // directory name
    QString findRegexp;      // regex
    bool    findMatchesFiles = false;
    QString installTo;       // GameData / GameRoot / Ships ...
    QString as;              // rename the first-level directory
    QStringList filter, filterRegexp, includeOnly, includeOnlyRegexp;

    bool isValid() const;    // any of file/find/findRegexp set
};
```

---

## 7. Registry

`src/ckan/registry.h` / `src/ckan/installedmodule.h`

### `Registry`

Read/write of `registry.json` with full official-CKAN compatibility
(`installed_modules` / `installed_files` / `sorted_repositories`).

```cpp
class CKAN_API Registry {
public:
    static const int LATEST_REGISTRY_VERSION = 3;

    static Registry fromJson(const QByteArray &json, QString *error = nullptr);
    bool loadFromJson(const QByteArray &json, QString *error = nullptr);   // in-place, thread-safe
    void clear();                                                          // reset to empty, thread-safe
    QByteArray toJson() const;                                             // thread-safe
    QRecursiveMutex *mutex() const;                                        // shared recursive lock

    QMap<QString, Repository> repositories;
    void setRepositories(const QMap<QString, Repository> &repos);
    QMap<QString, InstalledModule> installedModules;   // identifier -> InstalledModule
    QHash<QString, QString> installedFiles;            // relative path -> identifier
    QMap<QString, QString> installedDlls;              // manual dlls: identifier -> relative path
    int registryVersion = LATEST_REGISTRY_VERSION;
    bool isValid() const;

    InstalledModule *installed(const QString &identifier);
    const InstalledModule *installed(const QString &identifier) const;
    QString installedVersion(const QString &identifier) const;
    bool isInstalled(const QString &identifier) const;
    QString fileOwner(const QString &relativePath) const;

    void registerModule(const InstalledModule &im);     // also updates installedFiles
    void unregisterModule(const QString &identifier);   // removes its file attribution
};
```

> All copies of a `Registry` share the same `QRecursiveMutex` (held via `shared_ptr`), so a
> production instance passed by value keeps one stable lock. Use `mutex()` to wrap compound
> read/write loops.

### `InstalledModule`

One entry of `registry.json`'s `installed_modules`.

```cpp
class CKAN_API InstalledModule {
public:
    QString identifier;
    CkanModule module;
    QStringList files;             // paths relative to GameRoot
    bool autoInstalled = false;
    QString installTime;           // ISO 8601 UTC
    bool isValid() const;
    QJsonObject toJsonObject() const;
    static InstalledModule fromJsonObject(const QJsonObject &obj);
};
```

---

## 8. Repositories

`src/ckan/repository.h`

```cpp
struct CKAN_API Repository {
    QString name;
    QString uri;
    int     priority = 0;      // smaller = higher precedence
    bool    mirror   = false;
    QString comment;
    bool isValid() const;      // !name.isEmpty() && !uri.isEmpty()

    static Repository defaultKspRepo();
    static QString defaultRepoUrl();
    static QString repositoryListUrl();

    // presets for the settings page
    static Repository presetKspCkanBackup();  // KSP-CKAN backup (GitLab archive)
    static Repository presetSol();            // Sol / RSS-Reborn
    static Repository presetMechJeb2Dev();    // MechJeb2-dev (CI builds)
};
```

The default repository is the `KSP-CKAN/CKAN-meta` master tarball.

---

## 9. Game instance

`src/ckan/gameinstance.h` — Manages the CKAN data directory under a KSP game folder.
Normally you go through the `CKan` facade; use this class directly only when you need the internals.

```cpp
class CKAN_API GameInstance {
public:
    GameInstance();
    GameInstance(const QString &gameDir, const QString &name);
    void setupCkanDirectories();                      // ensure CKAN/ structure exists

    QString gameDir() const; QString name() const;
    QString ckanDir() const;                          // <gameDir>/CKAN
    QString downloadDir() const;                      // <gameDir>/CKAN/downloads
    QString historyDir() const;                       // <gameDir>/CKAN/history
    QString registryPath() const;                     // <gameDir>/CKAN/registry.json
    QString compatibleVersionsPath() const;           // <gameDir>/CKAN/compatible_ksp_versions.json

    QString toRelativeGameDir(const QString &abs) const;
    QString toAbsoluteGameDir(const QString &rel) const;

    GameVersion detectVersion() const;                // buildID files → build map, readme.txt fallback
    static GameVersion detectVersionFromDir(const QString &gameDir);   // read-only, no side effects

    static QStringList detectInstallKindTags(const QString &gameDir, bool *corrupted = nullptr);
    static QString suggestedInstanceName(const QString &gameDir);

    QMap<QString, QString> scanUnmanagedDlls() const; // identifier -> relative GameDir path
    QStringList manualGameDataFolders() const;        // manually-occupied top-level GameData folders

    Registry *registry();
    const Registry *registry() const;
    void loadRegistry();
    bool saveRegistry() const;                        // false if the lock cannot be held
    bool registryLockHeld() const;
    bool engageRegistryLock();                        // take the write lock (false if held elsewhere)
    void restoreRegistrySnapshot(const QByteArray &json);   // transaction rollback support

    bool isValid() const;                             // game files exist

    void setCustomDownloadDir(const QString &dir);
    QString customDownloadDir() const;
};
```

**Version detection** reads `buildID64.txt` / `buildID.txt` first (`build id = NNNN`, leading zeros
ignored) and converts through a built-in build-ID → version map (official `builds-ksp.json`,
covering 0.23.0.395 … 1.12.5.3190); both files are de-duplicated by taking the max version. On
miss it falls back to parsing the version line in `readme.txt`; an invalid version is returned
when all attempts fail.

---

## 10. Dependency resolution

`src/ckan/relationshipresolver.h`

### Result types

```cpp
struct CKAN_API ProviderChoice {
    QString provides;                // virtual package name
    QStringList requiredBy;          // identifiers of the mods requiring it
    QString requirement;             // constraint description (may be empty)
    QVector<CkanModule> candidates;  // compatible candidates, version-descending
};

struct CKAN_API ResolutionResult {
    QVector<CkanModule> modulesToInstall;    // dependency order (dependencies first)
    QVector<CkanModule> suggestedModules;    // cascade suggests (collected, NOT auto-installed)
    QVector<ProviderChoice> providerChoices; // virtual packages needing user choice
    QStringList notFound;                    // unsatisfiable dependencies
    QStringList conflicts;                   // conflict descriptions
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
                             const GameVersionRange &extraRange = GameVersionRange());
};
```

Resolution rules:

- BFS expansion over `depends`; virtual packages (`provides`) are indexed and resolved.
- For each dependency, pick the **highest** version satisfying the constraint and KSP compatibility
  (compatible with `kspVersion` or any range in `extraRange`; an invalid version disables filtering).
- An installed mod satisfies a dependency only if it meets the version constraint; otherwise a newer
  version is chosen (upgrade).
- Conflicts are checked **both ways** (newly-declared + already-selected), including version constraints.
- Recommended/suggested mods conflicting with the selected set are silently skipped; hard-dependency
  conflicts are recorded in `conflicts`.
- A non-empty `providerChoices` means the UI should let the user pick, then re-resolve.

---

## 11. Installer

`src/ckan/moduleinstaller.h` — `QObject`-based installer: download zip → miniz unzip → copy per
install rules into `GameData` → update registry.

```cpp
struct CKAN_API InstallResult {
    bool ok = false;
    QString error;
    bool cancelled = false;            // user cancelled (uninstall scenario, rolled back)
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

    void cancel();          // thread-safe abort
    void resetCancel();     // clear the cancel flag before a new run

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

**Transaction semantics** (`installFromCache` / `uninstall` / `uninstallMany`):

- With an empty `tx`, the installer creates an internal transaction: any failure (including cancel)
  rolls everything back (restored overwritten/deleted files, removed newly-written files, restored
  registry) and commits on success.
- With an external `tx` (e.g. upgrade = uninstall old + install new as one transaction), file
  operations are recorded into that transaction; this method does **not** save or roll back —
  the caller decides when to `commit()` / `rollback()`.

---

## 12. Repository index

`src/ckan/repoindex.h` — Download the `CKAN-meta` tarball, unzip it, and build an
`identifier → versions` index. Supports on-disk caching.

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

Multi-repo behavior:

- Repos are processed in `priority` ascending order (smaller = higher precedence; first wins).
- On `identifier+version` clash the higher-priority repo wins; download counts take the first hit.
- If one repo fails, its stale cache is used as fallback; overall success requires ≥ 1 repo to succeed.
- Mirror prefixes apply to GitHub-hosted repos only (prefix + repo URL); non-GitHub repos ignore mirrors
  to avoid downloading another repo's content by mistake.

---

## 13. Downloader

`src/ckan/downloader.h` — Simple HTTP downloader with mirror fallback, proxy, progress, timeout and cancel.

```cpp
class CKAN_API Downloader : public QObject {
    Q_OBJECT
public:
    using Validator = std::function<bool(const QByteArray &)>;          // false → try next mirror
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

    void downloadAsync(const QString &url, const QStringList &mirrors); // signals on completion

    void setProxyUrl(const QString &proxyUrl);  QString proxyUrl() const;
    void setDownloadRate(qint64 bytesPerSecond); qint64 downloadRate() const;

signals:
    void finished(const QByteArray &data, const QString &url);
    void failed(const QString &url, const QString &error);
};
```

Notes:

- `downloadProgressed` drives a local event loop on the calling thread; connect and idle timeouts are
  both 30 s; setting `cancelFlag` aborts immediately (returns `false`, error = "cancelled").
- `resumeAttempts > 0` enables resume: on an interrupted transfer (e.g. closed connection) the
  received bytes are kept and the remainder is requested via `Range`; if the server ignores `Range`
  (returns 200 full body), the buffer is cleared and the download restarts from zero.

---

## 14. Transactional file manager

`src/ckan/txfilemanager.h` — Makes file operations rollback-able (official: `ChinhDo.Transactions.TxFileManager`).

Every write/delete/overwrite first backs the original file/directory up into a transaction subdirectory;
`commit()` drops the backups; `rollback()` restores everything. Destruction without an explicit
commit/rollback auto-rolls back as a safety net.

```cpp
class CKAN_API TxFileManager {
public:
    explicit TxFileManager(const QString &txBaseDir);   // backups under <txBaseDir>/<unique subdir>
    ~TxFileManager();

    bool snapshot(const QString &absPath);   // record a file (no-op record if absent)
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

Rollback semantics:

- Overwritten/deleted files → restored from backup.
- Files created inside the transaction → deleted.
- Whole directories deleted → restored exactly as before the transaction.
- Directories created by `makePath` → removed deepest-first when empty.

---

## 15. Cross-process file lock

`src/ckan/filelock.h` — Mutual exclusion via exclusive lock-file creation (`registry.locked`),
with stale-lock (PID) detection and takeover. Non-copyable.

```cpp
class CKAN_API FileLock {
public:
    FileLock() = default;
    ~FileLock();                                  // releases
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

    bool acquire(const QString &lockPath);   // true if acquired or already held; false if held elsewhere
    void release();                          // delete the lock file; no-op if not held
    bool held() const;
    QString path() const;
};
```

Stale locks (unusable PID or exited process) are detected and cleared automatically, then one retry
is performed.

---

## 16. Mod pack import

`src/ckan/modpackio.h` — File-system + zip/JSON helpers for the “import mod pack” flow (no UI,
unit-testable). All functions are free functions in the `ckan` namespace.

```cpp
CKAN_API bool modpackZipGameDataPrefix(const QString &zipPath, QString *prefix, QString *error);
    // Probe the top-level GameData directory inside the zip, return the extraction prefix
    // (e.g. "GameData/" or "packname/GameData/"). false + error if none found.

CKAN_API QStringList modpackCkanDepends(const QByteArray &json, QString *error);
    // Parse the depends identifier list of a .ckan metapackage JSON (top-level depends[].name).

CKAN_API bool modpackClearGameData(const QString &gameDir, QString *error);
    // Clear the instance GameData (keeping Squad/SquadExpansion) and delete the CKAN registry.

CKAN_API bool modpackImportGameData(const QString &zipPath, const QString &gameDir,
                                    const std::function<void(int)> &progress,
                                    std::atomic_bool *cancelRequested, QString *error);
    // Import the zip's GameData content into the instance GameData (clear first, then extract).
    // progress: 0..1000 by extracted-byte ratio; cancelRequested=true aborts safely (returns false).
```

---

## 17. Thread-safety & error conventions

- **`CKan`** — `refreshIndex`, `downloadModules` and `installFromCache` are designed to run on a
  worker thread; progress callbacks are invoked on that thread. Read-only queries (`search`,
  `latestOf`, `installedVersion`, …) are safe to call from the UI thread concurrently with index
  updates (the facade locks internally).
- **`CKan::cancelInstall()`** and `ModuleInstaller::cancel()` are thread-safe and may be called from
  any thread to abort the current download/install.
- **`Registry`** — every copy shares one `QRecursiveMutex`; compound read-modify-write loops should
  hold `registry->mutex()`. `loadFromJson` / `clear` / `toJson` are thread-safe individually.
- **Error convention** — most methods that can fail accept a `QString *error`; on failure they return
  a default value (empty / `false` / invalid object) and fill `error`. They do **not** throw.
- **Cancellation** — `std::atomic_bool *cancelFlag` parameters allow external cancellation;
  `ModuleInstaller::resetCancel()` must be called at the start of a new run so a previous
  cancellation does not leak into it.
- **Transactions** — uninstall/install are atomic by default; pass an external `TxFileManager`
  only when you need to merge several operations into one transaction (e.g. upgrade).

---

*Generated from the public headers under `src/ckan/`. If the code and this document disagree, the headers win.*
