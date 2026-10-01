#!/usr/bin/env bash
# Benennt die SD-Karte der Uhr um (Standard: "MELDEZAHLER").
#
#   ./sd_benennen.sh            -> Label auf MELDEZAHLER setzen
#   ./sd_benennen.sh MEINNAME   -> Label auf MEINNAME setzen (max. 11 Zeichen)
#
# Benötigt sudo (fragt nach dem Passwort).
set -euo pipefail

LABEL="${1:-MELDEZAHLER}"

# SD-Karte finden: per blkid-Label (handhabt Labels mit Leerzeichen korrekt).
# Es wird zuerst das alte Label "AC Family", danach ggf. "MELDEZAHLER" probiert.
DEV=""
if command -v blkid >/dev/null 2>&1; then
  DEV="$(blkid -L 'AC Family' 2>/dev/null || true)"
  [ -z "$DEV" ] && DEV="$(blkid -L 'MELDEZAHLER' 2>/dev/null || true)"
fi

# Fallback: erste vfat-Partition auf Wechseldatenträger (sdb, sdc, ...)
if [ -z "$DEV" ]; then
  DEV="$(lsblk -n -r -o NAME,TYPE | awk '$2=="part" && $1 ~ /^sd[b-z]/ {print "/dev/"$1; exit}')"
fi

if [ -z "$DEV" ] || [ ! -b "$DEV" ]; then
  echo "SD-Karte nicht gefunden (steckt sie im Kartenleser?)."
  exit 1
fi

echo "SD-Karte: $DEV"
echo "Neues Label: $LABEL"

udisksctl unmount -b "$DEV" >/dev/null 2>&1 || true
sudo fatlabel "$DEV" "$LABEL"
udisksctl mount -b "$DEV" >/dev/null 2>&1 || true

echo "Fertig. Label = $LABEL"
