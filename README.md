# msdos2elks

`msdos2elks` is a host-side DOS binary rewriter.  It strips DOS `.COM` or
MZ `.EXE` file format state, emits native ELKS-loadable executables, and
rewrites statically recognized DOS and BIOS I/O calls into ELKS `int 80h`
syscall adapter stubs or compatibility stubs.

This is not a DOS emulator and it does not wrap the input binary in a runtime.
COM inputs are emitted as ELKS Minix a.out.  MZ EXE inputs are emitted as
OS/2 1.x NE by default, which requires `CONFIG_EXEC_OS2=y` in the target ELKS
build and lets the converter preserve more 16-bit segment layouts.

## Quick Start

```sh
git clone https://github.com/parabyte/msdos2elks.git
cd msdos2elks
./configure
make
make check
./msdos2elks hello.com hello
./msdos2elks program.exe program
```

Enable OS/2 executable support in ELKS before running converted `.EXE` outputs:
in upstream ELKS, run `make menuconfig`, open `Executable file formats`, enable
`Support OS/2 executables`, save, and rebuild.  This sets `CONFIG_EXEC_OS2=y`.

PKLITE MZ inputs supported by the built-in revealer are converted directly.
For other packed DOS inputs, use the unpack helper.  It tries a direct
conversion first, then uses locally available unpackers before retrying the
converter:

```sh
./unpack-and-convert.sh packed-or-sfx.exe program
```

Detailed conversion instructions are in `docs/CONVERTING.md`.  A technical
description of the rewriter is in `docs/HOW-IT-WORKS.md`.  Common failure
messages and fixes are collected in `docs/TROUBLESHOOTING.md`.

## Build

The package uses a small GNU-style configure script that writes `config.mk`.
It does not require autoconf.

```sh
./configure --prefix=/usr/local
make
make check
make install DESTDIR=/tmp/package-root
```

Useful configure options:

```text
--prefix=PATH            Installation prefix, default /usr/local.
--bindir=PATH            Program installation directory.
--cc=CC                  C compiler command.
--cflags=FLAGS           Optimizer/debug C flags.
--ldflags=FLAGS          Linker flags.
--enable-sanitizers      Add address/undefined-behavior sanitizer flags.
```

The `msdos2elks` and `*.o` build outputs are intentionally ignored by git.

## Use

```sh
./msdos2elks hello.com hello
./msdos2elks program.exe program
./unpack-and-convert.sh packed-or-sfx.exe program
tools/convert-directory.sh ~/dos-games converted-games
```

By default the converter is strict: if any DOS interrupt site cannot be
rewritten safely, it refuses to write the output.  Use `--partial` only when
you want a diagnostic build that may still contain unsupported DOS behavior.

Useful options:

```text
--format=auto|com|exe     Input format, default auto.
--stack=BYTES             ELKS minimum stack size.
--heap=BYTES              ELKS heap request.  0 keeps the ELKS default.
--bss=BYTES               Extra zero-filled ELKS bss.
--mz-output=os2|aout|auto MZ output mode.  Default os2.
--mz-code-seg=PARA        Override the MZ code segment paragraph.
--mz-data-seg=PARA        Override the MZ data segment paragraph.
--partial                 Write output even with unsupported DOS calls.
--verbose                 Print conversion details.
```

## Common Workflows

Convert one plain DOS program:

```sh
./msdos2elks APPS/HELLO.COM hello
```

Convert a packed executable with automatic unpack attempts:

```sh
./unpack-and-convert.sh GAMES/LEMMINGS/LEMMINGS.EXE lemmings
```

Batch-convert a directory tree, keeping logs for failures:

```sh
tools/convert-directory.sh ~/dos-programs converted
less converted/logs/program.log
```

Inspect why a strict conversion fails:

```sh
tools/explain-conversion.sh PROGRAM.EXE
```

Copy the converted executable and its original data files into an ELKS root or
disk image before running it.  Graphical programs usually need their original
resource files, not just the converted executable.

## Repository Layout

