---
description: Configure and build GitNortek with CMake and Ninja
---

Build GitNortek using the verified out-of-source Release workflow.

```bash
cd "$(pwd -P)"
mkdir -p build/release
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build/release
ninja -C build/release
```

If Qt is installed outside the default search path, add
`-DCMAKE_PREFIX_PATH=<qt-install-prefix>` to the configure command. Initialize
missing Git submodules only after a separate confirmation using `/deps`. Do not
invent a build directory or replace the verified workflow with recursive
deletion.
