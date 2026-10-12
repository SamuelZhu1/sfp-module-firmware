#!/usr/bin/env bash
# Flash the test build and run the HIL suite on the board.
#   scripts/run_bench.sh /dev/ttyACM0 [--interactive] [pytest args...]
set -euo pipefail
cd "$(dirname "$0")/.."
PORT=${1:?serial port, e.g. /dev/ttyACM0}; shift
idf.py -B build_test -DSDKCONFIG=build_test/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.test" -p "$PORT" flash
python3 -m pytest tests_hil --port "$PORT" "$@"
