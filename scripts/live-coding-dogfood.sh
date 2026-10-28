#!/bin/sh
set -u
umask 077

enabled="${AVA_LIVE_PROVIDER_SMOKE:-}"
if [ "$enabled" != "1" ] && [ "$enabled" != "true" ] && [ "$enabled" != "TRUE" ] && [ "$enabled" != "yes" ] && [ "$enabled" != "on" ]; then
  echo "classification=skipped/not opted in"
  echo "set AVA_LIVE_PROVIDER_SMOKE=1 and one provider credential to run the live coding dogfood"
  exit 77
fi

script_dir=$(CDPATH= cd "$(dirname "$0")" 2>/dev/null && pwd) || exit 1
. "$script_dir/live-provider-selection.sh" || exit 1
select_first_live_provider

if [ -z "$provider" ]; then
  echo "classification=skipped/no credential"
  echo "no supported provider credential environment variables are set"
  exit 77
fi

AVA_EXE="${AVA_EXE:-./build/ava}"
case "$AVA_EXE" in
  /*) ;;
  *) AVA_EXE="$PWD/$AVA_EXE" ;;
esac
if [ ! -x "$AVA_EXE" ]; then
  echo "classification=AVA regression"
  echo "ava executable is not runnable: $AVA_EXE"
  exit 1
fi

. "$script_dir/live-smoke-common.sh" || exit 1
init_live_smoke "live coding dogfood" "ava-live-coding-dogfood" "${AVA_LIVE_CODING_DOGFOOD_ROOT:-}" \
  AVA_LIVE_CODING_DOGFOOD_KEEP "${AVA_LIVE_CODING_DOGFOOD_KEEP:-}"
target_file="$workspace/src/task.txt"

mkdir -p "$workspace/src" "$workspace/.ava/skills/coding-smoke" "$home_dir" "$config_dir/ava" "$state_dir/ava" "$data_dir" ||
  live_smoke_error "cannot create workspace"
marker="AVA_LIVE_CODING_DONE_4172"
printf '%s\n' "# Live Coding Dogfood" "" "status: TODO" > "$target_file" ||
  live_smoke_error "cannot write fixture"
printf '%s\n' "live coding dogfood context" > "$workspace/AGENTS.md" ||
  live_smoke_error "cannot write context"
cat > "$workspace/.ava/skills/coding-smoke/SKILL.md" <<'EOF' || live_smoke_error "cannot write skill"
---
name: coding-smoke
description: Verify a coding edit by reading context, changing the requested file, and reporting only the marker.
---
Use this skill for live coding dogfood. Read the target file first, make the smallest exact edit requested by the user, and report only the requested marker.
EOF

workspace_json=$(json_escape "$workspace") || live_smoke_error "cannot encode workspace"
cat > "$state_dir/ava/project-trust.json" <<EOF || live_smoke_error "cannot write project trust"
{"schema_version":1,"decisions":[{"path":"$workspace_json","trusted":true}]}
EOF

provider_json=$(json_escape "$provider") || live_smoke_error "cannot encode provider"
model_json=$(json_escape "$model") || live_smoke_error "cannot encode model"
cat > "$config_dir/ava/models.json" <<EOF || live_smoke_error "cannot write model config"
{"default_provider":"$provider_json","default_model":"$model_json","models":[{"provider":"$provider_json","id":"$model_json","family":"live-coding-dogfood","context_window_tokens":8192,"max_output_tokens":2048,"supports_tools":true,"supports_streaming":false,"supports_reasoning":false,"reports_usage":true}]}
EOF

start_live_rpc

reply_permissions()
{
  if [ ! -s "$rpc_out" ]; then return 0; fi
  grep '"name":"permission_requested"' "$rpc_out" 2>/dev/null | while IFS= read -r line; do
    resolver_id=$(printf '%s\n' "$line" | sed -n 's/.*"resolver_request_id":"\([^"]*\)".*/\1/p')
    [ -n "$resolver_id" ] || continue
    if grep -qx "$resolver_id" "$replied_ids" 2>/dev/null; then continue; fi
    case "$line" in
      *'"tool_name":"skill"'*|*'"tool_name":"apply_patch"'*|*'"tool_name":"edit_file"'*|*'"tool_name":"write_file"'*)
        decision=allow
        reason="approved by live coding dogfood"
        ;;
      *)
        decision=deny
        reason="live coding dogfood only permits skill and file mutation tools"
        ;;
    esac
    printf '%s\n' "{\"id\":\"reply-$resolver_id\",\"type\":\"permission_reply\",\"request_id\":\"$resolver_id\",\"correlation_id\":\"prompt\",\"decision\":\"$decision\",\"reason\":\"$reason\"}" >&3
    echo "$resolver_id" >> "$replied_ids"
  done
}

