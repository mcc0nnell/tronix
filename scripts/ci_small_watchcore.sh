#!/usr/bin/env bash
# Focused offline Watchcore verification; optional, explicit pinned Celix/Python parity.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
export PYTHONDONTWRITEBYTECODE=1

echo '=== TRONIX Watchcore ci-small: public source lock ==='
python3 scripts/verify_watchcore_source.py
echo '=== TRONIX Watchcore ci-small: source-lock negative tests ==='
python3 -m unittest discover -s tests -p 'test_watchcore_source_lock.py' -v
echo '=== TRONIX Watchcore ci-small: license check ==='
npm run license:check

if [[ -z "${WATCHCORE_CELIX_BUILD:-}" ]]; then
    if [[ -n "${WATCHCORE_RIZZER_ROOT:-}" ]]; then
        echo 'WATCHCORE_RIZZER_ROOT requires a matching WATCHCORE_CELIX_BUILD' >&2
        exit 2
    fi
    echo '=== TRONIX Watchcore ci-small: original C++20 assertions ==='
    watchcore_build="$(mktemp -d)"
    trap 'rm -rf -- "$watchcore_build"' EXIT
    cmake -S integrations/atlas-watchcore -B "$watchcore_build" -DBUILD_TESTING=ON
    cmake --build "$watchcore_build" --target watchcore_tests --parallel 2
    "$watchcore_build/watchcore_tests"
    ctest --test-dir "$watchcore_build" --output-on-failure
    echo 'Celix lifecycle and Python parity NOT RUN (WATCHCORE_CELIX_BUILD unset)'
    echo '=== TRONIX Watchcore ci-small: standalone success ==='
    exit 0
fi

[[ "$WATCHCORE_CELIX_BUILD" = /* && -d "$WATCHCORE_CELIX_BUILD" && ! -L "$WATCHCORE_CELIX_BUILD" ]] || {
    echo 'WATCHCORE_CELIX_BUILD must be an existing, non-symlink, absolute build directory' >&2
    exit 2
}
native_build="$(realpath -e -- "$WATCHCORE_CELIX_BUILD")"
[[ -f "$native_build/CMakeCache.txt" ]] || {
    echo 'No CMake cache in pinned build directory' >&2
    exit 2
}
grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$PWD" "$native_build/CMakeCache.txt" || {
    echo 'Celix build does not belong to this exact TRONIX checkout' >&2
    exit 2
}
echo '=== TRONIX Watchcore ci-small: existing pinned Celix build ==='
cmake --build "$native_build" --target tronix_watchcore_unit_tests \
    tronix_watchcore_security_bundle tronix_watchcore_shadow \
    tronix_watchcore_reference tronix_demo_bundle --parallel 2
"$native_build/tronix_watchcore_unit_tests"
host="$(realpath -e -- "$native_build/tronix_watchcore_shadow")"
reference="$(realpath -e -- "$native_build/tronix_watchcore_reference")"
bundle="$(realpath -e -- "$native_build/tronix_watchcore_security.zip")"
wrong="$(realpath -e -- "$native_build/tronix_demo.zip")"

echo '=== TRONIX Watchcore ci-small: Celix oracle and negative tests ==='
python3 tests/watchcore_celix_integration.py "$host" "$reference" "$bundle" "$wrong"

if [[ -n "${WATCHCORE_RIZZER_ROOT:-}" ]]; then
    [[ "$WATCHCORE_RIZZER_ROOT" = /* && -d "$WATCHCORE_RIZZER_ROOT" ]] || {
       echo 'WATCHCORE_RIZZER_ROOT must be an absolute, existing owner-approved checkout' >&2
       exit 2
    }
    [[ -f "$WATCHCORE_RIZZER_ROOT/integrations/atlas-watcher/src/atlas_watcher/security.py" ]] || {
       echo 'Python reference source missing; refusing fixture substitution' >&2
       exit 2
    }
    echo '=== TRONIX Watchcore ci-small: 1,200 actual Python/Celix decisions ==='
    python3 tests/watchcore_celix_ledger.py "$host" "$bundle" "$wrong" "$WATCHCORE_RIZZER_ROOT"
    python3 tests/watchcore_celix_routing.py "$host" "$bundle" "$wrong" "$WATCHCORE_RIZZER_ROOT"
    python3 tests/watchcore_python_shadow_parity.py "$WATCHCORE_RIZZER_ROOT" "$host" "$bundle"
    echo 'PASS: 1,200 cross-runtime comparisons (240 security + 260 ledger + 240 cooldown + 300 routing + 160 role readiness)'
else
    echo '=== TRONIX Watchcore ci-small: public Celix fixtures only ==='
    python3 tests/watchcore_celix_ledger.py "$host" "$bundle" "$wrong"
    python3 tests/watchcore_celix_routing.py "$host" "$bundle" "$wrong"
    echo 'Python parity NOT RUN (WATCHCORE_RIZZER_ROOT unset)'
fi
echo '=== TRONIX Watchcore ci-small: success ==='
