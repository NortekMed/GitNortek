# Self-Hosted GitHub Runner Handoff

## Goal

Run trusted Linux release jobs from NortekMed public repositories on isolated,
disposable self-hosted infrastructure.

## Current Workflow

The package workflow is `.github/workflows/package.yml`:

- Fedora RPM runs in a digest-pinned Fedora 42 container on the
  `Public-Releases` runner group.
- GitHub release publishing runs in a digest-pinned Ubuntu 24.04 container on
  the same runner group.
- Windows EXE remains on GitHub-hosted `windows-2022`.
- The workflow runs only for version tags matching `v*.*.*`.
- Actions are pinned to commit SHAs.
- Default `GITHUB_TOKEN` access is read-only; only the release job receives
  `contents: write`.

## Runner Architecture

The runner host is SRV2. Public jobs do not run in the persistent TorizonBuoy
runner environment.

- Libvirt VM: `gha-public-01`.
- Guest: Ubuntu 24.04 LTS, 4 vCPUs, 6 GiB RAM, 4 GiB swap, and a 100 GiB
  virtual disk.
- Runner: GitHub Actions Runner 2.336.0 with automatic updates disabled.
- Runner group: `Public-Releases`.
- Allowed repository: `NortekMed/Gittyup`.
- Labels: `nortek-public-linux` and `gitnortek-linux`, in addition to the
  standard self-hosted Linux x64 labels.
- Network: dedicated NAT network with access to private IPv4 ranges rejected.
- The VM can access GitHub and public package registries but cannot access SRV2,
  the LAN, or existing VMs.

## Disposable Lifecycle

The immutable base image is:

```text
/home/vm-storage/gha-public/gha-public-base.qcow2
```

`github-public-runner.service` runs the root-owned controller at
`/usr/local/sbin/public-runner-controller`.

For each job, the controller:

1. Creates a fresh qcow2 overlay from the base image.
2. Boots `gha-public-01` on the isolated network.
3. Requests a short-lived GitHub App installation token.
4. Requests a one-hour runner registration token.
5. Registers one ephemeral runner in `Public-Releases`.
6. Waits for one job, with a six-hour job timeout.
7. Powers off the VM and destroys its overlay.
8. Removes stale runner registrations.
9. Starts a clean cycle for the next job.

No GitHub credentials are stored in the base image or persisted between jobs.

## GitHub Authentication

The controller uses the organization-owned `NortekMed SRV2 Public Runner`
GitHub App. Its only granted permission is:

```text
Organization self-hosted runners: read and write
```

The App private key is root-owned on SRV2. Do not copy it into the VM, this
repository, shell history, logs, or support messages. No classic or fine-grained
personal access token is used.

## Security Constraints

GitNortek is public. Keep these constraints in place:

- Allow public repositories explicitly in the dedicated runner group only.
- Add repositories to `Public-Releases` individually.
- Do not run fork pull-request workflows on this runner.
- Keep the active `Protect package tags` ruleset, which allows only repository
  administrators to create, update, or delete `v*` tags.
- Keep actions pinned to commit SHAs and containers pinned to digests.
- Keep default workflow token permissions read-only.
- Never attach the public runner VM to the LAN bridge.
- Never add private project keys, credentials, mounts, or network access.

Docker access inside the disposable guest is root-equivalent in that guest, but
does not provide access to the SRV2 Docker daemon or host filesystem.

## Operations

Inspect the controller and current VM:

```bash
ssh -t SRV2-master 'sudo systemctl status github-public-runner.service --no-pager'
ssh -t SRV2-master 'sudo journalctl -u github-public-runner.service --since today --no-pager'
ssh -t SRV2-master 'sudo virsh -c qemu:///system dominfo gha-public-01'
```

Stop new public jobs and clean up the current disposable VM:

```bash
ssh -t SRV2-master 'sudo systemctl stop github-public-runner.service'
```

Start the public runner controller:

```bash
ssh -t SRV2-master 'sudo systemctl start github-public-runner.service'
```

Do not restart the persistent TorizonBuoy runner when servicing this runner.

## Remaining Validation

1. Push the workflow change without creating a package tag.
2. Test a prerelease tag, which exercises RPM and release creation without the
   Windows package.
3. Confirm the first VM overlay is destroyed and a new runner registers for the
   release job.
4. Test a stable tag to verify RPM, Windows EXE, artifact transfer, and release
   publishing together.
