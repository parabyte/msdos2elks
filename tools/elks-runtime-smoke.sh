#!/usr/bin/env bash
# Boot ELKS in QEMU and run converted XT-era graphics applications.
#
# The converter tests prove that plain DOS COM and MZ EXE inputs can be
# rewritten.  This script is the next layer: it boots a standard ELKS image,
# mounts a FAT floppy containing converted programs, runs them one by one, and
# saves every VGA frame and debugger register snapshot used for inspection.

set -euo pipefail

usage()
{
  cat >&2 <<'EOF'
usage: tools/elks-runtime-smoke.sh ELKS_BOOT_FLOPPY APP_FLOPPY [OUTPUT_DIR]

Environment:
  ELKS_SMOKE_APP_DIR=exe       directory on APP_FLOPPY to run, usually exe
  ELKS_SMOKE_CASE_SUBDIR=0     run /mnt/$APP_DIR/gNNN/run from inside gNNN
  ELKS_SMOKE_PROGRAM=run       executable name for ELKS_SMOKE_CASE_SUBDIR=1
  ELKS_SMOKE_COUNT=100         number of G001..Gnnn programs to run
  ELKS_SMOKE_START=1           first numeric program id to run
  ELKS_SMOKE_TIMEOUT=12        seconds to wait for each program to return
  ELKS_SMOKE_ALLOW_TIMEOUT=0   treat timeout after launch as interactive success
  ELKS_SMOKE_KEYS='ret spc a ret'
                              QEMU sendkey names to inject after first capture
  ELKS_SMOKE_KEY_DELAY=0.25    seconds between injected keys
  ELKS_SMOKE_POST_KEY_DELAY=0.6
                              seconds to wait before the shell done marker
  ELKS_SMOKE_QEMU=qemu-system-i386
EOF
  exit 2
}

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  usage
fi

boot_image=$1
app_image=$2
out_dir=${3:-screenshots/runtime-$(date +%Y%m%d-%H%M%S)}
app_dir=${ELKS_SMOKE_APP_DIR:-exe}
case_subdir=${ELKS_SMOKE_CASE_SUBDIR:-0}
program_name=${ELKS_SMOKE_PROGRAM:-run}
case_count=${ELKS_SMOKE_COUNT:-100}
case_start=${ELKS_SMOKE_START:-1}
case_timeout=${ELKS_SMOKE_TIMEOUT:-12}
allow_timeout=${ELKS_SMOKE_ALLOW_TIMEOUT:-0}
smoke_keys=${ELKS_SMOKE_KEYS:-'ret spc a ret'}
smoke_key_delay=${ELKS_SMOKE_KEY_DELAY:-0.25}
smoke_post_key_delay=${ELKS_SMOKE_POST_KEY_DELAY:-0.6}
qemu=${ELKS_SMOKE_QEMU:-qemu-system-i386}

if [ ! -f "$boot_image" ]; then
  printf 'elks-runtime-smoke: boot image not found: %s\n' "$boot_image" >&2
  exit 1
fi

if [ ! -f "$app_image" ]; then
  printf 'elks-runtime-smoke: app image not found: %s\n' "$app_image" >&2
  exit 1
fi

if ! command -v "$qemu" >/dev/null 2>&1; then
  printf 'elks-runtime-smoke: qemu not found: %s\n' "$qemu" >&2
  exit 1
fi

if ! command -v nc >/dev/null 2>&1; then
  printf 'elks-runtime-smoke: nc is required for QEMU TCP monitor ports\n' >&2
  exit 1
fi

mkdir -p "$out_dir"
out_dir=$(cd "$out_dir" && pwd)

serial_log=$out_dir/serial.log
monitor_log=$out_dir/monitor.log
summary_tsv=$out_dir/summary.tsv
run_log=$out_dir/run.log
fifo=$out_dir/serial.in
serial_out=$out_dir/serial.out

: > "$serial_log"
: > "$monitor_log"
: > "$summary_tsv"
: > "$run_log"

printf 'case\tphase\twidth\theight\tmaxval\tnonblack_pixels\tunique_colors\tfile\n' \
  > "$summary_tsv"

