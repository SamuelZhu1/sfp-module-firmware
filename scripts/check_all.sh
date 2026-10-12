#!/usr/bin/env bash
# Everything CI runs that doesn't need the ESP-IDF container.
set -euo pipefail
cd "$(dirname "$0")/.."
make -C components/sfp/test_host test
make -C components/sfp/test_host sim
python3 -m pytest tests_hil -q
cppcheck --enable=warning,performance,portability --error-exitcode=1 --inline-suppr \
  --std=c11 -DSFP_TEST_HOOKS -I components/sfp/include -I components/sfp/src \
  -I components/shell/include components/sfp/src components/shell
