# Building the CasparCG Server

The CasparCG Server source code uses the CMake build system in order to easily
generate build systems for multiple platforms. CMake is basically a build
system for generating build systems.

On Windows we can use CMake to generate a .sln file and .vcproj files. On
Linux CMake can generate make files or ninja files. On macOS CMake generates
make files via the provided build script. Qt Creator has support for loading
CMakeLists.txt files directly.

# Dependency caching

CMake will automatically download some dependencies as part of the build process.
These are taken from https://github.com/CasparCG/dependencies/releases (make sure to expand the 'Assets' group under each release to see the files), most of which are direct copies of distributions from upstream.

During the build, you can specify the CMake option `CASPARCG_DOWNLOAD_MIRROR` to download from an alternate HTTP server (such as an internally hosted mirror), or `CASPARCG_DOWNLOAD_CACHE` to use a specific path on disk for the local cache of these files, by default a folder called `external` will be created inside the build directory to cache these files.

If you want to be able to build CasparCG offline, you may need to manually seed this cache. You can do so by placing the correct tar.gz or zip into a folder and using `CASPARCG_DOWNLOAD_CACHE` to tell CMake where to find it.
You can figure out which files you need by looking at each of the `ExternalProject_Add` function calls inside of [Bootstrap_Linux.cmake](./src/CMakeModules/Bootstrap_Linux.cmake), [Bootstrap_Windows.cmake](./src/CMakeModules/Bootstrap_Windows.cmake), or [Bootstrap_macOS.cmake](./src/CMakeModules/Bootstrap_macOS.cmake). Some of the ones listed are optional, depending on other CMake flags.

# Windows

## Building distributable

1. Install Visual Studio 2022.

