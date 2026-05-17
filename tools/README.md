# msdos2elks Tools

These helper tools are optional wrappers around the main `msdos2elks`
executable.

## `selftest.sh`

Build self-test used by `make check`.  It creates a tiny COM program in a
private temporary directory and verifies that the converter writes a non-empty
ELKS executable.

## `convert-directory.sh`

Batch converts `.COM` and `.EXE` files found below an input directory:

```sh
tools/convert-directory.sh ~/dos-programs converted
tools/convert-directory.sh ~/dos-programs converted --partial
```

By default it calls `unpack-and-convert.sh` so packed inputs are unpacked before
conversion.  Set `MSDOS2ELKS_NO_UNPACK=1` to call `msdos2elks` directly.

## `explain-conversion.sh`

Runs the converter in verbose partial mode and discards the output:

```sh
tools/explain-conversion.sh GAME.EXE
```

Use this when you need to understand why an input fails strict conversion.
