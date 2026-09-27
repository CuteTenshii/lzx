#include <lzx/zip.h>
#include <lzx/zip_writer.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

int usage()
{
    std::cerr << "usage:\n"
                 "  lzxzip list <archive>\n"
                 "  lzxzip extract <archive> <directory> [entry...]\n"
                 "  lzxzip test <archive>\n"
                 "  lzxzip create <archive> <directory>\n"
                 "  lzxzip update <archive> <directory>\n"
                 "  lzxzip delete <archive> <entry>...\n"
                 "\n"
                 "Reads zip archives whose entries are stored, deflated or compressed with\n"
                 "Xbox 360 XMemCompress LZX (method 21), and writes them in the layout of the\n"
                 "game archives that use it.\n"
                 "\n"
                 "create packs every file under <directory>, named by its path relative to it.\n"
                 "update does the same into an existing archive: files replace the entries\n"
                 "with the same name, new files are added, and all other entries are kept\n"
                 "without recompressing them. Entry names are matched ignoring case and\n"
                 "slash direction.\n";
    return 2;
}

const char* methodName(std::uint16_t method)
{
    switch (method) {
    case lzx::kMethodStored:
        return "store";
    case lzx::kMethodDeflate:
        return "deflate";
    case lzx::kMethodXMemCompress:
        return "lzx";
    default:
        return "?";
    }
}

/// Where `entry` goes under `directory`, refusing names that would escape it.
std::filesystem::path destinationFor(const std::filesystem::path& directory, const lzx::ZipEntry& entry)
{
    const std::filesystem::path relative = std::filesystem::path(entry.path()).relative_path().lexically_normal();
    if (relative.empty() || *relative.begin() == "..") {
        throw lzx::Error(entry.name + " would be written outside " + directory.string());
    }
    return directory / relative;
}

void extractEntry(const lzx::ZipArchive& archive, const lzx::ZipEntry& entry, const std::filesystem::path& directory)
{
    const std::filesystem::path destination = destinationFor(directory, entry);
    if (entry.isDirectory()) {
        std::filesystem::create_directories(destination);
        return;
    }
    std::filesystem::create_directories(destination.parent_path());
    const std::vector<std::uint8_t> data = archive.read(entry);
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) {
        throw lzx::Error("cannot write " + destination.string());
    }
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad() || !std::filesystem::exists(path)) {
        throw lzx::Error("cannot read " + path.string());
    }
    return data;
}

/// Every regular file under `directory`, keyed by its entry name: the path
/// relative to `directory` with forward slashes. Sorted, so archives come
/// out the same every time.
std::map<std::string, std::filesystem::path> filesUnder(const std::filesystem::path& directory)
{
    if (!std::filesystem::is_directory(directory)) {
        throw lzx::Error(directory.string() + " is not a directory");
    }
    std::map<std::string, std::filesystem::path> files;
    for (const auto& item : std::filesystem::recursive_directory_iterator(directory)) {
        if (item.is_regular_file()) {
            files.emplace(item.path().lexically_relative(directory).generic_string(), item.path());
        }
    }
    return files;
}

void addFile(lzx::ZipWriter& writer, const std::string& name, const std::filesystem::path& file)
{
    writer.add(name, readFile(file), lzx::DosDateTime::fromFileTime(std::filesystem::last_write_time(file)));
}

int create(const std::filesystem::path& archivePath, const std::filesystem::path& directory)
{
    if (std::filesystem::exists(archivePath)) {
        throw lzx::Error(archivePath.string() + " already exists; use update to change it");
    }
    const auto files = filesUnder(directory);
    lzx::ZipWriter writer(archivePath);
    for (const auto& [name, file] : files) {
        addFile(writer, name, file);
    }
    writer.finish();
    std::cout << files.size() << " files added\n";
    return 0;
}