```text
msdos2elks.c              Small umbrella translation unit.
src/internal.h            Shared converter types, constants, and helpers.
src/common.c              Byte-vector, relocation, and endian helpers.
src/cli.c                 Option parsing and host file input.
src/dos_bios_io.c         DOS, BIOS, and mouse I/O adapter emitters.
src/patch.c               Interrupt discovery and binary rewrite engine.
src/startup.c             PSP, argv, stack, and runtime memory setup.
src/com.c                 DOS COM input conversion.
src/mz_os2.c              DOS MZ parsing plus OS/2 NE output preparation.
src/output.c              ELKS a.out and OS/2 NE file writers.
src/main.c                Converter command entry point.
configure                 Host configuration script.
Makefile                  Build, check, install, and clean targets.
docs/CONVERTING.md        User conversion workflow.
docs/HOW-IT-WORKS.md      Architecture and binary rewriting notes.
docs/TROUBLESHOOTING.md   Common errors and validation fixes.
tools/                    Batch conversion, diagnostics, and self-test tools.
```

## What Is Rewritten

The tool rewrites supported `int 21h`, `int 10h`, `int 16h`, and `int 1Ah`
sites when the function number is proven by an immediately preceding
`mov ah, IMM8` or `mov ax, IMM16`.  It also compacts common wrapper forms such
as `mov ah, IMM8; ...register setup...; int 21h` when the intervening
instructions can be moved safely and no branch target or MZ relocation points
inside the moved range.  The replacement is a near call to native ELKS code
appended to the text segment.  If a DOS `int 21h` site is genuinely dynamic and
too small to replace in place, startup installs a process-local `int 21h`
handler in the real-mode interrupt vector table so shared DOS wrappers can
dispatch through the same ELKS adapters.

BIOS video conversion passes static `int 10h` video services through to the
machine BIOS whenever the function number is known.  Mode-set calls record the
requested BIOS mode; graphics and adapter-specific modes also claim the ELKS
console graphics lock and raw keyboard path so ELKS console output does not
paint over DOS programs that draw directly through video hardware.  The ROM
BIOS and target machine still decide which CGA, MDA, EGA, VGA, MCGA, or
adapter-specific modes are actually available.

Supported DOS functions:

```text
00h, 4Ch              terminate process
01h, 07h, 08h         read one console byte
06h                   direct console I/O, output or no-key status
0Bh, 0Dh              console input status and disk reset stubs
0Eh, 19h              select/get default drive, compatibility stubs
02h                   write one console byte
09h                   write '$'-terminated string to stdout
1Ah                   set DTA
0Fh-15h, 21h-23h,
27h-29h               legacy FCB calls, fail-closed/status stubs
1Bh, 1Ch              FAT allocation info compatibility stubs
2Fh                   get DTA
25h, 35h              set/get interrupt vector, compatibility stubs
2Ah, 2Ch, 2Eh, 30h    fixed date/time, set verify, DOS version
33h, 36h              Ctrl-Break and disk-free compatibility stubs
39h, 3Ah, 3Bh         mkdir, rmdir, chdir
3Ch, 3Dh, 3Eh         create, open, close
3Fh, 40h              read, write
41h                   unlink
42h                   lseek
43h, 44h, 47h         file attributes, basic IOCTL, get cwd stubs
2Bh, 2Dh, 4Dh         set date/time and get child return code stubs
4Eh, 4Fh              minimal find first, find next fail-closed
48h, 49h, 4Ah         DOS paragraph allocate/free/resize compatibility
54h, 57h              verify flag and file date/time compatibility stubs
56h                   rename, assuming ES == DS for the new path
```

Supported BIOS compatibility calls:

```text
int 10h AH=00h,01h,02h,05h,06h,07h
              video mode/cursor/page and scroll compatibility; mode set calls
              record the mode and pass through to BIOS for real hardware setup
int 10h AH=03h,08h,0Fh
              cursor/read char and BIOS video-mode query passthrough
int 10h AH=09h,0Ah,0Eh
              BIOS text/teletype passthrough for text-mode and title-screen
              rendering
int 10h AH=0Ch,0Dh
              BIOS pixel write/read passthrough for CGA/EGA graphics modes
int 10h AH=0Bh
              palette BIOS passthrough
int 10h AH=12h,1Ah,30h
              conservative no-EGA/VGA/enhanced-adapter probe stubs, so old
              DOS libraries keep using their legacy direct-video paths
int 10h other static AH
              generic BIOS video passthrough
int 16h AH=00h,01h,02h,10h,11h,12h
              stdin-backed blocking reads, no-key status, shift-flag stubs
int 1Ah AH=00h,01h,02h,04h
              deterministic clock/date compatibility stubs
```

