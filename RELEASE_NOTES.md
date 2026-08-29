# RELEASE NOTES · libckan-cpp

## v1.0.0 (2026-08-29)

`libckan` — the first official release of the standalone library. A full C++17/Qt6 re-implementation of the core of [CKAN](https://github.com/KSP-CKAN/CKAN), with data model (`CKAN-meta` repository) and `registry.json` format fully compatible with the official CKAN. Contains only the library core — no GUI, no launcher code.

> **Compatibility**: This release supports **Hello KSP Launcher 1.1.0 – 1.1.1** and can be used as a drop-in CKAN core for it.

---

### What's New

- **Install history snapshots** — new `CKan::writeHistorySnapshot()` / `historyDir()`: writes the currently installed mods (with versions) as an official history metapackage into `instance/CKAN/history/installed-{instance}-{timestamp}.ckan`, keeping only the latest 200 entries for rollback/inspection.
- **Single-module file import** — new `CKan::importModuleFile()` / `importStoreCache()`: parses a single mod from a local `.zip` or `.ckan` (`.zip` first scans embedded `*.ckan` metadata, then SHA256 matches known repository download hashes), and copies it into the cache directory for direct installation.
- **Download rate limiting** — `CKanConfig::downloadRateLimitBps` throttles each connection (bytes/sec, 0 = unlimited), applied to both index and module downloads via `Downloader::setDownloadRate()` / `RepoIndex::build*` / `ModuleInstaller::setDownloadRateLimitBps()`.
- **Concurrent index safety** — `m_index` / `m_downloadCounts` / `m_indexReady` are now guarded by a mutex (`m_indexMutex`), eliminating data races between background refresh and UI reads.

### Security & Robustness Fixes

- **Zip Slip protection** — install writes and modpack extraction verify the normalized target path stays inside the game directory / `GameData`; out-of-bounds paths are rejected (write/delete) with full rollback.
- **Decompression bomb protection** — repo-index gzip decompression is capped (256MB output / 64MB archive), streamed with ISIZE integrity check; corrupt or oversized archives are rejected.
- **Registry reset on missing file** — when `registry.json` is absent (e.g. after a modpack import that cleared it), the in-memory state is reset to empty so install/file-attribution logic is not distorted by stale data.
- **Progress callback thread fix** — the internal installer is explicitly moved to the main thread (`moveToThread`), so byte/install progress signals emitted from the thread pool are dispatched back to the UI thread; progress bars no longer stall.

---

### Feature Overview

- **Official-compatible semantic versioning** — `ModuleVersion` / `GameVersion` comparison (epoch, build-ID ignore rules).
- **Module metadata** — `CkanModule` JSON parse/serialize (incl. `tags`); modules missing `identifier`/`version` are rejected.
- **Relationships** — `depends` / `recommends` / `conflicts` / `provides` descriptors with version constraints.
- **Install rules** — `ModuleInstallDescriptor` parsing (`file` / `find` / `find_regexp`), `install_to` targets (`GameData` / `GameRoot` / `Ships` …).
- **Registry** — read/write of `registry.json` with full `installed_modules` / `installed_files` / `sorted_repositories` compatibility.
- **Dependency resolver** — BFS expansion, virtual-package (`provides`) resolution, automatic highest-version selection, cascading `suggests`, and conflict detection.
- **Repository index** — parses `CKAN-meta` tarballs into an *identifier → versions* map; priority merge across repositories (with `download_counts.json`), mirror fallback, disk caching.
- **Download / install** — HTTP download (mirrors, proxy, resume, rate limit, 1–8 parallel tasks), miniz ZIP extraction, SHA256 verification, install/uninstall/upgrade with file-attribution tracking and manual-mod (AD) recognition.
- **Transactional file manager** — atomic install/uninstall/upgrade with automatic rollback (`TxFileManager`).
- **Game instance** — lifecycle wrapper, read-only version detection (build-ID → version mapping), manual `GameData` DLL scan.
- **Modpack import/export** — export installed mods as an official `*.ckan` metapackage (dependency-topological order, excludes DLC/auto-installed/manual mods); import `.ckan`-driven zip packs (GameData prefix probe, clear-then-extract with progress & cancellation).
- **Cross-process file lock** — exclusive lock on `registry.locked` with stale-lock (PID) detection and takeover (`FileLock`).
- **Cache helpers** — official cache file naming, in-cache zip lookup, required-disk-space estimation.
- **Facade** — `CKan` + `CKanConfig` as the single public entry point; every public class exported via `CKAN_API`.

### Requirements

- CMake ≥ 3.16; C++17 compiler (GCC / Clang / MSVC); Qt6 development packages: **Core, Network, Concurrent, Test**.

### Platform Support

Currently fully tested and optimized for Windows 10/11. Linux/macOS support is theoretically possible but not actively maintained due to lack of testing environments.

### License

Copyright © Zhu Wenqian · [GPL-3.0](LICENSE)
