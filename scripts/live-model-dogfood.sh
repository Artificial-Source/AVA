#!/bin/sh
set -u
umask 077

enabled="${AVA_LIVE_PROVIDER_SMOKE:-}"
if [ "$enabled" != "1" ] && [ "$enabled" != "true" ] && [ "$enabled" != "TRUE" ] && [ "$enabled" != "yes" ] && [ "$enabled" != "on" ]; then
  echo "classification=skipped/not opted in"
  echo "set AVA_LIVE_PROVIDER_SMOKE=1 and one provider credential to run the full-binary live dogfood"
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
init_live_smoke "live dogfood" "ava-live-dogfood" "${AVA_LIVE_DOGFOOD_ROOT:-}" \
  AVA_LIVE_DOGFOOD_KEEP "${AVA_LIVE_DOGFOOD_KEEP:-}"

mkdir -p "$workspace/src" "$home_dir" "$config_dir/ava" "$state_dir" "$data_dir" ||
  live_smoke_error "cannot create workspace"
marker="ava-live-dogfood-marker"
printf '%s\n' "# Live Dogfood" "" "marker: $marker" > "$workspace/src/live-smoke.txt" ||
  live_smoke_error "cannot write fixture"
printf '%s\n' "live dogfood context" > "$workspace/AGENTS.md" ||
  live_smoke_error "cannot write context"
provider_json=$(json_escape "$provider") || live_smoke_error "cannot encode provider"
model_json=$(json_escape "$model") || live_smoke_error "cannot encode model"
cat > "$config_dir/ava/models.json" <<EOF || live_smoke_error "cannot write model config"
{"default_provider":"$provider_json","default_model":"$model_json","models":[{"provider":"$provider_json","id":"$model_json","family":"live-dogfood","context_window_tokens":8192,"max_output_tokens":512,"supports_tools":true,"supports_streaming":false,"supports_reasoning":false,"reports_usage":true}]}
EOF

start_live_rpc

reply_unexpected_permissions()
{
  if [ ! -s "$rpc_out" ]; then return 0; fi
  sed -n 's/.*"resolver_request_id":"\([^"]*\)".*/\1/p' "$rpc_out" | while IFS= read -r resolver_id; do
    [ -n "$resolver_id" ] || continue
    if grep -qx "$resolver_id" "$replied_ids" 2>/dev/null; then continue; fi
    printf '%s\n' "{\"id\":\"reply-$resolver_id\",\"type\":\"permission_reply\",\"request_id\":\"$resolver_id\",\"correlation_id\":\"prompt\",\"decision\":\"deny\",\"reason\":\"live dogfood only permits read-only tool use\"}" >&3
    echo "$resolver_id" >> "$replied_ids"
  done
}

printf '%s\n' "{\"id\":\"prompt\",\"type\":\"prompt\",\"protocol_version\":1,\"message\":\"Use the read_file tool to read src/live-smoke.txt, then reply with a short confirmation that includes the marker text. Do not use shell or edit tools.\"}" >&3

i=0
while ! grep -q '"final_text":"' "$rpc_out" 2>/dev/null; do
  reply_unexpected_permissions
  if ! kill -0 "$ava_pid" 2>/dev/null; then
    cat "$rpc_out" "$rpc_err" > "$combined_error" 2>/dev/null || true
    classify_failure "$combined_error"
    echo "$label live dogfood exited before final assistant response"
    cat "$rpc_err" >&2 2>/dev/null || true
    exit 1
  fi
  i=$((i + 1))
  if [ "$i" -gt 2400 ]; then
    cat "$rpc_out" "$rpc_err" > "$combined_error" 2>/dev/null || true
    classify_failure "$combined_error"
    echo "$label live dogfood timed out waiting for final assistant response"
    exit 1
  fi
  sleep 0.05
done

printf '%s\n' '{"id":"stats-after","type":"get_session_stats"}' >&3
printf '%s\n' '{"id":"validate-after","type":"validate_session"}' >&3
printf '%s\n' '{"id":"messages-after","type":"get_messages"}' >&3
finish_live_rpc

if [ "$ava_status" -ne 0 ]; then
  cat "$rpc_out" "$rpc_err" > "$combined_error" 2>/dev/null || true
  classify_failure "$combined_error"
  echo "$label live dogfood ava --rpc exited with $ava_status"
  cat "$rpc_err" >&2 2>/dev/null || true
  exit "$ava_status"
fi

if ! grep -q '"id":"validate-after".*"ok":true' "$rpc_out"; then
  echo "classification=AVA regression"
  echo "session validation did not report ok=true"
  exit 1
fi

if ! grep -q '"final_text":"[^"]' "$rpc_out"; then
  echo "classification=AVA regression"
  echo "final assistant response was empty"
  exit 1
fi

if ! grep -q '"name":"tool_start".*"tool":"read_file"' "$rpc_out" || ! grep -q '"name":"tool_result".*"tool":"read_file".*"status":"success"' "$rpc_out"; then
  echo "classification=provider-behavior/inconclusive"
  echo "$label returned a final response, but did not complete the requested read_file tool call"
  print_evidence_note
  exit 2
fi

if ! grep -q "$marker" "$rpc_out"; then
  echo "classification=provider-behavior/inconclusive"
  echo "$label used the read_file tool but did not include the marker in the final response"
  print_evidence_note
  exit 2
fi

echo "classification=passed"
echo "provider=$provider model=$model"
print_evidence_note
