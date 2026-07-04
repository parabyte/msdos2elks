# Converting DOS Programs For ELKS

This guide explains the normal host-side workflow for turning a DOS `.COM` or
MZ `.EXE` file into an ELKS-loadable program.

## 1. Build The Converter

```sh
git clone https://github.com/parabyte/msdos2elks.git
cd msdos2elks
./configure
make
make check
```

`make check` creates a tiny DOS COM program, converts it, and verifies that the
resulting ELKS executable is non-empty.  It is a host build sanity check, not a
substitute for booting the converted program inside ELKS.

Install is optional:

```sh
make install DESTDIR=/tmp/msdos2elks-package
```

## 2. Convert One Program

```sh
./msdos2elks HELLO.COM hello
./msdos2elks PROGRAM.EXE program
```

The output path is the final ELKS binary.  COM inputs become ELKS Minix a.out.
Flat-compatible MZ EXE inputs become native ELKS a.out by default.  Larger
segmented EXEs can fall back to OS/2 1.x NE; enable OS/2 executable support in
ELKS only for those outputs or when using `--mz-output=os2`.

The converter is strict by default: if it cannot safely rewrite a required DOS
feature, it exits with an error and does not leave behind a misleading
executable.

Useful options:

```text
--format=auto|com|exe     Override input format detection.
--stack=BYTES             Minimum ELKS stack request.
--heap=BYTES              ELKS heap request, or 0 for the ELKS default.
--bss=BYTES               Extra zero-filled memory for DOS-style scratch data.
--mz-output=os2|aout|auto MZ output mode.  Default auto.
--partial                 Diagnostic output even when unsupported sites remain.
--verbose                 Print layout and rewrite details.
```

`BYTES` values are integer byte counts.  Values larger than 65535 are rejected.
MZ `minalloc` values that exceed ELKS' representable maximum heap request are
saturated to `0xfff0`.

`--mz-output=aout` forces native Minix a.out for MZ inputs and fails if the
program cannot fit one 16-bit text window and one 16-bit data window.
`--mz-output=auto` chooses a.out for flat-compatible MZ inputs and NE for larger
segmented inputs.

## 3. Prepare Plain Inputs

Packed DOS executables, archives, and installer stubs are not converted.  Their
entry bytes belong to a loader or extractor, not the program that should be
rewritten for ELKS.  This project intentionally contains no compression or
decompression helper.  Prepare a plain DOS `.COM` or MZ `.EXE` outside this
tree, then convert that plain file.

## 4. Convert A Directory

```sh
tools/convert-directory.sh ~/dos-programs converted
```

Only successful conversions are copied to the output directory.  Per-input logs
are written under `converted/logs/`.  The script exits nonzero if any file fails
or if no file converts.

## 5. Stage And Test In ELKS

Copy the converted executable and the original program data files into the ELKS
filesystem image or target root.  For simple command-line programs, run the
binary from an ELKS shell and verify exit status and terminal output.

For graphical programs, test on the ELKS target configuration you care about
and inspect the real display output.  Host conversion proves that the binary was
rewritten and emitted; it does not prove that every device or video assumption
matches your ELKS system.

## 6. Treat `--partial` As Diagnostic

`--partial` is useful when studying failures because it allows the converter to
emit an output while reporting unsupported sites.  Do not publish partial
outputs as known-working ELKS binaries unless they have been tested in ELKS and
the remaining DOS behavior is intentional for that target.

## Legal Note

The converter does not include DOS games or commercial DOS software.  Convert
only programs you are legally allowed to possess, modify, and run on your ELKS
system.
