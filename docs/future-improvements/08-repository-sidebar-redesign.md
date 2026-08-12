# 08 - Repository Sidebar Redesign

Status: Planned, blueprint complete

Branch: `RepositorySidebarRedesign`

Visual reference: `/home/dbeltrando/Documents/Untitled.png`

Reference reviewed: GitKraken-style repository navigation with collapsible
sections, section counts, compact reference rows, a selected branch highlight,
and branch tracking badges. GitNortek will implement the interaction pattern
with its own palette, icons, terminology, and existing Git workflows.

## Goal

Redesign the global repository sidebar as a combined repository chooser and
active-repository navigator. Preserve GitNortek's existing repository, recent
repository, and hosting-account workflows while adding compact navigation for
the active repository's branches, stashes, tags, and submodules.

The implementation must be safe to complete over multiple development
sessions. This blueprint is the source of truth for scope, decisions, phase
status, validation, and handoff notes.

## Scope Decisions

- Modify the global `SideBar` beside the repository tabs, not just the
  repository-local commit-history pane.
- Keep a compact form of the existing Open, Recent, and hosting-account
  repository chooser.
- Add an active-repository navigator below the chooser.
- Include functional Local, Remote, Stashes, Tags, and Submodules sections.
- Display Cloud Patches, Pull Requests, GitHub Issues, and Teams as disabled
  zero-count placeholders until separate backend features are implemented.
- Do not display or implement a Worktrees section, count, action, setting, or
  placeholder.
- Preserve current reference interaction: single click selects or filters
  commit history; double click or a context-menu action performs checkout.
- Remove the existing embedded Branches/Remotes/Tags selector only after the
  new navigator reaches behavior parity and regression tests pass.
- Do not copy GitKraken trademarks or proprietary artwork. Use GitNortek/Qt
  icons and palette-derived styling.

## Non-Goals

- Linked Git worktree discovery or management.
- Cloud Patches integration.
- Pull request, issue, or team listing APIs.
- Replacing hosting-account persistence or the Start dialog.
- Changing ordinary working-directory status or `workdirChanged` behavior.
- Reworking the commit graph, diff view, or repository tabs beyond the wiring
  needed for active-repository synchronization.
- Making the navigator an editable branch, remote, or submodule configuration
  table.

## Current Architecture

### Global repository sidebar

- `src/ui/MainWindow.cpp` creates a splitter containing `SideBar` and
  `TabWidget`, and owns sidebar visibility animation and persistence.
- `src/ui/SideBar.cpp` contains the private `RepoModel`, progress delegate,
  Open/Recent/Remote account tree, row actions, and footer menus.
- `RepoModel` is anonymous, stores raw `QObject *` values in model indexes, and
  resets broadly when tabs, recent repositories, or accounts change.
- `RecentRepositories` and `Accounts` are shared with `StartDialog`; their data
  and existing settings must remain compatible.
- The current preferred sidebar width is 192 pixels and its resized width is
  not persisted.

### Repository-local references

- `RepoView` owns `ReferenceWidget` above `CommitList` in its internal left
  pane.
- `ReferenceModel` exposes Branches, Remotes, and Tags. Stash currently appears
  as one `refs/stash` pseudo-reference under Branches.
- `ReferenceWidget::referenceChanged` changes the commit walker and
  `referenceSelected` selects a reference tip in `CommitList`.
- `ReferenceView` builds checkout, rename, delete, push, merge, rebase, and
  squash menus by discovering its owning `RepoView` through widget ancestry.
  That mechanism cannot be used by a global sidebar.
- `RepoView::reference()` and `RepoView::selectReference()` provide part of the
  bridge required by a global navigator.

### Existing section support

| Section | Existing support | Initial delivery |
| --- | --- | --- |
| Local | Local branches, upstreams, checkout and branch actions | Functional |
| Remote | Remote-tracking branches and tracking-branch checkout flow | Functional |
| Stashes | Enumeration plus apply, pop, and drop operations | Functional after adding row model |
| Cloud Patches | None | Disabled zero-count placeholder |
| Pull Requests | Creation only; no listing model/API | Disabled zero-count placeholder |
| GitHub Issues | None | Disabled zero-count placeholder |
| Teams | None | Disabled zero-count placeholder |
| Tags | Enumeration, navigation, checkout, push, and deletion | Functional |
| Submodules | Enumeration, open, initialize, update, and status checks | Functional navigation |
| Worktrees | No product-level model or UI | Explicitly absent |