pick_port()
{
  base=$1
  end=$((base + 199))
  port=$base

  while [ "$port" -le "$end" ]; do
    if ! ( : >"/dev/tcp/127.0.0.1/$port" ) >/dev/null 2>&1; then
      printf '%u\n' "$port"
      return 0
    fi
    port=$((port + 1))
  done

  return 1
}

port_base=$((43000 + ($$ % 1000)))
serial_port=$(pick_port "$port_base") || {
  printf 'elks-runtime-smoke: no free serial TCP port\n' >&2
  exit 1
}
monitor_port=$(pick_port $((port_base + 300))) || {
  printf 'elks-runtime-smoke: no free monitor TCP port\n' >&2
  exit 1
}
gdb_port=$(pick_port $((port_base + 600))) || {
  printf 'elks-runtime-smoke: no free GDB TCP port\n' >&2
  exit 1
}

wait_port()
{
  port=$1
  tries=200

  while [ "$tries" -gt 0 ]; do
    if ( : >"/dev/tcp/127.0.0.1/$port" ) >/dev/null 2>&1; then
      return 0
    fi
    tries=$((tries - 1))
    sleep 0.1
  done

  return 1
}

monitor_cmd()
{
  cmd=$1

  {
    printf '### %s\n' "$cmd"
    printf '%s\n' "$cmd" |
      nc -q 0 -w 5 127.0.0.1 "$monitor_port" || true
  } >> "$monitor_log" 2>&1
}

gdb_snapshot()
{
  label=$1
  file=$out_dir/$label.gdb.txt

  if command -v gdb >/dev/null 2>&1; then
    timeout 12 gdb -q -nx -batch \
      -ex 'set pagination off' \
      -ex "target remote 127.0.0.1:$gdb_port" \
      -ex 'info registers' \
      -ex 'detach' \
      -ex 'quit' > "$file" 2>&1 || true
  else
    {
      printf 'gdb not found; saving QEMU monitor registers instead\n'
      monitor_cmd 'info registers'
    } > "$file" 2>&1
  fi
}

screendump()
{
  label=$1
  file=$out_dir/$label.ppm
  tries=50

  monitor_cmd "screendump $file"

  while [ "$tries" -gt 0 ]; do
    if [ -s "$file" ]; then
      return 0
    fi
    tries=$((tries - 1))
    sleep 0.1
  done

  if [ ! -s "$file" ]; then
    printf 'elks-runtime-smoke: screendump failed for %s\n' "$label" >&2
    return 1
  fi
}

summarize_ppm()
{
  case_name=$1
  phase=$2
  file=$3

  perl - "$case_name" "$phase" "$file" <<'PERL' >> "$summary_tsv"
use strict;
use warnings;

my ($case_name, $phase, $file) = @ARGV;
open my $fh, '<:raw', $file or die "$file: $!\n";

sub token {
  my ($fh) = @_;
  my $tok = '';
  my $ch;

  while (read ($fh, $ch, 1) == 1) {
    if ($ch eq '#') {
      my $discard;
      while (read ($fh, $discard, 1) == 1) {
        last if $discard eq "\n";
      }
      next;
    }
    last if $ch !~ /\s/;
  }

  $tok .= $ch;
  while (read ($fh, $ch, 1) == 1) {
    last if $ch =~ /\s/;
    $tok .= $ch;
  }

  return $tok;
}

my $magic = token ($fh);
die "$file: expected P6 PPM, got $magic\n" if $magic ne 'P6';
my $width = token ($fh);
my $height = token ($fh);
my $maxval = token ($fh);

my %colors;
my $nonblack = 0;
my $buf;

while (read ($fh, $buf, 12288)) {
  my $len = length ($buf);
  my $i = 0;

  while ($i + 2 < $len) {
    my $rgb = substr ($buf, $i, 3);
    ++$colors{$rgb};
    ++$nonblack if $rgb ne "\0\0\0";
    $i += 3;
  }
}

printf "%s\t%s\t%s\t%s\t%s\t%u\t%u\t%s\n",
  $case_name, $phase, $width, $height, $maxval, $nonblack,
  scalar (keys %colors), $file;
PERL
}

