#include "repository.h"

namespace ckan {

QString Repository::defaultRepoUrl()
{
    return QStringLiteral("https://github.com/KSP-CKAN/CKAN-meta/archive/master.tar.gz");
}

QString Repository::repositoryListUrl()
{
    return QStringLiteral("https://raw.githubusercontent.com/KSP-CKAN/CKAN-meta/master/repositories.json");
}

Repository Repository::defaultKspRepo()
{
    Repository r;
    r.name = QStringLiteral("KSP-CKAN");
    r.uri  = defaultRepoUrl();
    return r;
}

Repository Repository::presetKspCkanBackup()
{
    Repository r;
    r.name = QStringLiteral("KSP-CKAN 备用");
    r.uri  = QStringLiteral("https://gitlab.com/KSP-CKAN/CKAN-meta/-/archive/master/CKAN-meta-master.tar.gz");
    r.comment = QStringLiteral("GitLab 归档备用源");
    return r;
}

Repository Repository::presetSol()
{
    Repository r;
    r.name = QStringLiteral("Sol");
    r.uri  = QStringLiteral("https://github.com/RSS-Reborn/CKAN-meta/archive/main.tar.gz");
    r.comment = QStringLiteral("RSS-Reborn 的 Sol 模组仓库");
    return r;
}

Repository Repository::presetMechJeb2Dev()
{
    Repository r;
    r.name = QStringLiteral("MechJeb2-dev");
    r.uri  = QStringLiteral("https://ksp.sarbian.com/ckan/MechJeb2-ci.tar.gz");
    r.comment = QStringLiteral("MechJeb2 CI 开发版");
    return r;
}

} // namespace ckan