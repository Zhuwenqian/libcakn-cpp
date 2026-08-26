#include "modpackio.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QBuffer>

#include "miniz.h"

namespace ckan {

// 把 zip 完整读入内存并打开（避免 mz_zip_reader_init_file 按 ANSI 代码页解析非 ASCII 路径）。
static bool openZipFromFile(const QString &zipPath, QByteArray *zipData,
                            mz_zip_archive *zip, QString *error)
{
    QFile f(zipPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 ZIP 文件：%1").arg(zipPath);
        return false;
    }
    *zipData = f.readAll();
    f.close();
    memset(zip, 0, sizeof(*zip));
    if (!mz_zip_reader_init_mem(zip, zipData->constData(),
                                static_cast<size_t>(zipData->size()), 0)) {
        if (error) *error = QStringLiteral("不是有效的 ZIP 归档（可能已损坏）：%1").arg(zipPath);
        return false;
    }
    return true;
}

// 计算某个条目"包含 GameData 目录"的完整前缀。
// 如 "GameData/a.dll" -> "GameData/"；"Pkg/GameData/a.dll" -> "Pkg/GameData/"；否则返回空。
static QString entryGameDataPrefix(const char *name)
{
    const QString path = QString::fromUtf8(name);
    const QStringList parts = path.split(QLatin1Char('/'));
    for (int i = 0; i < parts.size(); ++i) {
        if (parts[i].compare(QStringLiteral("GameData"), Qt::CaseInsensitive) == 0) {
            QStringList prefixParts;
            for (int j = 0; j <= i; ++j)
                prefixParts << parts[j];
            return prefixParts.join(QLatin1Char('/')) + QLatin1Char('/');
        }
    }
    return QString();
}

// 从已打开的 zip 中探测 GameData 前缀：取所有含 GameData 目录条目中最浅的一个。
static bool detectGameDataPrefix(mz_zip_archive *zip, QString *prefix, QString *error)
{
    const mz_uint count = mz_zip_reader_get_num_files(zip);
    int bestDepth = INT_MAX;
    QString best;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(zip, i, &st))
            continue;
        const QString p = entryGameDataPrefix(st.m_filename);
        if (p.isEmpty())
            continue;
        const int depth = p.count(QLatin1Char('/'));
        if (depth < bestDepth) {
            bestDepth = depth;
            best = p;
        }
    }
    if (best.isEmpty()) {
        if (error) *error = QStringLiteral("ZIP 中未找到 GameData 目录。");
        return false;
    }
    if (prefix) *prefix = best;
    return true;
}

bool modpackZipGameDataPrefix(const QString &zipPath, QString *prefix, QString *error)
{
    QByteArray zipData;
    mz_zip_archive zip;
    if (!openZipFromFile(zipPath, &zipData, &zip, error))
        return false;
    const bool ok = detectGameDataPrefix(&zip, prefix, error);
    mz_zip_reader_end(&zip);
    return ok;
}

QStringList modpackCkanDepends(const QByteArray &json, QString *error)
{
    QStringList out;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("解析 CKAN 文件失败：%1").arg(pe.errorString());
        return out;
    }
    const QJsonValue depends = doc.object().value(QStringLiteral("depends"));
    if (!depends.isArray()) {
        if (error) *error = QStringLiteral("CKAN 文件缺少 depends 字段。");
        return out;
    }
    for (const QJsonValue &item : depends.toArray()) {
        if (item.isString()) {
            out << item.toString();
        } else if (item.isObject()) {
            const QString name = item.toObject().value(QStringLiteral("name")).toString();
            if (!name.isEmpty())
                out << name;
        }
    }
    if (out.isEmpty()) {
        if (error) *error = QStringLiteral("CKAN 文件不包含任何可安装模组。");
    }
    return out;
}

// 递归删除目录下所有内容；deleteRoot=true 时连同根目录一起删除。返回是否全部成功。
static bool removeDirContents(QDir dir, bool deleteRoot, QString *error)
{
    bool ok = true;
    const QStringList entries = dir.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    for (const QString &name : entries) {
        const QString abs = dir.filePath(name);
        const QFileInfo info(abs);
        if (info.isDir()) {
            if (!removeDirContents(QDir(abs), true, error)) {
                if (error && error->isEmpty()) *error = abs;
                ok = false;
            }
        } else {
            if (!QFile::remove(abs)) {
                if (error && error->isEmpty()) *error = abs;
                ok = false;
            }
        }
    }
    if (deleteRoot && ok && !dir.rmdir(QStringLiteral(".")))
        ok = false;
    return ok;
}

