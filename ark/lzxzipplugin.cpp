#include "lzxzipplugin.h"

#include "archiveentry.h"
#include "queries.h"

#include <KPluginFactory>
#include <KPluginMetaData>

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QThread>

#include <exception>
#include <filesystem>
#include <vector>

K_PLUGIN_CLASS_WITH_JSON(LzxZipPlugin, "kerfuffle_lzxzip.json")

using Kerfuffle::Archive;

namespace {

/// Zip's "made by" host value for Unix, whose external attributes carry a
/// mode in their high 16 bits.
constexpr std::uint8_t kHostUnix = 3;

/// The compression method name Ark's dialogs offer for new LZX archives.
const QString kLzxMethod = QStringLiteral("LZX");

QString methodName(std::uint16_t method)
{
    switch (method) {
    case lzx::kMethodStored:
        return QStringLiteral("Store");
    case lzx::kMethodDeflate:
        return QStringLiteral("Deflate");
    case lzx::kMethodXMemCompress:
        return kLzxMethod;
    default:
        return QString::number(method);
    }
}

/// An MS-DOS date and time, as zip stores them, in local time. Invalid for
/// the all-zero value some tools write.
QDateTime fromDosTime(std::uint16_t date, std::uint16_t time)
{
    const QDate day(1980 + (date >> 9), (date >> 5) & 0xF, date & 0x1F);
    const QTime clock(time >> 11, (time >> 5) & 0x3F, (time & 0x1F) * 2);
    return day.isValid() && clock.isValid() ? QDateTime(day, clock) : QDateTime();
}

QFileDevice::Permissions fromUnixMode(std::uint32_t mode)
{
    QFileDevice::Permissions permissions;
    const std::pair<std::uint32_t, QFileDevice::Permission> bits[] = {
        {0400, QFileDevice::ReadOwner},
        {0200, QFileDevice::WriteOwner},
        {0100, QFileDevice::ExeOwner},
        {0040, QFileDevice::ReadGroup},
        {0020, QFileDevice::WriteGroup},
        {0010, QFileDevice::ExeGroup},
        {0004, QFileDevice::ReadOther},
        {0002, QFileDevice::WriteOther},
        {0001, QFileDevice::ExeOther},
    };
    for (const auto& [bit, permission] : bits) {
        if ((mode & bit) != 0) {
            permissions |= permission;
        }
    }
    return permissions;
}

QString entryPath(const lzx::ZipEntry& entry)
{
    return QString::fromUtf8(entry.path());
}

} // namespace

LzxZipPlugin::LzxZipPlugin(QObject* parent, const QVariantList& args)
    : ReadWriteArchiveInterface(parent, args)
{
    const QFileInfo info(filename());
    bool plainZip = false;
    if (info.exists()) {
        try {
            plainZip = !lzx::ZipArchive::open(info.filesystemAbsoluteFilePath()).needsLzxReader();
        } catch (const lzx::Error& e) {
            // Not a zip lzx can parse; libzip, if present, reports on it.
            m_openError = QString::fromUtf8(e.what());
        }
    }

    if (!info.exists() || plainZip || !m_openError.isEmpty()) {
        m_libzip = loadLibzip(args);
        if (m_libzip != nullptr) {
            forwardSignals();
            return;
        }
        if (!info.exists()) {
            // Without libzip only LZX archives can be created.
            return;
        }
    }
    // An LZX archive, or a plain one when libzip is missing: lzx handles it,
    // and writing it back produces the game-archive layout.
    if (m_openError.isEmpty()) {
        openArchive();
    }
}

LzxZipPlugin::~LzxZipPlugin()
{
    for (Archive::Entry* entry : std::as_const(m_emittedEntries)) {
        // Entries might still be waiting in queued slots.
        entry->deleteLater();
    }
}

Kerfuffle::ReadWriteArchiveInterface* LzxZipPlugin::loadLibzip(const QVariantList& args)
{
    const KPluginMetaData metaData =
        KPluginMetaData::findPluginById(QStringLiteral("kerfuffle"), QStringLiteral("kerfuffle_libzip"));
    if (!metaData.isValid()) {
        return nullptr;
    }
    const QVariantList libzipArgs{args.value(0), QVariant::fromValue(metaData)};
    return KPluginFactory::instantiatePlugin<ReadWriteArchiveInterface>(metaData, this, libzipArgs).plugin;
}

