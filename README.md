# msdos2elks

`msdos2elks` rewrites plain DOS `.COM` and MZ `.EXE` programs into
ELKS-loadable executables.  It is a static converter, not a DOS emulator.

The converter is intentionally strict.  DOS file, console, keyboard, time, and
text-output paths are rewritten to ELKS calls or small compatibility stubs.
Raw video memory, ROM BIOS graphics calls, packed executables, archives,
installer stubs, TSRs, and arbitrary hardware access are rejected.

## Build

```sh
./configure
make
make check
```

## Use

```sh
./msdos2elks input.com output
./msdos2elks input.exe output
```

Useful options:

```text
--format=auto|com|exe
--mz-output=auto|aout|os2
--stack=BYTES
--heap=BYTES
--bss=BYTES
--partial
--verbose
```

Use `--partial` only for diagnostics.  Strict conversion is the normal path and
fails when unsupported DOS behavior would remain in the output.

## Notes

Inputs must already be unpacked plain DOS binaries.  This project contains no
compression or decompression support.

Flat-compatible MZ inputs are written as native ELKS a.out.  Segmented MZ input
can use OS/2 1.x NE output when the target ELKS build supports it.

More detail is in `docs/CONVERTING.md`, `docs/HOW-IT-WORKS.md`, and
`docs/TROUBLESHOOTING.md`.
