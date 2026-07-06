#!/usr/bin/env bash
# Generate and convert 100 tiny XT-era DOS smoke programs.
#
# The programs are intentionally small plain DOS inputs.  They are not
# compressed, archived, self-extracting, or title-specific game binaries.
# Each case exercises a statically recognizable interrupt surface that a real
# early DOS utility, installer, text tool, setup program, or simple game might
# use.  The suite only includes surfaces that strict conversion can route
# through ELKS syscalls/ioctls or deterministic non-hardware stubs.  BIOS
# graphics, raw video memory, and dynamic video helpers belong in the negative
# graphics smoke.  This host test proves rewriting and output generation; it
# does not replace booting the converted programs inside ELKS on the target
# machine.

set -euo pipefail

converter=${1:-./msdos2elks}
tmp=${TMPDIR:-/tmp}/msdos2elks-xt-smoke.$$
exit_dos=bb0000b8004ccd21

rm -rf "$tmp"
mkdir -p "$tmp/in" "$tmp/out" "$tmp/log"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if [ ! -x "$converter" ]; then
  printf 'xt-smoke: converter is not executable: %s\n' "$converter" >&2
  exit 1
fi

write_hex ()
{
  hex=$1
  out=$2

  perl -e 'print pack ("H*", $ARGV[0])' "$hex" > "$out"
}

run_case ()
{
  name=$1
  hex=$2
  input=$tmp/in/$name.com
  output=$tmp/out/$name
  log=$tmp/log/$name.log

  write_hex "$hex" "$input"
  if ! "$converter" --verbose "$input" "$output" > "$log" 2>&1; then
    printf 'xt-smoke: conversion failed for %s\n' "$name" >&2
    cat "$log" >&2
    exit 1
  fi
  if [ ! -s "$output" ]; then
    printf 'xt-smoke: converter wrote empty output for %s\n' "$name" >&2
    cat "$log" >&2
    exit 1
  fi
}

count=0

add_case ()
{
  count=$((count + 1))
  run_case "$(printf 'xt%03u_%s' "$count" "$1")" "$2"
}

for fn in \
  00 01 02 06 07 08 09 0b 0c 0d 0e 0f 10 11 12 13 14 15 \
  19 1a 1b 1c 21 22 23 25 27 28 29 2a 2b 2c 2d 2e 2f 30 \
  33 35 36 39 3a 3b 3c 3d 3e 3f 40 41 42 43 44 47 48 49 \
  4a 4c 4d 4e 4f 54 56 57 62
do
  add_case "dos21_${fn}" "b4${fn}cd21${exit_dos}"
done

for ax in 0000 0001 0002 0003 0007 0080 0081 0082 0083 0087
do
  add_case "bios10_textmode_${ax}" "b8${ax:2:2}${ax:0:2}cd10${exit_dos}"
done

for fn in 0e 0f 12 1a 30
do
  add_case "bios10_ah_${fn}" "b4${fn}cd10${exit_dos}"
done

for ch in 41 42 43 44 45 46 47 48 49 4a 4b
do
  add_case "bios10_teletype_${ch}" "b0${ch}b40ecd10${exit_dos}"
done

for fn in 00 01 02 10 11 12
do
  add_case "bios16_${fn}" "b4${fn}cd16${exit_dos}"
done

for fn in 00
do
  add_case "bios1a_${fn}" "b4${fn}cd1a${exit_dos}"
done

for fn in 00 01 02 03
do
  add_case "mouse33_${fn}" "b4${fn}cd33${exit_dos}"
done

if [ "$count" -ne 100 ]; then
  printf 'xt-smoke: internal case count is %u, expected 100\n' "$count" >&2
  exit 1
fi

printf 'xt-smoke: converted %u XT-era interrupt-surface programs\n' "$count"
