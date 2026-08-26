#ifndef CKAN_MODPACKIO_H
#define CKAN_MODPACKIO_H

#include "ckan_export.h"
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <functional>
#include <atomic>

namespace ckan {

// 整合包导入辅助函数（供启动器「导入整合包」使用）。
// 这些函数只做文件系统 + zip/JSON 处理，不涉及 UI，便于单元测试。

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