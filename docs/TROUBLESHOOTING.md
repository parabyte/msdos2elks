# Troubleshooting

This page lists common conversion and validation failures.

## `packed executable`

The input still contains a packer stub.  Use the unpack helper:

```sh
./unpack-and-convert.sh GAME.EXE game
```

If the helper cannot unpack the file automatically, install a tool for that
packer or provide a local hook:

```sh
MSDOS2ELKS_UNPACKER=/path/to/unpacker ./unpack-and-convert.sh GAME.EXE game
MSDOS2ELKS_UNPACK_CMD='my-unpacker "$MSDOS2ELKS_INPUT" "$MSDOS2ELKS_OUTPUT"' \
  ./unpack-and-convert.sh GAME.EXE game
```

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

If the program relies on direct CGA/EGA/VGA memory access, the target ELKS
configuration and hardware must expose that video memory in a way the process
can use.  Converted programs that set a BIOS graphics mode through a rewritten
`int 10h` call ask ELKS to stop console painting before the BIOS mode switch,
then restore a text mode and release the console lock on exit.  Programs that
switch hardware without a recognizable BIOS mode-set call may still need
target-side validation.

Legacy graphics libraries can misdetect a VGA BIOS and then select a memory
layout that does not match the code bundled with the program.  The converter
therefore reports no useful EGA/VGA/MCGA adapter information for static
discovery queries, while still allowing explicit BIOS mode-set calls and direct
video memory writes.

## `text or data segment too large`

The flat ELKS a.out path cannot represent the program in one 16-bit text window
and one 16-bit data window.  For MZ inputs, use the default OS/2 NE path and run
the result on an ELKS build with `CONFIG_EXEC_OS2=y`.

In upstream ELKS, run `make menuconfig`, open `Executable file formats`, enable
`Support OS/2 executables`, save, and rebuild.

## `/tmp` Fills During Unpacking

Move scratch space or lower the unpack limit:

```sh
MSDOS2ELKS_TMP_ROOT=/var/tmp ./unpack-and-convert.sh GAME.EXE game
MSDOS2ELKS_UNPACK_TMP_LIMIT_KB=65536 ./unpack-and-convert.sh GAME.EXE game
```