void LzxZipPlugin::forwardSignals()
{
    // Direct connections re-emit in the thread libzip emits from, so Ark's
    // jobs see the same threading as with libzip on its own.
    constexpr auto direct = Qt::DirectConnection;
    connect(m_libzip, &ReadOnlyArchiveInterface::cancelled, this, &ReadOnlyArchiveInterface::cancelled, direct);
    connect(m_libzip, &ReadOnlyArchiveInterface::error, this, &ReadOnlyArchiveInterface::error, direct);
    connect(m_libzip, &ReadOnlyArchiveInterface::entry, this, &ReadOnlyArchiveInterface::entry, direct);
    connect(m_libzip, &ReadOnlyArchiveInterface::progress, this, &ReadOnlyArchiveInterface::progress, direct);
    connect(m_libzip, &ReadOnlyArchiveInterface::info, this, &ReadOnlyArchiveInterface::info, direct);
    connect(m_libzip, &ReadOnlyArchiveInterface::finished, this, &ReadOnlyArchiveInterface::finished, direct);
    connect(m_libzip, &ReadOnlyArchiveInterface::testSuccess, this, &ReadOnlyArchiveInterface::testSuccess, direct);
    connect(m_libzip, &ReadOnlyArchiveInterface::compressionMethodFound, this,
        &ReadOnlyArchiveInterface::compressionMethodFound, direct);
    connect(m_libzip, &ReadOnlyArchiveInterface::encryptionMethodFound, this,
        &ReadOnlyArchiveInterface::encryptionMethodFound, direct);
    connect(m_libzip, &ReadOnlyArchiveInterface::userQuery, this, &ReadOnlyArchiveInterface::userQuery, direct);
    connect(m_libzip, &ReadWriteArchiveInterface::entryRemoved, this, &ReadWriteArchiveInterface::entryRemoved, direct);
}

void LzxZipPlugin::releaseLibzip()
{
    disconnect(m_libzip, nullptr, this, nullptr);
    // Jobs run in their own thread; the plugin belongs to the main thread.
    m_libzip->deleteLater();
    m_libzip = nullptr;
}

void LzxZipPlugin::pushState()
{
    m_libzip->setPassword(password());
    m_libzip->setHeaderEncryptionEnabled(isHeaderEncryptionEnabled());
}

void LzxZipPlugin::pullState()
{
    // libzip asks for a missing password itself and keeps the answer.
    if (m_libzip->password() != password()) {
        setPassword(m_libzip->password());
    }
    m_comment = m_libzip->comment();
    m_numberOfVolumes = m_libzip->numberOfVolumes();
    setMultiVolume(m_libzip->isMultiVolume());
}

template<typename Call>
bool LzxZipPlugin::forward(Call call)
{
    pushState();
    const bool result = call(m_libzip);
    pullState();
    return result;
}

bool LzxZipPlugin::openArchive()
{
    m_archive.reset();
    m_indexByPath.clear();
    try {
        m_archive = lzx::ZipArchive::open(QFileInfo(filename()).filesystemAbsoluteFilePath());
    } catch (const lzx::Error& e) {
        m_openError = QString::fromUtf8(e.what());
        return false;
    }
    m_openError.clear();
    const std::vector<lzx::ZipEntry>& entries = m_archive->entries();
    for (qsizetype i = 0; i < static_cast<qsizetype>(entries.size()); ++i) {
        m_indexByPath.insert(entryPath(entries[static_cast<std::size_t>(i)]), i);
    }
    return true;
}

