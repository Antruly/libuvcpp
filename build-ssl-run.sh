#!/bin/sh
# Rebuild build-ssl from current source and run its full suite.
# Prints a compact report; greps build output for real compile errors first
# (a failed build makes ctest run stale binaries and look green).
set -u
cd /d/public/libuvcpp/libuvcpp || exit 9

echo "### reconfigure"
cmake -S . -B build-ssl > /tmp/ssl_cfg.log 2>&1
echo "cfg rc=$?"

echo "### build"
cmake --build build-ssl --config Release --parallel 4 > /tmp/ssl_build.log 2>&1
echo "build rc=$?"
echo "--- error lines ---"
grep -E "error C[0-9]+|error LNK|error MSB" /tmp/ssl_build.log | head -20
echo "--- (end error lines) ---"

echo "### copy_test_dlls"
cmake --build build-ssl --config Release --target copy_test_dlls > /tmp/ssl_copy.log 2>&1
echo "copy rc=$?"

echo "### md5 check"
echo "lib      $(md5sum build-ssl/Release/uvcpp.dll | cut -d' ' -f1)"
for d in build-ssl/tests/unit/Release build-ssl/tests/functional/Release build-ssl/tests/expand/Release; do
  if [ -f "$d/uvcpp.dll" ]; then
    echo "$(md5sum "$d/uvcpp.dll" | cut -d' ' -f1)  $d"
  else
    echo "MISSING           $d"
  fi
done

echo "### ctest"
ctest --test-dir build-ssl -C Release --timeout 60 > /tmp/ssl_ctest.log 2>&1
echo "ctest rc=$?"
tail -15 /tmp/ssl_ctest.log
