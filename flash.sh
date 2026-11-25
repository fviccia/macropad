#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:-$(arduino-cli board list | awk '/usbmodem/ {print $1; exit}')}"

if [[ -z "$PORT" ]]; then
  echo "No se encontró el puerto serie del Arduino Pro Micro (/dev/cu.usbmodem*)."
  exit 1
fi

echo "Compilando y flasheando en $PORT..."
arduino-cli compile --fqbn arduino:avr:leonardo "$DIR"
arduino-cli upload -p "$PORT" --fqbn arduino:avr:leonardo "$DIR"
echo "Listo."
