# Future Improvements

Potential GitNortek improvements discovered by reviewing Gittyup forks and feature branches.

Detailed implementation plans live in `docs/future-improvements/`. Each feature must be implemented in its own commit. If implementation decisions change during development, update the relevant plan file in the same commit as the feature.

## Nicolas01/Gittyup Feature Candidates

### fetchHunksAsNeeded

Source branch: `Nicolas01/Gittyup:fetchHunksAsNeeded`

Plan: `docs/future-improvements/03-fetch-hunks-as-needed.md`

Lazy-load diff files and hunks based on scroll position.

Expected value:
- Improves performance on large diffs.
- Reduces UI stalls when opening commits with many changed files.
- Includes fixes around file, hunk, and line stage/unstage behavior.

Risk:
- High integration risk.
- Touches `DiffView`, `FileWidget`, `HunkWidget`, `Patch`, and `DiffTreeModel`.

### AlternativeTreeView

Source branch: `Nicolas01/Gittyup:AlternativeTreeView`

Plan: `docs/future-improvements/04-alternative-tree-view.md`

Improved file tree and diff navigation.

Expected value:
- Alternative tree view for changed files.
- Show only one tree view when a commit is selected.
- Preserve expanded/collapsed tree state.
- Show all diffs when a folder is selected.

Risk:
- High integration risk.
- Large UI refactor around `DiffView`, `TreeView`, and `DoubleTreeWidget`.

### MaximizeDetail

Source branch: `Nicolas01/Gittyup:MaximizeDetail`

Plan: `docs/future-improvements/05-maximize-detail.md`

Add an action to maximize/toggle the detail or diff panel.

Expected value:
- Better UX when reviewing large diffs.
- Lets users focus on the details pane without sidebar clutter.

Risk:
- Medium-high.
- May depend on the same diff-view refactor stack as `AlternativeTreeView`.

### RecurseUntrackedDirs

Source branch: `Nicolas01/Gittyup:RecurseUntrackedDirs`

Plan: `docs/future-improvements/06-recurse-untracked-dirs.md`

Improve recursive handling of untracked directories.

Expected value:
- Better staging/discard/visibility behavior for untracked folders.
- Useful for generated files, build directories, and newly added source trees.

Risk:
- Medium-high.
- Touches diff/file handling and may depend on diff-view refactor work.

### IgnorePatternSingleCommit

Source branch: `Nicolas01/Gittyup:IgnorePatternSingleCommit`

Plan: `docs/future-improvements/07-ignore-pattern-single-commit.md`

Add ignore-pattern actions from file context workflows.

Expected value:
- Faster ignore workflow for generated or unwanted files.
- Useful from file lists and status views.

Risk:
- Medium-high.
- Depends on file context menu and diff-view changes.

### ai_commit_message

Source branch: `Nicolas01/Gittyup:ai_commit_message`

Add AI-powered commit message generation.

Expected value:
- Could help generate commit messages from staged changes.

Risk:
- Product decision required.
- Adds privacy, API, credential, configuration, and dependency questions.
- Lower priority unless GitNortek explicitly wants AI features.

### RepositorySidebarRedesign

Visual reference: GitKraken-style active-repository navigation

Plan: `docs/future-improvements/08-repository-sidebar-redesign.md`

Combine the existing repository chooser with active-repository navigation for
local and remote branches, stashes, tags, and submodules.

Expected value:
- Faster access to repository references and status from the global sidebar.
- Preserves existing recent repository and hosting-account workflows.
- Establishes a tested replacement for the embedded reference selector.

Risk:
- High integration risk.
- Crosses global sidebar, repository-tab, reference-filter, stash, and submodule
  ownership boundaries.
- Explicitly excludes linked Git worktree UI and management.

### ptyxis_terminal

Source branch: `Nicolas01/Gittyup:ptyxis_terminal`

Plan: `docs/future-improvements/01-ptyxis-terminal.md`

Add Ptyxis terminal as a supported Linux terminal candidate.

Expected value:
- Small Linux desktop integration improvement.

Risk:
- Low.

### fix/qt-version-windows-build

Source branch: `Nicolas01/Gittyup:fix/qt-version-windows-build`

Plan: `docs/future-improvements/02-qt-windows-build.md`

Modern Qt 6.7 CI and packaging fixes.

Expected value:
- Windows Qt/MSVC build compatibility.
- AppImage Qt path fixes.
- GitHub release publishing fix.
- Windows theme palette refresh fix.

Risk:
- Low-medium.
- Mostly CI/package changes, with one runtime style fix.

## Suggested Port Order

1. `ptyxis_terminal`
2. `fix/qt-version-windows-build`
3. `fetchHunksAsNeeded`
4. `AlternativeTreeView`
5. `MaximizeDetail`
6. `RecurseUntrackedDirs`
7. `IgnorePatternSingleCommit`
8. `ai_commit_message` only if explicitly wanted

## NortekMed UI Roadmap

1. `RepositorySidebarRedesign`
