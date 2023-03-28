# ManifestDownloader
Commandline tool to download manifest files, parse them and download their content.
Run `ManifestDownloader --help` to see all available configuration options.

This tool should be able to download all files for League of Legends, Legends of Runeterra, Valorant and all games to come that use these manifest files.
If you're wondering where the manifest files come from, [here](https://github.com/Morilli/riot-manifests) is a list of saved manifests from various sources and python scripts that should (more or less) explain where to find them.

To download an optimized compiled executable, head on over to https://github.com/Morilli/ManifestDownloader/releases and download the most recent release.

## Building
To build it yourself, you'll need general C build tools (make, gcc) and additionally cmake to build pcre2 (as I can't be bothered to automate their configure build system). Note that when compiling with clang it may be required to export `AR=llvm-ar` to ensure archive files are read/written correctly.
Linux systems and msys/mingw should be able to compile perfectly, everything else I don't really know tbh.
Just run "make" in the project root and the executable should be built automatically.

By default, all dependency libraries (bearssl, zstd and pcre2) will only be built once and not be removed or remade on `make clean` or `make -B`.
To clean them as well, run `make clean-all` (will run `make clean` implicitly).
