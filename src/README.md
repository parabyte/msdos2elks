# Source Layout

The converter is split by subsystem:

```text
internal.h       Shared constants, structures, and standard includes.
common.c         Byte vectors, relocation vectors, endian helpers, and errors.
cli.c            Command-line parsing and host input loading.
dos_bios_io.c    DOS, BIOS, and mouse adapter code emitters.
patch.c          Interrupt-site scanning, wrapper compaction, and patching.
startup.c        PSP, argv, stack, COM return, and runtime memory setup.
com.c            DOS COM conversion.
mz_os2.c         DOS MZ parsing, relocation analysis, and OS/2 NE construction.
output.c         ELKS Minix a.out and OS/2 NE writers.
main.c           Command entry point.
```

The Makefile builds each `.c` file as a separate object and links the
host-side converter from those objects.  Shared internal functions are declared
in `internal.h`; subsystem-local scanners and emitters stay `static` inside
their source files.
