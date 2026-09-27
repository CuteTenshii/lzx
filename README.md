# lzx

A C++20 library for the LZX variant of the Xbox 360 XMemCompress API, and for the zip archives that store it as compression method 21, as Forza Horizon does. It compresses and decompresses, reads and writes those archives, and comes with a command-line tool and an Ark backend.

Stock Ark cannot open these archives: libzip refuses method 21, and libarchive writes each entry as zeros after an error. The archives also have quirks other readers trip over, all from the packing tool behind them:
- every entry records its data offset in extra field `0x1123`, and some archives leave out local file headers entirely;
- the largest (`tracks/colorado/bin.zip`) has over 230,000 entries without using zip64, so the entry count in the end record wraps around.

The library depends only on the standard library and zlib.

## How it was checked

Against the Forza Horizon disc:

- **Reading:** every entry of all 525 zip archives (488,993 entries, counting the two copies of the Colorado track) reads back and matches its CRC-32.
- **Compression:** every LZX entry outside the Colorado archives is re-encoded and decoded again by this library and by libmspack's independent LZX decoder, with identical results. Re-encoded archives come out within about 0.2% of the size of the originals, at about 4.5 MB/s.
- **Writing:** an archive extracted and repacked with `lzxzip` extracts back byte-identical.

What has not been checked is the game itself loading a repacked archive. The encoder only writes stream features found on the disc (one verbatim, aligned or stored block per 32 KiB chunk, complete Huffman codes, the same chunk framing as XMemCompress) and the writer copies the game archives' header values. The real test is running the game, for example in Xenia, with a repacked archive.

## Build

Requires CMake 3.21 or newer, a C++20 compiler and zlib.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

Options: `LZX_BUILD_TOOLS` (default `ON`), `LZX_BUILD_TESTS` (default `ON`), `LZX_BUILD_ARK_PLUGIN` (default `OFF`).

To also check real archives, point `LZX_SAMPLE_DIR` at a directory of `.zip` files (searched recursively). Without it the test is reported as skipped.

```sh
LZX_SAMPLE_DIR=/path/to/disc/media ctest --test-dir build -R realdata --output-on-failure
```

## Command-line tool

```sh
lzxzip list Horizon.zip                              # size, packed size, method and name of each entry
lzxzip extract Horizon.zip out/                      # everything
lzxzip extract Horizon.zip out/ debug/gradient01.xds # selected entries; case and slashes do not matter
lzxzip test Horizon.zip                              # check every entry's CRC

lzxzip create New.zip out/                           # pack a folder, names relative to it
lzxzip update Horizon.zip changes/                   # replace and add files, keep the rest as they are
lzxzip delete Horizon.zip debug/gradient01.xds
```

`update` is the one for modding: extract an archive, change some files, and put just those files into a folder with the same layout. Each one replaces the entry with the same name (keeping the archive's own spelling of it), new files are added at the end, and every other entry is copied without being recompressed.

New entries are LZX-compressed, or stored when compression does not make them smaller, as some game entries are. Archives are written to a temporary file that replaces the original only once complete.

## Library

```cpp
#include <lzx/zip.h>
#include <lzx/zip_writer.h>

const lzx::ZipArchive archive = lzx::ZipArchive::open("Horizon.zip");
const lzx::ZipEntry* entry = archive.find("debug/gradient01.xds");  // game-style lookup
std::vector<std::uint8_t> data = archive.read(*entry);              // CRC-checked

lzx::ZipWriter writer("Modded.zip");
writer.add("Debug/Gradient01.xds", data);                           // LZX, or stored if smaller
writer.copy(archive, archive.entries().front());                    // no recompression
writer.finish();
```

`lzx::compress()` and `lzx::decompress()` in `<lzx/lzx.h>` work on raw XMemCompress streams. Errors are thrown as `lzx::Error`.

With CMake, either add this directory with `add_subdirectory` and link `lzx::lzx`, or install it and use `find_package(lzx)`.

## Ark

The backend claims zip archives ahead of Ark's libzip plugin and decides per archive:

- **Archives libzip cannot read** (an entry uses LZX, or has no local header) are handled by lzx. Listing, extraction, preview, integrity tests, and adding, replacing, renaming, copying and deleting files all work. Every change rewrites the archive in the game layout, and only the files being added get compressed.
- **Every other zip** is handed to Ark's own libzip plugin, so it behaves exactly as before.
- **New archives** can be made as LZX archives by choosing the "LZX (Xbox 360)" compression method when creating them. Choosing it when adding to an ordinary zip is refused rather than silently falling back to another method.

It needs Qt 6, KF6 CoreAddons and FileMetaData, libzip's pkg-config file, and Ark's `kerfuffle` headers. Ark does not install those headers, so the build finds the installed `libkerfuffle`, reads its version, and downloads the matching Ark tag from invent.kde.org. To build offline, pass an Ark source tree of the same version with `-DLZX_ARK_SOURCE_DIR=...`.

```sh
cmake -S . -B build -DLZX_BUILD_ARK_PLUGIN=ON -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

This installs `kerfuffle/kerfuffle_lzxzip.so` into Qt's plugin directory. Ark's plugin interface is internal and has no stable ABI, so **rebuild the plugin whenever Ark is updated**. It can be turned off in Ark under Settings → Configure Ark → Plugins, which hands every zip back to libzip.

Messages from the backend are in English only.

## License

MIT. See [LICENSE](LICENSE).
