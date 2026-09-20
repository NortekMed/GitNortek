---
description: Clean the verified GitNortek release build directory
---

Remove generated files from the existing Release build directory using its
verified Ninja clean target:

```bash
cd "$(pwd -P)"
ninja -C build/release clean
```

This does not remove source files or the build directory itself. Do not use
recursive deletion as a substitute. Reconfigure and rebuild with `/build`
afterward.
