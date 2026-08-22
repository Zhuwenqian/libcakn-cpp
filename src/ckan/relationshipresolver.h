#ifndef CKAN_RELATIONSHIPRESOLVER_H
#define CKAN_RELATIONSHIPRESOLVER_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>

#include "ckan_export.h"
#include "ckanmodule.h"

namespace ckan {

class Registry;

// 依赖解析结果
struct CKAN_API ResolutionResult {
    QVector<CkanModule> modulesToInstall;   // 按依赖顺序（依赖在前）
    QVector<CkanModule> suggestedModules;   // 级联建议安装的可选模组（不随 modulesToInstall 自动安装）
    QStringList         notFound;           // 无法满足的依赖
    QStringList         conflicts;          // 冲突描述
    bool                conflicted = false;
    bool                missing    = false;
};

// 依赖解析器，对应 CKAN 的 RelationshipResolver。
// 基于仓库索引 + 已安装 registry 解析安装所需的完整模块集合。
class CKAN_API RelationshipResolver
{
public:
    // index: 仓库索引（identifier -> versions）
    RelationshipResolver(const QMap<QString, QVector<CkanModule>> &index);

    // 解析安装 modulesToInstall 所需的完整集合（含依赖）。
    // autoInstallRecommends: 是否自动安装 recommends。
    // withSuggests: 是否收集级联建议模组到 suggestedModules（仅收集，不自动安装）。
    ResolutionResult resolve(const QVector<CkanModule> &modulesToInstall,
                             const Registry &registry,
                             bool autoInstallRecommends = true,
                             bool withSuggests = false);

private:
    const QMap<QString, QVector<CkanModule>> &m_index;
};

} // namespace ckan

#endif // CKAN_RELATIONSHIPRESOLVER_H