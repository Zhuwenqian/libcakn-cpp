#ifndef CKAN_RELATIONSHIPRESOLVER_H
#define CKAN_RELATIONSHIPRESOLVER_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>

#include "ckan_export.h"
#include "ckanmodule.h"
#include "version.h"

namespace ckan {

class Registry;

// 多提供者选择项：某一虚拟包有多个候选提供者，需要用户决定安装哪个。
// 对应官方 CKAN 的 TooManyModsProvideKraken（本实现不直接抛异常，
// 而是收集到 ResolutionResult.providerChoices 交给 UI 层弹窗选择后重新解析）。
struct CKAN_API ProviderChoice {
    QString provides;               // 虚拟包名
    QStringList requiredBy;         // 依赖它的模块标识符（可能多个）
    QString requirement;            // 约束描述（如版本范围，可能为空）
    QVector<CkanModule> candidates; // 满足版本约束且 KSP 兼容的候选提供者（按版本降序）
};

// 依赖解析结果
struct CKAN_API ResolutionResult {
    QVector<CkanModule> modulesToInstall;   // 按依赖顺序（依赖在前）
    QVector<CkanModule> suggestedModules;   // 级联建议安装的可选模组（不随 modulesToInstall 自动安装）
    QVector<ProviderChoice> providerChoices; // 需用户选择的多提供者（非空时 UI 应先弹窗处理）
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
    // kspVersion: 当前游戏版本；解析候选时按 KSP 兼容性过滤（无效版本视为不过滤）。
    // extraRange: 用户勾选的额外兼容区间；候选若兼容当前版本或兼容该区间任一即算兼容
    //   （无效区间视为不启用）。空勾选时调用方传无效区间，仅按 kspVersion 判断。
    //   - 依赖候选只取满足版本约束且 KSP 兼容的最高版本；
    //   - 已安装模组必须满足依赖的版本约束才算已满足（不满足则选新版升级）；
    //   - 冲突做双向检测（新模块声明的 + 已选模块声明的，含版本约束）；
    //   - 推荐/建议模块与已选集合冲突时静默跳过，硬依赖冲突计入 conflicts。
    ResolutionResult resolve(const QVector<CkanModule> &modulesToInstall,
                             const Registry &registry,
                             bool autoInstallRecommends = true,
                             bool withSuggests = false,
                             const GameVersion &kspVersion = GameVersion(),
                             const GameVersionRange &extraRange = GameVersionRange());

private:
    const QMap<QString, QVector<CkanModule>> &m_index;
    GameVersion m_kspVersion;
    GameVersionRange m_extraRange;
};

} // namespace ckan

#endif // CKAN_RELATIONSHIPRESOLVER_H
