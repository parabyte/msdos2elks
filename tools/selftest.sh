#!/bin/sh
# Minimal host-side self-test for msdos2elks.
#
# The test creates a tiny DOS COM program and a tiny DOS MZ program:
#   mov ah,09h; mov dx,msg; int 21h
#   mov ax,4c00h; int 21h
#   msg db 'Hi$'
#
# The converter should recognize the DOS console write and process exit calls
# and produce a non-empty ELKS executable.  The MZ path is checked through
# explicit OS/2 NE output.  This does not boot ELKS; runtime validation should
# be done on the target ELKS system.

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
stack_input=$tmp/stack.exe
stack_output=$tmp/stack.ne
subseg_input=$tmp/subseg.exe
subseg_output=$tmp/subseg.ne
video_input=$tmp/video.com
video_output=$tmp/video.elks
ega_input=$tmp/ega.com
ega_output=$tmp/ega.elks
mda_input=$tmp/mda.com
mda_output=$tmp/mda.elks
vga_input=$tmp/vga.com
vga_output=$tmp/vga.elks
ega_query_input=$tmp/egaquery.com
ega_query_output=$tmp/egaquery.elks
svga_probe_input=$tmp/svgaprobe.com
svga_probe_output=$tmp/svgaprobe.elks
display_combo_input=$tmp/displaycombo.com
display_combo_output=$tmp/displaycombo.elks
log=$tmp/converter.log

printf '\264\011\272\014\001\315\041\270\000\114\315\041Hi$' > "$input"
printf '\115\132\045\000\001\000\000\000\002\000\000\000\377\377\000\000\000\001\000\000\000\000\000\000\034\000\000\000\000\000\000\000\270\000\114\315\041' > "$mz_input"
printf '\115\132\061\000\001\000\000\000\002\000\000\000\377\377\000\000\000\001\000\000\000\000\000\000\034\000\000\000\000\000\000\000\270\000\000\273\000\001\372\216\320\213\343\373\270\000\114\315\041' > "$stack_input"
printf '\115\132\102\000\001\000\001\000\002\000\000\000\377\377\000\000\000\001\000\000\000\000\000\000\034\000\000\000\003\000\000\000\016\037\270\001\000\216\300\046\241\020\000\270\000\114\315\041\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\064\022' > "$subseg_input"
printf '\270\004\000\315\020\267\001\060\333\264\013\315\020\270\000\114\315\041' > "$video_input"
printf '\270\020\000\315\020\270\000\114\315\041' > "$ega_input"
printf '\270\007\000\315\020\270\000\114\315\041' > "$mda_input"
printf '\270\023\000\315\020\270\000\114\315\041' > "$vga_input"
printf '\263\020\264\022\315\020\270\000\114\315\041' > "$ega_query_input"
printf '\264\060\315\020\270\000\114\315\041' > "$svga_probe_input"
printf '\270\000\032\315\020\270\000\114\315\041' > "$display_combo_input"

if ! "$converter" --verbose "$input" "$output" > "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if [ ! -s "$output" ]; then
  printf 'selftest: converter wrote no output\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose --mz-output=os2 "$mz_input" "$mz_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

mz_magic=$(od -An -tx1 -N2 "$mz_output" | tr -d ' \n')
ne_magic=$(dd if="$mz_output" bs=1 skip=64 count=2 2>/dev/null \
           | od -An -tx1 | tr -d ' \n')
if [ "$mz_magic" != 4d5a ] || [ "$ne_magic" != 4e45 ]; then
  printf 'selftest: MZ input did not produce explicit OS/2 NE output\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose --mz-data-seg=2 \
     "$stack_input" "$stack_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if od -v -An -tx1 "$stack_output" | tr -d ' \n' | grep -q 'fa8ed08be3fb'; then
  printf 'selftest: MZ OS/2 NE output kept a raw DOS stack switch\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose --mz-output=os2 --mz-data-seg=4 \
     "$subseg_input" "$subseg_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if ! od -v -An -tx1 "$subseg_output" | tr -d ' \n' | grep -q '26a12000'; then
  printf 'selftest: MZ OS/2 NE output did not rebase ES subsegment direct reference\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$video_input" "$video_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$ega_input" "$ega_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$mda_input" "$mda_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$vga_input" "$vga_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$ega_query_input" "$ega_query_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$svga_probe_input" "$svga_probe_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$display_combo_input" "$display_combo_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if ! grep -q 'direct-video=1' "$log"; then
  printf 'selftest: graphics video conversion did not claim direct-video output\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! od -v -An -tx1 "$vga_output" | tr -d ' \n' | grep -q 'b400cd10'; then
  printf 'selftest: VGA mode stub did not restore AH=00h\n' >&2
  cat "$log" >&2
  exit 1
fi

ega_query_hex=$(od -v -An -tx1 "$ega_query_output" | tr -d ' \n')
if ! printf '%s' "$ega_query_hex" | grep -q 'b310e8....90.*f8c3'; then
  printf 'selftest: EGA alternate-select probe did not get conservative success stub\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! od -v -An -tx1 "$svga_probe_output" | tr -d ' \n' | grep -q '31c031c931d2f8c3'; then
  printf 'selftest: enhanced video info probe did not return conservative zero result\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! od -v -An -tx1 "$display_combo_output" | tr -d ' \n' | grep -q '31c031dbf8c3'; then
  printf 'selftest: display-combination probe did not return conservative no-VGA result\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! od -v -An -tx1 "$video_output" | tr -d ' \n' | grep -q 'b400cd10'; then
  printf 'selftest: BIOS video mode stub did not restore AH=00h\n' >&2
  cat "$log" >&2
  exit 1
fi

video_hex=$(od -v -An -tx1 "$video_output" | tr -d ' \n')
for sig in 3c0472 b90144 b90344 b90244 b90444; do
  if ! printf '%s' "$video_hex" | grep -q "$sig"; then
    printf 'selftest: BIOS graphics console-lock signature %s missing\n' \
           "$sig" >&2
    cat "$log" >&2
    exit 1
  fi
done

if ! od -v -An -tx1 "$video_output" | tr -d ' \n' | grep -q 'b40bcd10'; then
  printf 'selftest: BIOS palette stub did not restore AH=0Bh\n' >&2
  cat "$log" >&2
  exit 1
fi

printf 'selftest: converted tiny COM to %s bytes and tiny MZ to OS/2 NE\n' \
       "$(wc -c < "$output")"
