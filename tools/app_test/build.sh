#!/bin/sh
# Build app_test.elf and pack the sample packages into sdcard/packages/ (copy them to /packages on the card):
#   app_test.bpk   (id app_test)    app_test_b.bpk (id app_test_b, same ELF, other id / name / icon)
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
sed 's/"app_test"/"app_test_b"/; s/"App test"/"App test B"/; s/"Thử app"/"Thử app B"/' package/manifest.json > "$B/manifest.json"
python3 ../make_bpk.py "$B" ../../sdcard/packages/app_test_b.bpk
rm -rf "$B"
