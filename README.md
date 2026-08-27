GitNortek
==================================

GitNortek is a NortekMed-maintained fork of [Gittyup](https://github.com/Murmele/Gittyup), a graphical Git client designed to help you understand and manage your source code history.

This fork is maintained at [NortekMed/GitNortek](https://github.com/NortekMed/GitNortek) and carries NortekMed-specific naming, packaging, and integration changes.

GitNortek is based on Gittyup, which is a continuation of the [GitAhead](https://github.com/gitahead/gitahead) client.

![GitNortek](rsrc/screenshots/main_dark_orig.png)

Table of contents
=================
<!--ts-->
   * [Features](#features)
   * [Build Environment](#build-environment)
   * [Dependencies](#dependencies)
   * [How to Build](#how-to-build)
   * [How to Install](#how-to-install)
   * [How to Contribute](#how-to-contribute)
   * [License](#license)
<!--te-->

Features
---------------
GitNortek keeps the core Gittyup feature set and adds NortekMed-specific naming, packaging, and integration changes.

Build Environment
-----------------

* C++17 compiler
  * Windows - MSVC >= 2017 recommended
  * Linux - GCC >= 6.2 recommended
  * macOS - Xcode >= 10.1 recommended
* CMake >= 3.19
* Ninja (optional)

Dependencies
------------

External dependencies can be satisfied by system libraries or installed
separately. Included dependencies are submodules of this repository. Some
submodules are optional or may also be satisfied by system libraries.

**External Dependencies**

* Qt (required >= 6.6)

**Included Dependencies**

* libgit2 (required)
* cmark (required)
* git (only needed for the credential helpers)
* libssh2 (needed by `libgit2` for SSH support)
* openssl (needed by `libssh2` and `libgit2` on some platforms)

Note that building `OpenSSL` on Windows requires `Perl` and `NASM`.

How to Build
------------

**Initialize Submodules**

    git submodule init
    git submodule update --depth 1

**Build OpenSSL**

    # Start from root of GitNortek repo.
    cd dep/openssl/openssl

Windows:

    perl Configure VC-WIN64A
    nmake

macOS (Intel):

    ./Configure darwin64-x86_64-cc no-shared
    make
    
macOS (Apple Silicon)

    ./Configure darwin64-arm64-cc no-shared
    make
    
Linux:

    ./config -fPIC
    make

**Configure Build**

    # Start from root of GitNortek repo.
    mkdir -p build/release
    cd build/release
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ../..

If you have Qt installed in a non-standard location, you may have to
specify the path to Qt by passing `-DCMAKE_PREFIX_PATH=<path-to-qt>`
where `<path-to-qt>` points to the Qt install directory that contains
`bin`, `lib`, etc.

**Build**
```
    ninja
```
    
How to Install
-----------------
### Linux

Install a GitNortek package from the [NortekMed repository releases](https://github.com/NortekMed/GitNortek/releases) when available, or build GitNortek from source using the instructions above.

How to Contribute
-----------------

We welcome contributions of all kinds, including bug fixes, new features,
documentation and translations. By contributing, you agree to release
your contributions under the terms of the license.

Contribute by following the typical
[GitHub workflow](https://docs.github.com/en/get-started/quickstart/github-flow)
for pull requests. Fork the repository and make changes on a new named
branch. Create pull requests against the `master` branch. Follow the
[seven guidelines](https://chris.beams.io/posts/git-commit/) to writing a
great commit message.

Prior to committing a change, please use `cl-fmt.sh` to ensure your code
adheres to the formatting conventions for this project. You can also use the
`setup-env.sh` script to install a pre-commit hook which will automatically
run `clang-format` against all modified files.

Prior to pushing a change, please ensure you run the unit tests to avoid any
regressions. These are run using `ctest` in `<build-dir>`.

License
-------

GitNortek, Gittyup, and GitAhead are licensed under the MIT license. See LICENSE.md for details.
