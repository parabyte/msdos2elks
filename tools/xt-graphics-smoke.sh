#!/usr/bin/env bash
# Generate and convert 100 tiny XT-era DOS graphics smoke programs.
#
# These are plain COM programs built from 8086-compatible instruction bytes.
# Each one selects an XT-era BIOS display mode, draws through INT 10h or the
# legacy CGA/MDA display aperture, pauses using the BIOS clock, and exits
# through DOS INT 21h.  That shape is deliberate: a runtime ELKS/QEMU smoke
# test can launch one program, capture the visible graphics state, and then
# verify that the ELKS console returns to text mode without depending on
# host keyboard injection timing.

set -euo pipefail

converter=${1:-./msdos2elks}
keep_dir=${2:-}
tmp=${TMPDIR:-/tmp}/msdos2elks-xt-graphics.$$

if [ -n "$keep_dir" ]; then
  root=$keep_dir
  rm -rf "$root"
else
  root=$tmp
fi

rm -rf "$tmp"
mkdir -p "$root/dos-com" "$root/dos-exe" "$root/elks-com" "$root/elks-exe" \
  "$root/log"
if [ -z "$keep_dir" ]; then
  trap 'rm -rf "$tmp"' EXIT HUP INT TERM
fi

if [ ! -x "$converter" ]; then
  printf 'xt-graphics-smoke: converter is not executable: %s\n' \
    "$converter" >&2
  exit 1
fi

perl - "$root/dos-com" "$root/dos-exe" <<'PERL'
use strict;
use warnings;

my ($com_dir, $exe_dir) = @ARGV;
my @cases;

sub bytes { return pack ('C*', @_); }
sub word { my ($v) = @_; return pack ('v', $v & 0xffff); }
sub add { push @cases, [ $_[0], $_[1] ]; }

sub mode {
  my ($m) = @_;
  return bytes (0xb8) . word ($m) . bytes (0xcd, 0x10);
}

sub pause_exit {
  return
    bytes (0xb4, 0x00, 0xcd, 0x1a) .  # BIOS get timer ticks, CX:DX.
    bytes (0x89, 0xd3) .              # BX = starting low tick word.
    bytes (0xb4, 0x00, 0xcd, 0x1a) .  # wait_loop: get current ticks.
    bytes (0x89, 0xd0) .              # AX = current low tick word.
    bytes (0x29, 0xd8) .              # AX = elapsed ticks, wraps safely.
    bytes (0x3d) . word (54) .        # roughly three seconds at 18.2 Hz.
    bytes (0x72, 0xf3) .              # jb wait_loop.
    bytes (0xb8, 0x00, 0x4c, 0xcd, 0x21);
}

sub palette {
  my ($bh, $bl) = @_;
  return bytes (0xb4, 0x0b, 0xb7, $bh, 0xb3, $bl, 0xcd, 0x10);
}

sub pixel {
  my ($color, $x, $y) = @_;
  return bytes (0xb4, 0x0c, 0xb0, $color & 3, 0xb7, 0) .
         bytes (0xb9) . word ($x) .
         bytes (0xba) . word ($y) .
         bytes (0xcd, 0x10);
}

sub read_pixel {
  my ($x, $y) = @_;
  return bytes (0xb4, 0x0d, 0xb7, 0) .
         bytes (0xb9) . word ($x) .
         bytes (0xba) . word ($y) .
         bytes (0xcd, 0x10);
}

sub tty {
  my ($ch, $color) = @_;
  return bytes (0xb4, 0x0e, 0xb0, ord ($ch), 0xb7, 0,
                0xb3, $color & 0x0f, 0xcd, 0x10);
}

sub write_char {
  my ($ch, $attr, $count) = @_;
  return bytes (0xb4, 0x09, 0xb0, ord ($ch), 0xb7, 0,
                0xb3, $attr & 0x0f) .
         bytes (0xb9) . word ($count) .
         bytes (0xcd, 0x10);
}

sub cursor {
  my ($row, $col) = @_;
  return bytes (0xb4, 0x02, 0xb7, 0, 0xb6, $row, 0xb2, $col,
                0xcd, 0x10);
}

sub get_mode {
  return bytes (0xb4, 0x0f, 0xcd, 0x10);
}

sub direct_cga_word {
  my ($offset, $value) = @_;
  return bytes (0xb8) . word (0xb800) .
         bytes (0x8e, 0xc0, 0xbf) . word ($offset) .
         bytes (0xb8) . word ($value) .
         bytes (0xab);
}

sub direct_mda_word {
  my ($offset, $value) = @_;
  return bytes (0xb8) . word (0xb000) .
         bytes (0x8e, 0xc0, 0xbf) . word ($offset) .
         bytes (0xb8) . word ($value) .
         bytes (0xab);
}

sub marker {
  my ($n) = @_;
  return tty ('G', 2) . tty (chr (ord ('0') + (($n / 10) % 10)), 3) .
         tty (chr (ord ('0') + ($n % 10)), 1);
}

