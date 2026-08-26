# cakn-cpp

A C++ re-implementation of [CKAN](https://github.com/KSP-CKAN/CKAN) — the Comma The Modding metadata **library** for *Kerbal Space Program*. Quicker and smaller than the original.

> This project was **extracted from the [Hello KSP Launcher](https://github.com/) (v1.x)** as a standalone, reusable library (`libckan`). It only contains the CKAN core — no GUI, no launcher logic.

---

## English

### What it is

`libckan` is a shared library (`.dll` / `.so` / `.dylib`) that re-implements the core of CKAN in modern C++ (C++17) on top of [Qt6](https://www.qt.io/). The data model (`CKAN-meta` repository) and `registry.json` format are fully compatible with the official CKAN.

### Features

- **Official-compatible semantic versioning** — `ModuleVersion` / `GameVersion` comparison (`epoch`, build-ID ignore rules).
- **Module metadata** — JSON parse/serialize of `CkanModule`; invalid modules (missing `identifier`/`version`) are rejected.
- **Relationships** — `depends` / `recommends` / `conflicts` / `provides` descriptors with version constraints.
- **Install rules** — `ModuleInstallDescriptor` parsing (`file` / `find` / `find_regexp`), `install_to` targets (`GameData` / `GameRoot` / `Ships` …).
- **Registry** — read/write of `registry.json` with full `installed_modules` / `installed_files` / `sorted_repositories` compatibility.
- **Dependency resolver** — BFS expansion over dependencies, virtual-package (`provides`) resolution, automatic highest-version selection, `suggests` cascade and conflict detection.
- **Repository index** — parse the `CKAN-meta` tarball into an *identifier → versions* map; priority merge across multiple repositories (with `download_counts.json`), per-repo mirror fallback, disk caching.
- **Downloader / Installer** — HTTP download (mirrors, proxy, resume, 1–8 parallel tasks), miniz ZIP unzip, SHA256 content verification, install/uninstall/upgrade with file-attribution tracking and manual-mod (AD) recognition.
- **Transactional filemanager** — atomic install/uninstall/upgrade with automatic rollback (`TxFileManager`).
- **Game instance** — lifecycle wrapper, read-only version detection (build-ID → version mapping), manual `GameData` DLL scan.
- **Mod pack export/import** — export installed mods as an official CKAN `*.ckan` metapackage (dependency-topological order, excludes DLC/AD); import a `.ckan`-driven zip pack (probe `GameData` prefix, clear then extract with progress & cancellation).
- **Cross-process file lock** — mutual exclusion on `registry.locked` via exclusive lock-file creation, with stale-lock (PID) detection and takeover (`FileLock`).
- **Cache helpers** — official CKAN cache filename, in-cache zip lookup, and required-disk-space estimation (`officialCacheFileName` / `findCacheZip` / `estimateRequiredBytes`).
- **Facade** — `CKan` + `CKanConfig` as the single public entry point; every public class is exported via `CKAN_API`.

### Requirements

- CMake ≥ 3.16
- A C++17 compiler (GCC / Clang / MSVC)
- Qt6 development packages: **Core, Network, Concurrent, Test**

### Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<your-qt6> .
cmake --build build --config Release
```

Output goes to `dist/` (library + test executable), so the test runs directly next to the DLL.

### Run tests

```sh
ctest --test-dir build -C Release
# or run directly:
./dist/test_libckan
```

Executes the whole QtTest suite (version comparison, install rules, registry, dependency resolution, virtual packages, conflicts, downloads, transaction rollback, …).

### Using the library

- Link `libckan` and define `CKAN_BUILD_SHARED` on the **consumer** side (uses `dllimport`); the library itself is built with `CKAN_BUILD_SHARED + CKAN_BUILDING_LIB` (uses `dllexport`).
- Include headers from `src/ckan`; the suggested API surface is the `ckan::CKan` facade + `CKanConfig`.

```cpp
#include <ckan.h>
using namespace ckan;

CKanConfig cfg;              // index cache dir, proxy, mirror prefixes, concurrency…
CKan ckan;
ckan.init(cfg);
for (const auto &m : ckan.search("MechJeb")) { /* … */ }
```

### Directory layout

```
src/ckan/          # CKAN core library (shared lib)
thirdparty/miniz   # ZIP archive (vendored)
tests/             # QtTest unit suite
dist/              # build output (gitignored)
```

### License

[GPL-3.0](LICENSE). It is a library; see the GPL note in `LICENSE` if you plan to link it from proprietary code.

---

## 中文

### 这是什么

`libckan` 是一个共享库（`.dll` / `.so` / `.dylib`），用现代 C++（C++17）+ [Qt6](https://www.qt.io/) 重写 CKAN 核心。数据模型（`CKAN-meta` 仓库）与 `registry.json` 格式完全兼容官方 CKAN。

### 起源

本工程从 **[Hello KSP Launcher](https://github.com/)**（v1.x）中**抽离**而来，作为独立、可复用的动态库发布。仓库只包含 CKAN 核心逻辑，无任何 GUI 与启动器代码。

### 功能特性

- **官方兼容的语义化版本** —— `ModuleVersion` / `GameVersion` 比较（含 epoch、build 忽略规则）。
- **模组元数据** —— `CkanModule` 的 JSON 解析/序列化，缺 `identifier`/`version` 判定为无效。
- **依赖关系** —— `depends` / `recommends` / `conflicts` / `provides` 描述符与版本约束判定。
- **安装规则** —— `ModuleInstallDescriptor` 解析（`file` / `find` / `find_regexp`），`install_to` 目标（`GameData` / `GameRoot` / `Ships` …）。
- **注册表** —— 读写 `registry.json`，`installed_modules` / `installed_files` / `sorted_repositories` 完整兼容。
- **依赖解析器** —— BFS 依赖展开，虚拟包（`provides`）索引解析，自动选最高版本，级联 `suggests` 与冲突检测。
- **仓库索引** —— 解析 `CKAN-meta` 压缩包为「标识符 → 多版本」索引；多仓库优先级合并（含 `download_counts.json`）、按仓库镜像回退、磁盘缓存。
- **下载/安装** —— HTTP 下载（镜像、代理、断点续传、1–8 并行）、miniz 解压、SHA256 内容校验、安装/卸载/升级的文件归属跟踪与手动模组（AD）识别。
- **事务文件管理** —— 安装/卸载/升级原子执行，失败自动回滚（`TxFileManager`）。
- **游戏实例** —— 生命周期封装，只读版本检测（build ID → 版本映射）、手动 `GameData` DLL 扫描。
- **整合包导入/导出** —— 把已安装模组导出为官方 CKAN `*.ckan` 元包（依赖拓扑序，排除 DLC/自动安装/手动模组）；导入 `.ckan` 驱动的 zip 整合包（探测 `GameData` 前缀、清空后解压，带进度与取消）。
- **跨进程文件锁** —— 以独占锁文件方式对 `registry.locked` 互斥，支持陈旧锁（PID）检测与接管（`FileLock`）。
- **下载缓存辅助** —— 官方 CKAN 缓存文件命名、缓存内 zip 查找、所需磁盘空间估算（`officialCacheFileName` / `findCacheZip` / `estimateRequiredBytes`）。
- **门面** —— `CKan` + `CKanConfig` 作为唯一对外入口；所有公开类经 `CKAN_API` 全量导出。

### 依赖

- CMake ≥ 3.16
- 支持 C++17 的编译器（GCC / Clang / MSVC）
- Qt6 开发包：**Core、Network、Concurrent、Test**

### 编译

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<你的Qt6路径> .
cmake --build build --config Release
```

产物统一输出到 `dist/`（动态库 + 测试可执行文件），测试可直接与 DLL 同目录运行。

### 运行测试

```sh
ctest --test-dir build -C Release
# 或直接运行：
./dist/test_libckan
```

运行整套 QtTest 用例（版本比较、安装规则、注册表、依赖解析、虚拟包、冲突、下载、事务回滚等）。

### 集成方式

- 消费方链接 `libckan` 并定义 `CKAN_BUILD_SHARED`（使用 `dllimport`）；库自身以 `CKAN_BUILD_SHARED + CKAN_BUILDING_LIB` 编译（使用 `dllexport`）。
- 头文件从 `src/ckan` 引入；建议只通过 `ckan::CKan` 门面 + `CKanConfig` 调用。

```cpp
#include <ckan.h>
using namespace ckan;

CKanConfig cfg;              // 索引缓存目录、代理、镜像前缀、并发数…
CKan ckan;
ckan.init(cfg);
for (const auto &m : ckan.search("MechJeb")) { /* … */ }
```

### 目录结构

```
src/ckan/          # CKAN 核心库（共享库）
thirdparty/miniz   # ZIP 归档（vendor 进仓库）
tests/             # QtTest 单元测试
dist/              # 构建产物（git 忽略）
```

### 许可

[GPL-3.0](LICENSE)。如计划在闭源程序里链接本库，请留意 `LICENSE` 中的 GPL 说明。