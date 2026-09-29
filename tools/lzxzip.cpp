#include <lzx/zip.h>
#include <lzx/zip_writer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shellapi.h>
#endif

namespace {

// Arguments, entry names and output are UTF-8 everywhere. On Windows,
// std::filesystem's narrow strings use the ANSI code page instead, so paths
// only cross between the two through these.
std::filesystem::path fromUtf8(std::string_view text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

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

#ifdef _WIN32
/// False for path components Windows does not treat as plain file names: a
/// colon names an alternate data stream, and names such as NUL or COM1.txt
/// open a device.
bool isPlainWindowsName(const std::string& component)
{
    constexpr std::string_view kReservedCharacters = "<>:\"|?*";
    for (const char c : component) {
        if (static_cast<unsigned char>(c) < 0x20 || kReservedCharacters.find(c) != std::string_view::npos) {
            return false;
        }
    }
    std::string stem = component.substr(0, component.find('.'));
    stem.erase(stem.find_last_not_of(' ') + 1);
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    constexpr std::array<std::string_view, 6> kDevices{"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    if (std::find(kDevices.begin(), kDevices.end(), stem) != kDevices.end()) {
        return false;
    }
    const bool numberedPort = stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT"))
        && stem[3] >= '1' && stem[3] <= '9';
    return !numberedPort;
}
#endif

/// Where `entry` goes under `directory`, refusing names that would escape it.
std::filesystem::path destinationFor(const std::filesystem::path& directory, const lzx::ZipEntry& entry)
{
    const std::filesystem::path relative = fromUtf8(entry.path()).relative_path().lexically_normal();
    if (relative.empty() || *relative.begin() == "..") {
        throw lzx::Error(entry.name + " would be written outside " + utf8(directory));
    }
#ifdef _WIN32
    for (const std::filesystem::path& component : relative) {
        if (!isPlainWindowsName(utf8(component))) {
            throw lzx::Error(entry.name + " is not a valid file name on Windows");
        }
    }
#endif
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
        throw lzx::Error("cannot write " + utf8(destination));
    }
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad() || !std::filesystem::exists(path)) {
        throw lzx::Error("cannot read " + utf8(path));
    }
    return data;
}

/// Every regular file under `directory`, keyed by its entry name: the path
/// relative to `directory` with forward slashes. Sorted, so archives come
/// out the same every time.
std::map<std::string, std::filesystem::path> filesUnder(const std::filesystem::path& directory)
{
    if (!std::filesystem::is_directory(directory)) {
        throw lzx::Error(utf8(directory) + " is not a directory");
    }
    std::map<std::string, std::filesystem::path> files;
    for (const auto& item : std::filesystem::recursive_directory_iterator(directory)) {
        if (item.is_regular_file()) {
            const std::u8string name = item.path().lexically_relative(directory).generic_u8string();
            files.emplace(std::string(name.begin(), name.end()), item.path());
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
        throw lzx::Error(utf8(archivePath) + " already exists; use update to change it");
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
            throw lzx::Error(name + " is not in " + utf8(archivePath));
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
    const std::filesystem::path archivePath = fromUtf8(args[1]);

    if (command == "create" && args.size() == 3) {
        return create(archivePath, fromUtf8(args[2]));
    }
    if (command == "update" && args.size() == 3) {
        return update(archivePath, fromUtf8(args[2]));
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
        const std::filesystem::path directory = fromUtf8(args[2]);
        if (args.size() == 3) {
            for (const lzx::ZipEntry& entry : archive.entries()) {
                extractEntry(archive, entry, directory);
            }
            return 0;
        }
        for (std::size_t i = 3; i < args.size(); ++i) {
            const lzx::ZipEntry* entry = archive.find(args[i]);
            if (entry == nullptr) {
                throw lzx::Error(args[i] + " is not in " + utf8(archive.path()));
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

/// The arguments after the program name, in UTF-8.
std::vector<std::string> arguments([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
#ifdef _WIN32
    // argv is in the ANSI code page, which cannot hold every file name.
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        throw lzx::Error("cannot read the command line");
    }
    std::vector<std::string> result;
    for (int i = 1; i < count; ++i) {
        result.push_back(utf8(std::filesystem::path(wide[i])));
    }
    LocalFree(wide);
    return result;
#else
    return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try {
        return run(arguments(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "lzxzip: " << e.what() << '\n';
        return 1;
    }
}
