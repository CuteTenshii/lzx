#include "zip_builder.h"

#include <lzx/zip.h>

#include "archiveentry.h"
#include "archiveinterface.h"

#include <KPluginFactory>
#include <KPluginMetaData>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <string>
#include <vector>

using Kerfuffle::Archive;
using Kerfuffle::ReadWriteArchiveInterface;

namespace {

std::vector<std::uint8_t> repeated(const std::string& text, int times)
{
    std::string result;
    for (int i = 0; i < times; ++i) {
        result += text;
    }
    return zipbuilder::bytes(result);
}

QByteArray toBytes(const std::vector<std::uint8_t>& data)
{
    return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size()));
}

void writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), data.size());
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// Loads the built plugin for `archivePath`, as Ark's Archive::create does.
std::unique_ptr<ReadWriteArchiveInterface> loadPlugin(const QString& archivePath)
{
    const KPluginMetaData metaData(QStringLiteral(LZX_TEST_PLUGIN_FILE));
    const QVariantList args{archivePath, QVariant::fromValue(metaData)};
    return std::unique_ptr<ReadWriteArchiveInterface>(
        KPluginFactory::instantiatePlugin<ReadWriteArchiveInterface>(metaData, nullptr, args).plugin);
}

/// Records everything a plugin reports while one call runs.
struct Recorder {
    QList<Archive::Entry*> entries;
    QStringList errors;
    QStringList removed;
    bool testPassed = false;

    explicit Recorder(ReadWriteArchiveInterface* plugin)
    {
        QObject::connect(plugin, &ReadWriteArchiveInterface::entry, [this](Archive::Entry* e) {
            entries << e;
        });
        QObject::connect(plugin, &ReadWriteArchiveInterface::error, [this](const QString& message) {
            errors << message;
        });
        QObject::connect(plugin, &ReadWriteArchiveInterface::entryRemoved, [this](const QString& path) {
            removed << path;
        });
        QObject::connect(plugin, &ReadWriteArchiveInterface::testSuccess, [this] {
            testPassed = true;
        });
    }

    Archive::Entry* find(const QString& path) const
    {
        for (Archive::Entry* e : entries) {
            if (e->fullPath() == path) {
                return e;
            }
        }
        return nullptr;
    }
};

} // namespace