## Desired Layout

The global sidebar contains two logical areas:

```text
Repository Sidebar
|-- Compact repository chooser
|   |-- Open repositories
|   |-- Recent repositories
|   `-- Hosting accounts and repositories
|-- Active repository navigator
|   |-- LOCAL
|   |-- REMOTE
|   |-- STASHES
|   |-- CLOUD PATCHES
|   |-- PULL REQUESTS
|   |-- GITHUB ISSUES
|   |-- TEAMS
|   |-- TAGS
|   `-- SUBMODULES
`-- Existing repository/account footer actions
```

The repository chooser must remain discoverable but should not consume most of
the sidebar. The active-repository navigator is the primary scrolling area.
The precise chooser height should be content-aware and adjustable during visual
validation rather than encoded as an arbitrary large fixed height.

## Visual Specification

- Use compact rows with consistent indentation and muted type icons.
- Separate sections with subtle palette-derived dividers.
- Show a disclosure indicator, section icon, uppercase section label, and
  right-aligned count on each header.
- Use a strong but theme-compatible selected-row background.
- Give the checked-out branch a distinct current/check indicator.
- Show ahead and behind tracking values at the right edge only when an upstream
  exists and the values are nonzero.
- Elide long names and expose full names and paths in tooltips.
- Keep counts and secondary status legible in light, dark, and custom themes.
- Avoid hard-coded colors except where a controlled semantic color cannot be
  obtained from the active palette.
- Establish final row heights, indent widths, divider colors, selected colors,
  and minimum/default widths through side-by-side manual comparison with the
  reference image.

## Interaction Specification

### Sections

- Header activation expands or collapses the section.
- Counts reflect the complete section, not only visible rows.
- Empty functional sections remain visible with a zero count.
- Placeholder sections are visible, disabled, and explain their unavailable
  state through a tooltip.
- Section expansion state survives model refresh and application restart.

### References

- Single click selects the reference through `RepoView` and preserves current
  commit-history filtering semantics.
- Double click invokes the existing checkout behavior.
- Remote checkout preserves the existing choice among detached checkout,
  resetting an existing local branch, and creating a tracking local branch.
- Context menus preserve valid existing actions for local branches, remote
  branches, and tags.
- A menu opened for one repository must continue to target that repository even
  if the active tab changes before an action is triggered.

### Stashes

- Show individual entries in `stash@{n}` order with their summary.
- Selecting an entry shows/selects the corresponding stash commit.
- Context actions apply, pop, or drop the exact stash index represented by the
  row.
- Removing the final stash restores HEAD selection as current code does.

### Submodules

- Show at least submodule name and path.
- Activation opens the submodule through existing `RepoView` behavior.
- Initial implementation may show initialized/update status only when it can be
  obtained without blocking paint or the UI thread.
- Configuration and destructive operations remain in existing menus/dialogs.

### Repository chooser

- Preserve opening, selecting, closing, and auto-hiding behavior for repository
  tabs.
- Preserve recent repository opening and removal.
- Preserve hosting account add, authorize, refresh, remove, clone, and local
  association workflows.
- Preserve clone, open, initialize, full-path, filtering, and full-remote-name
  footer actions.

## Proposed Architecture

Introduce repository-scoped components separate from the anonymous repository
chooser model:

- `src/ui/RepositoryNavigator.h`
- `src/ui/RepositoryNavigator.cpp`
- `src/ui/RepositoryNavigatorModel.h`
- `src/ui/RepositoryNavigatorModel.cpp`

Names may change if implementation reveals a smaller clean boundary. Any name
or ownership change must be recorded in this blueprint in the same commit.

### Ownership

- `SideBar` continues to own the repository chooser and footer.
- `SideBar` owns one `RepositoryNavigator` bound to the active `RepoView`.
- `RepositoryNavigator` owns its model, delegate, section expansion, selection,
  and context-menu presentation.
- `RepoView` remains the operation owner for checkout, branch, tag, stash, and
  submodule actions.
- The navigator receives an explicit `RepoView *`; it must not infer one from
  widget ancestry or fetch `MainWindow::currentView()` at action time.

### Active repository binding

On `TabWidget::currentChanged`:

1. Disconnect active-repository-specific connections.
2. Resolve the new `RepoView`, or clear the navigator when there is none.
3. Bind the model to the new repository.
4. Connect reference and submodule notifications.
5. Synchronize the selected reference from `RepoView::reference()`.
6. Restore section expansion, selection, and session scroll state.

Tab removal and `RepoView` destruction must clear all guarded pointers and
connections before model data can be requested.

### Model structure

Use explicit node types rather than identifying rows from tree position or raw
anonymous `QObject *` pointers. Required section node types are Local, Remote,
Stashes, CloudPatches, PullRequests, GitHubIssues, Teams, Tags, and Submodules.
There is deliberately no Worktrees enum value.

Rows should carry typed data needed by actions, such as `git::Reference`, stash
index/commit, or `git::Submodule`. Prefer value objects or guarded ownership;
never retain pointers whose lifetime is shorter than the active `RepoView`.

### Updates

- Reference changes update Local, Remote, and Tags from
  `RepositoryNotifier::referenceAdded`, `referenceRemoved`, and
  `referenceUpdated`.
- Stashes refresh after GitNortek stash operations and repository refreshes.
- Submodules refresh after `RepoView::submodulesChanged` and relevant repository
  refreshes.
- Broad resets are acceptable for the first correct implementation, but
  selection and expansion restoration must be deterministic.
- Incremental updates may replace resets after profiling and tests establish a
  safe baseline.

### Reference state

Move selected-reference synchronization behind a public `RepoView` boundary.
Add a signal for reference selection changes if necessary. During migration,
the old and new selectors must update one another without signal loops.

Normalize the `commit.refs.all` setting interpretation: `ReferenceWidget`
currently treats it as a bool while `CommitList` treats it as `RefsFilter`.
Preserve persisted meanings while making one typed interpretation authoritative.

### Context actions

Extract or rebuild reference context-menu construction around an explicit
`RepoView *` and `git::Reference`. Reuse existing operation methods rather than
duplicating Git behavior in the model or delegate.

## Persistence

Use a namespace distinct from the current hosting-account expansion settings:

```text
sidebar/repositoryNavigator/expanded/<section>
sidebar/repositoryNavigator/chooserExpanded
sidebar/repositoryNavigator/width
sidebar/repositoryNavigator/showTrackingStatus
```

- Start with global per-section expansion settings.
- Preserve per-repository selection and scroll position for the current session.
- Add persistent per-repository state only if user validation demonstrates a
  need and a stable repository identity is defined.
- Continue reading existing `sidebar`, recent repository, hosting account,
  full-path, filtering, and full-name settings unchanged.
- Persist the resized sidebar width and clamp invalid or obsolete values to a
  usable range.

## Performance Requirements

- Do not calculate ahead/behind graph differences from delegate paint or model
  `data()` calls.
- Compute tracking values asynchronously or in a bounded refresh step, cache by
  branch/upstream pair, and invalidate after reference changes, fetch, pull,
  push, and checkout.
- Skip tracking calculations for branches without upstreams.
- UI population and tab switching must remain responsive with hundreds of local
  and remote branches.
- Submodule update checks must not block navigator construction or painting.
- Host placeholder sections must not issue network requests.

## Accessibility

- Give the chooser, navigator, sections, and footer controls stable object names
  for tests and assistive technology.
- Provide accessible names/descriptions for disclosure controls, counts,
  current-branch state, tracking state, disabled placeholders, and icon-only
  actions.
- Support keyboard traversal, expand/collapse, activation, and context menus.
- Do not remove focus from the navigator as the current sidebar tree does.
- Verify selected, disabled, hover, and focus states with light and dark palettes.

## Implementation Phases

Each phase should be independently buildable. Do not begin a later destructive
migration until the preceding acceptance gate passes.

### Phase 0 - Baseline and blueprint

Status: Complete

1. Create `RepositorySidebarRedesign` from the current local `master`.
2. Add this blueprint and index it in `FutureImprovement.md`.
3. Record the baseline commit and worktree state in the implementation ledger.
4. Confirm the existing focused tests and available build directory.

Gate:
- Branch and blueprint exist.
- The blueprint captures all product decisions and the no-Worktrees constraint.

### Phase 1 - Existing sidebar regression coverage

Status: Complete

1. Add stable object names/test access for the current chooser and footer.
2. Add focused coverage for current Open, Recent, and hosting-account model
   behavior before restructuring it.
3. Cover tab insertion, selection, switching, removal, and sidebar visibility.
4. Reproduce and address any unbalanced model reset found when closing a tab
   through `RepoView::close()` before building on that path.

Gate:
- Existing sidebar workflows have automated regression coverage.
- `main_window` and the new focused test pass headlessly.

### Phase 2 - Repository navigator model

Status: Complete

1. Add explicit section and row types.
2. Populate Local, Remote, Tags, and Submodules.
3. Add individual stash rows.
4. Add disabled Cloud Patches, Pull Requests, GitHub Issues, and Teams sections.
5. Expose counts, icons, tooltips, current-reference state, and stable roles.
6. Assert exact section order and absence of Worktrees in tests.

Gate:
- Model tests pass for populated, empty, and unsupported sections.
- Model can be rebound or cleared without stale data.

### Phase 3 - Combined sidebar layout

Status: Not started

1. Split `SideBar` into a compact chooser area, primary navigator area, and
   existing footer.
2. Bind the navigator to the active tab.
3. Preserve chooser actions and selection behavior.
4. Add persisted section expansion and sidebar width.
5. Restore selection and scroll state on tab changes.

Gate:
- Switching and closing tabs cannot operate on stale repositories.
- Existing chooser tests and navigator binding tests pass.
- The panel remains usable at minimum supported window size.

### Phase 4 - Reference navigation and actions

Status: Not started

1. Add explicit `RepoView` reference-state synchronization.
2. Wire single-click history selection.
3. Wire double-click checkout.
4. Port valid local, remote, and tag context actions using an explicit
   repository view.
5. Preserve remote tracking-branch and detached checkout prompts.
6. Prevent actions from crossing repositories after a tab switch.

Gate:
- Reference selection and checkout behavior match the old selector.
- Existing `referencelist` tests and new action tests pass.

### Phase 5 - Tracking badges

Status: Not started

1. Add a cache for local branch ahead/behind values.
2. Calculate values outside painting and model data access.
3. Invalidate on local/remote reference and network-operation changes.
4. Render no-upstream, up-to-date, ahead, behind, and diverged states correctly.
5. Measure refresh cost with a repository containing many branches.

Gate:
- Tracking tests pass and repeated paints do not trigger graph calculations.
- Tab switching and reference refresh remain responsive.

### Phase 6 - Stash and submodule actions

Status: Not started

1. Add a `RepoView` bridge for selecting an individual stash.
2. Wire apply, pop, and drop to stable stash indexes.
3. Refresh stash count/selection after mutations.
4. Wire submodule activation to existing open behavior.
5. Add nonblocking initialized/update indicators where practical.

Gate:
- Stash operations target the displayed entry.
- Final-stash removal returns to HEAD.
- Submodule activation and refresh tests pass.

### Phase 7 - Visual, theme, and accessibility pass

Status: Not started

1. Implement compact section headers, rows, dividers, icons, counts, tracking
   badges, and selected/current states.
2. Tune sidebar width, chooser allocation, row height, indentation, and elision
   against the visual reference.
3. Add keyboard navigation, focus indication, accessible names, and tooltips.
4. Verify light, dark, and custom themes on supported platforms.

Gate:
- Manual visual checklist is complete.
- Text and controls remain legible at 100%, 150%, and 200% scaling where
  platform testing is available.

### Phase 8 - Remove embedded reference selector

Status: Not started

1. Confirm parity for Local, Remote, Tags, and stash navigation.
2. Remove `ReferenceWidget` from the `RepoView` layout.
3. Remove obsolete widget-only state and connections while retaining reusable
   models/actions still needed elsewhere.
4. Verify commit search, graph filter modes, checkout synchronization, links,
   and maximize-detail behavior.

Gate:
- No duplicate reference selector remains.
- All reference and commit-history tests pass.

### Phase 9 - Documentation and release readiness

Status: Not started

1. Update translatable strings and translation catalogs as required.
2. Add a `Changed` entry under the top development release in
   `docs/changelog.md`.
3. Replace public screenshots that show the old sidebar.
4. Run formatting, focused tests, the full suite, and a release build.
5. Update this blueprint to Implemented with final file and behavior notes.

Gate:
- Build, tests, formatting, documentation, and manual validation are complete.

## Test Plan

Add a focused test, preferably `test/repository_sidebar.cpp`, and retain or
extend `test/main_window.cpp`, `test/referencelist.cpp`,
`test/branches_panel.cpp`, and `test/Submodule.cpp` where ownership fits.

Required automated scenarios:

1. Sections appear in the specified order and no Worktrees section exists.
2. Local, Remote, Stashes, Tags, and Submodules report correct counts.
3. Unsupported service sections are disabled and report zero.
4. Empty repositories retain useful zero-count section headers.
5. HEAD/current branch indication updates after checkout.
6. Single click selects or filters history without checkout.
7. Double click and context-menu checkout target the active repository.
8. Remote checkout retains existing tracking/detached behavior.
9. Local/remote/tag add, remove, and update notifications refresh the model.
10. Ahead, behind, diverged, up-to-date, and no-upstream states are correct.
11. Stash rows preserve index order and mutations target the correct stash.
12. Submodule activation opens the intended submodule.
13. Tab switching replaces all repository-scoped rows and selection.
14. Closing the active tab leaves no stale pointers or signal callbacks.
15. Expansion and sidebar-width settings restore and invalid values are clamped.
16. Open, Recent, hosting account, clone, open, and initialize workflows remain
    available.
17. Keyboard expansion and activation work without a mouse.

Focused verification command:

```bash
ctest --test-dir build/release \
  -R '^(repository_sidebar|referencelist|branches_panel|Submodule|main_window)$' \
  --output-on-failure
