#!/bin/sh
# Build app_test.elf and pack the sample packages into sdcard/packages/ (copy them to /packages on the card):
#   app_test.bpk   (id app_test, every permission)   app_test_b.bpk (id app_test_b, same ELF, only imu + battery: the other calls must be refused)
set -e
cd "$(dirname "$0")"
python "$IDF_PATH/tools/idf.py" -G 'Unix Makefiles' set-target esp32p4
python "$IDF_PATH/tools/idf.py" elf
cp build/app_test.app.elf package/app_test.elf
mkdir -p ../../sdcard/packages
python3 ../make_bpk.py package ../../sdcard/packages/app_test.bpk
B="$(mktemp -d)"
cp package/app_test.elf "$B/"
cp icon_b.png "$B/icon.png"
cp manifest_b.json "$B/manifest.json"
python3 ../make_bpk.py "$B" ../../sdcard/packages/app_test_b.bpk
rm -rf "$B"

# the SDK example (its own build, no IDF project) and a package of an old API that the phone must refuse
make -C ../../sdk APP=examples/hello >/dev/null
cp ../../sdk/out/hello.bpk ../../sdcard/packages/hello.bpk
O="$(mktemp -d)"
cp ../../sdk/build/hello.elf "$O/"
sed 's/"hello_sdk"/"hello_old"/; s/"api": 3/"api": 2/; s/"Hello"/"Hello old"/; s/"Xin chào"/"Cũ"/' ../../sdk/examples/hello/manifest.json > "$O/manifest.json"
python3 ../make_bpk.py "$O" ../../sdcard/packages/hello_old.bpk
rm -rf "$O"
