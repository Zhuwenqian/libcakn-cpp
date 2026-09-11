#ifndef CKAN_MODPACKIO_H
#define CKAN_MODPACKIO_H

#include "ckan_export.h"
#include "version.h"
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <functional>
#include <atomic>

namespace ckan {

// 整合包导入辅助函数（供启动器「导入整合包」使用）。
// 这些函数只做文件系统 + zip/JSON 处理，不涉及 UI，便于单元测试。

// 整合包元数据条目（zip 根目录下的固定文件名，与 GameData 同层）。
// 导出时写入：launcherVersion=启动器版本(导入仅显示不校验)、gameVersion=游戏版本(精确到
// patch，如 "1.12.5")、name=整合包名、description=描述(可空)。
inline constexpr const char *kModpackMetaFileName = "hkspl_package.json";

// 读取 zip 根目录下整合包元数据文件 hkspl_package.json 的原始 JSON 字节。
// 返回状态：Ok=成功且 json 写回；NotFound=zip 中无该条目；ReadError=存在但打开/解压失败。
enum class ModpackMetaStatus { NotFound, ReadError, Ok };
CKAN_API ModpackMetaStatus modpackReadPackageMeta(const QString &zipPath,
                                                  QByteArray *json, QString *error);

// 整合包游戏版本与当前实例版本的 minor 级兼容判定：major 与 minor 都相同即视为兼容（patch
// 差异不影响，如 1.12.4 vs 1.12.5 兼容）。任一侧版本无效时返回 false（无法比较）。
CKAN_API bool modpackVersionCompatible(const GameVersion &pkgVersion,
                                       const GameVersion &currentVersion);

// 在 zip 中探测顶层 GameData 目录，返回解压前缀（如 "GameData/" 或 "包名/GameData/"）。
// 找不到任何 GameData 目录时返回 false 并填充 error。
CKAN_API bool modpackZipGameDataPrefix(const QString &zipPath, QString *prefix, QString *error);

// 解析 .ckan 元包 JSON 的 depends 标识符列表（取顶层 depends[] 中每项的 name）。
// 解析失败填充 error 并返回空列表。
CKAN_API QStringList modpackCkanDepends(const QByteArray &json, QString *error);

// 清空实例 GameData（保留 Squad/SquadExpansion），并删除实例 CKAN 注册表。
// 删除成功返回 true；error 记录首个失败路径。
CKAN_API bool modpackClearGameData(const QString &gameDir, QString *error);

// 把 zip 中 GameData 内容导入到实例 GameData（先清空现有文件，再解压）。
// progress 取 0..1000（按解压字节比例）；cancelRequested 为 true 时安全提前返回 false。
CKAN_API bool modpackImportGameData(const QString &zipPath, const QString &gameDir,
                                    const std::function<void(int)> &progress,
                                    std::atomic_bool *cancelRequested, QString *error);

} // namespace ckan

#endif // CKAN_MODPACKIO_H