The adapters preserve the DOS carry-flag success/error convention: successful
ELKS syscalls return with CF clear, and negative ELKS errors are converted to
positive DOS-style error numbers with CF set.

## Memory Model

`.COM` files are converted to split ELKS a.out with matching text and data
images at offset `0100h`.  The data segment receives a small PSP-shaped low
area so ordinary absolute `.COM` data references keep their offsets.  A native
startup stub builds the DOS command tail at PSP offsets `80h..ffh` from ELKS
`argc/argv`, leaves `ES == DS` as DOS COM programs expect, and then jumps to
the original entry point.

The startup code also pushes a DOS-style zero return word.  A native exit
trampoline is installed at text offset `0000h`, so `.COM` programs that exit
with a plain near `ret` are translated to normal ELKS process exit instead of
falling into host stack data.  Common far-return setup (`push ds; push 0`) is
rewritten to return through the native text trampoline, since DOS COM programs
used `CS == DS` for that idiom.

The COM rewriter also fixes common split-model assumptions by changing segment
setup such as `mov reg,cs ... mov ds,reg` to use the native data segment,
rewriting `cs:` data-memory prefixes to `ds:`, and neutralizing DOS stack
switch prologues of the form `cli; mov ss,...; mov sp,...; sti`.

By default, `.COM` conversion materializes 32768 bytes of zero-filled DOS-style
scratch memory after the file image before the converter's own runtime state.
This keeps uninitialized COM variables from colliding with the converter's heap
and DTA bookkeeping.  `--bss=BYTES` changes that low-memory scratch size for
COM inputs; values are integer byte counts and are truncated only when using
the default, never when explicitly requested.

MZ `.EXE` conversion emits one native ELKS text segment and one native ELKS data
segment.  The converter strips the MZ header, chooses the entry code paragraph
from `CS`, and chooses the data paragraph from `SS` or from the most common
non-code MZ relocation target.  Relocation records whose segment value points
inside the flattened text or data window are emitted as ELKS relocation records.
For far call/jump immediates, far pointer tables, and adjacent `mov offset;
mov segment` far-pointer construction, the offset word is increased by the
paragraph delta before the segment word is relocated.  This preserves many
multi-segment compiler layouts as long as the final text and data windows each
fit in 16 bits.

MZ outputs get a native startup stub that builds a DOS-style command tail at
`80h..ffh` in the stack segment and enters the translated program with
`DS == ES == SS`, matching DOS' initial PSP register convention closely enough
for programs that immediately switch `DS` to their relocated data segment.
Known DOS paragraph stack-normalizer prologues are removed so selector
arithmetic cannot corrupt the ELKS stack.

Memory values are integer byte counts.  `--stack`, `--heap`, and `--bss` reject
values larger than `65535`.  MZ `minalloc` is converted to an ELKS heap request;
values at or above `0xfff0` are saturated to ELKS' maximum-heap request value
`0xfff0`.  If the resulting text or data segment cannot fit in 16 bits, the
conversion fails instead of truncating.

The DOS memory allocator compatibility stub manages paragraph allocations in
the ELKS data segment for small DOS requests.  Allocation size is in 16-byte
paragraphs.  Successful `AH=48h` returns a segment value derived from the
process `DS`; `AH=49h` and `AH=4Ah` currently succeed without compaction.
Large MZ games that request a conventional-memory arena above one native
segment use ELKS far-memory allocation instead, with disk reads bounced through
the runtime stack segment when ELKS requires syscall buffers to match the exact
segment base.  Allocation failure returns DOS error `8` and the largest
available paragraph count in `BX`.

## Packed Inputs

Packed binaries are not converted while still packed.  The packed entry stub is
the wrong program to translate, so `msdos2elks` first reveals supported PKLITE
MZ inputs to a plain in-memory MZ image and converts that image.  Other known
LZEXE, EXEPACK/LZ-style, ZIP/SFX, and compressed installer signatures are
rejected.  Use `unpack-and-convert.sh` when the input may use a packer that is
not handled internally:

