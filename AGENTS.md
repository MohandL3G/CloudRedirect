# CloudRedirect — Agent Guide

## Build

**Windows:**
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
Produces `build/Release/cloud_redirect.dll` (C++20 DLL). Also publishes WPF UI (`ui/bin/publish/CloudRedirect.exe`) and the CLI (`build/Release/cloud_redirect_cli.exe`). The DLL is embedded into the UI/CLI binaries as a managed resource.

**Linux (32-bit for Steam):**
```bash
mkdir -p build-linux32 && cd build-linux32
cmake .. -DCMAKE_BUILD_TYPE=Release -DLINUX_32BIT=ON -DCMAKE_CXX_FLAGS=-m32
make cloud_redirect
```
Must target glibc ≤2.31 — use Ubuntu 20.04 or equivalent (Distrobox). `_GLIBCXX_USE_CXX11_ABI=0` is forced for Steam ABI compat.

**Flatpak (Linux UI):** `cd flatpak && ./build.sh` — requires `cloud_redirect.so` in `flatpak/` first.

## Project structure

| Path | What |
|---|---|
| `src/common/` | Platform-agnostic core (cloud storage, staging, VDF/protobuf/json parsing, RPC, miniz, metadata) |
| `src/providers/` | Cloud backends: Google Drive, OneDrive, FileBrowser Quantum, local disk |
| `src/platform/win/` | Windows: DLL injection (`dllmain.cpp`), cloud intercept, HTTP server, token store |
| `src/platform/linux/` | Linux: vtable hook, cloud hooks, init, HTTP server |
| `ui/` | WPF (.NET 8, `win-x64`) companion app — embeds DLL as resource |
| `ui-linux/` | Qt6 QML + QuickControls2 Linux UI |
| `cli-dotnet/` | .NET 8 STFixer CLI — shares `ui/Services/Patching/` via linked files |
| `cli-rust/` | Rust STFixer CLI port (`ureq`, `winreg`, AES-256-CBC, SHA2, MD5) |
| `flatpak/` | Flatpak manifest + build script for Linux distribution |

## CLIs (STFixer)

Two parallel implementations, both support `stfixer` and `help` commands:

```powershell
# .NET CLI (build/Release/cloud_redirect_cli.exe)
cloud_redirect_cli.exe /stfixer

# Rust CLI (cli-rust/) — same interface
CloudRedirectCLI.exe /stfixer
```

Both close Steam → download core DLLs → apply patches → deploy `cloud_redirect.dll` → enable auto-update in `config.json`.

## Version

Single source of truth: `Version.props` → `<ReleaseVersion>2.2.0</ReleaseVersion>`. Optionally includes `<ReleasePrerelease>` suffix (e.g. `-TEST11`). Wired into CMake and .NET projects via `Import`. Git SHA appended automatically (`CR_SO_VERSION`).

## Key quirks & gotchas

- **Linux 32-bit**: Steam is 32-bit, so `cloud_redirect.so` must be 32-bit. The UI copies it to `~/.local/share/SLSsteam/` at deploy time. `CR_BUNDLED_SO` env var points to the bundled `.so` for the wrapper script.
- **UI embeds the DLL**: Before building the UI, `build/Release/cloud_redirect.dll` must exist (built by CMake). The `.csproj` copies it to `ui/Resources/` as an embedded resource. Build order: `cmake --build build` → `dotnet publish` (or just run the top-level CMake).
- **`MINIZ_NO_STDIO`** is defined globally (miniz used for zip I/O without stdio).
- **Nullable warnings suppressed** in the WPF UI project (CS8600-CS8625) — considered cosmetic.
- **No tests directory** currently exists on disk. CMakeLists.txt defines test targets guarded by `if(EXISTS tests/...)` so they're silently skipped.
- **No local builds**: The agent must never build locally. All builds run in GitHub Actions (`.github/workflows/build.yml`) on push, PR, or manual trigger. Make commits and let CI verify compilation. If compilation info is needed, check the CI run or read relevant files for correctness.
- **CI workflow**: `.github/workflows/build.yml` builds on `windows-latest` via CMake + VS 2022 + .NET 8 on every push/PR/manual trigger, uploading all outputs as artifacts.
- **No linter / formatter / typecheck** configured at repo level.
