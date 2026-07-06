#!/bin/sh
# Minimal host-side self-test for msdos2elks.
#
# The test creates a tiny DOS COM program and a tiny DOS MZ program:
#   mov ah,09h; mov dx,msg; int 21h
#   mov ax,4c00h; int 21h
#   msg db 'Hi$'
#
# The converter should recognize the DOS console write and process exit calls
# and produce a non-empty ELKS executable.  The default MZ path is checked as
# native ELKS a.out output, while the explicit OS/2 NE path is kept as a
# separate compatibility check.  This does not boot ELKS; runtime validation
# should be done on the target ELKS system.

set -eu

converter=${1:-./msdos2elks}
tmp=${TMPDIR:-/tmp}/msdos2elks-check.$$

rm -rf "$tmp"
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

aout_text_hex()
{
  hdr=$(od -An -tu1 -j4 -N1 "$1" | tr -d ' ')
  text_size=$(od -An -tu4 -j8 -N4 "$1" | tr -d ' ')
  dd if="$1" bs=1 skip="$hdr" count="$text_size" 2>/dev/null \
    | od -v -An -tx1 | tr -d ' \n'
}

input=$tmp/hello.com
output=$tmp/hello.elks
mz_input=$tmp/tiny.exe
mz_aout_output=$tmp/tiny.elks
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
teletype_input=$tmp/teletype.com
teletype_output=$tmp/teletype.elks
ega_query_input=$tmp/egaquery.com
ega_query_output=$tmp/egaquery.elks
svga_probe_input=$tmp/svgaprobe.com
svga_probe_output=$tmp/svgaprobe.elks
display_combo_input=$tmp/displaycombo.com
display_combo_output=$tmp/displaycombo.elks
system_config_input=$tmp/systemconfig.com
system_config_output=$tmp/systemconfig.elks
ioctl_input=$tmp/ioctl.com
ioctl_output=$tmp/ioctl.elks
find_input=$tmp/find.com
find_output=$tmp/find.elks
packed_com_input=$tmp/packed.com
packed_com_output=$tmp/packed.elks
raw_video_input=$tmp/rawvideo.com
raw_video_output=$tmp/rawvideo.elks
large_com_input=$tmp/large.com
large_com_output=$tmp/large.elks
msc_stack_input=$tmp/mscstack.com
msc_stack_output=$tmp/mscstack.elks
cs_stack_input=$tmp/csstack.com
cs_stack_output=$tmp/csstack.elks
log=$tmp/converter.log

printf '\264\011\272\014\001\315\041\270\000\114\315\041Hi$' > "$input"
printf '\115\132\045\000\001\000\000\000\002\000\000\000\377\377\000\000\000\001\000\000\000\000\000\000\034\000\000\000\000\000\000\000\270\000\114\315\041' > "$mz_input"
printf '\115\132\061\000\001\000\000\000\002\000\000\000\377\377\000\000\000\001\000\000\000\000\000\000\034\000\000\000\000\000\000\000\270\000\000\273\000\001\372\216\320\213\343\373\270\000\114\315\041' > "$stack_input"
printf '\115\132\102\000\001\000\001\000\002\000\000\000\377\377\000\000\000\001\000\000\000\000\000\000\034\000\000\000\003\000\000\000\016\037\270\001\000\216\300\046\241\020\000\270\000\114\315\041\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\064\022' > "$subseg_input"
printf '\270\004\000\315\020\267\001\060\333\264\013\315\020\270\000\114\315\041' > "$video_input"
printf '\270\020\000\315\020\270\000\114\315\041' > "$ega_input"
printf '\270\007\000\315\020\270\000\114\315\041' > "$mda_input"
printf '\270\023\000\315\020\270\000\114\315\041' > "$vga_input"
printf '\260\130\264\016\315\020\270\000\114\315\041' > "$teletype_input"
printf '\263\020\264\022\315\020\270\000\114\315\041' > "$ega_query_input"
printf '\264\060\315\020\270\000\114\315\041' > "$svga_probe_input"
printf '\270\000\032\315\020\270\000\114\315\041' > "$display_combo_input"
printf '\264\300\315\025\270\000\114\315\041' > "$system_config_input"
printf '\273\001\000\270\000\104\315\041\270\000\114\315\041' > "$ioctl_input"
printf '\272\026\001\264\032\315\041\272\102\001\271\026\000\264\116\315\041\270\000\114\315\041' > "$find_input"
perl -e 'print "\0" x 44, "FOO.TXT\0"' >> "$find_input"
printf '\351\000\000UPX!' > "$packed_com_input"
printf '\270\000\270\216\300\277\000\000\270\101\007\253\270\000\114\315\041' > "$raw_video_input"
perl -e 'print "\xb8\x00\x4c\xcd\x21"; print "\0" x 37804' > "$large_com_input"
printf '\264\060\315\041\273\000\200\201\303\376\007\163\002\315\040\213\343\066\243\064\022\270\000\114\315\041' > "$msc_stack_input"
printf '\214\311\216\301\273\000\002\203\303\017\200\343\360\216\321\213\343\270\000\114\315\041' > "$cs_stack_input"

if ! "$converter" --verbose "$input" "$output" > "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if [ ! -s "$output" ]; then
  printf 'selftest: converter wrote no output\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$large_com_input" "$large_com_output" >> "$log" 2>&1; then
  printf 'selftest: large default COM conversion failed\n' >&2
  cat "$log" >&2
  exit 1
