#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")" && pwd)
converter=${MSDOS2ELKS_INNER_CONVERTER:-$root/msdos2elks}
max_candidates=${MSDOS2ELKS_UNPACK_MAX:-96}
tmp_limit_kb=${MSDOS2ELKS_UNPACK_TMP_LIMIT_KB:-131072}

usage ()
{
  cat <<EOF
usage: ${0##*/} [MSDOS2ELKS-OPTIONS] INPUT OUTPUT

Try msdos2elks directly, then recursively extract ZIP/SFX payloads and run
available unpackers before retrying conversion.

Optional unpackers:
  upx       auto-detected, writes a decompressed copy when supported
  unlzexe   auto-detected, runs on a temporary copy
  unp       auto-detected, extracts in a temporary directory
  7z        auto-detected, extracts archives/SFX files
  gamecomp  auto-detected, PKLITE reveal support when installed
  npm       auto-detected, runs @camoto/gamecomp helpers for LZEXE and a
            temporary patched PKLITE helper unless
            MSDOS2ELKS_UNPACK_NPM_GAMECOMP=0

Custom unpack hooks:
  MSDOS2ELKS_UNPACKER     command invoked as: command INPUT OUTPUT_DIR
  MSDOS2ELKS_UNPACK_CMD   shell command with MSDOS2ELKS_INPUT,
                          MSDOS2ELKS_OUTPUT_DIR, and MSDOS2ELKS_OUTPUT set
  MSDOS2ELKS_INNER_CONVERTER
                          converter executable, defaulting to ./msdos2elks
  MSDOS2ELKS_TMP_ROOT     temporary directory root, defaulting to TMPDIR or /tmp
  MSDOS2ELKS_UNPACK_TMP_LIMIT_KB
                          scratch-space cap in KiB, default 131072
  MSDOS2ELKS_UNPACK_NPM_GAMECOMP
                          set to 0 to disable npm-backed PKLITE unpacking
EOF
}

if [ "${1:-}" = "--help" ] || [ "$#" -lt 2 ]; then
  usage
  exit 2
fi

args=("$@")
last=$((${#args[@]} - 1))
input_pos=$((${#args[@]} - 2))
input=${args[$input_pos]}
output=${args[$last]}
conv_args=("${args[@]:0:$input_pos}")

if [ "$(basename "$converter")" = "$(basename "$0")" ]; then
  printf 'msdos2elks: refusing recursive unpack converter: %s\n' "$converter" >&2
  exit 2
fi

tmp_parent=${MSDOS2ELKS_TMP_ROOT:-${TMPDIR:-/tmp}}
mkdir -p "$tmp_parent"
tmp=$(mktemp -d "$tmp_parent/msdos2elks-unpack.XXXXXX")
queue_file=$tmp/queue
seen_file=$tmp/seen
last_log=$tmp/last.log
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

check_tmp_limit ()
{
  used=$(du -sk "$tmp" 2>/dev/null | awk '{ print $1 }')
  used=${used:-0}
  if [ "$used" -gt "$tmp_limit_kb" ]; then
    printf 'msdos2elks: unpack scratch exceeded %u KiB at %u KiB\n' \
           "$tmp_limit_kb" "$used" >&2
    exit 1
  fi
}

is_unpackable_failure ()
{
  grep -Eiq 'packed|compressed installer|SFX archive|ZIP/SFX|extract it before conversion|unpack it before conversion' "$1"
}

try_convert ()
{
  src=$1

  if "$converter" "${conv_args[@]}" "$src" "$output" > "$last_log" 2>&1; then
    cat "$last_log"
    return 0
  fi

  return 1
}

safe_name ()
{
  printf '%s' "$1" | tr '[:upper:]' '[:lower:]' \
    | sed 's/[^a-z0-9._+-]/-/g;s/--*/-/g;s/^-//;s/-$//'
}

enqueue_candidates ()
{
  dir=$1

  find "$dir" -type f \( -iname '*.exe' -o -iname '*.com' -o -iname '*.zip' \
       -o -iname '*.1' -o -iname '*.2' -o -iname '*.3' -o -iname '*.dat' \) \
       -printf '%s\t%p\n' 2>/dev/null \
    | sort -n \
    | cut -f2- >> "$queue_file"
}

looks_pklite ()
{
  grep -aqE 'PKLITE|PKWARE' "$1"
}

looks_lzexe ()
{
  grep -aqE 'LZ91|LZEXE' "$1"
}

extract_archives ()
{
  src=$1
  out=$2
  changed=1
  pass=0

  mkdir -p "$out"
  while [ "$changed" -eq 1 ] && [ "$pass" -lt 3 ]; do
    changed=0
    pass=$((pass + 1))

    find "$out" -type f ! -path '*/_extract-*/*' -print | while IFS= read -r f; do
      base=$(basename "$f")
      dest=$(dirname "$f")/_extract-$(safe_name "$base")

      if [ -d "$dest" ]; then
        continue
      fi

      if command -v unzip >/dev/null 2>&1 && unzip -tqq "$f" >/dev/null 2>&1; then
        mkdir -p "$dest"
        unzip -q -o "$f" -d "$dest" >/dev/null 2>&1 || true
        echo changed > "$dest/.msdos2elks-changed"
      elif command -v 7z >/dev/null 2>&1 && 7z t "$f" >/dev/null 2>&1; then
        mkdir -p "$dest"
        7z x -y "-o$dest" "$f" >/dev/null 2>&1 || true
        echo changed > "$dest/.msdos2elks-changed"
      fi
    done

    if find "$out" -name .msdos2elks-changed -type f -print -delete \
        | grep -q .; then
      changed=1
    fi
    check_tmp_limit
  done

  enqueue_candidates "$out"
}

run_pklite_unpackers ()
{
  src=$1
  out=$2
  direct=$out/pklite-gamecomp.exe
  patched=$out/pklite-gamecomp-patched.exe
  work=$out/gamecomp-patched

  if ! looks_pklite "$src"; then
    return
  fi

  if command -v gamecomp >/dev/null 2>&1; then
    if gamecomp -cmp-pklite < "$src" > "$direct" \
        2>"$out/pklite-gamecomp.log" && [ -s "$direct" ]; then
      printf '%s\n' "$direct" >> "$queue_file"
      check_tmp_limit
    fi
  fi

  if [ "${MSDOS2ELKS_UNPACK_NPM_GAMECOMP:-1}" != 0 ] \
      && command -v npm >/dev/null 2>&1 \
      && command -v node >/dev/null 2>&1; then
    mkdir -p "$work"
    if MSDOS2ELKS_PKLITE_SRC=$src \
       MSDOS2ELKS_PKLITE_OUT=$patched \
       MSDOS2ELKS_PKLITE_WORK=$work \
       npm exec --yes --package @camoto/gamecomp -- sh -c '
         set -e
         bin=$(command -v gamecomp)
         if command -v readlink >/dev/null 2>&1; then
           bin=$(readlink -f "$bin")
         fi
         pkg=$(cd "$(dirname "$bin")/.." && pwd)
         mods=$(cd "$pkg/../.." && pwd)
         rm -rf "$MSDOS2ELKS_PKLITE_WORK/node_modules"
         cp -R "$mods" "$MSDOS2ELKS_PKLITE_WORK/node_modules"
         pklite=$MSDOS2ELKS_PKLITE_WORK/node_modules/@camoto/gamecomp/formats/cmp-pklite.js
         MSDOS2ELKS_GAMECOMP_PKLITE=$pklite node -e "
const fs = require(\"fs\");
const path = process.env.MSDOS2ELKS_GAMECOMP_PKLITE;
let src = fs.readFileSync(path, \"utf8\");
const oldLine = \"if (count === 0x00) break;   // end of list\";
const newLine = \"if (count === 0x00 || (flagExtra && verBase < 0x010C && count === 0xFF)) break;   // end of list\";
if (!src.includes(oldLine))
  throw new Error(\"PKLITE relocation patch point not found\");
fs.writeFileSync(path, src.replace(oldLine, newLine));
"
         node "$MSDOS2ELKS_PKLITE_WORK/node_modules/@camoto/gamecomp/bin/gamecomp.js" \
           -cmp-pklite < "$MSDOS2ELKS_PKLITE_SRC" > "$MSDOS2ELKS_PKLITE_OUT"
       ' >"$out/pklite-gamecomp-patched.log" 2>&1 \
       && [ -s "$patched" ]; then
      printf '%s\n' "$patched" >> "$queue_file"
      check_tmp_limit
    fi
  fi
}

run_lzexe_unpackers ()
{
  src=$1
  out=$2
  direct=$out/lzexe-gamecomp.exe
  npm_out=$out/lzexe-npm-gamecomp.exe

  if ! looks_lzexe "$src"; then
    return
  fi

  if command -v gamecomp >/dev/null 2>&1; then
    if gamecomp -cmp-lzexe < "$src" > "$direct" \
        2>"$out/lzexe-gamecomp.log" && [ -s "$direct" ]; then
      printf '%s\n' "$direct" >> "$queue_file"
      check_tmp_limit
    fi
  fi

  if [ "${MSDOS2ELKS_UNPACK_NPM_GAMECOMP:-1}" != 0 ] \
      && command -v npm >/dev/null 2>&1; then
    if npm exec --yes --package @camoto/gamecomp -- \
        gamecomp -cmp-lzexe < "$src" > "$npm_out" \
        2>"$out/lzexe-npm-gamecomp.log" && [ -s "$npm_out" ]; then
      printf '%s\n' "$npm_out" >> "$queue_file"
      check_tmp_limit
    fi
  fi
}

run_unpackers ()
{
  src=$1
  slot=$2
  out=$tmp/unpack-$slot

  mkdir -p "$out"

  run_pklite_unpackers "$src" "$out"
  run_lzexe_unpackers "$src" "$out"

  if command -v unzip >/dev/null 2>&1 && unzip -tqq "$src" >/dev/null 2>&1; then
    unzip -q -o "$src" -d "$out/zip" >/dev/null 2>&1 || true
    check_tmp_limit
    extract_archives "$src" "$out/zip"
  fi

  if command -v 7z >/dev/null 2>&1 && 7z t "$src" >/dev/null 2>&1; then
    mkdir -p "$out/7z"
    7z x -y "-o$out/7z" "$src" >/dev/null 2>&1 || true
    check_tmp_limit
    extract_archives "$src" "$out/7z"
  fi

  if command -v upx >/dev/null 2>&1; then
    cp -p "$src" "$out/upx-input" 2>/dev/null || cp "$src" "$out/upx-input"
    check_tmp_limit
    if upx -q -d -o "$out/upx-output" "$out/upx-input" >/dev/null 2>&1; then
      printf '%s\n' "$out/upx-output" >> "$queue_file"
      check_tmp_limit
    fi
  fi

  if command -v unlzexe >/dev/null 2>&1; then
    cp -p "$src" "$out/unlzexe.exe" 2>/dev/null || cp "$src" "$out/unlzexe.exe"
    check_tmp_limit
    if (cd "$out" && unlzexe unlzexe.exe >/dev/null 2>&1); then
      enqueue_candidates "$out"
      printf '%s\n' "$out/unlzexe.exe" >> "$queue_file"
      check_tmp_limit
    fi
  fi

  if command -v unp >/dev/null 2>&1; then
    mkdir -p "$out/unp"
    cp -p "$src" "$out/unp/input" 2>/dev/null || cp "$src" "$out/unp/input"
    (cd "$out/unp" && unp input >/dev/null 2>&1) || true
    check_tmp_limit
    enqueue_candidates "$out/unp"
  fi

  if [ -n "${MSDOS2ELKS_UNPACKER:-}" ]; then
    mkdir -p "$out/custom"
    if "$MSDOS2ELKS_UNPACKER" "$src" "$out/custom" >/dev/null 2>&1; then
      check_tmp_limit
      enqueue_candidates "$out/custom"
    fi
  fi

  if [ -n "${MSDOS2ELKS_UNPACK_CMD:-}" ]; then
    mkdir -p "$out/cmd"
    MSDOS2ELKS_INPUT=$src \
    MSDOS2ELKS_OUTPUT_DIR=$out/cmd \
    MSDOS2ELKS_OUTPUT=$out/cmd/unpacked.exe \
      sh -c "$MSDOS2ELKS_UNPACK_CMD" >/dev/null 2>&1 || true
    check_tmp_limit
    enqueue_candidates "$out/cmd"
    [ -f "$out/cmd/unpacked.exe" ] && printf '%s\n' "$out/cmd/unpacked.exe" >> "$queue_file"
  fi
}

if try_convert "$input"; then
  exit 0
fi

if ! is_unpackable_failure "$last_log"; then
  cat "$last_log" >&2
  exit 1
fi

: > "$queue_file"
: > "$seen_file"
printf '%s\n' "$input" >> "$queue_file"

index=0
while [ "$index" -lt "$max_candidates" ]; do
  index=$((index + 1))
  src=$(sed -n "${index}p" "$queue_file" || true)
  [ -n "$src" ] || break
  [ -f "$src" ] || continue

  key=$(cd "$(dirname "$src")" && pwd)/$(basename "$src")
  if grep -Fxq "$key" "$seen_file" 2>/dev/null; then
    continue
  fi
  printf '%s\n' "$key" >> "$seen_file"

  if [ "$src" != "$input" ] && try_convert "$src"; then
    printf 'msdos2elks: unpacked candidate: %s\n' "$src" >&2
    exit 0
  fi

  if is_unpackable_failure "$last_log"; then
    run_unpackers "$src" "$index"
  fi
done

cat "$last_log" >&2
printf 'msdos2elks: no unpacked DOS image converted after %u candidates\n' \
       "$index" >&2
exit 1
