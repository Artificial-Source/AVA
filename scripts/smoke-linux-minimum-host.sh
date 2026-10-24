#!/usr/bin/env bash
# Exercise the unchanged archive on Ubuntu 24.04 with a Nehalem (x86-64-v2) CPU.
set -euo pipefail
if [[ $# != 2 || $1 != /* || $2 != /* ]]; then
  echo "usage: $0 ABS_CANDIDATE_DIRECTORY ABS_FAKE_PROVIDER" >&2
  exit 2
fi
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
engine=${AVA_CONTAINER_ENGINE:-docker}
"$engine" run --rm --platform linux/amd64 \
  -v "$1:/candidate:ro" -v "$2:/fake-provider:ro" -v "$repo_root:/source:ro" \
  ubuntu:24.04 bash -euo pipefail -c '
    apt-get update
    apt-get install -y libstdc++6 libncursesw6 libtinfo6 ncurses-base curl cmake python3 qemu-user
    cd /candidate
    sha256sum -c ./*.sha256
    mkdir -m 700 /smoke /smoke/home /smoke/config /smoke/state
    export HOME=/smoke/home XDG_CONFIG_HOME=/smoke/config XDG_STATE_HOME=/smoke/state
    tar -xzf ./*.tar.gz -C /smoke
    binaries=(/smoke/ava-*/bin/ava)
    [[ ${#binaries[@]} == 1 ]]
    binary=${binaries[0]}
    "$binary" --version
    "$binary" --help
    "$binary" doctor
    qemu-x86_64 -cpu Nehalem "$binary" --version
    qemu-x86_64 -cpu Nehalem "$binary" --help
    qemu-x86_64 -cpu Nehalem "$binary" doctor
    printf "#!/bin/sh\nexec qemu-x86_64 -cpu Nehalem %s \"\$@\"\n" "$binary" > /smoke/ava
    printf "#!/bin/sh\nexec qemu-x86_64 -cpu Nehalem /fake-provider \"\$@\"\n" > /smoke/provider
    chmod 700 /smoke/ava /smoke/provider
    cmake -DAVA_EXE=/smoke/ava -DAVA_FAKE_PROVIDER_EXE=/smoke/provider \
      -DAVA_CLI_TEST_ROOT=/smoke/model -P /source/tests/cli_headless_e2e_model_smoke.cmake
    echo "minimum-host smoke: native and Nehalem version/help/doctor, Nehalem fake-provider PASS"
  '
