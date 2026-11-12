#!/usr/bin/env bash
# Check the plugin's contract with the chain host (state size, restore,
# per-instance files, worker thread policy) against the built dsp.so.
#
# Runs in the build image: on an arm64 host (Apple Silicon) its "cross"
# compiler makes binaries the container can run. Needs build/dsp.so
# (./scripts/build.sh) and dist/surge/surge-data.
#
#   ./scripts/test_plugin.sh [extra contract_test.py args, e.g. --ref build/other_dsp.so]
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-surge-builder"

if [ ! -f "/.dockerenv" ]; then
    # SYS_NICE + rtprio: the test calls the plugin from a SCHED_FIFO 70 thread,
    # as the host's audio callback does, so the worker's inheritance is real
    exec docker run --rm --cap-add SYS_NICE --ulimit rtprio=99 \
        -v "$REPO_ROOT:/build" -w /build "$IMAGE_NAME" ./scripts/test_plugin.sh "$@"
fi

cd "$REPO_ROOT"
[ -f build/dsp.so ] || { echo "build/dsp.so missing: run ./scripts/build.sh"; exit 1; }
[ -d dist/surge/surge-data ] || { echo "dist/surge/surge-data missing: run ./scripts/build.sh"; exit 1; }
CXX="${CROSS_PREFIX}g++"
$CXX -O2 -std=c++17 -o /tmp/plugin_ops tests/plugin/plugin_ops.cpp -ldl -lpthread
python3 tests/plugin/contract_test.py --ops /tmp/plugin_ops --so build/dsp.so --data dist/surge/surge-data "$@"
