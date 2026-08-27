### vX.X.X - 2026-08-10 (DEV)

Bug Fix and Feature release

#### Added

* Add more information about the credential stores to give the user the possibility to easily see the tradeoffs of every credential store
* Add GitNortek package artifact workflows for Linux RPM and Windows installers
* Add submodule creation, update checks, and modification dialog support
* Add a toggle for the committed file tree
* Add Ptyxis terminal auto-detection on Linux
* Add Tamil translation
* Add repository navigator context actions for branches and submodules, including permanent submodule removal
* Show colored Pin summaries on the left and aligned P and O delta columns for submodules in the repository navigator
* Mark the checked-out local branch with a green checkbox and color ahead and behind indicators
* Align Compact Mode into configurable columns with content-aware widths, bottom-connected stash ancestry, and side-connected merge and divergence curves
* Display stashes as square nodes with dotted topology and stash actions in the combined commit graph
* Display GitHub or Gravatar author avatars inside larger commit graph nodes and commit details when avatars are enabled
* Commit checked-out submodule revisions directly from the repository navigator
* Open the amend dialog by clicking the current HEAD commit message
* Warn before pushing a parent commit whose initialized submodule pins cannot be proven available from their clone URLs
* Add a green Stage All Changes button above the working-directory file tree
* Checkout the latest fetched configured branch from behind-origin submodule context menus

#### Changed

* Show the repository sidebar at startup and keep it open when opening repositories
* Show Blame and Diff in the branch and commit pane when files are selected, with more default space for the commit history
* Use the full repository sidebar for navigation and keep only the 20 most recent repositories
* Brand Linux application and packages as GitNortek
* Refresh repositories automatically when Git metadata changes
* Improve diff, blame, commit description, and large binary refresh performance
* Fix path filtering, detached HEAD selection, merge tool visibility, and macOS Finder integration
* Improve Windows and macOS build/package compatibility
* Derive package versions from Git tags in CMake so local and CI package builds use the same version, including commit SHA and dirty state when needed
* Encode prerelease, commit SHA, and dirty state in RPM `Version` with underscores to distinguish package identities precisely
* Use UTC `YYYYMMDD.HHmm` timestamps as RPM package releases
* Avoid hangs and excessive memory use when displaying very large text diffs
* Preserve the selected commit when push or fetch updates remote references
* Offer to remove recent repositories that can no longer be opened
* Display consistent eight-character commit IDs in the interface and versions without Git's `g` prefix
* Preserve configured Compact Mode column widths when the commit graph expands
* Connect dotted stash ancestry from the side of its base commit node
* Keep the operation log open until manually collapsed and leave its toggle bar visible
* Avoid repeating destination commit IDs in generated submodule update messages
* Report only genuinely unavailable submodules in pre-push warnings with actionable error details
* Confirm before creating or tracking a remote branch on the first push, and remove automatic pushing after each commit
* Prevent transient false submodule changes during branch renames and repository scans
* Report local repository diagnostics and avoid reporting dirty worktrees or status errors as clean
* Run Linux package and release jobs on an isolated disposable self-hosted runner with pinned actions and container images
* Expose pull, push, and confirmed force push actions in the Remote menu, toolbar, current branch, and HEAD commit context menus
* Recheck submodule updates after changing submodule URL, path, or branch configuration
* Preserve fetched submodule origin comparisons when committing a new parent pin
* Double the default Branch / Tag width and preserve it during compact-view compression

----

### v2.0.0 - 2025-11-30

Bug Fix and Feature release

#### Added

* Add commit filter to show only the first parent in the commit list view
* Add backend support for SHA256 repos
* Fix email filter being not returning any results
* Add mmap based reading to speed up indexer

#### Changed

* Use Qt6
* Update libgit2 library

----

### v1.4.0 - 2024-04-24

Bug Fix and Feature release

#### Added

* UI(Commit List): Added a right-click menu entry to rename branches.
* UI(Main Menu): Added a menu-entry to rename the current branch.
* UI(Diff View): Added line wrapping option in Tools - Options - Diff
* Option to open Gittyup maximized at startup
* Hotkeys for Navigating CommitList
* Referring repos with their git-dir instead of their work-tree.
* Use columns for file name, directory, and state when files are shown as a list in TreeViews.
* Possibility to change language from the settings
* Linux: Publishing Appimage bundle
* Hide Untracked Files option to DoubleTreeWidget
* Add display options to the commit list

