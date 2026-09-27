// Reads every entry of every .zip under $LZX_SAMPLE_DIR and checks it
// against its CRC-32. Without the variable set, the test reports that it
// was skipped.

#include <lzx/zip.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

/// Returned by ctest's SKIP_RETURN_CODE when no sample directory is given.
constexpr int kSkipped = 77;

} // namespace

int main()
{
    const char* dir = std::getenv("LZX_SAMPLE_DIR");
    if (dir == nullptr || *dir == '\0') {
        std::cout << "LZX_SAMPLE_DIR is not set; skipping the real-data check\n";
        return kSkipped;
    }

    std::size_t archives = 0;
    std::size_t entries = 0;
    std::size_t failures = 0;
    for (const auto& file : std::filesystem::recursive_directory_iterator(dir)) {
        std::string extension = file.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (!file.is_regular_file() || extension != ".zip") {
            continue;
        }
        ++archives;
        try {
            const lzx::ZipArchive archive = lzx::ZipArchive::open(file.path());
            for (const lzx::ZipEntry& entry : archive.entries()) {
                if (entry.isDirectory()) {
                    continue;
                }
                ++entries;
                try {
                    (void)archive.read(entry);
                } catch (const lzx::Error& e) {
                    ++failures;
                    std::cout << "FAIL " << file.path().string() << ": " << e.what() << '\n';
                }
            }
        } catch (const lzx::Error& e) {
            ++failures;
            std::cout << "FAIL " << e.what() << '\n';
        }
    }
    std::cout << archives << " archives, " << entries << " entries, " << failures << " failures\n";
    if (archives == 0) {
        std::cout << "no .zip files found under " << dir << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
