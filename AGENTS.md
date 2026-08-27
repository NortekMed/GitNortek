# AGENTS.md

## Repo Shape
- GitNortek is a C++17 Qt 6.6+ desktop Git client built with CMake; the main executable is `gitnortek` from `src/app/GitNortek.cpp`.
- Build outputs belong under ignored `build*` directories; do not edit vendored code under `dep/` unless the task explicitly targets a dependency.
- `src/app` wires the GUI app, and `src/index/Indexer` builds the `gitnortek-indexer` helper.

## Setup And Build
- Initialize dependencies before configuring: `git submodule update --init --recursive`.
- Standard local configure/build from repo root: `mkdir -p build/release && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build/release && ninja -C build/release`.
- If Qt is outside the default search path, pass `-DCMAKE_PREFIX_PATH=<qt-install-prefix>`.
- CI configures with `-DUPDATE_TRANSLATIONS=ON -DGITNORTEK_CI_TESTS=ON`; keep this in mind when changing translations or tests that touch global settings.
- Default CMake uses bundled libgit2/cmark/lua/hunspell/git but system OpenSSL, libssh2, and Qt unless options such as `-DUSE_SYSTEM_LIBSSH2=OFF` are changed.

## Tests
- Full test target from an existing build: `ninja -C build/release check --verbose`.
- Focus one test with CTest: `ctest --test-dir build/release -R '^<test_name>$' --output-on-failure`.
- Test names come from `test/CMakeLists.txt` without the `test_` executable prefix, e.g. `Diff`, `log`, `SshConfig`.
- Tests run Qt headless via `QT_QPA_PLATFORM=offscreen`; Linux CI wraps the full check in `xvfb-run -a`.
- Before running tests that create commits, ensure Git has a usable identity, e.g. `git config --global user.name "Your Name"` and `git config --global user.email "youremail@yourdomain.com"`.

## Formatting
- Project formatting command: `./cl-fmt.sh`.
- `cl-fmt.sh` requires clang-format 13 and formats `src`, `test`, `l10n`, plus CMake files with `cmake-format==0.6.13`.
- Pre-commit only runs clang-format from `.pre-commit-config.yaml`; install it with `./setup-env.sh install-pre-commit` if needed.
- CI also checks CMake formatting with `cmake-format --check` while pruning nested dependency directories.

## Documentation And Changelog
- Keep `docs/changelog.md` updated for user-visible changes and maintainer-facing release/package changes.
- Add new entries under the top `vX.X.X - ... (DEV)` section, using `#### Added` or `#### Changed` as appropriate.
- Update the changelog when changing packaging, CI artifacts, package/version naming, installer behavior, update flow, or documented commands.
- Skip changelog entries for purely internal refactors only when they do not affect users, packages, CI artifacts, or maintainers.

## Packaging CI
- `.github/workflows/package.yml` builds release artifacts for the NortekMed fork: Fedora RPM via `-DGITNORTEK_LINUX_PACKAGE_GENERATOR=RPM` and Windows NSIS `.exe` via CPack.
- Linux package generation defaults to `TGZ`; pass `-DGITNORTEK_LINUX_PACKAGE_GENERATOR=RPM` when an RPM is required.
- Package versions are resolved in CMake from Git tags using `git describe`; local and CI builds should not duplicate version parsing.
- RPM `Version` uses the app version with `-` converted to `_`, and RPM `Release` uses the UTC package build timestamp `YYYYMMDD.HHmm`.