```

Full verification:

```bash
ninja -C build/release check --verbose
./cl-fmt.sh
```

If the existing build directory differs, record the exact commands used in the
implementation ledger rather than creating an undocumented parallel workflow.

## Manual Validation

- Linux and Windows; macOS when available.
- Light, dark, and custom themes.
- 100%, 150%, and 200% display scaling where available.
- No open repository and one/multiple repository tabs.
- No remotes, one remote, and multiple remotes.
- No upstream, ahead, behind, diverged, and up-to-date branches.
- Long and slash-separated branch/tag names.
- Hundreds of remote branches with scrolling.
- No stashes and multiple stashes.
- No submodules and multiple initialized/uninitialized submodules.
- No configured hosting account and an associated hosting repository.
- Sidebar collapse/restore, resizing, restart, and minimum window width.
- Keyboard-only traversal and context-menu invocation.

## Acceptance Criteria

- The global sidebar combines existing repository/account navigation with an
  active-repository navigator modeled on the supplied reference.
- Open, Recent, hosting accounts, clone, open, initialize, and existing settings
  remain functional.
- Local and Remote branches, Stashes, Tags, and Submodules display accurate
  active-repository data and counts.
- Single-click navigation and explicit checkout behavior remain predictable.
- Branch tracking status is correct and does not make painting or tab switching
  block.
- Cloud Patches, Pull Requests, GitHub Issues, and Teams are safe disabled
  placeholders and make no network requests.
- Worktrees are absent from UI, models, settings, actions, and tests.
- The old embedded reference selector is removed only after parity is verified.
- Selection, expansion, scrolling, and width restore safely.
- The sidebar is keyboard accessible and legible across themes.
- Focused and full tests pass, formatting passes, and user-visible documentation
  is updated.

## Risks and Mitigations

### Two sidebar concepts

Risk: global repository switching and repository-local history filtering can be
confused during migration.

Mitigation: keep ownership explicit, name new components for active-repository
navigation, and retain the old selector until parity tests pass.

### Stale active-repository pointers

Risk: tab switching/removal can leave model indexes or delayed menu actions
targeting a destroyed or different `RepoView`.

Mitigation: use guarded pointers, explicit connection ownership, model clearing,
and action captures tied to the repository active when the menu opened.

### Existing anonymous chooser model

Risk: raw pointer indexes and broad reset pairs can become unstable during
layout changes.

Mitigation: add regression tests first and keep the new repository-scoped model
separate. Refactor the chooser only where required.

### Performance

Risk: per-branch graph differences and submodule checks can freeze the UI.

Mitigation: cache tracking data, calculate outside paint/data paths, invalidate
deliberately, and keep submodule checks asynchronous.

### Shared settings and models

Risk: changing recent/account presentation can diverge from `StartDialog` or
break persisted installations.

Mitigation: preserve shared singleton behavior and existing keys; namespace only
new navigator state.

### Unsupported service expectations

Risk: visible placeholders may imply working integrations.

Mitigation: disable them, show zero, provide an unavailable tooltip, and track
backend integrations as separate plans.

### Path identity and linked worktrees

Risk: GitNortek can incidentally open linked worktree paths even though this
feature does not support worktree management.

Mitigation: omit worktree-specific discovery and UI, retain normal working-tree
status behavior, and ensure unusual repository paths fail safely without
corrupting chooser state.

## Multi-Session Protocol

At the start of every session:

1. Read this blueprint completely.
2. Run `git status --short --branch` and verify
   `RepositorySidebarRedesign` is active.
3. Read the implementation ledger below and identify the first incomplete
   phase.
4. Inspect uncommitted changes without reverting unrelated work.
5. Run the narrow baseline test for the area being changed.
6. Mark only one phase/task as in progress.

At the end of every session:

1. Run the narrowest relevant build/tests and record results.
2. Update phase statuses and the implementation ledger.
3. Record changed files, decisions, known failures, and the exact next action.
4. Update this blueprint in the same commit when an implementation decision
   changes.
5. Do not mark a phase complete until its gate passes.

## Implementation Ledger

Update this table throughout implementation. Newest entries go first.

| Date | Phase | State | Changes and validation | Next action |
| --- | --- | --- | --- | --- |
| 2026-08-12 | 2 | Complete | Added `RepositoryNavigatorModel` with typed Local, Remote, Stashes, Cloud Patches, Pull Requests, GitHub Issues, Teams, Tags, and Submodules sections; value-based reference/commit/submodule roles; disabled service placeholders; current-branch state; and notifier-driven refresh. There is no Worktrees model value. Extended `repository_sidebar` with Qt model invariant, order, placeholder, branch, remote, notifier, stash, tag, clear, and typed-role coverage. | Add the collapsible repository navigator view and expansion persistence without binding it to the global sidebar yet. |
| 2026-08-12 | 1 | Complete | Added an isolated `repository_sidebar` test covering empty Open/Recent rows, all hosting-provider placeholders, Recent removal through the footer, and offline account add/remove model updates without touching user settings or the network. `ctest -R '^(main_window|repository_sidebar)$'` passed. | Begin Phase 2 with the typed repository navigator model and model-focused tests. |
| 2026-08-12 | 1 | In progress | Added stable repository tree/footer identifiers and accessible footer button names. Centralized user-requested tab closure in `TabWidget::closeTab()` so tab-bar, menu, context-menu, and footer paths emit balanced model reset signals, including cancellation. Added `main_window` coverage for chooser roots, tab switching, sidebar removal, signal pairing, model refresh, and recent-row preservation. `test_main_window` built and `ctest -R '^main_window$'` passed. | Add focused Recent and hosting-account chooser/action coverage to complete the Phase 1 gate. |
| 2026-08-12 | 0 | Complete | Created `RepositorySidebarRedesign` from local `master`; added and indexed the blueprint. Baseline local `master` was ahead of `nortekmed/master` by two commits and the worktree was clean. `git diff --check` passed. | Begin Phase 1 by adding baseline regression coverage for the existing repository chooser and tab lifecycle. |

## Follow-Up Plans

Create separate blueprints before implementing network-backed sections:

- Pull request listing and navigation across supported hosting providers.
- GitHub issue listing and navigation.
- GitHub organization/team integration after product semantics are defined.
- Cloud Patches only if GitNortek chooses to implement an equivalent feature.

These follow-ups must not introduce a Worktrees section into this feature.