#### Changed

* UI(Commit List): Collapse multiple branch and tag right-click menu entries
                   into submenus. This affects the checkout and delete operations.
* Fix(Build System): Force usage of clang-format v13 to ensure consistent formatting.
* Fix: Commit highlighting
* Compact commit list: move commit message closer to commit graph
* Updating translations
* Close the branch drop-down list after the user checks out a branch.
* Fix: Delete timeline causes crash on windows
* Fix: Rebase: Use original commit message as default message
* Fix: Stage all button

----

### v1.3.0 - 2023-04-20

Performance Improvement and feature release

#### Added

* Colorized status badges
* Template: use first template as default template for the commit message
* Search function for the treeview
* Reworked credential store: add possibility to choose between different methods to store credentials

#### Changed

* Fix external diff in Flatpak build
* Fix windows credentials
* Fix force push to correct remote
* Fix tab title if more than three times a repository with the same name is opened
* Fix storing repository settings correctly, because otherwise they are not applied
* Fix language support
* Improved refresh velocity
* Fix storing and restoring current opened file when Gittyup refreshes
* Improved velocity for files with many hunks

----

### v1.2.2 - 2023-01-22

Bug fix release

#### Changed

* Fix flatpak install process

----

### v1.2.1 - 2023-01-22

Bug fix release

#### Added
* Possibility to hide avatar (Settings - Window - View - Show Avatars)
* Show log entry when a conflict during rebase happens

#### Changed

* Fix download url for flatpak and macos
* Fix Segmentation fault when ignoring files
* Fix discard of complete files and submodules
* Fix context menu entries
* Fix bytesize overflow
* Fix focus loose during scrolling in the Commitlist with the keyboard
* Do not crash when the repository is for some reason broken
* Fix crash if rebasing is not possible

----

### v1.2.0 - 2022-10-28

Bug fix and feature release

#### Added
* Add support for solving merge conflicts for whole files
* Solving binary conflicts directly in Gittyup
* Support rebasing with conflict solving
* Implement amending commits
* Possibility to init submodules after clone (Settings - General - Update submodules after pull and clone)
* Hiding menu bar (Application Settings - Window - Hide Menubar)
* Implement support for Gitea instances

#### Changed
* Fix Segmentation fault when using space to stage files
* Fix menubar color in dark theme
* Filter only branches, tags, remotes attached to selected commit
* Fix crash when global GIT config is invalid
* Fix crash when having errors while adding a remote account
* Fix updater on windows, macos and linux (flatpak)
* Fix discarding file leading to discarding submodule changes
* Fix rebase log messages during rebase
* Improve SSH config handling
* Application settings and repository settings can now be selected with a single settings button
* Use the full file context menu for the staging file list
* Fix Arch Linux build

----

### v1.1.2 - 2022-08-12

Bug fix release

#### Changed

* Fix bundled OpenSSL version incompatibility

----

### v1.1.1 - 2022-06-09

Bug fix release

#### Added
* Distinguish between commit author and committer
* Show image preview also for deleted files
* Official macOS release
* Show which kind of merge conflict occurred for each conflict

#### Changed
* Fix single line staging if not all hunks are loaded
* Fix cherrypick commit author
* Fix segmentation fault if submodule update fails
* Fix line staging with windows new lines
* Show first change in the diff view when loading
* Improved windows icon

----

### v1.1.0 - 2022-04-30

Second release of Gittyup

#### Added
* Button to directly access the terminal and the filebrowser
* Add support for running in single instance mode
* Customizable hotkeys
* Quick commit author overriding
* keyboard-interactive SSH auth
* Improved single line staging and replacing staging image to a more appropriate one
* Font customizing
* Options to switch between staging/unstaging treeview, single tree view and list view
* Do not automatically abort rebase if conflicts occur
* Add possibility to save file of any version on local system
* Add possibility to open a file of any version with default editor

----

### v1.0.0 - 2021-11-18

First version of the GitAhead Fork Gittyup

#### Added
* Staging of single lines
* Double tree view: Seeing staged and unstaged changes in different trees.
* Maximize History or Diff view by pressing Ctrl+M
* Ignore Pattern: Ability to ignore all files defined by a pattern instead of only one file
* Tag Viewer: When creating a new tag all available tags are visible. Makes it easier to create consistent tags.
* Commit Message template: Making it easier to write template based commit messages.

----
