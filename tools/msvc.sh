#!/usr/bin/env bash
# msvc.sh — run a Windows command line inside the MSVC x64 developer environment
# from WSL, with the Windows-side working directory set to a WSL directory.
#
# Usage:
#   ./msvc.sh -C <wsl-dir> '<windows command line>'
#   ./msvc.sh -C . 'cl /nologo /W4 hello.c'
#
# Notes:
#  * vcvars64.bat sets ~86 env vars (PATH, INCLUDE, LIB, WindowsSDKVersion, ...);
#    they are imported into the PowerShell session, then cmd.exe runs the command.
#  * cmd.exe cannot use a UNC path as its CWD, so we use `pushd <unc>` to get a
#    temporary drive mapping. Nothing is persisted after the process exits.
set -uo pipefail

workdir=""
while getopts ":C:" opt; do
  case $opt in
    C) workdir="$OPTARG" ;;
    *) echo "usage: $0 [-C wsl-dir] '<cmdline>'" >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
cmdline="$*"
[ -n "$cmdline" ] || { echo "usage: $0 [-C wsl-dir] '<cmdline>'" >&2; exit 2; }

prefix=""
if [ -n "$workdir" ]; then
  abs=$(readlink -f "$workdir") || exit 1
  # /home/me/x  ->  \\wsl.localhost\Ubuntu\home\me\x
  unc="\\\\wsl.localhost\\${WSL_DISTRO_NAME:-Ubuntu}${abs//\//\\}"
  prefix="pushd ${unc} && "
fi

# WSL only forwards variables listed in WSLENV to Windows child processes.
export MSVC_CMDLINE="${prefix}${cmdline}"
export WSLENV="${WSLENV:+${WSLENV}:}MSVC_CMDLINE"

exec powershell.exe -NoProfile -Command '
  $ErrorActionPreference = "Stop"
  $vsbat = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
  # Keep the PowerShell CWD on a real drive: cmd.exe cannot inherit a UNC CWD.
  Push-Location C:\
  $lines = & cmd.exe /c "call `"$vsbat`" >nul && set"
  foreach ($l in $lines) {
    if ($l -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
  }
  & cmd.exe /c $env:MSVC_CMDLINE
  exit $LASTEXITCODE
'
