# msdos2elks Tools

These helper tools are optional wrappers around the main `msdos2elks`
executable.

## `selftest.sh`

Build self-test used by `make check`.  It creates a tiny COM program in a
private temporary directory and verifies that the converter writes a non-empty
ELKS executable.

## `xt-smoke.sh`

Converts 100 generated XT-era DOS smoke programs covering DOS, BIOS video,
BIOS keyboard, BIOS clock, and mouse interrupt surfaces:

```sh
make smoke-xt
```

## `xt-graphics-smoke.sh`

Converts 100 generated XT-era graphics smoke programs as COM inputs and the
same 100 as plain MZ EXE inputs.  Each program draws through BIOS video calls
or the CGA/MDA memory aperture, pauses using the BIOS clock, and exits through
DOS:

```sh
make smoke-xt-graphics
tools/xt-graphics-smoke.sh ./msdos2elks /tmp/msdos2elks-gfx-corpus
```

## `elks-runtime-smoke.sh`

Boots ELKS in QEMU, mounts a FAT floppy containing converted programs, runs
the generated `g001`..`g100` applications from the lower-case FAT `exe`
directory, and stores every inspected VGA frame plus GDB register snapshot in
`screenshots/`:

```sh
tools/elks-runtime-smoke.sh ../elks/image/fd1440-fat.img /tmp/msdos2elks-gfx-runtime.img
make smoke-elks-runtime \
  ELKS_BOOT_IMAGE=../elks/image/fd1440-fat.img \
  ELKS_APP_IMAGE=/tmp/msdos2elks-gfx-runtime.img \
  SCREENSHOTS=screenshots/manual-run
```

For broad smoke passes where every application should start from a clean ELKS
console and kernel state, use the batch wrapper:

```sh
ELKS_SMOKE_COUNT=100 \
  tools/elks-runtime-batch.sh ../elks/image/fd1440-fat.img \
    /tmp/msdos2elks-gfx-runtime.img screenshots/exe-flat-100
```

It runs `elks-runtime-smoke.sh` once per application and collects the per-case
summaries into one batch `summary.tsv`.

Interactive real applications that are expected to keep running can be probed
with `ELKS_SMOKE_ALLOW_TIMEOUT=1`.  In that mode, a timeout after a successful
launch is recorded as a `timeout` screenshot phase instead of a failed run,
while serial crash signatures still fail the test.

## `convert-directory.sh`

Batch converts `.COM` and `.EXE` files found below an input directory:

```sh
tools/convert-directory.sh ~/dos-programs converted
tools/convert-directory.sh ~/dos-programs converted --partial
```

Inputs must already be plain DOS binaries.  Packed files and archives are
rejected by the converter rather than expanded by this project.

## `explain-conversion.sh`

Runs the converter in verbose partial mode and discards the output:

```sh
tools/explain-conversion.sh GAME.EXE
```

Use this when you need to understand why an input fails strict conversion.
