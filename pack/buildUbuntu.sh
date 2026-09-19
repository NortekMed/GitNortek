#!/bin/bash

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"

sudo apt update
sudo apt install -y build-essential libgl1-mesa-dev cmake libgit2-dev cmark git \
                    libssh2-1-dev openssl qtbase5-dev qtchooser qt5-qmake qtbase5-dev-tools qttools5-dev ninja-build
cd "$REPO_ROOT" || exit
git fetch
git submodule init
git submodule update
git pull
git checkout deps
cd "$REPO_ROOT/dep/openssl/openssl" || exit
./config -fPIC
make
cd "$REPO_ROOT" || exit
mkdir -vp "$REPO_ROOT/build/release"
cd "$REPO_ROOT/build/release" || exit
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ../..
ninja
