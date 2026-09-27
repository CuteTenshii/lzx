#pragma once

#include <lzx/zip.h>
#include <lzx/zip_writer.h>

#include "archiveinterface.h"

#include <QHash>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

/// Ark backend for zip archives.
///
/// Archives libzip cannot read (entries compressed with XMemCompress LZX, or
/// missing their local headers) are read and written with lzx: changes
/// rewrite the archive in the layout of Xbox 360 game archives, copying
/// untouched entries without recompressing them. New archives created with
/// the "LZX" compression method are written the same way. Every other zip is
/// handed to Ark's own libzip plugin, so normal archives behave as before.
class LzxZipPlugin : public Kerfuffle::ReadWriteArchiveInterface {
    Q_OBJECT

public:
    explicit LzxZipPlugin(QObject* parent, const QVariantList& args);
    ~LzxZipPlugin() override;

    bool list() override;
    bool testArchive() override;
    bool extractFiles(const QList<Kerfuffle::Archive::Entry*>& files, const QString& destinationDirectory,
        const Kerfuffle::ExtractionOptions& options) override;

    bool addFiles(const QList<Kerfuffle::Archive::Entry*>& files, const Kerfuffle::Archive::Entry* destination,
        const Kerfuffle::CompressionOptions& options, uint numberOfEntriesToAdd = 0) override;
    bool moveFiles(const QList<Kerfuffle::Archive::Entry*>& files, Kerfuffle::Archive::Entry* destination,
        const Kerfuffle::CompressionOptions& options) override;
    bool copyFiles(const QList<Kerfuffle::Archive::Entry*>& files, Kerfuffle::Archive::Entry* destination,
        const Kerfuffle::CompressionOptions& options) override;
    bool deleteFiles(const QList<Kerfuffle::Archive::Entry*>& files) override;
    bool addComment(const QString& comment) override;

    bool isReadOnly() const override;
    bool open() override;
    bool doKill() override;
    int moveRequiredSignals() const override;
    int copyRequiredSignals() const override;
    bool hasBatchExtractionProgress() const override;
    bool isLocked() const override;
    QString multiVolumeName() const override;

private:
    struct NewFile {
        QString name;
        QString path;
    };

    Kerfuffle::ReadWriteArchiveInterface* loadLibzip(const QVariantList& args);
    void forwardSignals();
    /// Stops forwarding to libzip once lzx owns the archive.
    void releaseLibzip();
    /// Copies the state Ark sets on this interface to the libzip plugin
    /// before a call, and the state the call changed back afterwards.
    void pushState();
    void pullState();
    template<typename Call>
    bool forward(Call call);

    /// Opens the archive with lzx and indexes its entries by path.
    bool openArchive();
    /// Writes a new version of the archive with `build`, replaces the file
    /// and reopens it. Emits an error and returns false on failure.
    bool rewrite(const std::function<void(lzx::ZipWriter&)>& build);
    QList<NewFile> filesToAdd(const QList<Kerfuffle::Archive::Entry*>& files,
        const Kerfuffle::Archive::Entry* destination) const;
    void writeFile(lzx::ZipWriter& writer, const NewFile& file) const;
    /// Pairs each archive path with its new path for a move or copy; paths
    /// ending in '/' stand for everything below them.
    QList<std::pair<QString, QString>> destinationPairs(const QList<Kerfuffle::Archive::Entry*>& files,
        const Kerfuffle::Archive::Entry* destination, bool move) const;

    bool extractEntry(const lzx::ZipEntry& entry, const QString& fullPath, const QString& rootNode,
        const QString& destinationDirectory, bool preservePaths, bool removeRootNode);

    Kerfuffle::ReadWriteArchiveInterface* m_libzip = nullptr;
    std::optional<lzx::ZipArchive> m_archive;
    QString m_openError;
    QHash<QString, qsizetype> m_indexByPath;
    QList<Kerfuffle::Archive::Entry*> m_emittedEntries;
    bool m_overwriteAll = false;
    bool m_skipAll = false;
};