wait_log()
{
  pattern=$1
  seconds=$2
  remaining=$((seconds * 10))

  while [ "$remaining" -gt 0 ]; do
    if grep -aq "$pattern" "$serial_log"; then
      return 0
    fi
    if ! kill -0 "$qemu_pid" >/dev/null 2>&1; then
      printf 'elks-runtime-smoke: QEMU exited while waiting for %s\n' \
        "$pattern" >&2
      return 1
    fi
    remaining=$((remaining - 1))
    sleep 0.1
  done

  return 1
}

serial_offset()
{
  wc -c < "$serial_log"
}

check_serial_errors()
{
  label=$1
  offset=$2
  chunk=$out_dir/$label.serial.txt

  tail -c +"$((offset + 1))" "$serial_log" > "$chunk"

  if grep -Eai \
      'not found|No such file|Permission denied|Exec format|Segmentation|Killed|panic|Oops' \
      "$chunk" >/dev/null 2>&1; then
    printf 'elks-runtime-smoke: serial error while running %s\n' "$label" >&2
    cat "$chunk" >&2
    return 1
  fi

  return 0
}

send_serial()
{
  printf '%s\r' "$1" >&3
}

send_serial_key()
{
  key=$1

  case "$key" in
    ret|enter)
      printf '\r' >&3
      ;;
    spc|space)
      printf ' ' >&3
      ;;
    esc)
      printf '\033' >&3
      ;;
    tab)
      printf '\t' >&3
      ;;
    backspace|bs)
      printf '\177' >&3
      ;;
    up)
      printf '\033[A' >&3
      ;;
    down)
      printf '\033[B' >&3
      ;;
    right)
      printf '\033[C' >&3
      ;;
    left)
      printf '\033[D' >&3
      ;;
    ?)
      printf '%s' "$key" >&3
      ;;
  esac
}

send_app_key()
{
  # ELKS can expose /dev/console through either the VGA keyboard path or the
  # serial console path, depending on the boot options used for the runtime
  # image.  Some smoke programs wait for a DOS BIOS keypress before returning.
  # Send the QEMU keyboard event first, then an equivalent serial byte sequence
  # for serial-backed consoles.  If the application is clock-driven or already
  # exited, these bytes are only harmless shell input.
  for key in $smoke_keys; do
    monitor_cmd "sendkey $key"
    send_serial_key "$key"
    sleep "$smoke_key_delay"
  done
  send_serial ''
  sleep "$smoke_post_key_delay"
}

qemu_args=(
  -accel tcg,one-insn-per-tb=on
  -nodefaults
  -machine isapc
  -cpu 486
  -m 8M
  -display none
  -vga std
  -rtc base=utc
  -serial "tcp:127.0.0.1:$serial_port,server=on,wait=off"
  -monitor "tcp:127.0.0.1:$monitor_port,server=on,wait=off"
  -gdb "tcp:127.0.0.1:$gdb_port"
  -no-reboot
  -drive "file=$boot_image,if=floppy,format=raw,index=0"
  -drive "file=$app_image,if=floppy,format=raw,index=1"
)

printf 'boot_image=%s\napp_image=%s\nout_dir=%s\nserial_port=%u\nmonitor_port=%u\ngdb_port=%u\n' \
  "$boot_image" "$app_image" "$out_dir" "$serial_port" "$monitor_port" \
  "$gdb_port" >> "$run_log"
printf 'app_dir=%s\ncase_subdir=%s\nprogram_name=%s\ncase_start=%s\ncase_count=%s\ncase_timeout=%s\nsmoke_keys=%s\n' \
  "$app_dir" "$case_subdir" "$program_name" "$case_start" "$case_count" \
  "$case_timeout" "$smoke_keys" >> "$run_log"

"$qemu" "${qemu_args[@]}" >> "$out_dir/qemu.log" 2>&1 &
qemu_pid=$!

cleanup()
{
  if [ -n "${qemu_pid:-}" ] && kill -0 "$qemu_pid" >/dev/null 2>&1; then
    monitor_cmd quit || true
    sleep 1
    kill "$qemu_pid" >/dev/null 2>&1 || true
    wait "$qemu_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "${serial_pid:-}" ]; then
    kill "$serial_pid" >/dev/null 2>&1 || true
    wait "$serial_pid" >/dev/null 2>&1 || true
  fi
  rm -f "$fifo"
}
trap cleanup EXIT HUP INT TERM

