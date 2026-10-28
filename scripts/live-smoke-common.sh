#!/bin/sh
# Sourced by the model/coding launchers; keep their opt-in and permission policy local.

json_escape()
{
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

classify_failure()
{
  text=$(tr '[:upper:]' '[:lower:]' < "$1")
  case "$text" in
    *401*|*403*|*auth*|*credential*|*api_key*|*api-key*|*unauthorized*|*forbidden*)
      echo "classification=credential/auth-blocked" ;;
    *429*|*rate*limit*|*quota*) echo "classification=provider/rate-limited" ;;
    *timeout*|*timed\ out*|*could\ not\ resolve*|*connection*|*network*|*curl*)
      echo "classification=network-blocked" ;;
    *) echo "classification=AVA regression" ;;
  esac
}

private_parent_error()
{
  echo "$live_smoke_name private parent rejected: $1" >&2
  exit 1
}

private_parent_metadata()
{
  if metadata=$(stat -c '%u %a' "$1" 2>/dev/null); then
    printf '%s\n' "$metadata"
    return 0
  fi
  if metadata=$(stat -f '%u %Lp' "$1" 2>/dev/null); then
    printf '%s\n' "$metadata"
    return 0
  fi
  return 1
}

validate_private_parent()
{
  parent=$1
  case "$parent" in
    /*) ;;
    *) private_parent_error "path must be absolute" ;;
  esac
  while [ "$parent" != "/" ] && [ "${parent%/}" != "$parent" ]; do
    parent=${parent%/}
  done
  [ -d "$parent" ] || private_parent_error "path must be an existing directory"
  [ ! -L "$parent" ] || private_parent_error "path must not be a symlink"
  parent_physical=$(CDPATH= cd -P "$parent" 2>/dev/null && pwd -P) ||
    private_parent_error "directory cannot be resolved"
  [ "$parent_physical" != "/" ] || private_parent_error "filesystem root is not allowed"
  if [ -n "${HOME:-}" ] && [ -d "$HOME" ]; then
    home_physical=$(CDPATH= cd -P "$HOME" 2>/dev/null && pwd -P) || home_physical=
    [ "$parent_physical" != "$home_physical" ] || private_parent_error "HOME itself is not allowed"
  fi
  checkout_physical=$(CDPATH= cd -P "$script_dir/.." 2>/dev/null && pwd -P) ||
    private_parent_error "checkout cannot be resolved"
  case "$parent_physical" in
    "$checkout_physical"|"$checkout_physical"/*)
      private_parent_error "the checkout and directories inside it are not allowed" ;;
  esac
  metadata=$(private_parent_metadata "$parent_physical") ||
    private_parent_error "directory ownership and mode cannot be inspected"
  set -- $metadata
  [ "$#" -eq 2 ] || private_parent_error "directory metadata is invalid"
  [ "$1" = "$(id -u)" ] || private_parent_error "directory must be owned by the current effective user"
  case "$2" in
    700|0700) ;;
    *) private_parent_error "directory mode must be exactly 0700" ;;
  esac
  printf '%s\n' "$parent_physical"
}

live_smoke_error()
{
  echo "$live_smoke_name setup/shutdown failed: $1" >&2
  exit 1
}

stop_live_rpc()
{
  exec 3>&-
  if [ -n "$ava_pid" ]; then
    # setsid owns a separate process group, including provider/tool descendants.
    # The PID fallback also covers failure before setsid established the group.
    kill -TERM "-$ava_pid" 2>/dev/null || kill -TERM "$ava_pid" 2>/dev/null || true
    shutdown_ticks=0
    while kill -0 "-$ava_pid" 2>/dev/null || kill -0 "$ava_pid" 2>/dev/null; do
      shutdown_ticks=$((shutdown_ticks + 1))
      [ "$shutdown_ticks" -lt 40 ] || break
      sleep 0.05
    done
    kill -KILL "-$ava_pid" 2>/dev/null || true
    kill -KILL "$ava_pid" 2>/dev/null || true
    wait "$ava_pid" 2>/dev/null || true
    ava_pid=
  fi
}

cleanup_live_smoke()
{
  # Ignore repeated signals while settling the owned child; preserve EXIT status.
  trap '' INT TERM HUP
  stop_live_rpc
  if [ -z "$live_smoke_keep" ]; then rm -rf -- "$root"; fi
}

print_evidence_note()
{
  if [ -n "$live_smoke_keep" ]; then
    if [ -z "$evidence_note_printed" ]; then
      echo "evidence_root=$root"
      evidence_note_printed=1
    fi
  else
    echo "set $live_smoke_keep_variable=1 to retain RPC logs"
  fi
}

init_live_smoke()
{
  live_smoke_name=$1
  live_smoke_prefix=$2
  live_smoke_parent=$3
  live_smoke_keep_variable=$4
  live_smoke_keep=$5
  if [ -n "$live_smoke_parent" ]; then
    private_parent=$(validate_private_parent "$live_smoke_parent") || exit 1
  else
    private_parent=${TMPDIR:-/tmp}
  fi
  root=$(mktemp -d "$private_parent/$live_smoke_prefix.XXXXXX") ||
    private_parent_error "could not allocate an evidence directory"
  ava_pid=
  trap cleanup_live_smoke EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  trap 'exit 129' HUP
  chmod 700 "$root" || private_parent_error "could not make the evidence directory private"
  workspace="$root/workspace"
  home_dir="$root/home"
  config_dir="$root/config"
  state_dir="$root/state"
  data_dir="$root/data"
  rpc_in="$root/rpc-input.fifo"
  rpc_out="$root/rpc-output.jsonl"
  rpc_err="$root/rpc-error.log"
  replied_ids="$root/replied-permissions.txt"
  combined_error="$root/combined-error.log"
  evidence_note_printed=
  if [ -n "$live_smoke_keep" ]; then print_evidence_note; fi
}

start_live_rpc()
{
  command -v setsid >/dev/null 2>&1 || live_smoke_error "Linux setsid is required"
  touch "$replied_ids" "$rpc_out" "$rpc_err" || live_smoke_error "cannot create RPC logs"
  mkfifo "$rpc_in" || live_smoke_error "cannot create RPC FIFO"
  # Linux FIFO O_RDWR avoids a blocking writer-open if the reader fails setup.
  # The child closes its inherited writer, so closing fd 3 still delivers EOF.
  exec 3<>"$rpc_in" || live_smoke_error "cannot open RPC FIFO"
  (cd "$workspace" || exit 1
   exec setsid sh -c 'printf ready > "$1" || exit 1; shift; exec "$@"' sh "$root/rpc-ready" \
     env HOME="$home_dir" XDG_CONFIG_HOME="$config_dir" XDG_STATE_HOME="$state_dir" \
     XDG_DATA_HOME="$data_dir" NO_COLOR=1 "$AVA_EXE" --rpc --allow read-only
  ) < "$rpc_in" > "$rpc_out" 2> "$rpc_err" 3>&- &
  ava_pid=$!
  startup_ticks=0
  while [ ! -s "$root/rpc-ready" ]; do
    kill -0 "$ava_pid" 2>/dev/null || live_smoke_error "RPC launcher exited during setup"
    startup_ticks=$((startup_ticks + 1))
    [ "$startup_ticks" -lt 100 ] || live_smoke_error "RPC launcher setup timed out"
    sleep 0.05
  done
}

finish_live_rpc()
{
  exec 3>&-
  shutdown_ticks=0
  while kill -0 "$ava_pid" 2>/dev/null; do
    shutdown_ticks=$((shutdown_ticks + 1))
    [ "$shutdown_ticks" -lt 100 ] || live_smoke_error "RPC process did not exit after EOF"
    sleep 0.05
  done
  if wait "$ava_pid"; then ava_status=0; else ava_status=$?; fi
  # Descendants can remain after their parent exits. Settle the group before
  # dropping its identity or deleting evidence, also on the successful path.
  stop_live_rpc
}