bool LzxZipPlugin::rewrite(const std::function<void(lzx::ZipWriter&)>& build)
{
    try {
        lzx::ZipWriter writer(QFileInfo(filename()).filesystemAbsoluteFilePath());
        if (m_archive) {
            writer.setComment(m_archive->comment());
        }
        build(writer);
        if (QThread::currentThread()->isInterruptionRequested()) {
            // The writer discards its temporary file; the archive is unchanged.
            return false;
        }
        writer.finish();
    } catch (const std::exception& e) {
        Q_EMIT error(QStringLiteral("Failed to write the archive: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
    if (!openArchive()) {
        Q_EMIT error(m_openError);
        return false;
    }
    return true;
}

QList<LzxZipPlugin::NewFile> LzxZipPlugin::filesToAdd(const QList<Archive::Entry*>& files,
    const Archive::Entry* destination) const
{
    // Ark passes paths relative to the working directory it has changed
    // into; entry names follow libzip: the destination folder, then the
    // path as given. Game archives have no directory entries, so folders
    // only contribute their files.
    const QString prefix = destination != nullptr ? destination->fullPath() : QString();
    QList<NewFile> result;
    for (const Archive::Entry* e : files) {
        const QString path = e->fullPath(Kerfuffle::NoTrailingSlash);
        if (QFileInfo(path).isDir()) {
            QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::Readable | QDir::NoDotAndDotDot,
                QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString file = it.next();
                result.append({prefix + QDir::fromNativeSeparators(file), file});
            }
        } else {
            result.append({prefix + QDir::fromNativeSeparators(path), path});
        }
    }
    for (NewFile& file : result) {
        while (file.name.startsWith(QLatin1Char('/'))) {
            file.name.remove(0, 1);
        }
        if (file.name.startsWith(QLatin1String("./"))) {
            file.name.remove(0, 2);
        }
    }
    return result;
}

void LzxZipPlugin::writeFile(lzx::ZipWriter& writer, const NewFile& file) const
{
    QFile input(file.path);
    if (!input.open(QIODevice::ReadOnly)) {
        throw lzx::Error("cannot read " + file.path.toStdString());
    }
    const QByteArray data = input.readAll();
    const auto time = lzx::DosDateTime::fromFileTime(
        std::filesystem::last_write_time(QFileInfo(file.path).filesystemAbsoluteFilePath()));
    writer.add(file.name.toStdString(),
        std::span(reinterpret_cast<const std::uint8_t*>(data.constData()), static_cast<std::size_t>(data.size())),
        time);
}

QList<std::pair<QString, QString>> LzxZipPlugin::destinationPairs(const QList<Archive::Entry*>& files,
    const Archive::Entry* destination, bool move) const
{
    // Ark's own helper maps paths the way libzip's moves and copies do.
    QStringList sources = entryFullPaths(files);
    if (move) {
        sources.sort();
    }
    const QStringList targets =
        entryPathsFromDestination(sources, destination, move ? static_cast<int>(entriesWithoutChildren(files).count()) : 0);
    QList<std::pair<QString, QString>> pairs;
    for (qsizetype i = 0; i < sources.size() && i < targets.size(); ++i) {
        pairs.append({sources.at(i), targets.at(i)});
    }
    return pairs;
}

bool LzxZipPlugin::list()
{
    // Entry counting restarts with every listing, as in libzip.
    m_numberOfEntries = 0;
    if (m_libzip != nullptr) {
        return forward([](ReadWriteArchiveInterface* libzip) {
            return libzip->list();
        });
    }
    if (!m_archive) {
        Q_EMIT error(m_openError);
        return false;
    }

    m_comment = QString::fromUtf8(m_archive->comment());
    const std::vector<lzx::ZipEntry>& entries = m_archive->entries();
    QSet<std::uint16_t> methodsSeen;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            break;
        }
        const lzx::ZipEntry& zipEntry = entries[i];
        auto* e = new Archive::Entry();
        e->setFullPath(entryPath(zipEntry));
        if (zipEntry.isDirectory()) {
            e->setProperty("isDirectory", true);
        }
        const QDateTime timestamp = fromDosTime(zipEntry.dosDate, zipEntry.dosTime);
        if (timestamp.isValid()) {
            e->setProperty("timestamp", timestamp);
        }
        e->setProperty("size", static_cast<qulonglong>(zipEntry.uncompressedSize));
        e->setProperty("compressedSize", static_cast<qlonglong>(zipEntry.compressedSize));
        if (!zipEntry.isDirectory()) {
            e->setProperty("CRC",
                QStringLiteral("%1").arg(static_cast<qulonglong>(zipEntry.crc32), 8, 16, QLatin1Char('0')).toUpper());
        }
        const QString method = methodName(zipEntry.method);
        e->setProperty("method", method);
        if (!methodsSeen.contains(zipEntry.method)) {
            methodsSeen.insert(zipEntry.method);
            Q_EMIT compressionMethodFound(method);
        }
        const std::uint32_t mode = zipEntry.externalAttributes >> 16;
        if (zipEntry.hostSystem == kHostUnix && mode != 0) {
            e->setProperty("permissions", permissionsToString(static_cast<mode_t>(mode)));
        }

        Q_EMIT entry(e);
        m_emittedEntries << e;
        Q_EMIT progress(static_cast<double>(i + 1) / static_cast<double>(entries.size()));
    }
    return true;
}