wait_port "$serial_port" || {
  printf 'elks-runtime-smoke: serial port did not open\n' >&2
  exit 1
}
wait_port "$monitor_port" || {
  printf 'elks-runtime-smoke: monitor port did not open\n' >&2
  exit 1
}

rm -f "$fifo"
mkfifo "$fifo"
nc 127.0.0.1 "$serial_port" < "$fifo" |
  tee "$serial_log" > "$serial_out" &
serial_pid=$!
exec 3> "$fifo"

gdb_snapshot boot
screendump boot
summarize_ppm BOOT boot "$out_dir/boot.ppm"

wait_log 'login:' 120 || {
  printf 'elks-runtime-smoke: no serial login prompt\n' >&2
  exit 1
}

send_serial root
sleep 1
send_serial 'stty -echo'
send_serial 'echo __ELKS_READY__'
wait_log '__ELKS_READY__' 20 || {
  printf 'elks-runtime-smoke: shell did not become ready\n' >&2
  exit 1
}

send_serial 'mkdir /mnt'
send_serial 'mount -t fat /dev/fd1 /mnt'
send_serial 'echo __ELKS_MOUNTED__'
wait_log '__ELKS_MOUNTED__' 30 || {
  printf 'elks-runtime-smoke: FAT app floppy did not mount\n' >&2
  exit 1
}

case_id=$case_start
case_end=$((case_start + case_count - 1))
failures=0

while [ "$case_id" -le "$case_end" ]; do
  case_name=$(printf 'G%03u' "$case_id")
  case_file=$(printf 'g%03u' "$case_id")
  done_marker="__DONE_${case_name}__"
  before=$case_name-before-key
  after=$case_name-after-return

  printf 'run %s/%s\n' "$app_dir" "$case_file" | tee -a "$run_log"
  run_start=$(serial_offset)
  if [ "$case_subdir" = 1 ]; then
    send_serial "cd /mnt/$app_dir/$case_file"
    sleep 0.2
    send_serial "./$program_name"
  else
    send_serial "/mnt/$app_dir/$case_file"
  fi
  sleep 1

  if ! check_serial_errors "$case_name-launch" "$run_start"; then
    gdb_snapshot "$case_name-launch-error"
    screendump "$case_name-launch-error" || true
    failures=$((failures + 1))
    break
  fi

  screendump "$before" || failures=$((failures + 1))
  summarize_ppm "$case_name" before-key "$out_dir/$before.ppm"
  gdb_snapshot "$case_name"

  send_app_key
  send_serial "echo $done_marker"

  if wait_log "$done_marker" "$case_timeout"; then
    if ! check_serial_errors "$case_name-run" "$run_start"; then
      gdb_snapshot "$case_name-run-error"
      screendump "$case_name-run-error" || true
      failures=$((failures + 1))
      break
    fi
    screendump "$after" || failures=$((failures + 1))
    summarize_ppm "$case_name" after-return "$out_dir/$after.ppm"
  elif [ "$allow_timeout" = 1 ]; then
    if ! check_serial_errors "$case_name-timeout" "$run_start"; then
      gdb_snapshot "$case_name-timeout-error"
      screendump "$case_name-timeout-error" || true
      failures=$((failures + 1))
      break
    fi
    printf 'interactive-timeout %s/%s\n' "$app_dir" "$case_name" |
      tee -a "$run_log"
    gdb_snapshot "$case_name-timeout"
    screendump "$case_name-timeout" || failures=$((failures + 1))
    summarize_ppm "$case_name" timeout "$out_dir/$case_name-timeout.ppm"
  else
    printf 'timeout %s/%s\n' "$app_dir" "$case_name" | tee -a "$run_log" >&2
    gdb_snapshot "$case_name-timeout"
    screendump "$case_name-timeout" || true
    failures=$((failures + 1))
    break
  fi

  case_id=$((case_id + 1))
done

printf 'elks-runtime-smoke: ran %u %s apps, failures=%u, output=%s\n' \
  "$((case_id - case_start))" "$app_dir" "$failures" "$out_dir"

if [ "$failures" -ne 0 ]; then
  exit 1
fi
