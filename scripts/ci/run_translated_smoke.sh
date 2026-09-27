#!/usr/bin/env bash

set -euo pipefail

downloads_dir="${DARTPLANT_TRANSLATED_DOWNLOADS_DIR:-build/ci/downloads}"
output_dir="${DARTPLANT_TRANSLATED_OUTPUT_DIR:-build/ci/translated}"
profile_rounds="${DARTPLANT_TRANSLATED_PROFILE_ROUNDS:-3}"

if ! [[ "$profile_rounds" =~ ^[1-9][0-9]*$ ]]; then
  echo "DARTPLANT_TRANSLATED_PROFILE_ROUNDS must be a positive integer" >&2
  exit 2
fi

mkdir -p "$output_dir/logcat" "$output_dir/runner" "$output_dir/reports"

mapfile -t manifests < <(find "$downloads_dir" -name manifest.json -type f | sort)
if (( ${#manifests[@]} == 0 )); then
  echo "no Flutter family manifests found under $downloads_dir" >&2
  exit 2
fi

status=0
for manifest in "${manifests[@]}"; do
  family="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["family"])' "$manifest")"
  flutter="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["flutter"])' "$manifest")"
  dart="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["dart"])' "$manifest")"
  mode="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["mode"])' "$manifest")"
  apk="$(dirname "$manifest")/app-$mode.apk"
  deferred_apk="$(dirname "$manifest")/deferred_probe-$mode.apk"

  # Profile AOT can map libapp.so and the native-translation runtime at
  # different addresses on each cold process start. Exercise callback/RET
  # veneer allocation more than once so a lucky ASLR layout cannot conceal
  # an intermittent near-branch allocation failure.
  rounds=1
  if [[ "$mode" == "profile" ]]; then rounds="$profile_rounds"; fi
  for (( round=1; round<=rounds; ++round )); do
    key="$family-$mode"
    if (( round > 1 )); then key="$key-round$round"; fi
    echo "translated smoke: $family $mode round $round/$rounds"
    if ! python3 scripts/ci/run_flutter_device.py \
      --apk "$apk" \
      --deferred-apk "$deferred_apk" \
      --test normal \
      --runtime-tier translated-smoke \
      --log "$output_dir/logcat/$key.log" \
      --metadata "$output_dir/runner/$key.json" \
      --timeout 120; then
      status=1
      continue
    fi

    if ! python3 scripts/ci/analyze_logcat.py \
      --log "$output_dir/logcat/$key.log" \
      --metadata "$output_dir/runner/$key.json" \
      --flutter "$flutter" \
      --dart "$dart" \
      --test normal \
      --runtime-tier translated-smoke \
      --report-json "$output_dir/reports/$key.json" \
      --summary-md "$output_dir/reports/$key.md" \
      --junit "$output_dir/reports/$key.xml"; then
      status=1
    fi
  done
done

exit "$status"
