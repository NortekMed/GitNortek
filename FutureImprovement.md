# Future Improvements

This roadmap lists only work that remains open. Detailed implementation plans live in `docs/future-improvements/`.

## Repository Sidebar Redesign

Plan: `docs/future-improvements/08-repository-sidebar-redesign.md`

Complete the active repository sidebar redesign, including the remaining reference actions, visual validation, removal of the superseded embedded selector, and release readiness work recorded in the blueprint.

Risk:
- High integration risk across global sidebar, repository-tab, reference-filter, stash, and submodule ownership boundaries.
- Explicitly excludes linked Git worktree UI and management.

## Undecided Candidate

### ai_commit_message

Source branch: `Nicolas01/Gittyup:ai_commit_message`

Add AI-powered commit message generation.

Expected value:
- Could help generate commit messages from staged changes.

Risk:
- Product decision required.
- Adds privacy, API, credential, configuration, and dependency questions.
- Lower priority unless GitNortek explicitly wants AI features.
