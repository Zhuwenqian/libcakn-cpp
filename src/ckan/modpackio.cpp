#include "modpackio.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>

#include <cstdio>
#ifdef Q_OS_WIN
#include <wchar.h>
#endif

#include "miniz.h"

namespace ckan {

// 以 FILE* 打开 zip 并封装 miniz 的 mz_zip_archive。
// 采用 mz_zip_reader_init_cfile 按需 fseek/fread 懒读取，避免把整包载入内存。
// 关闭时必须同时调用 mz_zip_reader_end 与 fclose：CFILE 模式下 miniz 不负责关闭文件。
struct OpenedZip {
    mz_zip_archive zip;
    FILE *file = nullptr;
    bool opened = false;

    ~OpenedZip() { close(); }

    bool open(const QString &zipPath, QString *error)
    {
        memset(&zip, 0, sizeof(mz_zip_archive));
#ifdef Q_OS_WIN
        // Windows 的 fopen 按 ANSI 代码页解析路径，含中文等非 ASCII 时失败，
        // 必须用宽字符版本 _wfopen 才能按 Unicode 路径正确打开。
        file = ::_wfopen(reinterpret_cast<const wchar_t *>(zipPath.utf16()), L"rb");
#else
        // POSIX：直接按 UTF-8 路径打开。
        file = ::fopen(zipPath.toUtf8().constData(), "rb");
#endif
        if (!file) {
            if (error) *error = QStringLiteral("无法打开 ZIP 文件：%1").arg(zipPath);
            return false;
        }
        // archive_size 传 0，由 miniz 自行探测文件大小。
        // 导入是顺序遍历，不需要 miniz 为二分查找构建中央目录排序索引，传
        // DO_NOT_SORT_CENTRAL_DIRECTORY 省掉这份数组内存（巨型包尤其明显）。
        if (!mz_zip_reader_init_cfile(&zip, file, 0, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
            if (error) *error = QStringLiteral("不是有效的 ZIP 归档（可能已损坏）：%1").arg(zipPath);
            ::fclose(file);
            file = nullptr;
            return false;
        }
        opened = true;
        return true;
    }

    void close()
    {
        if (opened) {
            mz_zip_reader_end(&zip);
            opened = false;
        }
        if (file) {
            ::fclose(file);
            file = nullptr;
        }
    }
};

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

// 校验 zip 条目名：必须是 prefix 下的文件，规范化（折叠 ..、统一分隔符）后
// 不得为空、不得为绝对路径、不得逃逸出 GameData。通过时把相对路径写入 relOut。
// 失败时填充 error 并返回 false。
static bool safeEntryRelPath(const QString &name, const QString &prefixUtf8,
                             QString *relOut, QString *error)
{
    QString rel = name.mid(prefixUtf8.size());
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString cleaned = QDir::cleanPath(rel);
    if (cleaned.isEmpty() || cleaned == QStringLiteral("..")
        || cleaned.startsWith(QStringLiteral("../"))
        || QFileInfo(cleaned).isAbsolute()) {
        if (error) *error = QStringLiteral("整合包包含不安全的文件路径：%1").arg(name);
        return false;
    }
    *relOut = cleaned;
    return true;
}

bool modpackZipGameDataPrefix(const QString &zipPath, QString *prefix, QString *error)
{
    OpenedZip zip;
    if (!zip.open(zipPath, error))
        return false;
    const bool ok = detectGameDataPrefix(&zip.zip, prefix, error);
    zip.close();
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
    OpenedZip zip;
    if (!zip.open(zipPath, error))
        return false;

    QString prefix;
    if (!detectGameDataPrefix(&zip.zip, &prefix, error)) {
        zip.close();
        return false;
    }

    // 第一遍：统计待解压总字节数，并全量校验路径安全（防 Zip Slip）。
    // 不保存条目列表（省内存），校验全部通过后才允许清空 GameData。
    const QString prefixUtf8 = prefix.toUtf8();
    const mz_uint count = mz_zip_reader_get_num_files(&zip.zip);
    qint64 totalBytes = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip.zip, i, &st))
            continue;
        const QString name = QString::fromUtf8(st.m_filename);
        if (!name.startsWith(prefix) || st.m_is_directory)
            continue;
        QString rel;
        if (!safeEntryRelPath(name, prefixUtf8, &rel, error)) {
            zip.close();
            return false;
        }
        totalBytes += static_cast<qint64>(st.m_uncomp_size);
    }

    // 先清空现有 GameData（保留官方文件夹），再解压。
    if (cancelRequested && cancelRequested->load()) {
        zip.close();
        if (error) *error = QStringLiteral("导入已取消。");
        return false;
    }
    if (!modpackClearGameData(gameDir, error)) {
        zip.close();
        return false;
    }

    const QDir targetData(gameDir + QStringLiteral("/GameData"));
    if (!targetData.exists() && !targetData.mkpath(QStringLiteral("."))) {
        zip.close();
        if (error) *error = QStringLiteral("无法创建 GameData 目录。");
        return false;
    }

    // 第二遍：逐条解压。KSP 模组以大量零散小文件为主（dll/cfg/png 通常几 KB~几百 KB），
    // 针对这一特点做两处优化：
    //  1) 按大小分流：小文件用 extract_to_heap 一次性解压 + 单次写盘，省去为每个小文件
    //     创建/释放流式迭代器（~100KB 缓冲）的开销；大文件才走流式迭代器，内存占用固定。
    //  2) 目录创建缓存：每个父目录只 mkpath 一次，避免几万个小文件重复目录系统调用。
    QByteArray ioBuf(64 * 1024, Qt::Uninitialized);
    const qint64 smallThreshold = 1024 * 1024; // <= 1MB 视为小文件（KSP 绝大多数模组文件在此范围）
    qint64 doneBytes = 0;
    qint64 lastReported = 0;
    const qint64 reportStep = 1024 * 1024; // 每解压 1MB 上报一次进度，减少跨线程投递开销
    QSet<QString> ensuredDirs;
    const auto reportProgress = [&]() {
        if (progress && totalBytes > 0 && doneBytes - lastReported >= reportStep) {
            lastReported = doneBytes;
            const int permille = static_cast<int>(doneBytes * 1000 / totalBytes);
            progress(permille > 1000 ? 1000 : permille);
        }
    };
    for (mz_uint i = 0; i < count; ++i) {
        if (cancelRequested && cancelRequested->load()) {
            zip.close();
            if (error) *error = QStringLiteral("导入已取消。");
            return false;
        }
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip.zip, i, &st))
            continue;
        const QString name = QString::fromUtf8(st.m_filename);
        if (!name.startsWith(prefix) || st.m_is_directory)
            continue;
        QString rel;
        if (!safeEntryRelPath(name, prefixUtf8, &rel, error)) { // 第一遍已保证通过，纵深防御
            zip.close();
            return false;
        }
        // 解压目标必须位于 GameData 目录内（Zip Slip 纵深防御）。
        const QString absPath = QDir::cleanPath(targetData.filePath(rel));
        if (!absPath.startsWith(targetData.absolutePath() + QLatin1Char('/'))) {
            zip.close();
            if (error) *error = QStringLiteral("解压目标超出 GameData：%1").arg(rel);
            return false;
        }
        const QString parentPath = QFileInfo(absPath).absolutePath();
        if (!ensuredDirs.contains(parentPath)) {
            const QDir parent(parentPath);
            if (!parent.exists() && !parent.mkpath(QStringLiteral("."))) {
                zip.close();
                if (error) *error = QStringLiteral("无法创建目录：%1").arg(parentPath);
                return false;
            }
            ensuredDirs.insert(parentPath);
        }

        QFile out(absPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            zip.close();
            if (error) *error = QStringLiteral("无法写入文件：%1").arg(absPath);
            return false;
        }
        const qint64 uncomp = static_cast<qint64>(st.m_uncomp_size);
        if (uncomp <= smallThreshold) {
            // 小文件：一次性解压到堆（大小受阈值限制，峰值内存可控），单次写盘。
            size_t size = 0;
            void *mem = mz_zip_reader_extract_to_heap(&zip.zip, i, &size, 0);
            if (!mem) {
                out.close();
                zip.close();
                if (error) *error = QStringLiteral("解压失败：%1").arg(rel);
                return false;
            }
            if (out.write(static_cast<const char *>(mem), static_cast<qint64>(size))
                != static_cast<qint64>(size)) {
                mz_free(mem);
                out.close();
                zip.close();
                if (error) *error = QStringLiteral("无法写入文件：%1").arg(absPath);
                return false;
            }
            mz_free(mem);
            out.close();
            doneBytes += static_cast<qint64>(size);
            reportProgress();
        } else {
            // 大文件：流式迭代器解压，内存固定；解压完整性由 iter_free 的 CRC 校验保证。
            mz_zip_reader_extract_iter_state *it = mz_zip_reader_extract_iter_new(&zip.zip, i, 0);
            if (!it) {
                out.close();
                zip.close();
                if (error) *error = QStringLiteral("解压失败：%1").arg(rel);
                return false;
            }
            size_t n = 0;
            while ((n = mz_zip_reader_extract_iter_read(
                        it, ioBuf.data(), static_cast<size_t>(ioBuf.size()))) > 0) {
                if (out.write(ioBuf.constData(), static_cast<qint64>(n)) != static_cast<qint64>(n)) {
                    mz_zip_reader_extract_iter_free(it);
                    out.close();
                    zip.close();
                    if (error) *error = QStringLiteral("无法写入文件：%1").arg(absPath);
                    return false;
                }
                doneBytes += static_cast<qint64>(n);
                if (cancelRequested && cancelRequested->load()) {
                    mz_zip_reader_extract_iter_free(it);
                    out.close();
                    zip.close();
                    if (error) *error = QStringLiteral("导入已取消。");
                    return false;
                }
                reportProgress();
            }
            // 返回 true 表示解压完整且 CRC 通过。
            const bool extractedOk = mz_zip_reader_extract_iter_free(it);
            out.close();
            if (!extractedOk) {
                zip.close();
                if (error) *error = QStringLiteral("解压失败：%1").arg(rel);
                return false;
            }
        }
    }

    zip.close();
    if (progress) progress(1000);
    return true;
}

} // namespace ckan