bool LzxZipPlugin::testArchive()
{
    if (m_libzip != nullptr) {
        return forward([](ReadWriteArchiveInterface* libzip) {
            return libzip->testArchive();
        });
    }
    if (!m_archive) {
        Q_EMIT error(m_openError);
        return false;
    }

    const std::vector<lzx::ZipEntry>& entries = m_archive->entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            return false;
        }
        if (entries[i].isDirectory()) {
            continue;
        }
        try {
            (void)m_archive->read(entries[i]);
        } catch (const lzx::Error& e) {
            Q_EMIT error(QString::fromUtf8(e.what()));
            return false;
        }
        Q_EMIT progress(static_cast<double>(i + 1) / static_cast<double>(entries.size()));
    }
    Q_EMIT testSuccess();
    return true;
}

bool LzxZipPlugin::extractFiles(const QList<Archive::Entry*>& files, const QString& destinationDirectory,
    const Kerfuffle::ExtractionOptions& options)
{
    if (m_libzip != nullptr) {
        return forward([&](ReadWriteArchiveInterface* libzip) {
            return libzip->extractFiles(files, destinationDirectory, options);
        });
    }
    if (!m_archive) {
        Q_EMIT error(m_openError);
        return false;
    }

    m_overwriteAll = false;
    m_skipAll = false;
    const bool removeRootNode = options.isDragAndDropEnabled();
    const std::vector<lzx::ZipEntry>& entries = m_archive->entries();

    if (files.isEmpty()) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (QThread::currentThread()->isInterruptionRequested()) {
                break;
            }
            if (!extractEntry(entries[i], entryPath(entries[i]), QString(), destinationDirectory,
                    options.preservePaths(), removeRootNode)) {
                return false;
            }
            Q_EMIT progress(static_cast<double>(i + 1) / static_cast<double>(entries.size()));
        }
        return true;
    }

    qsizetype done = 0;
    for (const Archive::Entry* e : files) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            break;
        }
        const QString fullPath = e->fullPath();
        const qsizetype index = m_indexByPath.value(fullPath, -1);
        if (index < 0) {
            // Ark shows folders the archive only implies; they have no entry.
            if (e->isDir()) {
                continue;
            }
            Q_EMIT error(QStringLiteral("%1 is not in the archive.").arg(fullPath));
            return false;
        }
        if (!extractEntry(entries[static_cast<std::size_t>(index)], fullPath, e->rootNode, destinationDirectory,
                options.preservePaths(), removeRootNode)) {
            return false;
        }
        Q_EMIT progress(static_cast<double>(++done) / static_cast<double>(files.size()));
    }
    return true;
}

