#!/usr/bin/env bash
# ps.sh — run a PowerShell script file from WSL.
#
# `powershell.exe -File` rejects UNC paths, and `-Command -` (stdin) misbehaves
# for multi-line scripts, so the script is passed as a UTF-16LE -EncodedCommand.
#
#   ./tools/ps.sh tests/sample-corners.ps1
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "usage: $0 <script.ps1>" >&2
  exit 2
fi

script="$1"
shift

if [ ! -f "$script" ]; then
  echo "no such script: $script" >&2
  exit 2
fi

encoded=$(iconv -f UTF-8 -t UTF-16LE "$script" | base64 -w0)
exec powershell.exe -NoProfile -ExecutionPolicy Bypass -EncodedCommand "$encoded"
