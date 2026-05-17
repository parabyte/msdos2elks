#!/usr/bin/env bash
# Print converter analysis for one input without keeping the generated output.
#
# This runs with --partial so unsupported sites are reported in context.  A
# partial output is useful for diagnostics only; release builds should use the
# strict default converter mode.

set -euo pipefail

script_dir=$(cd "$(dirname "$0")/.." && pwd)
converter=${MSDOS2ELKS:-$script_dir/msdos2elks}

usage ()
{
  cat <<EOF
Usage: ${0##*/} DOS_BINARY [MSDOS2ELKS-OPTIONS...]

Example:
  tools/explain-conversion.sh GAME.EXE --format=exe
EOF
}

if [ "${1:-}" = "--help" ] || [ "$#" -lt 1 ]; then
  usage
  exit 2
fi

input=$1
shift

tmp=$(mktemp -d "${TMPDIR:-/tmp}/msdos2elks-explain.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if [ ! -x "$converter" ]; then
  printf 'explain-conversion: converter is not executable: %s\n' "$converter" >&2
  exit 1
fi

printf 'explain-conversion: analyzing %s\n' "$input"
"$converter" --verbose --partial "$@" "$input" "$tmp/out"