2. Install 7-zip (https://www.7-zip.org/).

3. `git clone --single-branch --branch master https://github.com/CasparCG/server casparcg-server-master`

4. `cd casparcg-server-master`

5. `.\tools\windows\build.bat`

6. Copy the `dist\casparcg_server.zip` file for distribution

## Development using Visual Studio

1. Install Visual Studio 2022.

2. `git clone --single-branch --branch master https://github.com/CasparCG/server casparcg-server-master`

3. Open the cloned folder in Visual Studio.

4. Build All and ensure it builds successfully

# Linux

## Building on your system

We only officially support Ubuntu LTS releases, other distros may work but often run into build issues. We are happy to accept PRs to resolve these issues, but are unlikely to write fixes ourselves.

We currently document two approaches to building CasparCG. The recommended way is to use the `deb` packaging we have in the repository, but we only provide that for Ubuntu LTS releases.
Other deb based distros can work with some tweaks to one of those, other distros will need something else which is not documented here.

We also provide a script to produce a build in docker, but this is not recommended unless absolutely necessary. The resulting builds are often rather brittle depending on where they are used.

To perform a custom build, follow the Development steps below, and you may need to do some extra packaging steps, or install steps on the target systems.

### Building inside Docker

1. `git clone --single-branch --branch master https://github.com/CasparCG/server casparcg-server-master`
2. `cd casparcg-server-master`
3. `./tools/linux/build-in-docker`

If all goes to plan, a docker image `casparcg/server` has been created containing CasparCG Server.

### Extracting CasparCG Server from Docker

1. `./tools/linux/extract-from-docker`

You will then find a folder called `casparcg_server` which should contain everything you need to run CasparCG Server.

_Note: if you ran docker with sudo, CasparCG server will not be able to run without sudo out of the box. For security reasons we do not recommend to run CasparCG with sudo. Instead you can use chown to change the ownership of the CasparCG Server folder._

## Development

Before beginning, check the build options section below, to decide if you want to use any to simplify or customise your build.

1. `git clone --single-branch --branch master https://github.com/CasparCG/server casparcg-server-master`
2. `cd casparcg-server-master`
3. Install dependencies, this can be done with `sudo ./tools/linux/install-dependencies`
4. If using system CEF (default & recommended), `sudo add-apt-repository ppa:casparcg/ppa` and `sudo apt-get install casparcg-cef-142-dev`
5. `mkdir build && cd build`
6. `cmake ../src` You can add any of the build options from below to this command
7. `cmake --build . --parallel`
8. `cmake --install . --prefix staging`

If all goes to plan, a folder called 'staging' has been created with everything you need to run CasparCG server.

## Build options (Linux)

-DENABLE_HTML=OFF - useful if you lack CEF, and would like to build without that module.

-DUSE_STATIC_BOOST=ON - (Linux only, default OFF) statically link against Boost.

-DUSE_SYSTEM_CEF=OFF - (Linux only, default ON) use the version of CEF from your OS. This expects to be using builds from https://launchpad.net/~casparcg/+archive/ubuntu/ppa

-DENABLE_AVX2=ON (Linux only, default OFF) Enable the AVX and AVX2 instruction sets (requires a CPU that supports it)

-DDIAG_FONT_PATH - Specify an alternate path/font to use for the DIAG window. On linux, this will often want to be set to an absolute path of a font

-DCASPARCG_BINARY_NAME=casparcg-server - (Linux only) generate the executable with the specified name. This also reconfigures the install target to be a bit more friendly with system package managers.

# macOS

The macOS build uses Vulkan (via MoltenVK) instead of OpenGL. Apple Silicon (arm64) is required.

## Prerequisites

Install the required dependencies via Homebrew:

```bash
brew install boost ffmpeg tbb simde molten-vk
```

## Building

Use the provided build script:

```bash
./tools/macos/build.sh                          # Standard build
./tools/macos/build.sh --clean                  # Clean build from scratch
./tools/macos/build.sh --verbose                # Verbose output
./tools/macos/build.sh --jobs 8                 # Specify parallel jobs
./tools/macos/build.sh --no-html                # Build without CEF/HTML module
```

The binary will be at `build/shell/casparcg`. You can run it directly:

```bash
./build/shell/run_macos.sh
```

## Creating an app bundle / DMG

The build script can also package the build into a `.app` bundle and DMG for distribution:

```bash
./tools/macos/build.sh --package                          # Build + signed, notarized DMG
./tools/macos/build.sh --package --no-sign                # Build + unsigned app bundle + DMG
./tools/macos/build.sh --package --no-dmg --no-sign       # Build + unsigned app bundle only
```

Or run the packaging script separately after building:

```bash
./tools/macos/package.sh                        # Full: signed, notarized DMG
./tools/macos/package.sh --no-sign              # Unsigned app bundle + DMG
./tools/macos/package.sh --no-dmg --no-sign     # Unsigned app bundle only
```

The app bundle is created at `dist/CasparCG.app` and the DMG at `dist/CasparCG-X.Y.Z-macOS.dmg`.

## Code signing and notarization

For distribution outside the Mac App Store, the app must be signed and notarized. Create a `.env` file in the project root:

```bash
SIGNING_IDENTITY="Developer ID Application: Your Name (TEAMID)"
NOTARIZE_KEYCHAIN_PROFILE="CasparCG-Notarize"
```

To find your signing identity:
```bash
security find-identity -v -p codesigning
```

To set up the notarization keychain profile (one-time):
```bash
xcrun notarytool store-credentials "CasparCG-Notarize" \
    --apple-id "your@email.com" \
    --team-id "TEAMID" \
    --password "xxxx-xxxx-xxxx-xxxx"
```

The app-specific password can be generated at https://appleid.apple.com under "App-Specific Passwords".

## Running the app

From Finder:
```bash
open dist/CasparCG.app
```

From terminal:
```bash
dist/CasparCG.app/Contents/MacOS/casparcg-launcher
```

The launcher creates a working directory at `~/Library/Application Support/CasparCG/` containing the config file, media, templates, data, and logs.

## macOS build notes

- Uses Vulkan via MoltenVK (Metal backend) — OpenGL is not used
- CEF/HTML module is included by default (can be disabled with `--no-html`)
- OSD diagnostics overlay is not available on macOS
- NDI is bundled automatically if `libndi.dylib` is found on the system
