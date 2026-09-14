#!/bin/sh
# 两棵树都从当前源码重建 + 全量回归 + DLL 一致性核对。
cd /d/public/libuvcpp/libuvcpp || exit 9

for T in build-webapp build-ssl; do
  echo "############ $T"
  cmake -S . -B "$T" > /tmp/${T}_cfg.log 2>&1
  echo "  cfg rc=$?"
  cmake --build "$T" --config Release --parallel 4 > /tmp/${T}_build.log 2>&1
  echo "  build rc=$?"
  n=$(grep -cE "error C[0-9]+|error LNK|error MSB" /tmp/${T}_build.log)
  echo "  build errors: $n"
  if [ "$n" != "0" ]; then
    grep -E "error C[0-9]+|error LNK|error MSB" /tmp/${T}_build.log | head -10
  fi
  cmake --build "$T" --config Release --target copy_test_dlls > /tmp/${T}_copy.log 2>&1
  lib=$(md5sum "$T/Release/uvcpp.dll" | cut -d' ' -f1)
  echo "  lib md5: $lib"
  bad=0
  for d in "$T/tests/unit/Release" "$T/tests/functional/Release" "$T/tests/expand/Release"; do
    if [ -f "$d/uvcpp.dll" ]; then
      m=$(md5sum "$d/uvcpp.dll" | cut -d' ' -f1)
      [ "$m" = "$lib" ] || { echo "  MISMATCH $d ($m)"; bad=1; }
    fi
  done
  [ "$bad" = "0" ] && echo "  all test DLLs match"
  ctest --test-dir "$T" -C Release --timeout 60 > /tmp/${T}_ctest.log 2>&1
  echo "  ctest rc=$?"
  tail -5 /tmp/${T}_ctest.log
done