```sh
./unpack-and-convert.sh game.exe game
MSDOS2ELKS_UNPACKER=/path/to/unpacker ./unpack-and-convert.sh game.exe game
MSDOS2ELKS_UNPACK_CMD='my-unpacker "$MSDOS2ELKS_INPUT" "$MSDOS2ELKS_OUTPUT"' \
  ./unpack-and-convert.sh game.exe game
```

The helper tries the converter directly first, recursively extracts ZIP/SFX
payloads with `unzip` or `7z`, runs available `upx`, `unlzexe`, and `unp`
tools, and then retries every produced `.EXE`/`.COM`.  LZEXE and unsupported
PKLITE variants can still be handled with `gamecomp` when available; if `npm`
and `node` are present, the helper can also run `@camoto/gamecomp` directly for
LZEXE and a temporary patched copy for the old PKLITE 1.03 extra-compression
relocation terminator used by titles such as Lemmings.  Set
`MSDOS2ELKS_UNPACK_NPM_GAMECOMP=0` to disable that npm-backed path.
Custom unpack hooks remain available for packers that need separate licensed or
locally supplied tools.  If no unpacker produces a plain DOS image, conversion
fails rather than emitting a fake ELKS executable.

The unpack helper uses a private temporary directory and deletes it on exit.
`MSDOS2ELKS_TMP_ROOT` can move that scratch area off `/tmp`, and
`MSDOS2ELKS_UNPACK_TMP_LIMIT_KB` caps per-input unpack scratch use.  The default
cap is `131072` KiB.

## Limits

The converter intentionally refuses arbitrary DOS features.  It does not
emulate DOS, TSRs, arbitrary direct hardware access, custom interrupt handlers,
packed executables that have not been unpacked, or self-modifying code that
expects `CS == DS`.  Dynamic DOS `int 21h` sites are handled through the
process-local interrupt vector adapter, but dynamic BIOS calls are passthrough
unless a static adapter can be safely installed.  Direct BIOS/video calls that
are not statically rewritten are left in place so hardware-oriented programs can
still use the machine when ELKS and the target environment permit it; this is
passthrough behavior, not full graphics emulation.  Multi-segment MZ support is
limited to segment values that can be
flattened into one 16-bit text or data window; arbitrary segment-register
switching and far runtime models (`LDS`/`LES`, custom far returns,
environment-block walkers) may still need deeper whole-program segment
rewriting.  The FCB layer is a compatibility layer, not a full DOS directory and
random-record implementation.  ZIP/SFX archives with `.EXE` names must be
extracted before conversion.

## OS/2 NE Output

MZ `.EXE` conversion emits an OS/2 1.x NE executable by default.  ELKS can load
these when `CONFIG_EXEC_OS2=y` is enabled in the target ELKS build.  The NE path
preserves several 16-bit code/data segments, emits internal segment fixups, and
keeps each segment under the ELKS loader's `MAX_SEGS` limit, currently 5.

Use `--mz-output=aout` only when you deliberately want the older flat Minix
a.out MZ path.  Use `--mz-output=auto` to emit a.out for flat-compatible MZ
inputs and NE only when the flat shape cannot represent the program.

The NE writer caps default heap requests so the ELKS loader can fit the auto
data segment, stack, argv copy, and heap inside one 64 KiB segment.  Explicit
`--heap` or `--stack` requests are still treated as hard requirements and fail
if they cannot be represented.

This is the path used for large pre-1992 MZ games such as Lemmings after
PKLITE reveal.  It removes the previous flat-converter failure where the DOS
data segment started beyond the first 64K.  Large games may need an ELKS boot
profile with less kernel heap and cache pressure so `fmemalloc` can satisfy
their DOS arena request; for example, Lemmings CGA was validated with
`task=6 buf=8 cache=4 file=20 inode=24 heap=15000`.  Runtime validation for NE
binaries must be done inside ELKS or on real hardware; `elksemu` only accepts
ELKS Minix a.out binaries and reports NE files as not being ELKS binaries.
