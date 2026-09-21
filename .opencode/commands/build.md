---
description: Configure and build the project with CMake
---

Build the GitNortek C++17 project using the documented CMake and Ninja workflow.

## Configure

From the project root, configure the Release build in `build/release`:

```bash
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build/release
```

If Qt is installed outside the default search path, add the documented option:

```bash
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<qt-install-prefix> -S . -B build/release
```

## Build

```bash
ninja -C build/release
```

The project enables tests by default. Run the documented test target separately
when verification is needed:

```bash
ninja -C build/release check --verbose
```

Do not invent a clean command or use broad recursive deletion. Preserve the
project's existing build directory and dependency state.
