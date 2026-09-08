#!/usr/bin/env bash

set -euo pipefail

downloads_dir="${DARTPLANT_TRANSLATED_DOWNLOADS_DIR:-build/ci/downloads}"
output_dir="${DARTPLANT_TRANSLATED_OUTPUT_DIR:-build/ci/translated}"

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
  apk="$(dirname "$manifest")/app-release.apk"

  if ! python3 scripts/ci/run_flutter_device.py \
    --apk "$apk" \
    --test normal \
    --runtime-tier translated-smoke \
    --log "$output_dir/logcat/$family.log" \
    --metadata "$output_dir/runner/$family.json" \
    --timeout 120; then
    status=1
    continue
  fi

  if ! python3 scripts/ci/analyze_logcat.py \
    --log "$output_dir/logcat/$family.log" \
    --metadata "$output_dir/runner/$family.json" \
    --flutter "$flutter" \
    --dart "$dart" \
    --test normal \
    --runtime-tier translated-smoke \
    --report-json "$output_dir/reports/$family.json" \
    --summary-md "$output_dir/reports/$family.md" \
    --junit "$output_dir/reports/$family.xml"; then
    status=1
  fi
done

exit "$status"