bool LzxZipPlugin::extractEntry(const lzx::ZipEntry& entry, const QString& fullPath, const QString& rootNode,
    const QString& destinationDirectory, bool preservePaths, bool removeRootNode)
{
    QString base = destinationDirectory;
    if (!base.endsWith(QLatin1Char('/'))) {
        base += QLatin1Char('/');
    }

    // Destination rules follow Ark's libzip plugin.
    QString destination;
    if (preservePaths) {
        if (!removeRootNode || rootNode.isEmpty()) {
            destination = base + fullPath;
        } else {
            destination = base + fullPath.mid(rootNode.size());
        }
    } else {
        if (entry.isDirectory()) {
            return true;
        }
        destination = base + QFileInfo(fullPath).fileName();
    }

    if (entry.isDirectory()) {
        if (!QDir().mkpath(destination)) {
            Q_EMIT error(QStringLiteral("Failed to create directory: %1").arg(destination));
            return false;
        }
        return true;
    }
    if (!QDir().mkpath(QFileInfo(destination).path())) {
        Q_EMIT error(QStringLiteral("Failed to create directory: %1").arg(QFileInfo(destination).path()));
        return false;
    }

    QString renamedEntry = fullPath;
    while (!m_overwriteAll && QFileInfo::exists(destination)) {
        if (m_skipAll) {
            return true;
        }
        Kerfuffle::OverwriteQuery query(renamedEntry);
        query.setArchiveFileName(filename());
        query.setArchiveMimeType(mimetype().name());
        query.setDestination(destination);
        Q_EMIT userQuery(&query);
        query.waitForResponse();

        if (query.responseCancelled()) {
            Q_EMIT cancelled();
            return false;
        }
        if (query.responseSkip()) {
            return true;
        }
        if (query.responseAutoSkip()) {
            m_skipAll = true;
            return true;
        }
        if (query.responseRename()) {
            const QString newName = QFileInfo(query.newFilename()).fileName();
            destination = QFileInfo(destination).path() + QLatin1Char('/') + newName;
            renamedEntry = QFileInfo(fullPath).path() + QLatin1Char('/') + newName;
        } else if (query.responseOverwriteAll()) {
            m_overwriteAll = true;
            break;
        } else if (query.responseOverwrite()) {
            break;
        }
    }

    std::vector<std::uint8_t> data;
    try {
        data = m_archive->read(entry);
    } catch (const lzx::Error& e) {
        Q_EMIT error(QStringLiteral("Failed to read %1: %2").arg(fullPath, QString::fromUtf8(e.what())));
        return false;
    }

    QFile file(destination);
    if (!file.open(QIODevice::WriteOnly)) {
        Q_EMIT error(QStringLiteral("Failed to open file for writing: %1").arg(destination));
        return false;
    }
    if (file.write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size()))
        != static_cast<qint64>(data.size())) {
        Q_EMIT error(QStringLiteral("Failed to write data for entry: %1").arg(fullPath));
        return false;
    }
    const std::uint32_t mode = entry.externalAttributes >> 16;
    if (entry.hostSystem == kHostUnix && mode != 0) {
        file.setPermissions(fromUnixMode(mode));
    }
    // Flush first, or the buffered write would update the time again.
    file.flush();
    const QDateTime timestamp = fromDosTime(entry.dosDate, entry.dosTime);
    if (timestamp.isValid()) {
        file.setFileTime(timestamp, QFileDevice::FileModificationTime);
    }
    file.close();
    return true;
}

