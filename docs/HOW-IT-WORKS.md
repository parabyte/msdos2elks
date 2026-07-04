# How msdos2elks Works

`msdos2elks` is a static binary rewriter.  It does not emulate DOS, run the
input under a monitor, or wrap the original file in a compatibility shell.  The
output is intended to be loaded by ELKS as a normal application.

COM inputs are emitted as ELKS Minix a.out.  MZ EXE inputs use automatic output
selection: native ELKS a.out when the program can be flattened, and OS/2 1.x NE
only for segmented cases that need it.

## Input Detection

The converter accepts DOS `.COM` files and MZ `.EXE` files.  `--format=auto`
checks for MZ/ZM headers and otherwise treats the input as COM.  Known packed,
archive, and self-extracting signatures are rejected early because the bytes at
the entry point belong to a loader stub, not the program that should run.  The
project intentionally contains no compression or decompression code.

## Source Layout

The source is split by converter subsystem:

```text
src/dos_bios_io.c  emits DOS, BIOS, and mouse compatibility stubs
src/patch.c        finds interrupt sites and rewrites them to stub calls
src/startup.c      builds PSP, argv, stack, and runtime memory state
src/com.c          handles COM layout
src/mz_os2.c       handles MZ parsing and OS/2 NE segment construction
src/output.c       writes ELKS a.out and OS/2 NE executable files
```

The Makefile builds these modules as separate objects and links them into the
host converter.  Shared internal helpers are declared in `src/internal.h`, and
subsystem-local helpers stay `static` in their own files.

## COM Layout

COM programs normally enter at offset `0100h` with `CS == DS == ES == SS`.
ELKS split a.out programs have distinct text and data segments, so the converter
creates a PSP-shaped low data area, copies the COM image at `0100h`, builds the
DOS command tail from ELKS `argc/argv`, and enters the original offset.

Common COM assumptions are patched where they can be proven locally:

```text
mov reg,cs / mov ds,reg     rewritten to load the ELKS data segment
cs: data references         rewritten to ds: where safe
DOS stack switch prologues  neutralized so ELKS owns the real stack
near ret exit idiom         routed through a native exit trampoline
```

The default COM scratch area is 32768 bytes.  `--bss=BYTES` changes that size.

## MZ And Multi-Segment Layout

For MZ inputs, the default output is automatic.  The converter emits one ELKS
text segment and one ELKS data segment when the MZ can be flattened.  It
chooses a code paragraph from the MZ `CS` entry state and a data paragraph from
`SS` or relocation evidence, then maps MZ relocation records into ELKS
relocation records when the referenced segment fits inside the selected text or
data window.

Far calls, far jumps, far pointer tables, and adjacent offset/segment
construction are adjusted when the target paragraph is known.  If flattening
would truncate a segment, strict conversion fails.

`--mz-output=auto` emits a.out when the MZ can be flattened, and emits NE when
flattening cannot represent the program.  The NE path preserves multiple
16-bit segments and internal fixups for ELKS builds with `CONFIG_EXEC_OS2=y`.

## Interrupt Rewriting

The main compatibility layer is built by replacing recognized DOS and BIOS
interrupt calls with near calls into adapter code appended to the output text
segment.

Static sites are recognized when the function number is immediately known:

```text
mov ah,IMM8; int 21h
mov ax,IMM16; int 10h
```

Some wrapper functions are compacted before replacement when the intervening
register setup instructions can be moved safely and no branch target or MZ
relocation points into the moved range.

Supported DOS file and console calls are translated to ELKS `int 80h` syscalls.
Compatibility-only DOS services return deterministic DOS-style status values.
Adapters preserve the DOS carry-flag convention: success returns with carry
clear, while negative ELKS errors become positive DOS error numbers with carry
set.

## BIOS Video And Keyboard

Basic BIOS video and keyboard services are handled in two ways:

```text
text output and keyboard reads       translated through ELKS console I/O
mode setup and direct video access   passed through or handled by small stubs
```

Direct video memory access is often left intact because many DOS programs write
to CGA/EGA/VGA memory themselves.  When a converted program switches to a BIOS
graphics or adapter-specific mode through a rewritten `int 10h` mode-set call,
the runtime asks ELKS for the console graphics lock and raw keyboard mode before
calling the ROM BIOS.  That prevents ELKS console output from painting over the
DOS program while still leaving the real BIOS and hardware in control of the
selected video mode.  The startup code records the current text mode, and the
exit path releases raw keyboard mode, restores that text mode when practical,
and then releases the graphics lock.

Enhanced adapter discovery is intentionally conservative.  Static `int 10h`
queries such as EGA alternate-select, display-combination, and enhanced video
information report no useful enhanced-adapter service, while explicit mode-set
calls still pass through to the BIOS.  This favors the broad CGA/EGA/MDA
direct-memory paths used by many XT-era DOS libraries instead of letting a
modern VGA BIOS steer them into incompatible adapter-specific layouts.

## FCB And DOS Memory Calls

Legacy FCB calls are implemented as fail-closed or status stubs.  This lets many
programs probe for old DOS interfaces without receiving impossible success
responses.  The layer is not a full DOS FCB filesystem implementation.

DOS memory allocation calls operate inside the ELKS data segment.  Allocation
requests are in 16-byte paragraphs.  Failures return DOS error `8` and the
largest available paragraph count in `BX`.

## Failure Policy

Strict conversion fails when unsupported behavior would remain in a way likely
to mislead users.  That is intentional: a failed conversion is easier to debug
than an apparently native binary that silently depends on unimplemented DOS
state.

Use `--partial --verbose` or `tools/explain-conversion.sh` to study a failure,
then add a general rewrite or adapter that applies to a class of programs.
