#ifndef CKAN_GAMEINSTANCE_H
#define CKAN_GAMEINSTANCE_H

#include <QString>
#include <QDir>
#include <QMap>

#include "ckan_export.h"
#include "registry.h"
#include "version.h"
#include "filelock.h"

namespace ckan {

// 游戏实例，对应 CKAN 的 GameInstance。
// 负责管理单个 KSP 游戏目录下的 CKAN 数据目录。
class CKAN_API GameInstance
{
public:
    GameInstance() = default;
    GameInstance(const QString &gameDir, const QString &name);

    // 初始化：确保 CKAN 目录结构存在
    void setupCkanDirectories();

    // 路径
    QString gameDir() const { return m_gameDir; }
    // 实例显示名（构造时传入）
    QString name() const { return m_name; }
    QString ckanDir() const { return m_gameDir + QStringLiteral("/CKAN"); }
    QString downloadDir() const { return m_gameDir + QStringLiteral("/CKAN/downloads"); }
    QString historyDir() const { return m_gameDir + QStringLiteral("/CKAN/history"); }
    QString registryPath() const { return ckanDir() + QStringLiteral("/registry.json"); }
    QString compatibleVersionsPath() const { return ckanDir() + QStringLiteral("/compatible_ksp_versions.json"); }

    // 相对路径转换
    QString toRelativeGameDir(const QString &abs) const;
    QString toAbsoluteGameDir(const QString &rel) const;

    // 检测已安装的 KSP 版本（优先 buildID 文件经 build 映射表换算，其次 readme 兜底）
    GameVersion detectVersion() const;

    // 只读版本检测：直接按游戏目录检测版本，不创建任何 CKAN 目录结构。
    // 供 Steam 发现等只读扫描场景使用（不产生副作用）。
    static GameVersion detectVersionFromDir(const QString &gameDir);

    // 只读扫描 GameData 顶层文件夹，推导实例的安装类型标签，按固定顺序返回
    // （RSS → Sol → RO → RP-1；若不含这些模组且仅有官方目录，则返回 { Clean Stock }）。
    // 不区分大小写。corrupted 输出：GameData 缺失或缺少运行必需的 Squad 目录时为 true。
    static QStringList detectInstallKindTags(const QString &gameDir, bool *corrupted = nullptr);

    // 只读推导建议实例名：'KSP' + 空格 + 版本号 + 空格分隔的标签
    // （如 "KSP 1.12.5 RSS RO"、"KSP 1.12.5 Clean Stock"；版本检测失败时省略版本号）。
    static QString suggestedInstanceName(const QString &gameDir);

    // 扫描 GameData 下所有 .dll（排除 KSP 官方目录），推导手动安装模组的标识符。
    // 返回 identifier -> 相对 GameDir 路径，对应官方 ScanUnmanagedFiles/DllPathToIdentifier。
    QMap<QString, QString> scanUnmanagedDlls() const;

    // 扫描 GameData 下顶层文件夹，返回“手动占用”的文件夹（相对 GameDir，如 "GameData/SomeMod"）。
    // 判定：存在且非官方目录(Squad/SquadExpansion)，且不属于任何已登记安装模组的文件。
    // 覆盖无 DLL 的手动模组（纯配置/纹理），供安装前冲突预扫描使用。
    QStringList manualGameDataFolders() const;

    // 注册表读写（自动加载/保存 registry.json）。
    // registry.locked 文件锁：独占创建，防多个进程并发写坏 registry.json。
    // 加载时获取（最佳努力，失败仍只读加载）；保存前确保持有锁，拿不到锁则跳过写入并返回 false。
    Registry *registry();
    const Registry *registry() const { return &m_registry; }
    void loadRegistry();
    bool saveRegistry() const;
    // 当前进程是否持有注册表锁（另一进程占用时为 false）
    bool registryLockHeld() const { return m_registryLocked; }
    // 尝试获取注册表写锁（另一进程占用返回 false）。成功后锁在实例生命周期内保持。
    // 供"模组管理"页在加载索引前门控：锁被其他进程占用时不加载并等待。
    bool engageRegistryLock() { acquireRegistryLock(); return m_registryLocked; }

    // 事务回滚：用事务开始时的注册表 JSON 快照恢复内存状态，并同步写回 registry.json。
    // 空快照表示事务开始时注册表为空（会写入空注册表）。
    void restoreRegistrySnapshot(const QByteArray &json);

    // 报告是否有效（存在游戏文件）
    bool isValid() const;

    // 游戏目录（默认下载缓存目录：启动器 downloads，区别于 CKAN/downloads）
    // 外部可覆盖
    void setCustomDownloadDir(const QString &dir) { m_customDownloadDir = dir; }
    QString customDownloadDir() const { return m_customDownloadDir; }

private:
    QString m_gameDir;
    QString m_name;
    QString m_customDownloadDir;
    Registry m_registry;
    bool m_registryLoaded = false;
    mutable FileLock m_registryLock;   // registry.locked 跨进程互斥
    mutable bool     m_registryLocked = false;

    // 获取注册表锁（const 因为需要从 const saveRegistry 调用）。已持有直接成功。
    bool acquireRegistryLock() const;
};

} // namespace ckan

#endif // CKAN_GAMEINSTANCE_H