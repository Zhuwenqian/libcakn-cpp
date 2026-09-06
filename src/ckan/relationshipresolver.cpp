#include "relationshipresolver.h"
#include "registry.h"
#include "version.h"

#include <QSet>
#include <QMutex>
#include <algorithm>

namespace ckan {

RelationshipResolver::RelationshipResolver(const QMap<QString, QVector<CkanModule>> &index)
    : m_index(index)
{
}

namespace {

// 收集满足版本约束且 KSP 兼容的全部候选（按版本降序；ksp 无效时不过滤兼容性）。
// 兼容判定：兼容当前实例版本（ksp）或兼容用户勾选的额外区间（extraRange，任一满足即可）；
// extraRange 无效（未勾选）时仅按 ksp 判断。
// 无满足候选时返回空列表（调用方据此判定依赖缺失，而非退回不满足约束的版本）。
QVector<CkanModule> pickCandidates(const QVector<CkanModule> &candidates,
                                   const Relationship &rel, const GameVersion &ksp,
                                   const GameVersionRange &extraRange)
{
    QVector<CkanModule> out;
    for (const CkanModule &c : candidates) {
        bool compatible = c.isCompatible(ksp);
        if (!compatible && (extraRange.lowerSet() || extraRange.upperSet()))
            compatible = c.isCompatible(extraRange);
        if (!compatible) continue; // KSP 兼容性过滤
        if (rel.versionSatisfies(c.version))
            out.append(c);
    }
    std::sort(out.begin(), out.end(), [](const CkanModule &a, const CkanModule &b) {
        return ModuleVersion(a.version) > ModuleVersion(b.version);
    });
    return out;
}

// 版本约束的可读描述（用于多提供者选择弹窗提示），空约束返回空串
QString constraintText(const Relationship &rel)
{
    QStringList parts;
    if (!rel.minVersion.isEmpty())
        parts << (rel.minInclusive ? QStringLiteral(">= %1").arg(rel.minVersion)
                                   : QStringLiteral("> %1").arg(rel.minVersion));
    if (!rel.maxVersion.isEmpty())
        parts << (rel.maxInclusive ? QStringLiteral("<= %1").arg(rel.maxVersion)
                                   : QStringLiteral("< %1").arg(rel.maxVersion));
    return parts.join(QStringLiteral(" 且 "));
}

// 收集某模块提供的所有虚拟包名
QSet<QString> providedBy(const CkanModule &m)
{
    QSet<QString> s;
    s.insert(m.identifier);
    for (const Relationship &r : m.provides)
        if (!r.name.isEmpty()) s.insert(r.name);
    return s;
}

// 版本感知的"依赖已满足"判定（对应官方 descriptor.MatchesAny + WithinBounds）：
// - 提供者是 DLL（无模块条目，无版本）→ 满足任意版本约束；
// - 提供者经 provides 提供虚拟包 → 无法校验版本，视为满足；
// - 提供者是同标识符模块 → 其版本必须满足 rel 的版本约束。
// 修复：已安装模组版本不足时不再被误判为已满足。
bool dependencySatisfied(const QMap<QString, CkanModule> &selectedByIdent,
                         const QMap<QString, QString> &providedToOwner,
                         const Relationship &rel)
{
    const auto it = providedToOwner.constFind(rel.name);
    if (it == providedToOwner.constEnd())
        return false;
    const QString &owner = it.value();
    const auto modIt = selectedByIdent.constFind(owner);
    if (modIt == selectedByIdent.constEnd())
        return true; // DLL：无版本，满足任意版本约束
    const CkanModule &m = modIt.value();
    if (m.identifier != rel.name)
        return true; // 经虚拟包提供，无法校验版本
    return rel.versionSatisfies(m.version);
}

// 双向冲突检测（含版本约束），对应官方 CkanModule.ConflictsWith：
// 1) m 声明的 conflicts 命中已选集合中的模块（校验版本）或虚拟包（无法校验版本，直接冲突）；
// 2) 已选集合中模块声明的 conflicts 命中 m 或其提供的虚拟包。
// 修复：此前只检查新模块声明的冲突，已安装/已选模块反向声明的冲突被漏判。
QString conflictsWith(const CkanModule &m,
                      const QMap<QString, CkanModule> &selectedByIdent,
                      const QMap<QString, QString> &providedToOwner)
{
    for (const Relationship &c : m.conflicts) {
        const auto selIt = selectedByIdent.constFind(c.name);
        if (selIt != selectedByIdent.constEnd()) {
            if (c.versionSatisfies(selIt.value().version))
                return QStringLiteral("%1 %2 conflicts with %3 %4")
                    .arg(m.identifier, m.version, c.name, selIt.value().version);
            continue;
        }
        if (providedToOwner.contains(c.name)) // 冲突目标为虚拟包
            return QStringLiteral("%1 %2 conflicts with %3")
                .arg(m.identifier, m.version, c.name);
    }

    const QSet<QString> mProvided = providedBy(m);
    for (auto it = selectedByIdent.constBegin(); it != selectedByIdent.constEnd(); ++it) {
        const CkanModule &sel = it.value();
        for (const Relationship &c : sel.conflicts) {
            if (c.name == m.identifier) {
                if (c.versionSatisfies(m.version))
                    return QStringLiteral("%1 %2 conflicts with %3 %4")
                        .arg(sel.identifier, sel.version, m.identifier, m.version);
                continue;
            }
            if (mProvided.contains(c.name)) // 已选模块冲突目标为 m 提供的虚拟包
                return QStringLiteral("%1 %2 conflicts with %3 %4")
                    .arg(sel.identifier, sel.version, m.identifier, m.version);
        }
    }
    return QString();
}
} // namespace

ResolutionResult RelationshipResolver::resolve(const QVector<CkanModule> &modulesToInstall,
                                               const Registry &registry,
                                               bool autoInstallRecommends,
                                               bool withSuggests,
                                               const GameVersion &kspVersion,
                                               const GameVersionRange &extraRange)
{
    // 整个解析过程读取已安装 registry；与后台安装/扫描写互斥（递归锁，内部方法嵌套安全）
    QMutexLocker regLock(registry.mutex());
    m_kspVersion = kspVersion;
    m_extraRange = extraRange;
    ResolutionResult result;
    QMap<QString, CkanModule> selectedByIdent;   // identifier -> module（已安装 + 本次选中）
    QMap<QString, QString>    providedToOwner;   // virtual name -> provider identifier
    QSet<QString>             installedIdentifiers; // 已安装（registry）标识符，用于区分升级/版本冲突
    QSet<QString>             hardIds;           // 用户请求或硬依赖引入 → 冲突为硬冲突
    QSet<QString>             queued;            // 已入队待处理

    // 已安装的模块视为已提供（版本感知的满足判定见 dependencySatisfied）
    for (auto it = registry.installedModules.constBegin();
         it != registry.installedModules.constEnd(); ++it) {
        const InstalledModule &im = it.value();
        installedIdentifiers.insert(im.identifier);
        selectedByIdent[im.identifier] = im.module;
        for (const QString &p : providedBy(im.module))
            if (!providedToOwner.contains(p)) providedToOwner[p] = im.identifier;
    }

    // 手动安装（DLL 扫描，AD）的模块也视为已提供：
    // 若某个待装模组依赖一个 AD 模组，直接视为已满足，不再下载。
    for (auto it = registry.installedDlls.constBegin();
         it != registry.installedDlls.constEnd(); ++it) {
        const QString id = it.key();
        if (id.isEmpty()) continue;
        if (!providedToOwner.contains(id)) providedToOwner[id] = id;
    }

    // 虚拟包索引：虚拟包名 -> 提供该虚拟包的候选模块列表
    QMap<QString, QVector<CkanModule>> virtualIndex;
    for (auto it = m_index.constBegin(); it != m_index.constEnd(); ++it) {
        for (const CkanModule &cand : it.value()) {
            const QStringList prov = cand.providesList();
            for (const QString &p : prov) {
                QVector<CkanModule> &v = virtualIndex[p];
                bool exists = false;
                for (const CkanModule &existing : v)
                    if (existing.identifier == cand.identifier) { exists = true; break; }
                if (!exists)
                    v.append(cand);
            }
        }
    }

    QVector<CkanModule> queue = modulesToInstall;
    for (const CkanModule &m : queue) {
        queued.insert(m.identifier);
        hardIds.insert(m.identifier); // 用户显式请求 → 硬
    }

    // 虚拟包名 -> result.providerChoices 中的下标（多提供者去重 & 累积依赖方）
    QMap<QString, int> choiceIndex;

    // 处理队列（BFS 依赖展开）
    for (int i = 0; i < queue.size(); ++i) {
        const CkanModule m = queue.at(i);
        const QString id = m.identifier;

        // 已在选中集合？
        if (selectedByIdent.contains(id)) {
            const CkanModule &existing = selectedByIdent[id];
            if (existing.version == m.version)
                continue; // 同版本已安装/已选中
            if (installedIdentifiers.contains(id)) {
                // 已安装同标识符但版本不同 → 升级：替换选中条目，新版进入安装集
                selectedByIdent[id] = m;
                for (const QString &p : providedBy(m))
                    if (!providedToOwner.contains(p)) providedToOwner[p] = id;
            } else {
                // 两个待装项要求同标识符不同版本 → 版本冲突（官方 InconsistentKraken）
                result.conflicts << QStringLiteral("%1 %2 与已选 %3 %4 版本冲突")
                                    .arg(id, m.version, existing.identifier, existing.version);
                result.conflicted = true;
                continue;
            }
        } else {
            // 新模块：双向冲突裁决。冲突模块不进入安装集。
            const QString conflict = conflictsWith(m, selectedByIdent, providedToOwner);
            if (!conflict.isEmpty()) {
                if (hardIds.contains(id)) {
                    result.conflicts << conflict;
                    result.conflicted = true;
                }
                continue; // 推荐/建议引入的冲突静默跳过
            }
            selectedByIdent[id] = m;
            for (const QString &p : providedBy(m))
                if (!providedToOwner.contains(p)) providedToOwner[p] = id;
        }

        // 处理依赖（depends 硬性；recommends 视 autoInstallRecommends）
        auto enqueueDep = [&](const CkanModule &dep, bool hard,
                              const std::function<bool(const CkanModule &)> &satisfies,
                              const QString &depLabel) {
            const QString depId = dep.identifier;
            if (selectedByIdent.contains(depId)) {
                const CkanModule &existing = selectedByIdent[depId];
                if (satisfies(existing))
                    return; // 已安装/已选且版本满足
                if (installedIdentifiers.contains(depId)) {
                    // 已安装版本不足 → 入队新版升级
                    if (!queued.contains(depId)) { queue.append(dep); queued.insert(depId); }
                    hardIds.insert(depId);
                    return;
                }
                // 已选不同版本且不满足 → 版本冲突
                result.conflicts << QStringLiteral("%1 需要 %2，但已选 %3 %4")
                                    .arg(id, depLabel, existing.identifier, existing.version);
                result.conflicted = true;
                return;
            }
            // 全新候选：入队
            if (!queued.contains(depId)) {
                queue.append(dep);
                queued.insert(depId);
            }
            if (hard) hardIds.insert(depId); // 硬依赖 → 冲突为硬冲突
        };

        auto processRel = [&](const Relationship &rel, bool optional, bool isRecommend) {
            // ---- any_of：任一子依赖满足即可；否则取任一可用且不冲突的候选 ----
            if (!rel.anyOf.isEmpty()) {
                QStringList names;
                for (const Relationship &sub : rel.anyOf)
                    names << (sub.name.isEmpty() ? QStringLiteral("(未知)") : sub.name);
                // 任一子依赖已满足 → 整个 any_of 满足
                for (const Relationship &sub : rel.anyOf)
                    if (dependencySatisfied(selectedByIdent, providedToOwner, sub))
                        return;
                // 收集所有子关系的可用候选（按提供者去重）
                QVector<CkanModule> pool;
                {
                    QSet<QString> seen;
                    for (const Relationship &sub : rel.anyOf) {
                        QVector<CkanModule> candidates;
                        const auto found = m_index.constFind(sub.name);
                        if (found != m_index.constEnd())
                            candidates = found.value();
                        else {
                            const auto vf = virtualIndex.constFind(sub.name);
                            if (vf != virtualIndex.constEnd())
                                candidates = vf.value();
                        }
                        const QVector<CkanModule> valid = pickCandidates(candidates, sub, m_kspVersion, m_extraRange);
                        for (const CkanModule &c : valid)
                            if (!seen.contains(c.identifier)) { seen.insert(c.identifier); pool.append(c); }
                    }
                }
                if (pool.isEmpty()) {
                    // 任一子依赖都找不到可用候选
                    if (!optional) { result.notFound << names.join(QStringLiteral(" 或 ")); result.missing = true; }
                    return;
                }
                std::sort(pool.begin(), pool.end(), [](const CkanModule &a, const CkanModule &b) {
                    return ModuleVersion(a.version) > ModuleVersion(b.version);
                });
                // 冲突回退：从最高版本起选第一个不冲突的候选；全部冲突 → 视为缺失
                const CkanModule *chosen = nullptr;
                for (const CkanModule &c : pool)
                    if (conflictsWith(c, selectedByIdent, providedToOwner).isEmpty()) { chosen = &c; break; }
                if (!chosen) {
                    if (!optional) { result.notFound << names.join(QStringLiteral(" 或 ")); result.missing = true; }
                    return;
                }
                // 任一候选都不被已选版本满足（已在上面 return 过），升级/版本冲突交给主循环处理
                enqueueDep(*chosen, !optional,
                           [](const CkanModule &) { return false; },
                           names.join(QStringLiteral(" 或 ")));
                return;
            }

            // 版本感知的已满足判定
            if (dependencySatisfied(selectedByIdent, providedToOwner, rel))
                return;
            // 候选：优先按 identifier 精确匹配，其次按虚拟包索引匹配
            QVector<CkanModule> candidates;
            const auto found = m_index.constFind(rel.name);
            if (found != m_index.constEnd())
                candidates = found.value();
            else {
                const auto vf = virtualIndex.constFind(rel.name);
                if (vf != virtualIndex.constEnd())
                    candidates = vf.value();
            }
            if (candidates.isEmpty()) {
                if (!optional) { result.notFound << rel.name; result.missing = true; }
                return;
            }
            // 过滤版本约束 + KSP 兼容，按版本降序
            const QVector<CkanModule> valid = pickCandidates(candidates, rel, m_kspVersion, m_extraRange);
            if (valid.isEmpty()) {
                // 无满足版本约束且 KSP 兼容的候选
                if (!optional) { result.notFound << rel.name; result.missing = true; }
                return;
            }
            // 按提供者模块标识符去重（同模块的多个版本只算一个提供者）
            QVector<CkanModule> distinct;
            {
                QSet<QString> seen;
                for (const CkanModule &c : valid)
                    if (!seen.contains(c.identifier)) { seen.insert(c.identifier); distinct.append(c); }
            }
            // 虚拟包且有多个不同提供者 → 不自动选最高版本，交 UI 弹窗让用户选择
            // （对应官方 TooManyModsProvideKraken；本实现收集到 providerChoices 供 UI 处理后重新解析）。
            const bool isVirtual = (m_index.constFind(rel.name) == m_index.constEnd());
            if (isVirtual && distinct.size() > 1) {
                // 若某个候选提供者已被选中或已入队（UI 循环中用户选过的提供者，
                // 或本次请求中已明确要装），则该依赖已由它提供，不再弹窗选择。
                bool queuedProvider = false;
                for (const CkanModule &c : distinct) {
                    if (selectedByIdent.contains(c.identifier) || queued.contains(c.identifier)) {
                        queuedProvider = true;
                        break;
                    }
                }
                if (queuedProvider)
                    return; // 依赖将由已入队/已选中的提供者满足
                const auto ci = choiceIndex.constFind(rel.name);
                if (ci == choiceIndex.constEnd()) {
                    choiceIndex[rel.name] = result.providerChoices.size();
                    ProviderChoice pc;
                    pc.provides    = rel.name;
                    pc.requiredBy.append(id);
                    pc.requirement = constraintText(rel);
                    pc.candidates  = distinct;
                    result.providerChoices.append(pc);
                } else {
                    ProviderChoice &pc = result.providerChoices[ci.value()];
                    if (!pc.requiredBy.contains(id)) pc.requiredBy.append(id);
                }
                return; // 等待用户选择后重新解析
            }
            // 单选：取满足约束的最高版本
            const CkanModule dep = distinct.first();
            const QString depLabel = rel.version.isEmpty() ? rel.name : rel.name + QLatin1Char(' ') + rel.version;
            enqueueDep(dep, !isRecommend,
                       [&rel](const CkanModule &m) { return rel.versionSatisfies(m.version); },
                       depLabel);
        };
        for (const Relationship &rel : m.depends) processRel(rel, false, false);
        if (autoInstallRecommends)
            for (const Relationship &rel : m.recommends) processRel(rel, true, true);
    }

    // 收集级联建议模组（仅收集，不加入安装集；由 UI 层弹窗让用户勾选）
    if (withSuggests) {
        QSet<QString> suggestedSeen;   // 去重，防止级联成环
        QVector<CkanModule> suggestQueue;
        auto enqueueSuggests = [&](const CkanModule &m) {
            for (const Relationship &rel : m.suggests) {
                if (dependencySatisfied(selectedByIdent, providedToOwner, rel))
                    continue; // 已安装/已选中（含版本满足），无需建议
                QVector<CkanModule> candidates;
                const auto found = m_index.constFind(rel.name);
                if (found != m_index.constEnd())
                    candidates = found.value();
                else {
                    const auto vf = virtualIndex.constFind(rel.name);
                    if (vf != virtualIndex.constEnd())
                        candidates = vf.value();
                }
                if (candidates.isEmpty())
                    continue; // 建议找不到对应的模组，忽略
                const QVector<CkanModule> validSugs = pickCandidates(candidates, rel, m_kspVersion, m_extraRange);
                if (validSugs.isEmpty())
                    continue; // 无 KSP 兼容且满足约束的候选
                const CkanModule sug = validSugs.first();
                if (suggestedSeen.contains(sug.identifier)
                    || providedToOwner.contains(sug.identifier))
                    continue; // 去重 / 已安装或已选中
                if (!conflictsWith(sug, selectedByIdent, providedToOwner).isEmpty())
                    continue; // 建议模块与已选集合冲突 → 静默跳过
                suggestedSeen.insert(sug.identifier);
                result.suggestedModules.append(sug);
                suggestQueue.append(sug); // 级联：建议模组的建议也继续收集
            }
        };
        for (const CkanModule &m : queue) enqueueSuggests(m);
        for (int i = 0; i < suggestQueue.size(); ++i)
            enqueueSuggests(suggestQueue.at(i));
    }

    // 组装结果（保持依赖在前：按入队顺序；只取与当前选中版本一致的队列项）
    for (const CkanModule &m : queue) {
        const auto it = selectedByIdent.constFind(m.identifier);
        if (it != selectedByIdent.constEnd() && it.value().version == m.version)
            result.modulesToInstall.append(m);
    }
    return result;
}

} // namespace ckan