bool modpackClearGameData(const QString &gameDir, QString *error)
{
    const QDir gameData(gameDir + QStringLiteral("/GameData"));
    if (gameData.exists()) {
        // 递归删除除 Squad/SquadExpansion 外的所有内容（这些文件夹被视为官方，必须保留）。
        const QStringList entries =
            gameData.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
        for (const QString &name : entries) {
            if (name.compare(QStringLiteral("Squad"), Qt::CaseInsensitive) == 0
                || name.compare(QStringLiteral("SquadExpansion"), Qt::CaseInsensitive) == 0)
                continue;
            const QFileInfo info(gameData.filePath(name));
            bool ok;
            if (info.isDir())
                ok = removeDirContents(QDir(info.absoluteFilePath()), true, error);
            else
                ok = QFile::remove(info.absoluteFilePath());
            if (!ok) {
                if (error && error->isEmpty()) *error = info.absoluteFilePath();
                return false;
            }
        }
    }
    // 清空模组后同步删除 CKAN 注册表，避免残留的手动安装记录与新整合包冲突。
    const QString registryPath = gameDir + QStringLiteral("/CKAN/registry.json");
    if (QFile::exists(registryPath) && !QFile::remove(registryPath)) {
        if (error) *error = QStringLiteral("无法删除注册表：%1").arg(registryPath);
        return false;
    }
    return true;
}

bool modpackImportGameData(const QString &zipPath, const QString &gameDir,
                           const std::function<void(int)> &progress,
                           std::atomic_bool *cancelRequested, QString *error)
{
    QByteArray zipData;
    mz_zip_archive zip;
    if (!openZipFromFile(zipPath, &zipData, &zip, error))
        return false;

    QString prefix;
    if (!detectGameDataPrefix(&zip, &prefix, error)) {
        mz_zip_reader_end(&zip);
        return false;
    }

    // 预扫描：统计 GameData 下待解压文件的总字节数，用于进度。
    const QString prefixUtf8 = prefix.toUtf8();
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    qint64 totalBytes = 0;
    QVector<mz_uint> targets;
    QVector<QString> relPaths;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            continue;
        const QString name = QString::fromUtf8(st.m_filename);
        if (!name.startsWith(prefix) || st.m_is_directory)
            continue;
        totalBytes += static_cast<qint64>(st.m_uncomp_size);
        targets.append(i);
        relPaths.append(name.mid(prefixUtf8.size()));
    }

    // 先清空现有 GameData（保留官方文件夹），再解压。
    if (cancelRequested && cancelRequested->load()) {
        mz_zip_reader_end(&zip);
        if (error) *error = QStringLiteral("导入已取消。");
        return false;
    }
    if (!modpackClearGameData(gameDir, error)) {
        mz_zip_reader_end(&zip);
        return false;
    }

    const QDir targetData(gameDir + QStringLiteral("/GameData"));
    if (!targetData.exists() && !targetData.mkpath(QStringLiteral("."))) {
        mz_zip_reader_end(&zip);
        if (error) *error = QStringLiteral("无法创建 GameData 目录。");
        return false;
    }

    qint64 doneBytes = 0;
    const int n = targets.size();
    for (int idx = 0; idx < n; ++idx) {
        if (cancelRequested && cancelRequested->load()) {
            mz_zip_reader_end(&zip);
            if (error) *error = QStringLiteral("导入已取消。");
            return false;
        }
        const mz_uint fileIndex = targets[idx];
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, fileIndex, &st)) {
            mz_zip_reader_end(&zip);
            if (error) *error = QStringLiteral("读取 ZIP 条目信息失败。");
            return false;
        }
        size_t size = 0;
        void *mem = mz_zip_reader_extract_to_heap(&zip, fileIndex, &size, 0);
        if (!mem) {
            mz_zip_reader_end(&zip);
            if (error) *error = QStringLiteral("解压失败：%1").arg(relPaths[idx]);
            return false;
        }

        const QString relPath = relPaths[idx];
        const QString absPath = targetData.filePath(relPath);
        const QDir parent = QFileInfo(absPath).absoluteDir();
        if (!parent.exists() && !parent.mkpath(QStringLiteral("."))) {
            mz_free(mem);
            mz_zip_reader_end(&zip);
            if (error) *error = QStringLiteral("无法创建目录：%1").arg(parent.absolutePath());
            return false;
        }

        QFile out(absPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            mz_free(mem);
            mz_zip_reader_end(&zip);
            if (error) *error = QStringLiteral("无法写入文件：%1").arg(absPath);
            return false;
        }
        out.write(QByteArray::fromRawData(static_cast<const char *>(mem),
                                          static_cast<int>(size)));
        out.close();
        mz_free(mem);

        doneBytes += static_cast<qint64>(st.m_uncomp_size);
        if (progress && totalBytes > 0) {
            const int permille = static_cast<int>(doneBytes * 1000 / totalBytes);
            progress(permille > 1000 ? 1000 : permille);
        }
    }

    mz_zip_reader_end(&zip);
    if (progress) progress(1000);
    return true;
}

} // namespace ckan