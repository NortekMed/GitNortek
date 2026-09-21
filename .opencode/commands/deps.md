---
description: Inspect and manage Git submodule dependencies
---

Manage GitNortek's Git submodule dependencies. Dependency changes require a
separate direct confirmation; the setup confirmation does not approve them.

## Initialize Submodules

Inspect status first, then ask before running either documented initialization
sequence:

```bash
git submodule init
git submodule update --depth 1
```

For the project's full recursive initialization procedure, ask before running:

```bash
git submodule update --init --recursive
```

## Check Submodule Status

```bash
git submodule status
```

## Update Dependencies

Inspect the requested paths and ask before updating any submodule. Do not run
updates automatically or replace pinned revisions without explicit approval.

## Remove or Add Dependencies

Do not add or remove a submodule during routine setup. Require explicit approval
of the exact Git commands and verify the target path before changing repository
metadata.
