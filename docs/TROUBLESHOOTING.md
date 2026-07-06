# Troubleshooting

This page lists common conversion and validation failures.

## `packed executable`

The input still contains a packer, archive, or installer stub.  This converter
does not include compression or decompression code.  Prepare a plain DOS
`.COM` or MZ `.EXE` outside this tree, then rerun `msdos2elks` on that plain
file.

## `unsupported int`

The converter found a DOS or BIOS interrupt site that it cannot safely rewrite.
Inspect it with:

```sh
tools/explain-conversion.sh PROGRAM.EXE
```

The right fix is usually a general adapter or wrapper rewrite that applies to a
class of programs.  Avoid adding title-specific byte patches.

## Converted Program Starts But Cannot Find Files

Most DOS games need data files beside the executable.  Copy the original data
directory into ELKS along with the converted binary, preserving relative paths
and short filenames where possible.

## No Graphics Appear

Host conversion does not prove graphical correctness.  Run the program inside
the ELKS target environment with the target video mode and inspect the real
display output.

Strict conversion now refuses graphics paths that cannot go through the ELKS
kernel.  Direct `A000h`, `B000h`, or `B800h` video-memory setup is reported as
unsupported, and BIOS graphics mode changes, palette changes, pixel
reads/writes, cursor/page/scroll services, and dynamic `int 10h` helpers are
also rejected.  A successful conversion should not contain a ROM BIOS video
interrupt or a raw video-memory drawing path.

Legacy graphics libraries can misdetect a VGA BIOS and then select a memory
layout that does not match the code bundled with the program.  The converter
therefore reports no useful EGA/VGA/MCGA adapter information for static
discovery queries.  If a program still needs pixel graphics, ELKS needs a
kernel graphics interface that the converter can target; adding a title-specific
BIOS or raw-memory escape is intentionally not supported.

## `text or data segment too large`

The flat ELKS a.out path cannot represent the program in one 16-bit text window
and one 16-bit data window.  For MZ inputs, use `--mz-output=auto` or
`--mz-output=os2` and run any resulting NE file on an ELKS build with
`CONFIG_EXEC_OS2=y`.

In upstream ELKS, run `make menuconfig`, open `Executable file formats`, enable
`Support OS/2 executables`, save, and rebuild.
