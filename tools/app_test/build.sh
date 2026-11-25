#!/bin/sh
# Build app_test.elf. Copy it to the card as /sdcard/appdata/app_test/app_test.elf (and it is embedded in the elf test component as a fallback).
set -e
cd "$(dirname "$0")"
python "$IDF_PATH/tools/idf.py" -G 'Unix Makefiles' set-target esp32p4
python "$IDF_PATH/tools/idf.py" elf
cp build/app_test.app.elf app_test.elf
cp app_test.elf ../../components/phone_elf_test/app_test.elf
mkdir -p ../../sdcard/appdata/app_test && cp app_test.elf ../../sdcard/appdata/app_test/app_test.elf
ls -l app_test.elf
