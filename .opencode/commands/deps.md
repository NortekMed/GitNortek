---
description: Inspect and manage GitNortek Git submodules
---

GitNortek uses Git submodules for bundled dependencies. Inspect first:

```bash
git submodule status
```

The setup confirmation does not approve dependency changes. Ask for a direct
affirmative answer before running this state-changing command:

```bash
git submodule update --init --recursive
```

Do not update, remove, or repair submodules automatically. Preserve the
project's existing `.gitmodules` URLs and pinned commits. Use normal read-only
status inspection before every state-changing operation.