class TestArkPlugin : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void lzxArchivesAreReadByLzx()
    {
        QTemporaryDir dir;
        const auto texture = repeated("texel data ", 5000);
        const QString path = dir.filePath(QStringLiteral("Horizon.zip"));
        writeFile(path,
            toBytes(zipbuilder::build({
                {"Debug\\Gradient01.xds", texture, 21, true},
                {"readme.txt", zipbuilder::bytes("hello"), 0, true},
                {"Empty/", {}, 0, true},
            })));

        const auto plugin = loadPlugin(path);
        QVERIFY(plugin);
        QVERIFY(!plugin->isReadOnly());
        Recorder recorder(plugin.get());

        QVERIFY(plugin->list());
        QCOMPARE(recorder.entries.size(), 3);
        QCOMPARE(plugin->numberOfEntries(), 3u);
        const Archive::Entry* xds = recorder.find(QStringLiteral("Debug/Gradient01.xds"));
        QVERIFY(xds);
        QCOMPARE(xds->property("size").toULongLong(), static_cast<qulonglong>(texture.size()));
        QCOMPARE(xds->property("method").toString(), QStringLiteral("LZX"));
        QCOMPARE(xds->property("timestamp").toDateTime(), QDateTime(QDate(2011, 8, 26), QTime(12, 34, 56)));
        QVERIFY(recorder.find(QStringLiteral("Empty/"))->isDir());

        const QString out = dir.filePath(QStringLiteral("out"));
        Kerfuffle::ExtractionOptions options;
        options.setPreservePaths(true);
        QVERIFY(plugin->extractFiles({}, out, options));
        QCOMPARE(readFile(out + QStringLiteral("/Debug/Gradient01.xds")), toBytes(texture));
        QCOMPARE(readFile(out + QStringLiteral("/readme.txt")), QByteArray("hello"));
        QVERIFY(QFileInfo(out + QStringLiteral("/Empty")).isDir());
        QCOMPARE(QFileInfo(out + QStringLiteral("/readme.txt")).lastModified(),
            QDateTime(QDate(2011, 8, 26), QTime(12, 34, 56)));

        const QString flat = dir.filePath(QStringLiteral("flat"));
        options.setPreservePaths(false);
        QVERIFY(plugin->extractFiles({recorder.find(QStringLiteral("Debug/Gradient01.xds"))}, flat, options));
        QCOMPARE(readFile(flat + QStringLiteral("/Gradient01.xds")), toBytes(texture));

        QVERIFY(plugin->testArchive());
        QVERIFY(recorder.testPassed);
        QVERIFY(recorder.errors.isEmpty());
    }

    void lzxArchivesCanBeEdited()
    {
        QTemporaryDir dir;
        const auto texture = repeated("texel data ", 5000);
        const QString path = dir.filePath(QStringLiteral("Horizon.zip"));
        writeFile(path,
            toBytes(zipbuilder::build({
                {"Debug\\Gradient01.xds", texture, 21, true},
                {"readme.txt", zipbuilder::bytes("hello"), 0, true},
            })));
        const auto plugin = loadPlugin(path);
        Recorder recorder(plugin.get());
        QVERIFY(plugin->list());

        // Add a folder and replace an existing file, from Ark's working
        // directory, into the archive's root.
        const QString work = dir.filePath(QStringLiteral("work"));
        QVERIFY(QDir().mkpath(work + QStringLiteral("/Mods")));
        writeFile(work + QStringLiteral("/Mods/new.xds"), toBytes(repeated("new texels ", 3000)));
        writeFile(work + QStringLiteral("/readme.txt"), "changed");
        const QString previous = QDir::currentPath();
        QDir::setCurrent(work);
        Archive::Entry folder(nullptr, QStringLiteral("Mods/"));
        Archive::Entry readme(nullptr, QStringLiteral("readme.txt"));
        Kerfuffle::CompressionOptions compression;
        compression.setGlobalWorkDir(work);
        const bool added = plugin->addFiles({&folder, &readme}, nullptr, compression);
        QDir::setCurrent(previous);
        QVERIFY2(added, qPrintable(recorder.errors.join(QLatin1Char('\n'))));

        {
            const lzx::ZipArchive archive = lzx::ZipArchive::open(path.toStdString());
            QCOMPARE(archive.entries().size(), std::size_t{3});
            QCOMPARE(toBytes(archive.read(*archive.find("readme.txt"))), QByteArray("changed"));
            const lzx::ZipEntry* newTexture = archive.find("Mods/new.xds");
            QVERIFY(newTexture);
            QCOMPARE(newTexture->method, lzx::kMethodXMemCompress);
            QVERIFY(newTexture->offsetField.has_value());
            // The untouched entry keeps its original spelling and data.
            QCOMPARE(QString::fromStdString(archive.entries().front().name), QStringLiteral("Debug\\Gradient01.xds"));
            QCOMPARE(toBytes(archive.read(archive.entries().front())), toBytes(texture));
        }

        // Rename a file, then copy and delete a folder.
        recorder.entries.clear();
        QVERIFY(plugin->list());
        Archive::Entry renamed(nullptr, QStringLiteral("notes.txt"));
        QVERIFY(plugin->moveFiles({recorder.find(QStringLiteral("readme.txt"))}, &renamed, compression));
        QVERIFY(recorder.removed.contains(QStringLiteral("readme.txt")));

        recorder.entries.clear();
        QVERIFY(plugin->list());
        Archive::Entry backup(nullptr, QStringLiteral("Backup/"));
        QVERIFY(plugin->copyFiles({recorder.find(QStringLiteral("notes.txt"))}, &backup, compression));

        recorder.removed.clear();
        Archive::Entry debugFolder(nullptr, QStringLiteral("Debug/"));
        QVERIFY(plugin->deleteFiles({&debugFolder}));
        QVERIFY(recorder.removed.contains(QStringLiteral("Debug/Gradient01.xds")));

        const lzx::ZipArchive archive = lzx::ZipArchive::open(path.toStdString());
        QVERIFY(archive.find("readme.txt") == nullptr);
        QVERIFY(archive.find("Debug/Gradient01.xds") == nullptr);
        QCOMPARE(toBytes(archive.read(*archive.find("notes.txt"))), QByteArray("changed"));
        QCOMPARE(toBytes(archive.read(*archive.find("Backup/notes.txt"))), QByteArray("changed"));
        QVERIFY(archive.find("Mods/new.xds") != nullptr);
        QVERIFY(recorder.errors.isEmpty());
    }

    void newArchivesCanUseLzx()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("new.zip"));
        const auto plugin = loadPlugin(path);
        Recorder recorder(plugin.get());
        writeFile(dir.filePath(QStringLiteral("file.bin")), toBytes(repeated("game data ", 4000)));
        const QString previous = QDir::currentPath();
        QDir::setCurrent(dir.path());
        Archive::Entry file(nullptr, QStringLiteral("file.bin"));
        Kerfuffle::CompressionOptions compression;
        compression.setGlobalWorkDir(dir.path());
        compression.setCompressionMethod(QStringLiteral("LZX"));
        const bool ok = plugin->addFiles({&file}, nullptr, compression);
        QDir::setCurrent(previous);
        QVERIFY2(ok, qPrintable(recorder.errors.join(QLatin1Char('\n'))));
        QCOMPARE(recorder.entries.size(), 1);

        const lzx::ZipArchive archive = lzx::ZipArchive::open(path.toStdString());
        QVERIFY(archive.needsLzxReader());
        QCOMPARE(toBytes(archive.read(*archive.find("file.bin"))), toBytes(repeated("game data ", 4000)));
    }

    void lzxIsRefusedForPlainZips()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("plain.zip"));
        writeFile(path, toBytes(zipbuilder::build({{"a.txt", zipbuilder::bytes("alpha"), 0, false}})));
        const auto plugin = loadPlugin(path);
        Recorder recorder(plugin.get());
        writeFile(dir.filePath(QStringLiteral("b.txt")), "bravo");
        Archive::Entry file(nullptr, QStringLiteral("b.txt"));
        Kerfuffle::CompressionOptions compression;
        compression.setCompressionMethod(QStringLiteral("LZX"));
        QVERIFY(!plugin->addFiles({&file}, nullptr, compression));
        QVERIFY(recorder.errors.join(QString()).contains(QLatin1String("LZX")));
    }

    void corruptLzxArchivesFailTheTest()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("bad.zip"));
        writeFile(path, toBytes(zipbuilder::build({{"bad.bin", repeated("x", 100), 21, true, true}})));
        const auto plugin = loadPlugin(path);
        Recorder recorder(plugin.get());
        QVERIFY(!plugin->testArchive());
        QVERIFY(!recorder.testPassed);
        QVERIFY(recorder.errors.join(QString()).contains(QLatin1String("CRC")));
    }

    void plainZipsGoToLibzip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("plain.zip"));
        writeFile(path,
            toBytes(zipbuilder::build({
                {"a.txt", zipbuilder::bytes("alpha"), 0, false},
                {"b.txt", repeated("bravo ", 200), 8, false},
            })));

        const auto plugin = loadPlugin(path);
        QVERIFY(plugin);
        QVERIFY(!plugin->isReadOnly());
        Recorder recorder(plugin.get());
        QVERIFY(plugin->list());
        QCOMPARE(recorder.entries.size(), 2);
        // The listing came from libzip.
        QCOMPARE(recorder.find(QStringLiteral("b.txt"))->property("method").toString(), QStringLiteral("Deflate"));

        const QString out = dir.filePath(QStringLiteral("out"));
        Kerfuffle::ExtractionOptions options;
        QVERIFY(plugin->extractFiles({}, out, options));
        QCOMPARE(readFile(out + QStringLiteral("/b.txt")), toBytes(repeated("bravo ", 200)));

        const QString work = dir.filePath(QStringLiteral("work"));
        QVERIFY(QDir().mkpath(work));
        writeFile(work + QStringLiteral("/added.txt"), "added content");
        const QString previous = QDir::currentPath();
        QDir::setCurrent(work);
        Archive::Entry added(nullptr, QStringLiteral("added.txt"));
        Kerfuffle::CompressionOptions compression;
        compression.setGlobalWorkDir(work);
        const bool addedOk = plugin->addFiles({&added}, nullptr, compression);
        QDir::setCurrent(previous);
        QVERIFY2(addedOk, qPrintable(recorder.errors.join(QLatin1Char('\n'))));

        QVERIFY(plugin->deleteFiles({recorder.find(QStringLiteral("a.txt"))}));
        QCOMPARE(recorder.removed, QStringList{QStringLiteral("a.txt")});

        const lzx::ZipArchive result = lzx::ZipArchive::open(path.toStdString());
        QVERIFY(result.find("a.txt") == nullptr);
        QCOMPARE(toBytes(result.read(*result.find("added.txt"))), QByteArray("added content"));
    }

    void newArchivesAreCreatedByLibzip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("new.zip"));
        const auto plugin = loadPlugin(path);
        QVERIFY(plugin);
        QVERIFY(!plugin->isReadOnly());

        writeFile(dir.filePath(QStringLiteral("file.txt")), "new file");
        const QString previous = QDir::currentPath();
        QDir::setCurrent(dir.path());
        Archive::Entry file(nullptr, QStringLiteral("file.txt"));
        Kerfuffle::CompressionOptions compression;
        compression.setGlobalWorkDir(dir.path());
        const bool ok = plugin->addFiles({&file}, nullptr, compression);
        QDir::setCurrent(previous);
        QVERIFY(ok);

        const lzx::ZipArchive result = lzx::ZipArchive::open(path.toStdString());
        QCOMPARE(toBytes(result.read(*result.find("file.txt"))), QByteArray("new file"));
    }
};

int main(int argc, char** argv)
{
    // Entries load icons, which needs a GUI application; no display is used.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestArkPlugin test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_arkplugin.moc"
