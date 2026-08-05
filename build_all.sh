#!/bin/bash
# Komplettes Build-Skript: Baut alles, führt es zusammen und legt es im bin/ Ordner ab

echo "============================================"
echo "1. Bringe das Dateisystem (SPIFFS) auf den neuesten Stand"
echo "============================================"
pio run -e app -t buildfs

echo "============================================"
echo "2. Kompiliere Updater-Firmware"
echo "============================================"
pio run -e updater

echo "============================================"
echo "3. Kompiliere App-Firmware"
echo "============================================"
pio run -e app

echo "============================================"
echo "4. Erstelle Factory-Image (Merge)"
echo "============================================"
mkdir -p bin
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32 merge_bin \
    -o bin/merged_firmware.bin \
    --flash_mode dio --flash_freq 40m --flash_size 4MB \
    0x1000 .pio/build/updater/bootloader.bin \
    0x8000 .pio/build/updater/partitions.bin \
    0xe000 ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
    0x10000 .pio/build/updater/firmware.bin \
    0x110000 .pio/build/app/firmware.bin \
    0x310000 .pio/build/app/spiffs.bin

echo "============================================"
echo "FERTIG! Alle Dateien liegen im Ordner 'bin/':"
echo "- bin/updater_firmware.bin (Start: 0x10000)"
echo "- bin/app_firmware.bin     (Start: 0x110000)"
echo "- bin/merged_firmware.bin  (Start: 0x0000)"
echo "============================================"
