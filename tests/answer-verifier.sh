#!/usr/bin/env bash
# Real Celix bundle registration and deterministic positive/negative checks.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$(realpath "$(test $# -eq 0 && printf '%s' "$root/build" || printf '%s' "$1")")"
cli="$build/tronix_verify_once"
verifier="$build/tronix_answer_verifier.zip"
demo="$build/tronix_demo.zip"
test -x "$cli" && test -f "$verifier" && test -f "$demo"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
printf '42\n' > "$tmp/answer"
printf 'wrong\n' > "$tmp/wrong"
: > "$tmp/empty"
ln -s "$tmp/answer" "$tmp/link"
mkdir "$tmp/parent"
ln -s "$tmp/parent" "$tmp/parentlink"
cp "$tmp/answer" "$tmp/parent/answer"
python3 - "$tmp/oversized" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[1]).write_bytes(b'x' * 65537)
PY
sha() { sha256sum "$1" | cut -d ' ' -f1; }
hash="$(sha "$tmp/answer")"
verifier_pin="$(sha "$verifier")"
demo_pin="$(sha "$demo")"
bad_expected="z$(printf '%s' "$hash" | cut -c2-)"
pin_prefix=0
if [ "$(printf '%s' "$verifier_pin" | cut -c1)" = 0 ]; then pin_prefix=1; fi
bad_pin="$pin_prefix$(printf '%s' "$verifier_pin" | cut -c2-)"
case_count=0
case_run() {
  local label="$1" wanted="$2" code="$3" input="$4" expected="$5" bundle="$6" pin="$7"
  local output actual
  set +e
  output="$("$cli" --task-id "$label" --input "$input" \
      --expected-sha256 "$expected" --bundle "$bundle" --bundle-sha256 "$pin" 2>"$tmp/stderr")"
  actual=$?
  set -e
  python3 - "$label" "$wanted" "$code" "$actual" "$output" <<'PY'
import json,sys
label,wanted,code,actual,payload=sys.argv[1:]
assert int(code)==int(actual),(label,"exit",code,actual)
doc=json.loads(payload)
assert doc["protocol"]=="tronix.answer.verify/v1"
assert doc["taskId"]==label
assert doc["status"]==wanted,(label,doc)
assert doc["objectiveTruthEstablished"] is False
assert doc["published"] is False
assert doc["scope"]=="precommitted-sha256-byte-equality-only"
assert doc["elapsedNs"] >= 0
PY
  case_count=$((case_count+1))
  echo "PASS $label ($wanted)"
}
case_run match VERIFIED 0 "$tmp/answer" "$hash" "$verifier" "$verifier_pin"
case_run mismatch REJECTED 1 "$tmp/wrong" "$hash" "$verifier" "$verifier_pin"
case_run empty VERIFIED 0 "$tmp/empty" "$(sha "$tmp/empty")" "$verifier" "$verifier_pin"
case_run badExpected UNVERIFIABLE 3 "$tmp/answer" "$bad_expected" "$verifier" "$verifier_pin"
case_run oversized UNVERIFIABLE 3 "$tmp/oversized" "$hash" "$verifier" "$verifier_pin"
case_run missing UNVERIFIABLE 3 "$tmp/missing" "$hash" "$verifier" "$verifier_pin"
case_run symlink UNVERIFIABLE 3 "$tmp/link" "$hash" "$verifier" "$verifier_pin"
case_run parentlink UNVERIFIABLE 3 "$tmp/parentlink/answer" "$hash" "$verifier" "$verifier_pin"
case_run bundleMismatch UNVERIFIABLE 3 "$tmp/answer" "$hash" "$verifier" "$bad_pin"
case_run noVerifier UNVERIFIABLE 3 "$tmp/answer" "$hash" "$demo" "$demo_pin"
echo "$case_count native Celix verifier checks passed"