sub mz_exe {
  my ($code) = @_;
  my $header_len = 32;
  my $file_len = $header_len + length ($code);
  my $pages = int (($file_len + 511) / 512);
  my $last = $file_len % 512;

  return
    bytes (0x4d, 0x5a) .          # MZ signature.
    word ($last) .                # Bytes used in final 512-byte page.
    word ($pages) .               # Total 512-byte pages in file.
    word (0) .                    # No relocation entries.
    word ($header_len / 16) .     # Header size in paragraphs.
    word (0) .                    # No extra minimum allocation.
    word (0xffff) .               # Maximum extra allocation.
    word (0) .                    # SS relative to load image.
    word (0x0100) .               # SP: enough for tiny smoke cases.
    word (0) .                    # No checksum.
    word (0) .                    # IP at start of load image.
    word (0) .                    # CS at start of load image.
    word (0x001c) .               # Traditional relocation-table offset.
    word (0) .                    # Overlay number.
    bytes (0, 0, 0, 0) .          # Pad to 32-byte header.
    $code;
}

for my $m (0x04, 0x05, 0x06) {
  for my $i (0 .. 9) {
    my $x = 8 + ($i * 23);
    my $y = 8 + ($i * 11);
    add ("mode_${m}_pix_$i",
         mode ($m) . palette (0, $i & 1) . marker ($i) .
         pixel ($i, $x, $y) . read_pixel ($x, $y) . pause_exit ());
  }
}

for my $i (0 .. 19) {
  my $x1 = 4 + ($i * 9);
  my $x2 = 260 - ($i * 7);
  my $y1 = 12 + ($i * 4);
  my $y2 = 180 - ($i * 3);
  add ("cga_cross_$i",
       mode (0x04) . pixel ($i, $x1, $y1) . pixel ($i + 1, $x2, $y1) .
       pixel ($i + 2, $x1, $y2) . pixel ($i + 3, $x2, $y2) .
       pause_exit ());
}

for my $i (0 .. 14) {
  my $off = ($i * 160) & 0x1ffe;
  my $val = 0x1111 | (($i & 3) << 8);
  add ("cga_mem_$i",
       mode (0x04) . direct_cga_word ($off, $val) .
       direct_cga_word ($off + 2, $val ^ 0x3333) . pause_exit ());
}

for my $i (0 .. 14) {
  my $row = $i % 12;
  my $col = ($i * 5) % 60;
  add ("text_mix_$i",
       mode (0x03) . cursor ($row, $col) . write_char ('A', 0x0e, 3) .
       tty (' ', 7) . tty ('O', 2) . tty ('K', 2) . get_mode () .
       pause_exit ());
}

for my $i (0 .. 9) {
  add ("mda_text_$i",
       mode (0x07) . direct_mda_word (($i * 160) & 0x0ffe,
                                      0x0700 | ord ('M')) .
       get_mode () . pause_exit ());
}

for my $i (0 .. 9) {
  add ("probe_$i",
       mode (0x04) . marker ($i) .
       bytes (0xb4, 0x12, 0xcd, 0x10) .
       bytes (0xb4, 0x1a, 0xcd, 0x10) .
       bytes (0xb4, 0x30, 0xcd, 0x10) .
       pixel (($i % 3) + 1, 20 + $i, 20 + $i) . pause_exit ());
}

if (@cases != 100) {
  die "internal case count is " . scalar (@cases) . ", expected 100\n";
}

for my $i (0 .. $#cases) {
  my $base = sprintf ('G%03u', $i + 1);
  my $com_path = "$com_dir/$base.COM";
  my $exe_path = "$exe_dir/$base.EXE";

  open my $com, '>:raw', $com_path or die "$com_path: $!\n";
  print {$com} $cases[$i][1];
  close $com or die "$com_path: $!\n";

  open my $exe, '>:raw', $exe_path or die "$exe_path: $!\n";
  print {$exe} mz_exe ($cases[$i][1]);
  close $exe or die "$exe_path: $!\n";
}
PERL

com_count=0
for input in "$root"/dos-com/G*.COM; do
  name=$(basename "$input" .COM)
  output=$root/elks-com/$name
  log=$root/log/com-$name.log
  com_count=$((com_count + 1))
  if ! "$converter" --verbose "$input" "$output" > "$log" 2>&1; then
    printf 'xt-graphics-smoke: COM conversion failed for %s\n' "$name" >&2
    cat "$log" >&2
    exit 1
  fi
  if [ ! -s "$output" ]; then
    printf 'xt-graphics-smoke: converter wrote empty COM output for %s\n' \
      "$name" >&2
    cat "$log" >&2
    exit 1
  fi
done

exe_count=0
for input in "$root"/dos-exe/G*.EXE; do
  name=$(basename "$input" .EXE)
  output=$root/elks-exe/$name
  log=$root/log/exe-$name.log
  exe_count=$((exe_count + 1))
  if ! "$converter" --verbose --mz-output=auto "$input" "$output" \
       > "$log" 2>&1; then
    printf 'xt-graphics-smoke: EXE conversion failed for %s\n' "$name" >&2
    cat "$log" >&2
    exit 1
  fi
  if [ ! -s "$output" ]; then
    printf 'xt-graphics-smoke: converter wrote empty EXE output for %s\n' \
      "$name" >&2
    cat "$log" >&2
    exit 1
  fi
done

if [ "$com_count" -ne 100 ] || [ "$exe_count" -ne 100 ]; then
  printf 'xt-graphics-smoke: converted %u COM and %u EXE cases, expected 100 each\n' \
    "$com_count" "$exe_count" >&2
  exit 1
fi

printf 'xt-graphics-smoke: converted %u COM and %u MZ EXE XT-era graphics programs' \
  "$com_count" "$exe_count"
if [ -n "$keep_dir" ]; then
  printf ' into %s\n' "$root"
else
  printf '\n'
fi
