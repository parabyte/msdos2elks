#!/usr/bin/env bash
# Run converted ELKS graphics applications with one fresh ELKS boot per case.
#
# The single-VM runtime smoke is useful while debugging one program, but BIOS
# keyboard injection can leave different console states after a graphics app.
# This wrapper keeps the evidence clean by rebooting ELKS for each case and
# collecting the per-case screenshot, serial, monitor, and GDB files under one
# output directory.

set -euo pipefail

usage()
{
  cat >&2 <<'EOF'
usage: tools/elks-runtime-batch.sh ELKS_BOOT_FLOPPY APP_FLOPPY [OUTPUT_DIR]

Environment:
  ELKS_SMOKE_APP_DIR=exe       directory on APP_FLOPPY to run
  ELKS_SMOKE_CASE_SUBDIR=0     run /mnt/$APP_DIR/gNNN/run from inside gNNN
  ELKS_SMOKE_PROGRAM=run       executable name for ELKS_SMOKE_CASE_SUBDIR=1
  ELKS_SMOKE_COUNT=100         number of G001..Gnnn programs to run
  ELKS_SMOKE_START=1           first numeric program id to run
  ELKS_SMOKE_TIMEOUT=25        seconds to wait for each program to return
  ELKS_SMOKE_ALLOW_TIMEOUT=0   treat timeout after launch as interactive success
EOF
  exit 2
}

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  usage
fi

boot_image=$1
app_image=$2
out_dir=${3:-screenshots/runtime-batch-$(date +%Y%m%d-%H%M%S)}
case_count=${ELKS_SMOKE_COUNT:-100}
case_start=${ELKS_SMOKE_START:-1}
case_timeout=${ELKS_SMOKE_TIMEOUT:-25}
allow_timeout=${ELKS_SMOKE_ALLOW_TIMEOUT:-0}
app_dir=${ELKS_SMOKE_APP_DIR:-exe}
case_subdir=${ELKS_SMOKE_CASE_SUBDIR:-0}
program_name=${ELKS_SMOKE_PROGRAM:-run}

mkdir -p "$out_dir"
out_dir=$(cd "$out_dir" && pwd)
summary=$out_dir/summary.tsv
run_log=$out_dir/run.log

touch "$run_log"
if [ ! -s "$summary" ]; then
  printf 'case\tphase\twidth\theight\tmaxval\tnonblack_pixels\tunique_colors\tfile\n' \
    > "$summary"
fi

case_id=$case_start
case_end=$((case_start + case_count - 1))

while [ "$case_id" -le "$case_end" ]; do
  case_name=$(printf 'G%03u' "$case_id")
  case_out=$out_dir/$case_name

  printf 'batch run %s/%s\n' "$app_dir" "$case_name" | tee -a "$run_log"
  ELKS_SMOKE_APP_DIR=$app_dir \
  ELKS_SMOKE_COUNT=1 \
  ELKS_SMOKE_START=$case_id \
  ELKS_SMOKE_TIMEOUT=$case_timeout \
  ELKS_SMOKE_ALLOW_TIMEOUT=$allow_timeout \
  ELKS_SMOKE_CASE_SUBDIR=$case_subdir \
  ELKS_SMOKE_PROGRAM=$program_name \
    "$(dirname "$0")/elks-runtime-smoke.sh" \
      "$boot_image" "$app_image" "$case_out" | tee -a "$run_log"

  if [ -f "$case_out/summary.tsv" ]; then
    tail -n +2 "$case_out/summary.tsv" >> "$summary"
  fi

  case_id=$((case_id + 1))
done

printf 'elks-runtime-batch: ran %u isolated apps, output=%s\n' \
  "$case_count" "$out_dir"