bool LzxZipPlugin::addFiles(const QList<Archive::Entry*>& files, const Archive::Entry* destination,
    const Kerfuffle::CompressionOptions& options, uint numberOfEntriesToAdd)
{
    const bool wantsLzx = options.compressionMethod() == kLzxMethod;
    const bool creating = !QFileInfo::exists(filename());
    if (m_libzip != nullptr) {
        if (!wantsLzx) {
            return forward([&](ReadWriteArchiveInterface* libzip) {
                return libzip->addFiles(files, destination, options, numberOfEntriesToAdd);
            });
        }
        if (!creating) {
            Q_EMIT error(QStringLiteral(
                "LZX compression can only be used for new archives and archives that already use it."));
            return false;
        }
        // A new archive made with LZX belongs to lzx from here on.
        releaseLibzip();
    } else if (!m_archive && !creating) {
        Q_EMIT error(m_openError);
        return false;
    }

    const QList<NewFile> newFiles = filesToAdd(files, destination);
    QHash<QString, qsizetype> byKey;
    for (qsizetype i = 0; i < newFiles.size(); ++i) {
        byKey.insert(QString::fromStdString(lzx::ZipArchive::normalizeName(newFiles.at(i).name.toStdString())), i);
    }
    QSet<qsizetype> written;
    const bool ok = rewrite([&](lzx::ZipWriter& writer) {
        const auto total = static_cast<double>(newFiles.size() + (m_archive ? m_archive->entries().size() : 0));
        std::size_t done = 0;
        if (m_archive) {
            // Existing entries keep their place; one with the same name as a
            // new file is replaced by it, as libzip does.
            for (const lzx::ZipEntry& entry : m_archive->entries()) {
                if (QThread::currentThread()->isInterruptionRequested()) {
                    return;
                }
                const auto match = byKey.constFind(
                    QString::fromStdString(lzx::ZipArchive::normalizeName(entry.name)));
                if (match == byKey.cend()) {
                    writer.copy(*m_archive, entry);
                } else {
                    writeFile(writer, {QString::fromStdString(entry.name), newFiles.at(match.value()).path});
                    written.insert(match.value());
                }
                Q_EMIT progress(static_cast<double>(++done) / total);
            }
        }
        for (qsizetype i = 0; i < newFiles.size(); ++i) {
            if (QThread::currentThread()->isInterruptionRequested()) {
                return;
            }
            if (!written.contains(i)) {
                writeFile(writer, newFiles.at(i));
            }
            Q_EMIT progress(static_cast<double>(++done) / total);
        }
    });
    // Relist so Ark's view matches the rewritten archive, as libzip does.
    return ok && list();
}

bool LzxZipPlugin::moveFiles(const QList<Archive::Entry*>& files, Archive::Entry* destination,
    const Kerfuffle::CompressionOptions& options)
{
    if (m_libzip != nullptr) {
        return forward([&](ReadWriteArchiveInterface* libzip) {
            return libzip->moveFiles(files, destination, options);
        });
    }
    if (!m_archive) {
        Q_EMIT error(m_openError);
        return false;
    }

    const QList<std::pair<QString, QString>> pairs = destinationPairs(files, destination, true);
    QStringList removed;
    const bool ok = rewrite([&](lzx::ZipWriter& writer) {
        for (const lzx::ZipEntry& entry : m_archive->entries()) {
            const QString path = entryPath(entry);
            QString target;
            for (const auto& [from, to] : pairs) {
                if (path == from) {
                    target = to;
                    break;
                }
                if (from.endsWith(QLatin1Char('/')) && path.startsWith(from)) {
                    target = to + path.mid(from.size());
                    break;
                }
            }
            if (target.isEmpty()) {
                writer.copy(*m_archive, entry);
            } else {
                writer.copy(*m_archive, entry, target.toStdString());
                removed << path;
            }
        }
    });
    if (!ok) {
        return false;
    }
    for (const auto& [from, to] : pairs) {
        if (from.endsWith(QLatin1Char('/')) && !removed.contains(from)) {
            removed << from;
        }
    }
    for (const QString& path : std::as_const(removed)) {
        Q_EMIT entryRemoved(path);
    }
    return list();
}

bool LzxZipPlugin::copyFiles(const QList<Archive::Entry*>& files, Archive::Entry* destination,
    const Kerfuffle::CompressionOptions& options)
{
    if (m_libzip != nullptr) {
        return forward([&](ReadWriteArchiveInterface* libzip) {
            return libzip->copyFiles(files, destination, options);
        });
    }
    if (!m_archive) {
        Q_EMIT error(m_openError);
        return false;
    }

    const QList<std::pair<QString, QString>> pairs = destinationPairs(files, destination, false);
    const bool ok = rewrite([&](lzx::ZipWriter& writer) {
        for (const lzx::ZipEntry& entry : m_archive->entries()) {
            writer.copy(*m_archive, entry);
        }
        for (const lzx::ZipEntry& entry : m_archive->entries()) {
            const QString path = entryPath(entry);
            for (const auto& [from, to] : pairs) {
                if (path == from) {
                    writer.copy(*m_archive, entry, to.toStdString());
                    break;
                }
                if (from.endsWith(QLatin1Char('/')) && path.startsWith(from)) {
                    writer.copy(*m_archive, entry, (to + path.mid(from.size())).toStdString());
                    break;
                }
            }
        }
    });
    return ok && list();
}