int update(const std::filesystem::path& archivePath, const std::filesystem::path& directory)
{
    const auto files = filesUnder(directory);
    std::map<std::string, const std::pair<const std::string, std::filesystem::path>*> byKey;
    for (const auto& file : files) {
        byKey.emplace(lzx::ZipArchive::normalizeName(file.first), &file);
    }

    const lzx::ZipArchive archive = lzx::ZipArchive::open(archivePath);
    lzx::ZipWriter writer(archivePath);
    writer.setComment(archive.comment());
    std::set<std::string> used;
    std::size_t replaced = 0;
    for (const lzx::ZipEntry& entry : archive.entries()) {
        const auto match = byKey.find(lzx::ZipArchive::normalizeName(entry.name));
        if (match == byKey.end()) {
            writer.copy(archive, entry);
            continue;
        }
        // Keep the archive's own spelling of the name, which the game may
        // rely on.
        addFile(writer, entry.name, match->second->second);
        used.insert(match->first);
        ++replaced;
    }
    std::size_t added = 0;
    for (const auto& [key, file] : byKey) {
        if (!used.contains(key)) {
            addFile(writer, file->first, file->second);
            ++added;
        }
    }
    writer.finish();
    std::cout << replaced << " replaced, " << added << " added, " << archive.entries().size() - replaced
              << " kept\n";
    return 0;
}

int remove(const std::filesystem::path& archivePath, const std::vector<std::string>& names)
{
    const lzx::ZipArchive archive = lzx::ZipArchive::open(archivePath);
    std::set<const lzx::ZipEntry*> doomed;
    for (const std::string& name : names) {
        const lzx::ZipEntry* entry = archive.find(name);
        if (entry == nullptr) {
            throw lzx::Error(name + " is not in " + archivePath.string());
        }
        doomed.insert(entry);
    }
    lzx::ZipWriter writer(archivePath);
    writer.setComment(archive.comment());
    for (const lzx::ZipEntry& entry : archive.entries()) {
        if (!doomed.contains(&entry)) {
            writer.copy(archive, entry);
        }
    }
    writer.finish();
    std::cout << doomed.size() << " removed\n";
    return 0;
}

int run(const std::vector<std::string>& args)
{
    if (args.size() < 2) {
        return usage();
    }
    const std::string& command = args[0];
    const std::filesystem::path archivePath = args[1];

    if (command == "create" && args.size() == 3) {
        return create(archivePath, args[2]);
    }
    if (command == "update" && args.size() == 3) {
        return update(archivePath, args[2]);
    }
    if (command == "delete" && args.size() >= 3) {
        return remove(archivePath, std::vector<std::string>(args.begin() + 2, args.end()));
    }

    const lzx::ZipArchive archive = lzx::ZipArchive::open(archivePath);

    if (command == "list" && args.size() == 2) {
        for (const lzx::ZipEntry& entry : archive.entries()) {
            std::cout << entry.uncompressedSize << '\t' << entry.compressedSize << '\t' << methodName(entry.method)
                      << '\t' << entry.path() << '\n';
        }
        return 0;
    }

    if (command == "extract" && args.size() >= 3) {
        const std::filesystem::path directory = args[2];
        if (args.size() == 3) {
            for (const lzx::ZipEntry& entry : archive.entries()) {
                extractEntry(archive, entry, directory);
            }
            return 0;
        }
        for (std::size_t i = 3; i < args.size(); ++i) {
            const lzx::ZipEntry* entry = archive.find(args[i]);
            if (entry == nullptr) {
                throw lzx::Error(args[i] + " is not in " + archive.path().string());
            }
            extractEntry(archive, *entry, directory);
        }
        return 0;
    }

    if (command == "test" && args.size() == 2) {
        std::size_t failures = 0;
        for (const lzx::ZipEntry& entry : archive.entries()) {
            if (entry.isDirectory()) {
                continue;
            }
            try {
                (void)archive.read(entry);
            } catch (const lzx::Error& e) {
                std::cerr << "FAIL " << e.what() << '\n';
                ++failures;
            }
        }
        std::cout << archive.entries().size() << " entries, " << failures << " failed\n";
        return failures == 0 ? 0 : 1;
    }

    return usage();
}

} // namespace

int main(int argc, char** argv)
{
    try {
        return run(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::exception& e) {
        std::cerr << "lzxzip: " << e.what() << '\n';
        return 1;
    }
}