printf '%s\n' "{\"id\":\"prompt\",\"type\":\"prompt\",\"protocol_version\":1,\"message\":\"Use the skill tool to load coding-smoke. Then use read_file to inspect src/task.txt. Then use apply_patch or edit_file to replace the line status: TODO with status: $marker. Do not use bash or network tools. Reply with exactly $marker and no other text.\"}" >&3

i=0
while ! grep -q '"final_text":"' "$rpc_out" 2>/dev/null; do
  reply_permissions
  if ! kill -0 "$ava_pid" 2>/dev/null; then
    cat "$rpc_out" "$rpc_err" > "$combined_error" 2>/dev/null || true
    classify_failure "$combined_error"
    echo "$label live coding dogfood exited before final assistant response"
    cat "$rpc_err" >&2 2>/dev/null || true
    print_evidence_note
    exit 1
  fi
  i=$((i + 1))
  if [ "$i" -gt 3600 ]; then
    cat "$rpc_out" "$rpc_err" > "$combined_error" 2>/dev/null || true
    classify_failure "$combined_error"
    echo "$label live coding dogfood timed out waiting for final assistant response"
    print_evidence_note
    exit 1
  fi
  sleep 0.05
done

reply_permissions
printf '%s\n' '{"id":"stats-after","type":"get_session_stats"}' >&3
printf '%s\n' '{"id":"validate-after","type":"validate_session"}' >&3
printf '%s\n' '{"id":"messages-after","type":"get_messages"}' >&3
finish_live_rpc

if [ "$ava_status" -ne 0 ]; then
  cat "$rpc_out" "$rpc_err" > "$combined_error" 2>/dev/null || true
  classify_failure "$combined_error"
  echo "$label live coding dogfood ava --rpc exited with $ava_status"
  cat "$rpc_err" >&2 2>/dev/null || true
  print_evidence_note
  exit "$ava_status"
fi

if ! grep -qx "status: $marker" "$target_file"; then
  echo "classification=provider-behavior/inconclusive"
  echo "$label returned but did not persist the requested edit"
  print_evidence_note
  exit 2
fi

for needle in \
  '"id":"validate-after"' \
  '"ok":true' \
  '"tool":"skill"' \
  '"tool":"read_file"' \
  '"status":"success"' \
  '"name":"permission_requested"' \
  '"name":"permission_replied"' \
  '"decision":"allow"' \
  '"tool_name":"skill"'; do
  if ! grep -q "$needle" "$rpc_out"; then
    echo "classification=provider-behavior/inconclusive"
    echo "$label live coding dogfood missing expected RPC evidence: $needle"
    print_evidence_note
    exit 2
  fi
done

if ! grep -q '"tool":"apply_patch"' "$rpc_out" && ! grep -q '"tool":"edit_file"' "$rpc_out"; then
  echo "classification=provider-behavior/inconclusive"
  echo "$label edited the file without expected apply_patch/edit_file RPC evidence"
  print_evidence_note
  exit 2
fi

if ! grep -q "\"final_text\":\"$marker\"" "$rpc_out"; then
  echo "classification=provider-behavior/inconclusive"
  echo "$label did not return the exact coding marker"
  print_evidence_note
  exit 2
fi

session_file=
for candidate in "$state_dir"/ava/sessions/*/*.jsonl "$state_dir"/ava/sessions/*.jsonl; do
  if [ -f "$candidate" ]; then
    session_file="$candidate"
    break
  fi
done

if [ -z "$session_file" ]; then
  echo "classification=AVA regression"
  echo "live coding dogfood did not persist a session file"
  print_evidence_note
  exit 1
fi

for needle in \
  '"type":"assistant_output_item"' \
  '"type":"assistant_turn_commit"' \
  '"kind":"function_call"' \
  '"type":"tool_result"' \
  '"assistant_output_entry_id"' \
  '"type":"permission_decision"' \
  '"tool_name":"skill"' \
  '"operation":"edit"' \
  '"resolution":"allow"' \
  "$marker"; do
  if ! grep -q "$needle" "$session_file"; then
    echo "classification=AVA regression"
    echo "persisted session missing expected coding evidence: $needle"
    print_evidence_note
    exit 1
  fi
done

if ! grep -q '"tool_name":"apply_patch"' "$session_file" && ! grep -q '"tool_name":"edit_file"' "$session_file"; then
  echo "classification=AVA regression"
  echo "persisted session missing expected edit tool permission evidence"
  print_evidence_note
  exit 1
fi

if grep -q '"type":"assistant_message"' "$session_file" || grep -q '"type":"tool_call"' "$session_file"; then
  echo "classification=AVA regression"
  echo "new live turn unexpectedly used legacy v3 assistant/tool-call records"
  print_evidence_note
  exit 1
fi

echo "classification=passed"
echo "provider=$provider model=$model"
print_evidence_note
