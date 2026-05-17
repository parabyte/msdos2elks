#!/usr/bin/env bash
# Convert every DOS COM/EXE file in a directory tree.
#
# Successful conversions are written directly to the output directory.  Logs
# are kept separately so a failed title can be inspected without polluting the
# converted program set.  The unpack helper is preferred when present because a
# large fraction of DOS-era executables are LZEXE, PKLITE, EXEPACK, or SFX
# wrapped and must be revealed before static rewriting is meaningful.

set -euo pipefail

script_dir=$(cd "$(dirname "$0")/.." && pwd)
converter=${MSDOS2ELKS:-$script_dir/msdos2elks}
unpack_helper=$script_dir/unpack-and-convert.sh

usage ()
{
  cat <<EOF
Usage: ${0##*/} INPUT_DIR OUTPUT_DIR [MSDOS2ELKS-OPTIONS...]

Examples:
  tools/convert-directory.sh ~/dos-games converted
  tools/convert-directory.sh ~/dos-games converted --partial

Environment:
  MSDOS2ELKS=/path/to/msdos2elks        override converter path
  MSDOS2ELKS_NO_UNPACK=1                skip unpack-and-convert.sh
  MSDOS2ELKS_TMP_ROOT=/path             unpack scratch directory root
EOF
}

if [ "${1:-}" = "--help" ] || [ "$#" -lt 2 ]; then
  usage
  exit 2
fi

input_dir=$1
output_dir=$2
shift 2

if [ ! -d "$input_dir" ]; then
  printf 'convert-directory: input is not a directory: %s\n' "$input_dir" >&2
  exit 1
fi

if [ ! -x "$converter" ]; then
  printf 'convert-directory: converter is not executable: %s\n' "$converter" >&2
  exit 1
fi

mkdir -p "$output_dir" "$output_dir/logs"
list=$(mktemp "${TMPDIR:-/tmp}/msdos2elks-list.XXXXXX")
trap 'rm -f "$list"' EXIT HUP INT TERM

find "$input_dir" -type f \( -iname '*.com' -o -iname '*.exe' \) \
  -print | sort > "$list"

safe_stem ()
{
  stem=$1
  stem=$(printf '%s' "$stem" | sed 's/[^A-Za-z0-9._-]/_/g')
  if [ -z "$stem" ]; then
    stem=program
  fi
  printf '%s' "$stem"
}

next_output ()
{
  base=$1
  out=$base
  n=1

  while [ -e "$out" ]; do
    out=$base-$n
    n=$((n + 1))
  done

  printf '%s' "$out"
}

total=0
ok=0
failed=0

while IFS= read -r input; do
  [ -n "$input" ] || continue
  total=$((total + 1))

  base=${input##*/}
  stem=${base%.*}
  safe=$(safe_stem "$stem")
  out=$(next_output "$output_dir/$safe")
  log=$output_dir/logs/$safe.log

  printf 'convert-directory: %s -> %s\n' "$input" "$out"

  if [ "${MSDOS2ELKS_NO_UNPACK:-0}" != 1 ] && [ -x "$unpack_helper" ]; then
    if "$unpack_helper" "$@" "$input" "$out" > "$log" 2>&1; then
      ok=$((ok + 1))
      continue
    fi
  else
    if "$converter" "$@" "$input" "$out" > "$log" 2>&1; then
      ok=$((ok + 1))
      continue
    fi
  fi

  rm -f "$out"
  failed=$((failed + 1))
  printf 'convert-directory: failed, see %s\n' "$log" >&2
done < "$list"

printf 'convert-directory: %u scanned, %u converted, %u failed\n' \
       "$total" "$ok" "$failed"

if [ "$failed" -ne 0 ] || [ "$ok" -eq 0 ]; then
  exit 1
fi