fi

if [ ! -s "$large_com_output" ]; then
  printf 'selftest: converter wrote no large COM output\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$msc_stack_input" "$msc_stack_output" >> "$log" 2>&1; then
  printf 'selftest: Microsoft-style COM stack conversion failed\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! od -v -An -tx1 "$msc_stack_output" | tr -d ' \n' \
   | grep -q 'bb008081c3fe077302cd20909036a33412'; then
  printf 'selftest: Microsoft-style COM output did not patch top-of-memory SP setup\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$cs_stack_input" "$cs_stack_output" >> "$log" 2>&1; then
  printf 'selftest: CS-based COM stack conversion failed\n' >&2
  cat "$log" >&2
  exit 1
fi

cs_stack_hdr=$(od -An -tu1 -j4 -N1 "$cs_stack_output" | tr -d ' ')
cs_stack_text=$(od -An -tu4 -j8 -N4 "$cs_stack_output" | tr -d ' ')
cs_stack_hex=$(dd if="$cs_stack_output" bs=1 skip="$cs_stack_hdr" \
                 count="$cs_stack_text" 2>/dev/null \
               | od -v -An -tx1 | tr -d ' \n')
if ! printf '%s' "$cs_stack_hex" \
   | grep -q '8cc98ec1bb000283c30f80e3f090909090'; then
  printf 'selftest: CS-based COM output did not patch SS:SP stack switch\n' >&2
  cat "$log" >&2
  exit 1
fi
if printf '%s' "$cs_stack_hex" | grep -q '8ed18be3'; then
  printf 'selftest: CS-based COM output kept raw mov ss,cx; mov sp,bx\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$mz_input" "$mz_aout_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

mz_aout_magic=$(od -An -tx1 -N4 "$mz_aout_output" | tr -d ' \n')
if [ "$mz_aout_magic" != 01033004 ]; then
  printf 'selftest: default MZ input did not produce native ELKS a.out output\n' >&2
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

if ! "$converter" --verbose --mz-output=os2 --mz-data-seg=2 \
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

if "$converter" --verbose "$video_input" "$video_output" >> "$log" 2>&1; then
  printf 'selftest: BIOS graphics video input was not rejected\n' >&2
  cat "$log" >&2
  exit 1
fi

if "$converter" --verbose "$ega_input" "$ega_output" >> "$log" 2>&1; then
  printf 'selftest: EGA graphics video input was not rejected\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$mda_input" "$mda_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if "$converter" --verbose "$vga_input" "$vga_output" >> "$log" 2>&1; then
  printf 'selftest: VGA graphics video input was not rejected\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$teletype_input" "$teletype_output" >> "$log" 2>&1; then
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

if ! "$converter" --verbose "$system_config_input" "$system_config_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$ioctl_input" "$ioctl_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if ! "$converter" --verbose "$find_input" "$find_output" >> "$log" 2>&1; then
  cat "$log" >&2
  exit 1
fi

if "$converter" --verbose "$raw_video_input" "$raw_video_output" >> "$log" 2>&1; then
  printf 'selftest: raw video memory COM input was not rejected\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! grep -q 'unsupported raw DOS video memory segment b800' "$log"; then
  printf 'selftest: raw video memory rejection message missing\n' >&2
  cat "$log" >&2
  exit 1
fi

if "$converter" --verbose "$packed_com_input" "$packed_com_output" >> "$log" 2>&1; then
  printf 'selftest: packed COM input was not rejected\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! grep -q 'COM executable appears to be UPX packed' "$log"; then
  printf 'selftest: packed COM rejection message missing\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! grep -q 'unsupported int 10h AH=00' "$log"; then
  printf 'selftest: BIOS graphics mode rejection message missing\n' >&2
  cat "$log" >&2
  exit 1
fi

mda_text_hex=$(aout_text_hex "$mda_output")
if printf '%s' "$mda_text_hex" | grep -q 'cd10'; then
  printf 'selftest: text-mode video stub left a BIOS int 10h in output\n' >&2
  cat "$log" >&2
  exit 1
fi

teletype_text_hex=$(aout_text_hex "$teletype_output")
if ! printf '%s' "$teletype_text_hex" \
   | grep -q 'bb0100.*ba0100b80400cd80'; then
  printf 'selftest: BIOS teletype adapter did not route output through ELKS write\n' >&2
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

if ! od -v -An -tx1 "$system_config_output" | tr -d ' \n' | grep -q 'b80100f9c3'; then
  printf 'selftest: system-configuration probe did not return carry-set failure\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! od -v -An -tx1 "$ioctl_output" | tr -d ' \n' \
   | grep -q '3c00751731d283fb03730eb28009db750580ca01eb0380ca02f8c3'; then
  printf 'selftest: DOS IOCTL device-info stub did not mark std handles as devices\n' >&2
  cat "$log" >&2
  exit 1
fi

if ! od -v -An -tx1 "$find_output" | tr -d ' \n' \
   | grep -q 'c6451500c745160060c74518211689451a89551c'; then
  printf 'selftest: DOS find-first stub did not initialize DTA metadata\n' >&2
  cat "$log" >&2
  exit 1
fi

printf 'selftest: converted tiny COM to %s bytes and tiny MZ to native a.out; explicit NE checked\n' \
       "$(wc -c < "$output")"
