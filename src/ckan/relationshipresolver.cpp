#include "relationshipresolver.h"
#include "registry.h"
#include "version.h"

#include <QSet>

namespace ckan {

RelationshipResolver::RelationshipResolver(const QMap<QString, QVector<CkanModule>> &index)
    : m_index(index)
{
}

namespace {
// 选择满足版本约束的最高版本
CkanModule pickBest(const QVector<CkanModule> &candidates, const Relationship &rel)
{
    QVector<CkanModule> sorted = candidates;
    std::sort(sorted.begin(), sorted.end(), [](const CkanModule &a, const CkanModule &b) {
        return ModuleVersion(a.version) > ModuleVersion(b.version);
    });
    for (const CkanModule &c : sorted) {
        if (rel.versionSatisfies(c.version))
            return c;
    }
    return sorted.isEmpty() ? CkanModule() : sorted.first();
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

// 判断某依赖目标是否已被集合中的某个模块提供
bool providedBySet(const QSet<QString> &providedNames, const QString &target)
{
    return providedNames.contains(target);
}

// 检查某模块与已选集合的冲突
QString conflictWith(const CkanModule &m, const QSet<QString> &selectedIds,
                     const QMap<QString, CkanModule> &selectedByIdent,
                     const QMap<QString, QString> &providedToOwner)
{
    for (const Relationship &c : m.conflicts) {
        if (selectedIds.contains(c.name))
            return QStringLiteral("%1 conflicts with %2").arg(m.identifier, c.name);
        // 冲突目标可能是虚拟包
        if (providedToOwner.contains(c.name))
            return QStringLiteral("%1 conflicts with %2").arg(m.identifier, providedToOwner.value(c.name));
    }
    return QString();
}
} // namespace

ResolutionResult RelationshipResolver::resolve(const QVector<CkanModule> &modulesToInstall,
                                               const Registry &registry,
                                               bool autoInstallRecommends,
                                               bool withSuggests)
{
    ResolutionResult result;
    QMap<QString, CkanModule> selectedByIdent;   // identifier -> module
    QMap<QString, QString>    providedToOwner;   // virtual name -> provider identifier
    QSet<QString>             selectedProvided;  // 所有已提供的名称（含虚拟）

    // 已安装的模块视为已提供
    for (auto it = registry.installedModules.constBegin();
         it != registry.installedModules.constEnd(); ++it) {
        const InstalledModule &im = it.value();
        selectedByIdent[im.identifier] = im.module;
        const QSet<QString> prov = providedBy(im.module);
        selectedProvided.unite(prov);
        for (const QString &p : prov)
            if (!providedToOwner.contains(p)) providedToOwner[p] = im.identifier;
    }

    // 手动安装（DLL 扫描，AD）的模块也视为已提供：
    // 若某个待装模组依赖一个 AD 模组，直接视为已满足，不再下载。
    for (auto it = registry.installedDlls.constBegin();
         it != registry.installedDlls.constEnd(); ++it) {
        const QString id = it.key();
        if (id.isEmpty()) continue;
        selectedProvided.insert(id);
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
    QSet<QString> queued;
    for (const CkanModule &m : queue) queued.insert(m.identifier);

    // 处理队列（BFS 依赖展开）
    for (int i = 0; i < queue.size(); ++i) {
        const CkanModule m = queue.at(i);
        if (selectedProvided.contains(m.identifier))
            continue; // 已安装或已选中

        // 冲突检查
        QSet<QString> selectedIdentifiers;
        for (auto it = selectedByIdent.constBegin(); it != selectedByIdent.constEnd(); ++it)
            selectedIdentifiers.insert(it.key());
        const QString conflict = conflictWith(m, selectedIdentifiers, selectedByIdent, providedToOwner);
        if (!conflict.isEmpty()) {
            result.conflicts << conflict;
            result.conflicted = true;
        }

        // 加入选中集合
        selectedByIdent[m.identifier] = m;
        const QSet<QString> prov = providedBy(m);
        selectedProvided.unite(prov);
        for (const QString &p : prov)
            if (!providedToOwner.contains(p)) providedToOwner[p] = m.identifier;

        // 处理依赖
        auto processRel = [&](const Relationship &rel, bool optional) {
            // 已满足？
            if (providedBySet(selectedProvided, rel.name))
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
                if (!optional) {
                    result.notFound << rel.name;
                    result.missing = true;
                }
                return;
            }
            const CkanModule dep = pickBest(candidates, rel);
            if (dep.isValid() && !selectedProvided.contains(dep.identifier)
                && !queued.contains(dep.identifier)) {
                queue.append(dep);
                queued.insert(dep.identifier);
            }
        };
        for (const Relationship &rel : m.depends) processRel(rel, false);
        if (autoInstallRecommends)
            for (const Relationship &rel : m.recommends) processRel(rel, true);
    }

    // 收集级联建议模组（仅收集，不加入安装集；由 UI 层弹窗让用户勾选）
    if (withSuggests) {
        QSet<QString> suggestedSeen;   // 去重，防止级联成环
        QVector<CkanModule> suggestQueue;
        auto enqueueSuggests = [&](const CkanModule &m) {
            for (const Relationship &rel : m.suggests) {
                if (providedBySet(selectedProvided, rel.name))
                    continue; // 已安装/已选中，无需建议
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
                const CkanModule sug = pickBest(candidates, rel);
                if (!sug.isValid() || selectedProvided.contains(sug.identifier)
                    || suggestedSeen.contains(sug.identifier))
                    continue;
                suggestedSeen.insert(sug.identifier);
                result.suggestedModules.append(sug);
                suggestQueue.append(sug); // 级联：建议模组的建议也继续收集
            }
        };
        for (const CkanModule &m : queue) enqueueSuggests(m);
        for (int i = 0; i < suggestQueue.size(); ++i)
            enqueueSuggests(suggestQueue.at(i));
    }

    // 组装结果（保持依赖在前：按入队顺序）
    for (const CkanModule &m : queue) {
        if (selectedByIdent.contains(m.identifier))
            result.modulesToInstall.append(m);
    }
    return result;
}

} // namespace ckan