# Getting Started

This guide helps you build and run CrossPoint locally.

## Prerequisites

- PlatformIO Core (`pio`) or VS Code + PlatformIO IDE
- Python 3.8+
- `clang-format` 21+ in your `PATH` (CI uses clang-format 21)
- USB-C cable
- Xteink X4 device for hardware testing

If `./bin/clang-format-fix` fails with either of these errors, install clang-format 21:

- `clang-format: No such file or directory`
- `.clang-format: error: unknown key 'AlignFunctionDeclarations'`

Examples:

```sh
# Debian/Ubuntu (try this first)
sudo apt-get update && sudo apt-get install -y clang-format-21

# If the package is unavailable, add LLVM apt repo and retry
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 21
sudo apt-get update
sudo apt-get install -y clang-format-21

# macOS (Homebrew)
brew install clang-format
```

Then verify:

```sh
clang-format-21 --version
```

The reported major version must be 21 or newer.

## Clone and initialize

```sh
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader
```

If you already cloned without submodules:

```sh
git submodule update --init --recursive
```

Enable the repository-managed Git hooks (required once per clone):

```sh
git config core.hooksPath .githooks
chmod +x .githooks/pre-commit
```

## Build

```sh
pio run
```

### Windows: use a short PlatformIO core directory

On Windows, keep PlatformIO's core directory at a short path such as `C:\pio`:

```sh
setx PLATFORMIO_CORE_DIR C:\pio
```

The default (`C:\Users\<name>\.platformio`) makes the build fail with a
`filename or extension is too long` spawn error from the assembler.

Since Arduino-ESP32 3.3.11 the framework contributes ~270 `-I` flags, and GCC
forwards all of them to `as`. Measured on the C3 with the default path, that
command line is **32,771 bytes** against Windows' hard 32,767-character
`CreateProcess` limit -- four bytes over. Every C++ compile dies, and because the
toolchain's `as` is a wrapper that re-spawns the real assembler, it panics
instead of reporting anything useful. A short core directory saves ~18
characters per include path, which is far more than enough headroom.

Linux and macOS are unaffected; their limits are orders of magnitude higher.

## Flash

```sh
pio run --target upload
```

## First checks before opening a PR

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

## What to read next

- [Architecture Overview](./architecture.md)
- [Development Workflow](./development-workflow.md)
- [Testing and Debugging](./testing-debugging.md)
