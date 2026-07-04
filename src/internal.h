#ifndef MSDOS2ELKS_INTERNAL_H
#define MSDOS2ELKS_INTERNAL_H

/*
 * msdos2elks - rewrite selected MS-DOS binaries as native ELKS programs.
 *
 * The converter is deliberately a static binary rewriter rather than a DOS
 * emulator or a wrapper.  It parses COM and MZ images, removes the DOS load
 * format, emits ELKS Minix a.out for COM inputs and flat-compatible MZ inputs,
 * and keeps OS/2 1.x NE output as an explicit or automatic fallback when a
 * segmented EXE cannot be represented in one native ELKS text/data window.
 *
 * Conversion happens in conservative phases:
 *
 *   1. identify the input shape and reject packed images whose entry bytes
 *      still belong to a loader stub instead of the real program;
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
 * audit when adding a new interrupt adapter or executable shape.  This tree
 * intentionally contains no compression or decompression support; packed DOS
 * programs must be made plain before they reach the converter.
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
  int startup_video_mode_set;
  uint16_t stack;
  uint16_t heap;
  uint16_t bss;
  uint16_t mz_code_seg;
  uint16_t mz_data_seg;
  uint8_t startup_video_mode;
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
  int dynamic_int10;
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
  uint16_t video_restore_mode_off;
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

void die (const char *msg) __attribute__ ((noreturn));
void die_errno (const char *path) __attribute__ ((noreturn));

uint16_t get16 (const uint8_t *p);
void put16 (uint8_t *p, uint16_t v);
void vec_append (struct byte_vec *v, const uint8_t *data, size_t len);
void vec_append_zeros (struct byte_vec *v, size_t len);
void emit8 (struct byte_vec *v, uint8_t b);
void emit16 (struct byte_vec *v, uint16_t w);
void emit32 (struct byte_vec *v, uint32_t l);
int bios_video_mode_needs_console_lock (uint8_t mode);
void reloc_add (struct reloc_vec *v, uint32_t addr, uint16_t sym);
void ne_reloc_add (struct ne_reloc_vec *v, uint16_t src_chain,
                   uint8_t src_type, uint8_t flags, uint8_t segment,
                   uint16_t offset);

void parse_options (int argc, char **argv, struct options *opts);
uint8_t *read_file (const char *path, size_t *len_out);

void convert_com (const uint8_t *input, size_t input_len,
                  const struct options *opts, struct image *img,
                  struct patch_stats *stats);
void convert_mz (const uint8_t *input, size_t input_len,
                 const struct options *opts, struct image *img,
                 struct patch_stats *stats);

void write_elks (const char *path, const struct image *img);
void free_image (struct image *img);

void report_unsupported (const struct patch_stats *stats);
void patch_dos_io (struct byte_vec *text, struct patch_stats *stats,
                   const struct runtime_info *rt,
                   const struct reloc_vec *text_rels);

void patch_com_segment_setup (struct byte_vec *text,
                              struct patch_stats *stats);
void patch_mz_stack_setup (struct byte_vec *text,
                           struct patch_stats *stats);
void patch_dos_stack_switches (struct byte_vec *text,
                               struct patch_stats *stats);
void append_com_argv_startup (struct image *img, int install_int21,
                              uint16_t int21_handler,
                              const struct runtime_info *rt,
                              int raw_keyboard, int direct_video,
                              int install_int16, uint16_t int16_handler,
                              int startup_video_mode_set,
                              uint8_t startup_video_mode);
void install_com_return_exit (struct image *img,
                              const struct runtime_info *rt);
void append_mz_argv_startup (struct image *img, uint16_t original_entry,
                             int install_int21, uint16_t int21_handler,
                             const struct runtime_info *rt,
                             int raw_keyboard, int direct_video,
                             int install_int16, uint16_t int16_handler,
                             int startup_video_mode_set,
                             uint8_t startup_video_mode);
void init_image_memory (struct image *img, const struct options *opts);
uint16_t align_para (uint32_t bytes);
void append_runtime_state_to_data (struct byte_vec *data, uint16_t heap,
                                   uint16_t stack, uint16_t bss,
                                   struct runtime_info *rt);
void append_runtime_state (struct image *img, struct runtime_info *rt);
size_t scan_instruction_len (const uint8_t *p, size_t avail);

void emit_install_int21_vector (struct byte_vec *v, uint16_t handler_off);
void emit_install_int16_vector (struct byte_vec *v, uint16_t handler_off);
void emit_save_initial_video_mode (struct byte_vec *v,
                                   const struct runtime_info *rt);
void emit_startup_video_mode (struct byte_vec *v,
                              const struct runtime_info *rt, uint8_t mode);
void emit_claim_console_video (struct byte_vec *v,
                               const struct runtime_info *rt);
void emit_stdin_raw_mode (struct byte_vec *v,
                          const struct runtime_info *rt);
void emit_exit_stub (struct byte_vec *v, int use_al,
                     const struct runtime_info *rt);
int emit_bios_keyboard_stub_for_fn (struct byte_vec *v, uint8_t fn,
                                    const struct runtime_info *rt);
uint16_t emit_bios_dynamic_video_stub (struct byte_vec *v,
                                       const struct runtime_info *rt);
int emit_stub_for_interrupt (struct byte_vec *v, uint8_t intr, uint8_t fn,
                             const struct runtime_info *rt);
uint16_t append_int21_interrupt_handler (struct byte_vec *text,
                                         const struct runtime_info *rt);
uint16_t append_int16_interrupt_handler (struct byte_vec *text,
                                         const struct runtime_info *rt);

#endif /* MSDOS2ELKS_INTERNAL_H */
