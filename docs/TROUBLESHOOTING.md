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
and one 16-bit data window.  For MZ inputs, use `--mz-output=auto` or
`--mz-output=os2` and run any resulting NE file on an ELKS build with
`CONFIG_EXEC_OS2=y`.

In upstream ELKS, run `make menuconfig`, open `Executable file formats`, enable
`Support OS/2 executables`, save, and rebuild.
