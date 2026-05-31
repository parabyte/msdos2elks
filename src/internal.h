#ifndef MSDOS2ELKS_INTERNAL_H
#define MSDOS2ELKS_INTERNAL_H

/*
 * msdos2elks - rewrite selected MS-DOS binaries as native ELKS programs.
 *
 * The converter is deliberately a static binary rewriter rather than a DOS
 * emulator or a wrapper.  It parses COM and MZ images, removes the DOS load
 * format, emits ELKS Minix a.out for COM inputs, and emits OS/2 1.x NE by
 * default for MZ inputs so DOS EXE segment layout can be preserved for ELKS
 * builds with CONFIG_EXEC_OS2 enabled.  A flat MZ-to-a.out path remains
 * available for users who explicitly request it.
 *
 * Conversion happens in conservative phases:
 *
 *   1. identify the input shape, reveal supported packed MZ images, and
 *      reject packed images that must still be unpacked before the real
 *      program can be inspected;
 *   2. construct ELKS text/data layout, relocation records, startup code, PSP
 *      compatible command-tail state, and DOS-style memory scratch space;
 *   3. replace statically proven DOS/BIOS I/O interrupt sites with near calls
 *      into appended ELKS adapter code, while leaving direct hardware access in
 *      place when the program and target machine can reasonably handle it;
 *   4. refuse output in strict mode if an unsupported DOS feature would remain
 *      and could make the result look valid while behaving incorrectly.
 *
 * The comments in the implementation focus on boundary decisions and binary
 * layout details, since those are the parts future maintainers most need to
 * audit when adding a new interrupt adapter or executable shape.
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define COM_ORG             0x100u

#define ELKS_SPLITID        0x04300301ul
#define ELKS_HDR_SIZE       32u
#define ELKS_RELOC_HDR_SIZE 48u

#define ELKS_R_SEGWORD      80u
#define ELKS_S_TEXT         0xfffeu
#define ELKS_S_DATA         0xfffdu

#define NE_MAX_SEGS         5u
#define NE_CODE_CHUNK       0xe000u
#define NE_MZ_STUB_SIZE     0x40u
#define NE_HDR_SIZE         0x40u
#define NESEG_CODE          0x0000u
#define NESEG_DATA          0x0001u
#define NESEG_PRELOAD       0x0040u
#define NESEG_RELOCINFO     0x0100u
#define NEFIXSRC_SEGMENT    0x02u
#define NEFIXSRC_FARADDR    0x03u
#define NEFIXFLG_INTERNAL   0x00u

#define ELKS_MAX16          0xffffu
#define ELKS_MAX_HEAP       0xfff0u
#define ELKS_DEFAULT_STACK  4096u
#define ELKS_DOSISH_STACK   8192u
#define COM_DEFAULT_BSS     32768u
#define NE_ARG_SLACK        512u

#define MZ_MAGIC            0x5a4du
#define ZM_MAGIC            0x4d5au

struct byte_vec
{
  uint8_t *data;
  size_t len;
  size_t cap;
};

struct reloc_rec
{
  uint32_t vaddr;
  uint16_t sym;
};

struct reloc_vec
{
  struct reloc_rec *data;
  size_t len;
  size_t cap;
};

struct ne_reloc_rec
{
  uint16_t src_chain;
  uint8_t src_type;
  uint8_t flags;
  uint8_t segment;
  uint16_t offset;
};

struct ne_reloc_vec
{
  struct ne_reloc_rec *data;
  size_t len;
  size_t cap;
};

struct ne_seg_image
{
  struct byte_vec bytes;
  struct reloc_vec guards;
  struct ne_reloc_vec rels;
  uint32_t mz_base;
  uint32_t mz_len;
  uint16_t flags;
  uint16_t min_alloc;
};

struct options
{
  enum { FMT_AUTO, FMT_COM, FMT_EXE } format;
  enum { MZ_OUT_OS2, MZ_OUT_AOUT, MZ_OUT_AUTO } mz_output;
  int partial;
  int verbose;
  int stack_set;
  int heap_set;
  int bss_set;
  int mz_code_set;
  int mz_data_set;
  uint16_t stack;
  uint16_t heap;
  uint16_t bss;
  uint16_t mz_code_seg;
  uint16_t mz_data_seg;
  const char *input;
  const char *output;
};

struct image
{
  int os2_ne;
  struct byte_vec text;
  struct byte_vec data;
  struct reloc_vec trel;
  struct reloc_vec drel;
  uint16_t entry;
  uint16_t stack;
  uint16_t heap;
  uint16_t bss;
  struct ne_seg_image ne_seg[NE_MAX_SEGS];
  unsigned ne_nsegs;
  unsigned ne_auto_data;
  unsigned ne_entry_seg;
};

struct unsupported_site
{
  uint32_t offset;
  uint8_t intr;
  uint8_t fn;
  int known;
};

struct patch_stats
{
  unsigned patched;
  unsigned unsupported;
  int dynamic_int21;
  int dynamic_int16;
  int bios_keyboard_input;
  int direct_video_output;
  unsigned com_segfix;
  unsigned stackfix;
  struct unsupported_site first[16];
  unsigned first_len;
};

struct runtime_info
{
  uint16_t heap_next_off;
  uint16_t heap_limit_off;
  uint16_t dta_off_off;
  uint16_t video_mode_off;
  uint16_t heap_base_seg_off;
  uint16_t keyboard_fd_off;
  uint16_t keyboard_mode_off;
  uint16_t keyboard_pending_off;
  uint16_t io_buf_off;
  uint16_t media_id_off;
};

struct mz_header
{
  uint16_t magic;
  uint16_t cblp;
  uint16_t cp;
  uint16_t crlc;
  uint16_t cparhdr;
  uint16_t minalloc;
  uint16_t maxalloc;
  uint16_t ss;
  uint16_t sp;
  uint16_t csum;
  uint16_t ip;
  uint16_t cs;
  uint16_t lfarlc;
  uint16_t ovno;
};

enum mz_section
{
  MZ_SEC_NONE,
  MZ_SEC_TEXT,
  MZ_SEC_DATA
};

struct mz_segmap
{
  enum mz_section section;
  uint32_t delta;
};

struct mz_imm_store
{
  size_t start;
  size_t imm;
  uint8_t modrm;
  int32_t disp;
};

static int pklite_reveal_mz (const uint8_t *input, size_t input_len,
                             uint8_t **out, size_t *out_len, int verbose);

#endif /* MSDOS2ELKS_INTERNAL_H */
