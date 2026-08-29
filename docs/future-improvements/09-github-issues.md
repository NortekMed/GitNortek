# GitHub Issues Navigator

## Scope

Populate the repository navigator's GitHub Issues section with up to 50 open
issues, ordered by most recent update. Pull requests are excluded.

## Repository Selection

- Discover GitHub repositories from configured Git remotes.
- Prefer a persisted selection, then the default remote, `origin`, `upstream`,
  and remaining remotes by name.
- Show the active `Issues repository` selector as the first row inside the
  expanded GitHub Issues section whenever an eligible remote exists.
- Persist the selected remote in repository app configuration under
  `sidebar.githubIssues.remote`.
- Treat `github.com` remotes as eligible for anonymous public access.
- Use credentials only when the remote exactly matches a repository belonging
  to a configured GitHub account.
- Require an exact configured-account match for GitHub Enterprise remotes.

## Request Behavior

- Use GitHub's REST issue search with `is:issue is:open`, `per_page=50`, and
  descending update order.
- Send bearer credentials only over HTTPS.
- Keep loaded rows visible when a manual refresh fails.
- Cancel logically stale results when the active repository, remote, or account
  changes.
- Do not poll periodically.

## Presentation

- Display rows as `#<number> <title>` with author and URL in the tooltip.
- Show loading, empty, failure, refreshing, and stale states without counting
  status rows as issues.
- Keep row clicks inside the navigator and provide an explicit, validated
  HTTP(S) `Open in Browser` context action.
- Provide a `Refresh Issues` context action.

## Validation

- Test URL parsing and anonymous/authenticated request safety with a local HTTP
  server.
- Test parsing, result limits, malformed responses, and HTTP errors.
- Test model states, origin/upstream selection, persistence, stale callbacks,
  remote updates, and preservation of the selected Git reference.

## Fast Issue Credentials

The Fast Issue toolbar action reuses the Git credential helper configured by
`credential.helper` for `github.com`; it does not require a GitNortek account.
The stored credential must be a PAT with `Issues: Read and write` for
`NortekMed/GitNortek` and `Members: Read-only` for the `NortekMed`
organization. The token must also be authorized for the organization when SAML
SSO is enabled. SSH keys can authenticate Git transport but cannot authenticate
GitHub REST API requests.
