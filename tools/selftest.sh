#!/bin/sh
# Minimal host-side self-test for msdos2elks.
#
# The test creates a tiny DOS COM program and a tiny DOS MZ program:
#   mov ah,09h; mov dx,msg; int 21h
#   mov ax,4c00h; int 21h
#   msg db 'Hi$'
#
# The converter should recognize the DOS console write and process exit calls
# and produce a non-empty ELKS executable.  This does not boot ELKS; runtime
# validation should be done on the target ELKS system.

set -eu

converter=${1:-./msdos2elks}
tmp=${TMPDIR:-/tmp}/msdos2elks-check.$$

rm -rf "$tmp"
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

input=$tmp/hello.com
output=$tmp/hello.elks
mz_input=$tmp/tiny.exe
mz_output=$tmp/tiny.ne
log=$tmp/converter.log

printf '\264\011\272\014\001\315\041\270\000\114\315\041Hi$' > "$input"
printf '\115\132\045\000\001\000\000\000\002\000\000\000\377\377\000\000\000\001\000\000\000\000\000\000\034\000\000\000\000\000\000\000\270\000\114\315\041' > "$mz_input"

if ! "$converter" --verbose "$input" "$output" > "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if [ ! -s "$output" ]; then
  printf 'selftest: converter wrote no output\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$mz_input" "$mz_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

mz_magic=$(od -An -tx1 -N2 "$mz_output" | tr -d ' \n')
ne_magic=$(dd if="$mz_output" bs=1 skip=64 count=2 2>/dev/null \
           | od -An -tx1 | tr -d ' \n')
if [ "$mz_magic" != 4d5a ] || [ "$ne_magic" != 4e45 ]; then
  printf 'selftest: MZ input did not produce OS/2 NE output by default\n' >&2
  cat "$log" >&2
  exit 1
fi

printf 'selftest: converted tiny COM to %s bytes and tiny MZ to OS/2 NE\n' \
       "$(wc -c < "$output")"
