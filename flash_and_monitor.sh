#!/bin/bash
# Sucht automatisch den USB-Port (z.B. CH340), flasht die App-Firmware und öffnet sofort den Serial Monitor (Baud 115200)

echo "============================================"
echo "Starte Build, Upload und automatischen Serial Monitor..."
echo "Zum Beenden des Monitors später 'Ctrl+C' drücken."
echo "============================================"

# pio run: Schritt 1 = Dateisystem (MP3s), Schritt 2 = Firmware + Serial Monitor
pio run -e app -t uploadfs
sleep 2
pio run -e app -t upload -t monitor