bool LzxZipPlugin::deleteFiles(const QList<Archive::Entry*>& files)
{
    if (m_libzip != nullptr) {
        return forward([&](ReadWriteArchiveInterface* libzip) {
            return libzip->deleteFiles(files);
        });
    }
    if (!m_archive) {
        Q_EMIT error(m_openError);
        return false;
    }

    QSet<QString> exact;
    QStringList folders;
    for (const Archive::Entry* e : files) {
        const QString path = e->fullPath();
        if (path.endsWith(QLatin1Char('/'))) {
            folders << path;
        } else {
            exact.insert(path);
        }
    }
    QStringList removed;
    const bool ok = rewrite([&](lzx::ZipWriter& writer) {
        for (const lzx::ZipEntry& entry : m_archive->entries()) {
            const QString path = entryPath(entry);
            const bool inFolder = std::any_of(folders.cbegin(), folders.cend(), [&](const QString& folder) {
                return path.startsWith(folder);
            });
            if (exact.contains(path) || inFolder) {
                removed << path;
            } else {
                writer.copy(*m_archive, entry);
            }
        }
    });
    if (!ok) {
        return false;
    }
    // Folders are only implied by paths, so Ark is told about them too.
    for (const QString& folder : std::as_const(folders)) {
        if (!removed.contains(folder)) {
            removed << folder;
        }
    }
    for (qsizetype i = 0; i < removed.size(); ++i) {
        Q_EMIT entryRemoved(removed.at(i));
        Q_EMIT progress(static_cast<double>(i + 1) / static_cast<double>(removed.size()));
    }
    return true;
}

bool LzxZipPlugin::addComment(const QString& comment)
{
    if (m_libzip != nullptr) {
        return forward([&](ReadWriteArchiveInterface* libzip) {
            return libzip->addComment(comment);
        });
    }
    if (!m_archive) {
        Q_EMIT error(m_openError);
        return false;
    }
    const bool ok = rewrite([&](lzx::ZipWriter& writer) {
        writer.setComment(comment.toStdString());
        for (const lzx::ZipEntry& entry : m_archive->entries()) {
            writer.copy(*m_archive, entry);
        }
    });
    if (ok) {
        m_comment = comment;
    }
    return ok;
}

bool LzxZipPlugin::isReadOnly() const
{
    if (m_libzip != nullptr) {
        return m_libzip->isReadOnly();
    }
    // A new LZX archive has no file yet; the base class checks the folder.
    if (!m_archive && QFileInfo::exists(filename())) {
        return true;
    }
    return ReadWriteArchiveInterface::isReadOnly();
}

bool LzxZipPlugin::open()
{
    return m_libzip != nullptr ? m_libzip->open() : ReadWriteArchiveInterface::open();
}

bool LzxZipPlugin::doKill()
{
    return m_libzip != nullptr ? m_libzip->doKill() : ReadWriteArchiveInterface::doKill();
}

int LzxZipPlugin::moveRequiredSignals() const
{
    return m_libzip != nullptr ? m_libzip->moveRequiredSignals() : ReadWriteArchiveInterface::moveRequiredSignals();
}

int LzxZipPlugin::copyRequiredSignals() const
{
    return m_libzip != nullptr ? m_libzip->copyRequiredSignals() : ReadWriteArchiveInterface::copyRequiredSignals();
}

bool LzxZipPlugin::hasBatchExtractionProgress() const
{
    return m_libzip != nullptr ? m_libzip->hasBatchExtractionProgress()
                               : ReadWriteArchiveInterface::hasBatchExtractionProgress();
}

bool LzxZipPlugin::isLocked() const
{
    return m_libzip != nullptr ? m_libzip->isLocked() : ReadWriteArchiveInterface::isLocked();
}

QString LzxZipPlugin::multiVolumeName() const
{
    return m_libzip != nullptr ? m_libzip->multiVolumeName() : ReadWriteArchiveInterface::multiVolumeName();
}

#include "lzxzipplugin.moc"
