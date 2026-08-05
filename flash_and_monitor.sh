#!/bin/bash
# Sucht automatisch den USB-Port (z.B. CH340), flasht die App-Firmware und öffnet sofort den Serial Monitor (Baud 115200)

echo "============================================"
echo "Starte Build, Upload und automatischen Serial Monitor..."
echo "Zum Beenden des Monitors später 'Ctrl+C' drücken."
echo "============================================"

# pio run mit '-t upload' und '-t monitor' kombiniert alles in einem Befehl!
pio run -e app -t upload -t monitor
