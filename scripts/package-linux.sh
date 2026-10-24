#!/usr/bin/env bash
set -euo pipefail
umask 077

if [[ $(uname -s) != Linux ]]; then
  echo "error: scripts/package-linux.sh supports Linux hosts only" >&2
  exit 2
fi

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary=""
fake_provider=""
output_dir=""
binary_supplied=false
require_release_qualified=false
release_build=""

usage() {
  cat <<'EOF'
Usage: scripts/package-linux.sh [--build-dir ABS | --binary ABS] [--fake-provider ABS] [--output-dir ABS] [--require-release-qualified]

Without --build-dir or --binary, configure a fresh private Release build and build ava plus the fake-provider.
With --build-dir, reuse a sealed release-candidate build, with no configure or build command.
With --binary, snapshot that executable once and stage it only when its exact version matches this checkout.
A supplied --fake-provider is likewise snapshotted once before model smoke.
--require-release-qualified enforces static source/gitlink/license/version/native-architecture/dynamic-dependency/package gates; --build-dir additionally enforces the inspected Linux x64 floor and sealed build identity. CI evidence remains required.
If --output-dir is omitted, a new private unpredictable directory is created.
Python 3 is required for provenance, packaging-time link verification, and secure publication.
EOF
}

require_absolute() {
  local option=$1
  local value=$2
  if [[ $value != /* ]]; then
    echo "error: $option requires an absolute path: $value" >&2
    exit 2
  fi
}

while (($# > 0)); do
  case "$1" in
    --build-dir|--binary|--fake-provider|--output-dir)
      if (($# < 2)); then
        echo "error: $1 requires a value" >&2
        usage >&2
        exit 2
      fi
      require_absolute "$1" "$2"
      case "$1" in
        --build-dir) release_build=$2 ;;
        --binary) binary=$2; binary_supplied=true ;;
        --fake-provider) fake_provider=$2 ;;
        --output-dir) output_dir=$2 ;;
      esac
      shift 2
      ;;
    --require-release-qualified)
      require_release_qualified=true
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -n $release_build ]]; then
  if [[ $binary_supplied == true || -n $fake_provider ]]; then
    echo "error: --build-dir cannot be combined with --binary or --fake-provider" >&2
    exit 2
  fi
  python3 "$repo_root/scripts/release-candidate.py" verify-build --repo "$repo_root" --build-dir "$release_build"
  binary="$release_build/ava"
  fake_provider="$release_build/tests/ava_fake_provider_server"
fi

if [[ $require_release_qualified == true && $binary_supplied == true ]]; then
  echo "error: --require-release-qualified rejects supplied-binary mode" >&2
  exit 2
fi

required_commands=(python3 tar sha256sum)
if [[ $binary_supplied == false || -n $fake_provider ]]; then
  required_commands+=(cmake)
fi
for command in "${required_commands[@]}"; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "error: required packaging command is unavailable: $command" >&2
    exit 2
  fi
done

repository_containment() {
  python3 "$repo_root/scripts/publish-linux-artifacts.py" --repository-containment "$repo_root" "$1"
}

choose_temp_base() {
  local candidate classification
  for candidate in "${TMPDIR:-}" /var/tmp /tmp; do
    [[ -n $candidate && -d $candidate && -w $candidate ]] || continue
    classification=$(repository_containment "$candidate") || return 1
    if [[ $classification == outside ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

temp_base=$(choose_temp_base) || {
  echo "error: no writable temporary directory outside the repository" >&2
  exit 1
}

if [[ -z $output_dir ]]; then
  output_dir=$(mktemp -d --tmpdir="$temp_base" ava-release-output.XXXXXXXX)
  chmod 0700 -- "$output_dir"
else
  output_containment=$(repository_containment "$output_dir") || exit 1
  if [[ $output_containment == inside ]]; then
    echo "error: output directory must be outside the repository: $output_dir" >&2
    exit 2
  fi
  if [[ ! -e $output_dir && ! -L $output_dir ]]; then
    mkdir -m 0700 -- "$output_dir"
  fi
fi
output_identity=$(python3 "$repo_root/scripts/publish-linux-artifacts.py" --check "$output_dir" --repository-root "$repo_root")

work_root=$(mktemp -d --tmpdir="$temp_base" ava-package-linux.XXXXXXXX)
chmod 0700 -- "$work_root"
work_containment=$(repository_containment "$work_root") || exit 1
if [[ $work_containment == inside ]]; then
  echo "error: private packaging work directory unexpectedly resolved inside the repository: $work_root" >&2
  exit 1
fi
cleanup() {
  rm -rf -- "$work_root"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ -z $binary ]]; then
  # A static package-qualification build must not inherit artifacts or configuration from a
  # developer tree. Keep the Release tree private with the package transaction.
  release_build="$work_root/release-build"
  release_preset=release
  if [[ $(uname -m) == x86_64 ]]; then
    release_preset=release-candidate
  fi
  cmake --preset "$release_preset" -S "$repo_root" -B "$release_build" \
    -DAVA_ENABLE_GITACHE=OFF \
    -DEnableLibcwd=OFF \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_nlohmann_json=ON
  cmake --build "$release_build" --parallel 6 --target ava ava_fake_provider_server
  binary="$release_build/ava"
  if [[ -z $fake_provider ]]; then
    fake_provider="$release_build/tests/ava_fake_provider_server"
  fi
fi

input_root="$work_root/inputs"
mkdir -m 0700 -- "$input_root"
python3 "$repo_root/scripts/publish-linux-artifacts.py" \
  --snapshot-executable "$binary" "$input_root/ava"
binary="$input_root/ava"
if [[ -n $fake_provider ]]; then
  python3 "$repo_root/scripts/publish-linux-artifacts.py" \
    --snapshot-executable "$fake_provider" "$input_root/fake-provider"
  fake_provider="$input_root/fake-provider"
fi
if [[ -f $release_build/release-build.json ]]; then
  python3 "$repo_root/scripts/inspect-linux-artifact.py" "$binary"
fi

version_output=$("$binary" --version)
if [[ $version_output =~ ^ava\ ([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
  version=${BASH_REMATCH[1]}
else
  echo "error: expected exact version output 'ava X.Y.Z', got: $version_output" >&2
  exit 1
fi
project_version=$(python3 - "$repo_root/CMakeLists.txt" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(r"project\s*\(\s*ava\b.*?\bVERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\b", text, re.DOTALL | re.IGNORECASE)
if not match:
    raise SystemExit("unable to parse top-level project(ava VERSION X.Y.Z)")
print(match.group(1))
PY
)
if [[ $version != "$project_version" ]]; then
  echo "error: binary version ava $version does not match current checkout project(ava VERSION $project_version)" >&2
  exit 1
fi

host_machine=$(uname -m)
host_machine=$(printf '%s' "$host_machine" | tr '[:upper:]' '[:lower:]')
case "$host_machine" in
  x86_64|amd64) host_architecture=x86_64 ;;
  aarch64|arm64) host_architecture=aarch64 ;;
  *)
    host_architecture=$(printf '%s' "$host_machine" | sed 's/[^a-z0-9._-]/-/g')
    if [[ -z $host_architecture ]]; then
      echo "error: unable to normalize host architecture" >&2
      exit 1
    fi
    if [[ $host_architecture != [a-z0-9]* ]]; then
      host_architecture="unknown-$host_architecture"
    fi
    ;;
esac

neutral_stage="$work_root/neutral-stage"
stage="$neutral_stage"
doc_root="$stage/share/doc/ava"
doc_sources=(
  docs/core/usage.md
  docs/core/configuration.md
  docs/core/context-resources.md
  docs/core/custom-providers.md
  docs/core/environment-variables.md
  docs/core/providers.md
  docs/core/subagents.md
  docs/core/thinking-modes.md
  docs/core/tools.md
  docs/interfaces/themes-keybindings.md
  docs/operations/testing.md
  docs/operations/terminal-setup.md
  docs/operations/troubleshooting.md
  docs/operations/diagnostics.md
  docs/operations/release-checklist.md
  docs/extensions/lsp.md
  docs/extensions/mcp.md
  docs/extensions/plugin-system.md
  docs/security/sandboxing.md
  docs/security/containment.md
  docs/development/session-versioning.md
  docs/development/side-effect-safety-checklist.md
  docs/headless-protocol.md
  docs/rpc-protocol.md
  docs/acp.md
  docs/acp-support.json
  docs/session-format.md
  docs/plugin-compatibility-policy.md
  docs/interop/evidence/README.md
  docs/interop/evidence/zed-1.9.0-2026-07-14.md
  docs/product/mvp-coverage-ledger.md
  docs/schema/theme.schema.json
)

if [[ $binary_supplied == false ]]; then
  cmake --install "$release_build" --prefix "$stage" --component ava
else
  install -D -m 0644 -- "$repo_root/docs/operations/release-artifact-readme.md" "$doc_root/README.md"
  install -D -m 0644 -- "$repo_root/LICENSE" "$doc_root/LICENSE"
  install -D -m 0644 -- "$repo_root/THIRD_PARTY_NOTICES.md" "$doc_root/THIRD_PARTY_NOTICES.md"
  for source in "${doc_sources[@]}"; do
    install -D -m 0644 -- "$repo_root/$source" "$doc_root/$source"
  done
fi
# cmake --install reads the build tree, so always replace its executable with
# the exact private snapshot used for version validation and package smoke.
install -D -m 0755 -- "$binary" "$stage/bin/ava"
if [[ $binary_supplied == false ]]; then
  build_mode=source-build
else
  build_mode=supplied-binary
fi
provenance_command=(
  python3 "$repo_root/scripts/generate-release-provenance.py"
  --repo "$repo_root"
  --binary "$binary"
  --binary-version "$version"
  --build-mode "$build_mode"
  --host-architecture "$host_architecture"
  --output "$doc_root/PROVENANCE.json"
)
if [[ $require_release_qualified == true ]]; then
  provenance_command+=(--qualification-mode --require-release-qualified)
fi
"${provenance_command[@]}"

arch=$(python3 - "$doc_root/PROVENANCE.json" <<'PY'
import json
import pathlib
import re
import sys

try:
    provenance = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as error:
    raise SystemExit(f"error: unable to read staged provenance architecture: {error}")
architecture = provenance.get("architecture")
if not isinstance(architecture, str) or not architecture:
    raise SystemExit("error: staged provenance does not contain a detected ELF architecture")
labels = {"x86_64": "x64", "aarch64": "arm64"}
label = labels.get(architecture, architecture)
if re.fullmatch(r"[a-z0-9][a-z0-9._-]*", label) is None:
    raise SystemExit(f"error: staged provenance contains an unsafe ELF architecture: {architecture!r}")
print(label)
PY
)
package_name="ava-${version}-linux-${arch}"
mkdir -m 0700 -- "$work_root/stage"
stage="$work_root/stage/$package_name"
mv -- "$neutral_stage" "$stage"
doc_root="$stage/share/doc/ava"

expected_files=(bin/ava share/doc/ava/README.md share/doc/ava/LICENSE share/doc/ava/THIRD_PARTY_NOTICES.md share/doc/ava/PROVENANCE.json)
for source in "${doc_sources[@]}"; do
  expected_files+=("share/doc/ava/$source")
done
mapfile -t staged_files < <(cd -- "$stage" && find . \( -type f -o -type l \) -printf '%P\n' | LC_ALL=C sort)
mapfile -t sorted_expected < <(printf '%s\n' "${expected_files[@]}" | LC_ALL=C sort)
if [[ ${staged_files[*]} != "${sorted_expected[*]}" ]]; then
  printf 'error: staged package contents differ from the AVA allowlist\n' >&2
  printf 'staged:\n%s\n' "${staged_files[*]}" >&2
  printf 'expected:\n%s\n' "${sorted_expected[*]}" >&2
  exit 1
fi
if [[ $require_release_qualified == true ]]; then
  echo "static package/provenance qualification: passed (full release qualification requires the release ledger)"
else
  echo "static package/provenance qualification: see share/doc/ava/PROVENANCE.json (accepted-binary artifacts are unqualified)"
fi
if find "$stage" -type l -print -quit | grep -q .; then
  echo "error: staged package contains a symlink" >&2
  exit 1
fi
python3 "$repo_root/scripts/verify-markdown-links.py" "$doc_root"

archive_name="$package_name.tar.gz"
checksum_name="$archive_name.sha256"
artifact_work="$work_root/artifacts"
mkdir -p -- "$artifact_work"
tar -C "$work_root/stage" -czf "$artifact_work/$archive_name" "$package_name"
(
  cd -- "$artifact_work"
  sha256sum "$archive_name" > "$checksum_name"
  sha256sum -c "$checksum_name"
)

extract_root="$work_root/extracted"
mkdir -p -- "$extract_root"
tar -C "$extract_root" -xzf "$artifact_work/$archive_name"
extracted="$extract_root/$package_name"
[[ -x $extracted/bin/ava ]]
smoke_home="$work_root/smoke-home"
smoke_config="$work_root/smoke-config"
smoke_state="$work_root/smoke-state"
mkdir -p -- "$smoke_home" "$smoke_config" "$smoke_state"
run_smoke() {
  HOME=$smoke_home XDG_CONFIG_HOME=$smoke_config XDG_STATE_HOME=$smoke_state "$@"
}
[[ $(run_smoke "$extracted/bin/ava" --version) == "ava $version" ]]
help_output=$(run_smoke "$extracted/bin/ava" --help)
grep -q 'Usage' <<<"$help_output"
if [[ -f $release_build/release-build.json ]]; then
  doctor_output=$(run_smoke "$extracted/bin/ava" doctor)
  [[ -n $doctor_output ]]
fi
packages_output=$(run_smoke "$extracted/bin/ava" packages list)
grep -qi 'deferred' <<<"$packages_output"

if [[ -n $fake_provider && -x $fake_provider ]]; then
  cmake \
    -DAVA_EXE="$extracted/bin/ava" \
    -DAVA_FAKE_PROVIDER_EXE="$fake_provider" \
    -DAVA_CLI_TEST_ROOT="$work_root/model-smoke" \
    -P "$repo_root/tests/cli_headless_e2e_model_smoke.cmake"
  echo "package model smoke: passed"
else
  echo "package model smoke: skipped (no executable --fake-provider supplied)"
fi

if [[ -f $release_build/release-build.json ]]; then
  python3 "$repo_root/scripts/release-candidate.py" verify-build --repo "$repo_root" --build-dir "$release_build"
fi

# Publish the checksum first and archive last; the archive is the pair's commit marker.
python3 "$repo_root/scripts/publish-linux-artifacts.py" \
  --repository-root "$repo_root" \
  --output "$output_dir" \
  --expected-directory-identity "$output_identity" \
  --file "$artifact_work/$checksum_name" "$checksum_name" \
  --file "$artifact_work/$archive_name" "$archive_name"

printf 'artifact: %s\n' "$output_dir/$archive_name"
printf 'checksum: %s\n' "$output_dir/$checksum_name"
