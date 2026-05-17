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
 *   1. identify the input shape and reject packed images that must be
 *      unpacked before the real program can be inspected;
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

static void
die (const char *msg)
{
  fprintf (stderr, "msdos2elks: %s\n", msg);
  exit (1);
}

static void
die_errno (const char *path)
{
  fprintf (stderr, "msdos2elks: %s: %s\n", path, strerror (errno));
  exit (1);
}

static uint16_t
get16 (const uint8_t *p)
{
  return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static void
put16 (uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
}

static void
vec_reserve (struct byte_vec *v, size_t add)
{
  uint8_t *p;
  size_t ncap;

  if (add > SIZE_MAX - v->len)
    die ("buffer size overflow");
  if (v->len + add <= v->cap)
    return;

  ncap = v->cap ? v->cap : 256u;
  while (ncap < v->len + add)
    {
      if (ncap > SIZE_MAX / 2u)
        die ("buffer capacity overflow");
      ncap *= 2u;
    }

  p = (uint8_t *) realloc (v->data, ncap);
  if (!p)
    die ("out of memory");
  v->data = p;
  v->cap = ncap;
}

static void
vec_append (struct byte_vec *v, const uint8_t *data, size_t len)
{
  vec_reserve (v, len);
  memcpy (v->data + v->len, data, len);
  v->len += len;
}

static void
vec_append_zeros (struct byte_vec *v, size_t len)
{
  vec_reserve (v, len);
  memset (v->data + v->len, 0, len);
  v->len += len;
}

static void
emit8 (struct byte_vec *v, uint8_t b)
{
  vec_reserve (v, 1u);
  v->data[v->len++] = b;
}

static void
emit16 (struct byte_vec *v, uint16_t w)
{
  emit8 (v, (uint8_t) w);
  emit8 (v, (uint8_t) (w >> 8));
}

static void
emit32 (struct byte_vec *v, uint32_t l)
{
  emit16 (v, (uint16_t) l);
  emit16 (v, (uint16_t) (l >> 16));
}

static void
reloc_add (struct reloc_vec *v, uint32_t addr, uint16_t sym)
{
  struct reloc_rec *p;
  size_t ncap;

  if (v->len == v->cap)
    {
      ncap = v->cap ? v->cap * 2u : 32u;
      if (ncap < v->cap)
        die ("relocation capacity overflow");
      p = (struct reloc_rec *) realloc (v->data, ncap * sizeof (*v->data));
      if (!p)
        die ("out of memory");
      v->data = p;
      v->cap = ncap;
    }

  v->data[v->len].vaddr = addr;
  v->data[v->len].sym = sym;
  v->len++;
}

static void
ne_reloc_add (struct ne_reloc_vec *v, uint16_t src_chain, uint8_t src_type,
              uint8_t flags, uint8_t segment, uint16_t offset)
{
  struct ne_reloc_rec *p;
  size_t ncap;

  if (v->len == v->cap)
    {
      ncap = v->cap ? v->cap * 2u : 32u;
      if (ncap < v->cap)
        die ("NE relocation capacity overflow");
      p = (struct ne_reloc_rec *) realloc (v->data,
                                           ncap * sizeof (*v->data));
      if (!p)
        die ("out of memory");
      v->data = p;
      v->cap = ncap;
    }

  v->data[v->len].src_chain = src_chain;
  v->data[v->len].src_type = src_type;
  v->data[v->len].flags = flags;
  v->data[v->len].segment = segment;
  v->data[v->len].offset = offset;
  v->len++;
}

static uint16_t
parse_u16 (const char *s, const char *what)
{
  char *end;
  unsigned long v;

  errno = 0;
  v = strtoul (s, &end, 0);
  if (errno || *end || v > ELKS_MAX16)
    {
      fprintf (stderr, "msdos2elks: bad %s '%s'\n", what, s);
      exit (1);
    }
  return (uint16_t) v;
}

static int
parse_prefixed_arg (const char *arg, const char *name, const char **value)
{
  size_t n = strlen (name);

  if (strncmp (arg, name, n) == 0 && arg[n] == '=')
    {
      *value = arg + n + 1u;
      return 1;
    }
  return 0;
}

static void
usage (FILE *out)
{
  fprintf (out,
           "usage: msdos2elks [OPTIONS] INPUT OUTPUT\n"
           "\n"
           "Options:\n"
           "  --format=auto|com|exe\n"
           "  --stack=BYTES\n"
           "  --heap=BYTES\n"
           "  --bss=BYTES\n"
           "  --mz-output=os2|aout|auto\n"
           "  --mz-code-seg=PARA\n"
           "  --mz-data-seg=PARA\n"
           "  --partial\n"
           "  --verbose\n");
}

static void
parse_options (int argc, char **argv, struct options *opts)
{
  int i;
  const char *value;

  memset (opts, 0, sizeof (*opts));
  opts->format = FMT_AUTO;
  opts->mz_output = MZ_OUT_OS2;
  opts->stack = ELKS_DEFAULT_STACK;
  opts->heap = 0;

  for (i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--help") == 0 || strcmp (argv[i], "-h") == 0)
        {
          usage (stdout);
          exit (0);
        }
      else if (strcmp (argv[i], "--partial") == 0)
        opts->partial = 1;
      else if (strcmp (argv[i], "--verbose") == 0)
        opts->verbose = 1;
      else if (parse_prefixed_arg (argv[i], "--format", &value))
        {
          if (strcmp (value, "auto") == 0)
            opts->format = FMT_AUTO;
          else if (strcmp (value, "com") == 0)
            opts->format = FMT_COM;
          else if (strcmp (value, "exe") == 0 || strcmp (value, "mz") == 0)
            opts->format = FMT_EXE;
          else
            die ("--format must be auto, com, or exe");
        }
      else if (parse_prefixed_arg (argv[i], "--stack", &value))
        {
          opts->stack = parse_u16 (value, "stack size");
          opts->stack_set = 1;
        }
      else if (parse_prefixed_arg (argv[i], "--heap", &value))
        {
          opts->heap = parse_u16 (value, "heap size");
          opts->heap_set = 1;
        }
      else if (parse_prefixed_arg (argv[i], "--bss", &value))
        {
          opts->bss = parse_u16 (value, "bss size");
          opts->bss_set = 1;
        }
      else if (parse_prefixed_arg (argv[i], "--mz-output", &value))
        {
          if (strcmp (value, "os2") == 0 || strcmp (value, "ne") == 0)
            opts->mz_output = MZ_OUT_OS2;
          else if (strcmp (value, "aout") == 0
                   || strcmp (value, "minix") == 0)
            opts->mz_output = MZ_OUT_AOUT;
          else if (strcmp (value, "auto") == 0)
            opts->mz_output = MZ_OUT_AUTO;
          else
            die ("--mz-output must be os2, aout, or auto");
        }
      else if (parse_prefixed_arg (argv[i], "--mz-code-seg", &value))
        {
          opts->mz_code_seg = parse_u16 (value, "MZ code segment");
          opts->mz_code_set = 1;
        }
      else if (parse_prefixed_arg (argv[i], "--mz-data-seg", &value))
        {
          opts->mz_data_seg = parse_u16 (value, "MZ data segment");
          opts->mz_data_set = 1;
        }
      else if (argv[i][0] == '-')
        {
          fprintf (stderr, "msdos2elks: unknown option '%s'\n", argv[i]);
          usage (stderr);
          exit (1);
        }
      else if (!opts->input)
        opts->input = argv[i];
      else if (!opts->output)
        opts->output = argv[i];
      else
        die ("too many file names");
    }

  if (!opts->input || !opts->output)
    {
      usage (stderr);
      exit (1);
    }
}

static uint8_t *
read_file (const char *path, size_t *len_out)
{
  FILE *fp;
  long pos;
  size_t got;
  uint8_t *buf;

  fp = fopen (path, "rb");
  if (!fp)
    die_errno (path);
  if (fseek (fp, 0, SEEK_END) != 0)
    die_errno (path);
  pos = ftell (fp);
  if (pos < 0)
    die_errno (path);
  if (fseek (fp, 0, SEEK_SET) != 0)
    die_errno (path);

  buf = (uint8_t *) malloc ((size_t) pos ? (size_t) pos : 1u);
  if (!buf)
    die ("out of memory");
  got = fread (buf, 1, (size_t) pos, fp);
  if (got != (size_t) pos)
    die_errno (path);
  if (fclose (fp) != 0)
    die_errno (path);

  *len_out = (size_t) pos;
  return buf;
}

static void
emit_dos_tail (struct byte_vec *v)
{
  static const uint8_t tail[] = {
    0x85, 0xc0,             /* test ax, ax */
    0x78, 0x02,             /* js error */
    0xf8,                   /* clc */
    0xc3,                   /* ret */
    0xf7, 0xd8,             /* neg ax */
    0xf9,                   /* stc */
    0xc3                    /* ret */
  };

  vec_append (v, tail, sizeof (tail));
}

static void
emit_pushpop_dx_bx (struct byte_vec *v)
{
  emit8 (v, 0x52);          /* push dx */
  emit8 (v, 0x5b);          /* pop bx */
}

static void
emit_save_bx_cx_dx (struct byte_vec *v)
{
  emit8 (v, 0x53);
  emit8 (v, 0x51);
  emit8 (v, 0x52);
}

static void
emit_restore_dx_cx_bx (struct byte_vec *v)
{
  emit8 (v, 0x5a);
  emit8 (v, 0x59);
  emit8 (v, 0x5b);
}

static void
emit_syscall_path1 (struct byte_vec *v, uint16_t nr)
{
  emit_save_bx_cx_dx (v);
  emit_pushpop_dx_bx (v);
  emit8 (v, 0xb8);
  emit16 (v, nr);
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit_restore_dx_cx_bx (v);
  emit_dos_tail (v);
}

static void
emit_success_stub (struct byte_vec *v)
{
  emit8 (v, 0x31);
  emit8 (v, 0xc0);          /* xor ax, ax */
  emit8 (v, 0xf8);          /* clc */
  emit8 (v, 0xc3);
}

static void
emit_clear_carry_stub (struct byte_vec *v)
{
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_fail_stub (struct byte_vec *v)
{
  emit8 (v, 0xb8);
  emit16 (v, 1);            /* invalid function */
  emit8 (v, 0xf9);          /* stc */
  emit8 (v, 0xc3);
}

static void
emit_al_status_stub (struct byte_vec *v, uint8_t status)
{
  emit8 (v, 0xb0);
  emit8 (v, status);
  emit8 (v, 0xc3);
}

static void
emit_get_version_stub (struct byte_vec *v)
{
  emit8 (v, 0xb8);
  emit16 (v, 0x0005);       /* DOS 5.0, AL = major, AH = minor */
  emit8 (v, 0xbb);
  emit16 (v, 0);
  emit8 (v, 0xb9);
  emit16 (v, 0);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_get_date_stub (struct byte_vec *v)
{
  emit8 (v, 0xb8);
  emit16 (v, 2);            /* Tuesday */
  emit8 (v, 0xb9);
  emit16 (v, 1991);
  emit8 (v, 0xba);
  emit16 (v, 0x0101);       /* DH = month, DL = day */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_get_time_stub (struct byte_vec *v)
{
  emit8 (v, 0xb9);
  emit16 (v, 0x0c00);       /* CH = hour, CL = minute */
  emit8 (v, 0xba);
  emit16 (v, 0);            /* DH = second, DL = centisecond */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_file_time_stub (struct byte_vec *v)
{
  emit8 (v, 0xb9);
  emit16 (v, 0x6000);       /* 12:00:00 in DOS packed time */
  emit8 (v, 0xba);
  emit16 (v, 0x1621);       /* 1991-01-01 in DOS packed date */
  emit8 (v, 0x31);
  emit8 (v, 0xc0);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_get_dta_stub (struct byte_vec *v, const struct runtime_info *rt)
{
  emit8 (v, 0x1e);          /* push ds */
  emit8 (v, 0x07);          /* pop es */
  emit8 (v, 0x8b);
  emit8 (v, 0x1e);
  emit16 (v, rt->dta_off_off);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_get_psp_stub (struct byte_vec *v)
{
  emit8 (v, 0x8c);
  emit8 (v, 0xdb);          /* bx = ds, where the PSP-shaped area lives */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_mouse_stub (struct byte_vec *v)
{
  emit8 (v, 0x31);
  emit8 (v, 0xc0);          /* ax = 0, no mouse installed/status clear */
  emit8 (v, 0x31);
  emit8 (v, 0xdb);          /* bx = 0 */
  emit8 (v, 0x31);
  emit8 (v, 0xc9);          /* cx = 0 */
  emit8 (v, 0x31);
  emit8 (v, 0xd2);          /* dx = 0 */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_get_drive_stub (struct byte_vec *v)
{
  emit8 (v, 0x31);
  emit8 (v, 0xc0);          /* A: */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_select_drive_stub (struct byte_vec *v)
{
  emit8 (v, 0xb8);
  emit16 (v, 2);            /* number of drives */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_get_vector_stub (struct byte_vec *v)
{
  emit8 (v, 0x1e);          /* push ds */
  emit8 (v, 0x07);          /* pop es */
  emit8 (v, 0x31);
  emit8 (v, 0xdb);          /* bx = 0 */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_get_disk_free_stub (struct byte_vec *v)
{
  emit8 (v, 0xb8);
  emit16 (v, 1);            /* sectors per cluster */
  emit8 (v, 0xbb);
  emit16 (v, 0x1000);       /* available clusters */
  emit8 (v, 0xb9);
  emit16 (v, 512);          /* bytes per sector */
  emit8 (v, 0xba);
  emit16 (v, 0x1000);       /* total clusters */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_get_cwd_stub (struct byte_vec *v)
{
  emit8 (v, 0x56);          /* push si */
  emit8 (v, 0xc6);
  emit8 (v, 0x04);
  emit8 (v, 0x00);          /* root cwd: empty ASCIIZ path at DS:SI */
  emit8 (v, 0x5e);
  emit_success_stub (v);
}

static void
emit_zero_cx_success_stub (struct byte_vec *v)
{
  emit8 (v, 0x31);
  emit8 (v, 0xc9);          /* xor cx, cx */
  emit_success_stub (v);
}

static void
emit_zero_dx_success_stub (struct byte_vec *v)
{
  emit8 (v, 0x31);
  emit8 (v, 0xd2);          /* xor dx, dx */
  emit_success_stub (v);
}

static void
emit_find_fail_stub (struct byte_vec *v)
{
  emit8 (v, 0xb8);
  emit16 (v, 18);           /* no more files */
  emit8 (v, 0xf9);
  emit8 (v, 0xc3);
}

static void
emit_set_dta_stub (struct byte_vec *v, const struct runtime_info *rt)
{
  emit8 (v, 0x89);
  emit8 (v, 0x16);
  emit16 (v, rt->dta_off_off);
  emit_success_stub (v);
}

static void
emit_find_first_stub (struct byte_vec *v, const struct runtime_info *rt)
{
  static const uint8_t prefix[] = {
    0x53,                   /* push bx */
    0x51,                   /* push cx */
    0x52,                   /* push dx */
    0x56,                   /* push si */
    0x57,                   /* push di */
    0x06,                   /* push es */
    0x1e,                   /* push ds */
    0x07,                   /* pop es */
    0x89, 0xd6,             /* mov si, dx */
    0x89, 0xd3,             /* mov bx, dx */
    0xb9, 0x80, 0x00,       /* scan at most 128 bytes */
    0xac,                   /* scan: lodsb */
    0x08, 0xc0,             /* or al, al */
    0x74, 0x14,             /* jz copy */
    0x3c, 0x5c,             /* cmp al, '\\' */
    0x74, 0x0c,             /* jz mark */
    0x3c, 0x2f,             /* cmp al, '/' */
    0x74, 0x08,             /* jz mark */
    0x3c, 0x3a,             /* cmp al, ':' */
    0x74, 0x04,             /* jz mark */
    0xe2, 0xed,             /* loop scan */
    0xeb, 0x04,             /* jmp copy */
    0x89, 0xf3,             /* mark: mov bx, si */
    0xe2, 0xe7              /* loop scan */
  };
  static const uint8_t suffix[] = {
    0xc7, 0x45, 0x1a, 0x01, 0x00, /* DTA size low word: nonzero */
    0xc7, 0x45, 0x1c, 0x00, 0x00, /* DTA size high word */
    0x83, 0xc7, 0x1e,       /* add di, 30; DTA filename field */
    0x89, 0xde,             /* mov si, bx */
    0xb9, 0x0d, 0x00,       /* mov cx, 13 */
    0xac,                   /* copy_loop: lodsb */
    0xaa,                   /* stosb */
    0x08, 0xc0,             /* or al, al */
    0x74, 0x06,             /* jz done */
    0xe2, 0xf8,             /* loop copy_loop */
    0xc6, 0x45, 0xff, 0x00, /* mov byte [di-1], 0 */
    0x07,                   /* done: pop es */
    0x5f,                   /* pop di */
    0x5e,                   /* pop si */
    0x5a,                   /* pop dx */
    0x59,                   /* pop cx */
    0x5b,                   /* pop bx */
    0x31, 0xc0,             /* xor ax, ax */
    0xf8,                   /* clc */
    0xc3                    /* ret */
  };

  vec_append (v, prefix, sizeof (prefix));
  emit8 (v, 0x8b);
  emit8 (v, 0x3e);
  emit16 (v, rt->dta_off_off);
  vec_append (v, suffix, sizeof (suffix));
}

static void
emit_alloc_stub (struct byte_vec *v, const struct runtime_info *rt)
{
  size_t ja_pos;
  size_t fail_pos;

  emit8 (v, 0x51);          /* push cx */
  emit8 (v, 0x52);          /* push dx */
  emit8 (v, 0x8b);
  emit8 (v, 0x0e);
  emit16 (v, rt->heap_next_off);
  emit8 (v, 0x8b);
  emit8 (v, 0x16);
  emit16 (v, rt->heap_limit_off);
  emit8 (v, 0x89);
  emit8 (v, 0xd0);          /* ax = limit */
  emit8 (v, 0x29);
  emit8 (v, 0xc8);          /* ax = available paragraphs */
  emit8 (v, 0x39);
  emit8 (v, 0xc3);          /* cmp bx, ax */
  emit8 (v, 0x77);          /* ja fail */
  ja_pos = v->len;
  emit8 (v, 0);
  emit8 (v, 0x89);
  emit8 (v, 0xc8);          /* ax = relative allocation paragraph */
  emit8 (v, 0x01);
  emit8 (v, 0xd9);          /* cx += bx */
  emit8 (v, 0x89);
  emit8 (v, 0x0e);
  emit16 (v, rt->heap_next_off);
  emit8 (v, 0x8c);
  emit8 (v, 0xda);          /* dx = ds */
  emit8 (v, 0x01);
  emit8 (v, 0xd0);          /* ax += ds */
  emit8 (v, 0x5a);
  emit8 (v, 0x59);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
  fail_pos = v->len;
  emit8 (v, 0x89);
  emit8 (v, 0xc3);          /* bx = largest available block */
  emit8 (v, 0xb8);
  emit16 (v, 8);            /* insufficient memory */
  emit8 (v, 0x5a);
  emit8 (v, 0x59);
  emit8 (v, 0xf9);
  emit8 (v, 0xc3);
  v->data[ja_pos] = (uint8_t) (fail_pos - (ja_pos + 1u));
}

static void
emit_exit_stub (struct byte_vec *v, int use_al)
{
  if (use_al)
    {
      emit8 (v, 0xb4);
      emit8 (v, 0x00);      /* mov ah, 0 */
      emit8 (v, 0x50);
      emit8 (v, 0x5b);      /* bx = al */
    }
  else
    {
      emit8 (v, 0xbb);
      emit16 (v, 0);        /* bx = 0 */
    }
  emit8 (v, 0xb8);
  emit16 (v, 1);            /* exit */
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit8 (v, 0xc3);
}

static void
emit_open_stub (struct byte_vec *v)
{
  emit_save_bx_cx_dx (v);
  emit8 (v, 0xb4);
  emit8 (v, 0x00);          /* ax = al */
  emit8 (v, 0x50);
  emit8 (v, 0x59);          /* cx = ax */
  emit8 (v, 0x80);
  emit8 (v, 0xe1);
  emit8 (v, 0x03);          /* and cl, 3 */
  emit_pushpop_dx_bx (v);   /* bx = path */
  emit8 (v, 0xba);
  emit16 (v, 0);            /* mode = 0 */
  emit8 (v, 0xb8);
  emit16 (v, 5);            /* open */
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit_restore_dx_cx_bx (v);
  emit_dos_tail (v);
}

static void
emit_create_stub (struct byte_vec *v)
{
  emit_save_bx_cx_dx (v);
  emit_pushpop_dx_bx (v);
  emit8 (v, 0xb9);
  emit16 (v, 0x0241);       /* O_CREAT | O_TRUNC | O_WRONLY */
  emit8 (v, 0xba);
  emit16 (v, 0x01b6);       /* 0666 */
  emit8 (v, 0xb8);
  emit16 (v, 5);            /* open */
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit_restore_dx_cx_bx (v);
  emit_dos_tail (v);
}

static void
emit_close_stub (struct byte_vec *v)
{
  emit8 (v, 0x53);          /* push bx */
  emit8 (v, 0xb8);
  emit16 (v, 6);            /* close */
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit8 (v, 0x5b);          /* pop bx */
  emit_dos_tail (v);
}

static void
emit_readwrite_stub (struct byte_vec *v, uint16_t nr)
{
  emit_save_bx_cx_dx (v);
  emit8 (v, 0x51);
  emit8 (v, 0x52);
  emit8 (v, 0x59);
  emit8 (v, 0x5a);          /* cx = old dx, dx = old cx */
  emit8 (v, 0xb8);
  emit16 (v, nr);
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit_restore_dx_cx_bx (v);
  emit_dos_tail (v);
}

static void
emit_write_char_stub (struct byte_vec *v)
{
  size_t js_pos;
  size_t err_pos;

  emit_save_bx_cx_dx (v);
  emit8 (v, 0xbb);
  emit16 (v, 1);            /* stdout */
  emit8 (v, 0x89);
  emit8 (v, 0xe1);          /* cx = sp, points at saved dx */
  emit8 (v, 0xba);
  emit16 (v, 1);
  emit8 (v, 0xb8);
  emit16 (v, 4);            /* write */
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit8 (v, 0x85);
  emit8 (v, 0xc0);
  emit8 (v, 0x78);
  js_pos = v->len;
  emit8 (v, 0);
  emit8 (v, 0x5a);          /* restore dx */
  emit8 (v, 0x88);
  emit8 (v, 0xd0);          /* al = dl */
  emit8 (v, 0x59);
  emit8 (v, 0x5b);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
  err_pos = v->len;
  emit_restore_dx_cx_bx (v);
  emit8 (v, 0xf7);
  emit8 (v, 0xd8);
  emit8 (v, 0xf9);
  emit8 (v, 0xc3);
  v->data[js_pos] = (uint8_t) (err_pos - (js_pos + 1u));
}

static void
emit_direct_console_stub (struct byte_vec *v)
{
  size_t je_pos;
  size_t status_pos;

  emit8 (v, 0x80);
  emit8 (v, 0xfa);
  emit8 (v, 0xff);          /* cmp dl, 0xff */
  emit8 (v, 0x74);          /* je status */
  je_pos = v->len;
  emit8 (v, 0);
  emit_write_char_stub (v);
  status_pos = v->len;
  emit8 (v, 0x30);
  emit8 (v, 0xc0);          /* xor al, al */
  emit8 (v, 0x38);
  emit8 (v, 0xc0);          /* cmp al, al; ZF set: no key */
  emit8 (v, 0xc3);
  v->data[je_pos] = (uint8_t) (status_pos - (je_pos + 1u));
}

static void
emit_read_char_stub (struct byte_vec *v, int echo)
{
  size_t js_pos;
  size_t err_pos;

  emit_save_bx_cx_dx (v);
  emit8 (v, 0x55);          /* push bp */
  emit8 (v, 0x31);
  emit8 (v, 0xc0);          /* ax = 0 */
  emit8 (v, 0x50);          /* temp byte */
  emit8 (v, 0x89);
  emit8 (v, 0xe5);          /* bp = sp */
  emit8 (v, 0xbb);
  emit16 (v, 0);            /* stdin */
  emit8 (v, 0x89);
  emit8 (v, 0xe9);          /* cx = bp */
  emit8 (v, 0xba);
  emit16 (v, 1);
  emit8 (v, 0xb8);
  emit16 (v, 3);            /* read */
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit8 (v, 0x85);
  emit8 (v, 0xc0);
  emit8 (v, 0x78);
  js_pos = v->len;
  emit8 (v, 0);
  if (echo)
    {
      emit8 (v, 0xbb);
      emit16 (v, 1);
      emit8 (v, 0x89);
      emit8 (v, 0xe9);      /* cx = bp */
      emit8 (v, 0xba);
      emit16 (v, 1);
      emit8 (v, 0xb8);
      emit16 (v, 4);
      emit8 (v, 0xcd);
      emit8 (v, 0x80);
    }
  emit8 (v, 0x8a);
  emit8 (v, 0x46);
  emit8 (v, 0x00);          /* al = [bp] */
  emit8 (v, 0x83);
  emit8 (v, 0xc4);
  emit8 (v, 0x02);          /* discard temp */
  emit8 (v, 0x5d);
  emit_restore_dx_cx_bx (v);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
  err_pos = v->len;
  emit8 (v, 0x83);
  emit8 (v, 0xc4);
  emit8 (v, 0x02);
  emit8 (v, 0x5d);
  emit_restore_dx_cx_bx (v);
  emit8 (v, 0xf7);
  emit8 (v, 0xd8);
  emit8 (v, 0xf9);
  emit8 (v, 0xc3);
  v->data[js_pos] = (uint8_t) (err_pos - (js_pos + 1u));
}

static void
emit_bios_write_al_stub (struct byte_vec *v, int count_from_cx)
{
  size_t jz_pos;
  size_t done_pos;
  size_t loop_pos;
  size_t jnz_pos;

  emit8 (v, 0x50);          /* save ax */
  emit8 (v, 0x53);
  emit8 (v, 0x51);
  emit8 (v, 0x52);
  emit8 (v, 0x57);
  emit8 (v, 0x50);          /* stack byte to write */
  if (count_from_cx)
    {
      emit8 (v, 0x89);
      emit8 (v, 0xcf);      /* di = cx */
    }
  else
    {
      emit8 (v, 0xbf);
      emit16 (v, 1);        /* di = 1 */
    }
  emit8 (v, 0x85);
  emit8 (v, 0xff);          /* test di, di */
  emit8 (v, 0x74);
  jz_pos = v->len;
  emit8 (v, 0);
  loop_pos = v->len;
  emit8 (v, 0xbb);
  emit16 (v, 1);            /* stdout */
  emit8 (v, 0x89);
  emit8 (v, 0xe1);          /* cx = sp */
  emit8 (v, 0xba);
  emit16 (v, 1);
  emit8 (v, 0xb8);
  emit16 (v, 4);            /* write */
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit8 (v, 0x4f);          /* dec di */
  emit8 (v, 0x75);
  jnz_pos = v->len;
  emit8 (v, 0);
  done_pos = v->len;
  emit8 (v, 0x83);
  emit8 (v, 0xc4);
  emit8 (v, 0x02);          /* discard stack byte */
  emit8 (v, 0x5f);
  emit8 (v, 0x5a);
  emit8 (v, 0x59);
  emit8 (v, 0x5b);
  emit8 (v, 0x58);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);

  v->data[jz_pos] = (uint8_t) (done_pos - (jz_pos + 1u));
  v->data[jnz_pos] = (uint8_t) (loop_pos - (jnz_pos + 1u));
}

static void
emit_bios_get_cursor_stub (struct byte_vec *v)
{
  emit8 (v, 0x31);
  emit8 (v, 0xc9);          /* cx = 0 */
  emit8 (v, 0x31);
  emit8 (v, 0xd2);          /* dx = 0 */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_bios_set_video_mode_stub (struct byte_vec *v, const struct runtime_info *rt)
{
  emit8 (v, 0xa2);
  emit16 (v, rt->video_mode_off);  /* mov [video_mode], al */
  emit8 (v, 0xcd);
  emit8 (v, 0x10);                 /* let BIOS switch real hardware */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_bios_get_video_mode_stub (struct byte_vec *v, const struct runtime_info *rt)
{
  emit8 (v, 0xa0);
  emit16 (v, rt->video_mode_off);  /* al = current converted mode */
  emit8 (v, 0xb4);
  emit8 (v, 0x50);                 /* 80 columns */
  emit8 (v, 0x31);
  emit8 (v, 0xdb);                 /* active page 0 */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_bios_read_char_attr_stub (struct byte_vec *v)
{
  emit8 (v, 0xb8);
  emit16 (v, 0x0720);       /* blank character, normal attribute */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_bios_key_status_stub (struct byte_vec *v)
{
  emit8 (v, 0x31);
  emit8 (v, 0xc0);          /* no key available */
  emit8 (v, 0x39);
  emit8 (v, 0xc0);          /* ZF set */
  emit8 (v, 0xc3);
}

static int
emit_bios_mode13_address (struct byte_vec *v)
{
  int i;

  emit8 (v, 0x89);
  emit8 (v, 0xd0);          /* ax = dx (row) */
  emit8 (v, 0x89);
  emit8 (v, 0xc7);          /* di = ax */
  for (i = 0; i < 6; i++)
    {
      emit8 (v, 0xd1);
      emit8 (v, 0xe7);      /* di <<= 1, total row * 64 */
    }
  emit8 (v, 0x89);
  emit8 (v, 0xc6);          /* si = ax */
  for (i = 0; i < 8; i++)
    {
      emit8 (v, 0xd1);
      emit8 (v, 0xe6);      /* si <<= 1, total row * 256 */
    }
  emit8 (v, 0x01);
  emit8 (v, 0xf7);          /* di += si, row * 320 */
  emit8 (v, 0x01);
  emit8 (v, 0xcf);          /* di += cx (column) */
  emit8 (v, 0xb8);
  emit16 (v, 0xa000);       /* mode 13h packed VGA framebuffer */
  emit8 (v, 0x8e);
  emit8 (v, 0xc0);          /* es = ax */

  return 1;
}

static void
emit_bios_write_pixel_stub (struct byte_vec *v, const struct runtime_info *rt)
{
  size_t jne_pos;
  size_t bios_pos;

  emit8 (v, 0x80);
  emit8 (v, 0x3e);
  emit16 (v, rt->video_mode_off);
  emit8 (v, 0x13);          /* cmp byte [video_mode], 13h */
  emit8 (v, 0x75);
  jne_pos = v->len;
  emit8 (v, 0);

  emit8 (v, 0x53);          /* push bx */
  emit8 (v, 0x51);          /* push cx */
  emit8 (v, 0x52);          /* push dx */
  emit8 (v, 0x56);          /* push si */
  emit8 (v, 0x57);          /* push di */
  emit8 (v, 0x06);          /* push es */
  emit8 (v, 0x88);
  emit8 (v, 0xc3);          /* bl = al (color) */
  emit_bios_mode13_address (v);
  emit8 (v, 0x88);
  emit8 (v, 0xd8);          /* al = bl */
  emit8 (v, 0x26);
  emit8 (v, 0x88);
  emit8 (v, 0x05);          /* es:[di] = al */
  emit8 (v, 0x07);          /* pop es */
  emit8 (v, 0x5f);          /* pop di */
  emit8 (v, 0x5e);          /* pop si */
  emit8 (v, 0x5a);          /* pop dx */
  emit8 (v, 0x59);          /* pop cx */
  emit8 (v, 0x5b);          /* pop bx */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);

  bios_pos = v->len;
  emit8 (v, 0xcd);
  emit8 (v, 0x10);          /* preserve BIOS behavior outside mode 13h */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);

  v->data[jne_pos] = (uint8_t) (bios_pos - (jne_pos + 1u));
}

static void
emit_bios_read_pixel_stub (struct byte_vec *v, const struct runtime_info *rt)
{
  size_t jne_pos;
  size_t bios_pos;

  emit8 (v, 0x80);
  emit8 (v, 0x3e);
  emit16 (v, rt->video_mode_off);
  emit8 (v, 0x13);          /* cmp byte [video_mode], 13h */
  emit8 (v, 0x75);
  jne_pos = v->len;
  emit8 (v, 0);

  emit8 (v, 0x53);          /* push bx */
  emit8 (v, 0x51);          /* push cx */
  emit8 (v, 0x52);          /* push dx */
  emit8 (v, 0x56);          /* push si */
  emit8 (v, 0x57);          /* push di */
  emit8 (v, 0x06);          /* push es */
  emit_bios_mode13_address (v);
  emit8 (v, 0x26);
  emit8 (v, 0x8a);
  emit8 (v, 0x05);          /* al = es:[di] */
  emit8 (v, 0x07);          /* pop es */
  emit8 (v, 0x5f);          /* pop di */
  emit8 (v, 0x5e);          /* pop si */
  emit8 (v, 0x5a);          /* pop dx */
  emit8 (v, 0x59);          /* pop cx */
  emit8 (v, 0x5b);          /* pop bx */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);

  bios_pos = v->len;
  emit8 (v, 0xcd);
  emit8 (v, 0x10);          /* preserve BIOS behavior outside mode 13h */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);

  v->data[jne_pos] = (uint8_t) (bios_pos - (jne_pos + 1u));
}

static int
emit_bios_video_stub_for_fn (struct byte_vec *v, uint8_t fn,
                             const struct runtime_info *rt)
{
  switch (fn)
    {
    case 0x00:              /* set video mode */
      emit_bios_set_video_mode_stub (v, rt);
      return 1;
    case 0x01:              /* set cursor shape */
    case 0x02:              /* set cursor position */
    case 0x05:              /* select active display page */
    case 0x06:              /* scroll up */
    case 0x07:              /* scroll down */
    case 0x0b:              /* set palette/background */
      emit_clear_carry_stub (v);
      return 1;
    case 0x03:              /* get cursor position and size */
      emit_bios_get_cursor_stub (v);
      return 1;
    case 0x08:              /* read character/attribute at cursor */
      emit_bios_read_char_attr_stub (v);
      return 1;
    case 0x09:              /* write character/attribute at cursor */
    case 0x0a:              /* write character at cursor */
      emit_bios_write_al_stub (v, 1);
      return 1;
    case 0x0c:              /* write graphics pixel */
      emit_bios_write_pixel_stub (v, rt);
      return 1;
    case 0x0d:              /* read graphics pixel */
      emit_bios_read_pixel_stub (v, rt);
      return 1;
    case 0x0e:              /* teletype output */
      emit_bios_write_al_stub (v, 0);
      return 1;
    case 0x0f:              /* get current video mode */
      emit_bios_get_video_mode_stub (v, rt);
      return 1;
    case 0x12:              /* EGA/VGA alternate select */
    case 0x30:              /* PCjr/MCGA/VGA miscellaneous services */
      emit_clear_carry_stub (v);
      return 1;
    case 0x1a:              /* get display combination code */
      emit8 (v, 0xb8);
      emit16 (v, 0x001a);   /* AL = 1Ah means function supported */
      emit8 (v, 0xbb);
      emit16 (v, 0x0008);   /* color display */
      emit8 (v, 0xf8);
      emit8 (v, 0xc3);
      return 1;
    default:
      return 0;
    }
}

static int
emit_bios_keyboard_stub_for_fn (struct byte_vec *v, uint8_t fn)
{
  switch (fn)
    {
    case 0x00:              /* read key */
    case 0x10:              /* enhanced read key */
      emit_read_char_stub (v, 0);
      return 1;
    case 0x01:              /* check key */
    case 0x11:              /* enhanced check key */
      emit_bios_key_status_stub (v);
      return 1;
    case 0x02:              /* get shift flags */
    case 0x12:
      emit_success_stub (v);
      return 1;
    default:
      return 0;
    }
}

static int
emit_bios_clock_stub_for_fn (struct byte_vec *v, uint8_t fn)
{
  switch (fn)
    {
    case 0x00:              /* get timer ticks */
      emit8 (v, 0x31);
      emit8 (v, 0xc0);      /* no midnight rollover */
      emit8 (v, 0x31);
      emit8 (v, 0xc9);
      emit8 (v, 0x31);
      emit8 (v, 0xd2);      /* tick count 0 */
      emit8 (v, 0xf8);
      emit8 (v, 0xc3);
      return 1;
    case 0x01:              /* set timer ticks */
      emit_clear_carry_stub (v);
      return 1;
    case 0x02:              /* get RTC time, BCD */
      emit8 (v, 0x31);
      emit8 (v, 0xc0);
      emit8 (v, 0xb9);
      emit16 (v, 0x1200);   /* 12:00 */
      emit8 (v, 0x31);
      emit8 (v, 0xd2);
      emit8 (v, 0xf8);
      emit8 (v, 0xc3);
      return 1;
    case 0x04:              /* get RTC date, BCD */
      emit8 (v, 0xb9);
      emit16 (v, 0x1991);
      emit8 (v, 0xba);
      emit16 (v, 0x0101);
      emit8 (v, 0xf8);
      emit8 (v, 0xc3);
      return 1;
    default:
      return 0;
    }
}

static void
emit_string_stub (struct byte_vec *v)
{
  size_t loop_pos;
  size_t je_pos;
  size_t jmp_pos;
  size_t found_pos;

  emit8 (v, 0x53);
  emit8 (v, 0x51);
  emit8 (v, 0x52);
  emit8 (v, 0x56);          /* save si */
  emit8 (v, 0x89);
  emit8 (v, 0xd6);          /* si = dx */
  emit8 (v, 0x31);
  emit8 (v, 0xc9);          /* cx = length */
  loop_pos = v->len;
  emit8 (v, 0x80);
  emit8 (v, 0x3c);
  emit8 (v, 0x24);          /* cmpb '$', [si] */
  emit8 (v, 0x74);
  je_pos = v->len;
  emit8 (v, 0);
  emit8 (v, 0x46);          /* inc si */
  emit8 (v, 0x41);          /* inc cx */
  emit8 (v, 0xeb);
  jmp_pos = v->len;
  emit8 (v, 0);
  found_pos = v->len;
  emit8 (v, 0xbb);
  emit16 (v, 1);            /* stdout */
  emit8 (v, 0x51);
  emit8 (v, 0x52);
  emit8 (v, 0x59);
  emit8 (v, 0x5a);          /* cx = old dx, dx = old cx */
  emit8 (v, 0xb8);
  emit16 (v, 4);
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit8 (v, 0x5e);
  emit_restore_dx_cx_bx (v);
  emit_dos_tail (v);

  v->data[je_pos] = (uint8_t) (found_pos - (je_pos + 1u));
  v->data[jmp_pos] = (uint8_t) (loop_pos - (jmp_pos + 1u));
}

static void
emit_lseek_stub (struct byte_vec *v)
{
  size_t js_pos;
  size_t err_pos;

  emit8 (v, 0x53);
  emit8 (v, 0x51);
  emit8 (v, 0x52);
  emit8 (v, 0x51);          /* high word */
  emit8 (v, 0x52);          /* low word, sp points here */
  emit8 (v, 0x89);
  emit8 (v, 0xe1);          /* cx = sp */
  emit8 (v, 0xb4);
  emit8 (v, 0x00);
  emit8 (v, 0x89);
  emit8 (v, 0xc2);          /* dx = origin */
  emit8 (v, 0xb8);
  emit16 (v, 19);           /* lseek */
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit8 (v, 0x85);
  emit8 (v, 0xc0);
  emit8 (v, 0x78);
  js_pos = v->len;
  emit8 (v, 0);
  emit8 (v, 0x58);          /* ax = low */
  emit8 (v, 0x5a);          /* dx = high */
  emit8 (v, 0x83);
  emit8 (v, 0xc4);
  emit8 (v, 0x02);          /* discard saved dx */
  emit8 (v, 0x59);
  emit8 (v, 0x5b);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
  err_pos = v->len;
  emit8 (v, 0x83);
  emit8 (v, 0xc4);
  emit8 (v, 0x04);          /* discard temp offset */
  emit_restore_dx_cx_bx (v);
  emit8 (v, 0xf7);
  emit8 (v, 0xd8);
  emit8 (v, 0xf9);
  emit8 (v, 0xc3);
  v->data[js_pos] = (uint8_t) (err_pos - (js_pos + 1u));
}

static void
emit_rename_stub (struct byte_vec *v)
{
  emit_save_bx_cx_dx (v);
  emit_pushpop_dx_bx (v);
  emit8 (v, 0x57);
  emit8 (v, 0x59);          /* cx = di, assumes es == ds */
  emit8 (v, 0xb8);
  emit16 (v, 38);           /* rename */
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit_restore_dx_cx_bx (v);
  emit_dos_tail (v);
}

static int
emit_stub_for_fn (struct byte_vec *v, uint8_t fn,
                  const struct runtime_info *rt)
{
  switch (fn)
    {
    case 0x00:
      emit_exit_stub (v, 0);
      return 1;
    case 0x01:
      emit_read_char_stub (v, 1);
      return 1;
    case 0x02:
      emit_write_char_stub (v);
      return 1;
    case 0x06:
      emit_direct_console_stub (v);
      return 1;
    case 0x07:
    case 0x08:
      emit_read_char_stub (v, 0);
      return 1;
    case 0x09:
      emit_string_stub (v);
      return 1;
    case 0x0d:
      emit_success_stub (v);
      return 1;
    case 0x0f:              /* FCB open: fail closed, no native handle */
      emit_al_status_stub (v, 0xff);
      return 1;
    case 0x10:              /* FCB close: no-op success */
      emit_al_status_stub (v, 0);
      return 1;
    case 0x11:              /* FCB find first */
    case 0x12:              /* FCB find next */
    case 0x13:              /* FCB delete */
      emit_al_status_stub (v, 0xff);
      return 1;
    case 0x14:              /* FCB sequential read */
      emit_al_status_stub (v, 1);       /* EOF */
      return 1;
    case 0x15:              /* FCB sequential write */
      emit_al_status_stub (v, 1);
      return 1;
    case 0x0b:
      emit_success_stub (v);
      return 1;
    case 0x0e:
      emit_select_drive_stub (v);
      return 1;
    case 0x19:
      emit_get_drive_stub (v);
      return 1;
    case 0x1b:
    case 0x1c:
      emit_get_disk_free_stub (v);
      return 1;
    case 0x1a:
      emit_set_dta_stub (v, rt);
      return 1;
    case 0x21:              /* FCB random read */
      emit_al_status_stub (v, 1);
      return 1;
    case 0x22:              /* FCB random write */
    case 0x23:              /* FCB get file size */
      emit_al_status_stub (v, 0xff);
      return 1;
    case 0x27:              /* FCB random block read */
      emit_al_status_stub (v, 1);
      return 1;
    case 0x28:              /* FCB random block write */
    case 0x29:              /* parse filename into FCB */
      emit_al_status_stub (v, 0xff);
      return 1;
    case 0x2a:
      emit_get_date_stub (v);
      return 1;
    case 0x2c:
      emit_get_time_stub (v);
      return 1;
    case 0x2b:              /* set date */
    case 0x2d:              /* set time */
      emit_success_stub (v);
      return 1;
    case 0x2e:
      emit_success_stub (v);
      return 1;
    case 0x2f:
      emit_get_dta_stub (v, rt);
      return 1;
    case 0x25:
      emit_success_stub (v);
      return 1;
    case 0x30:
      emit_get_version_stub (v);
      return 1;
    case 0x33:
      emit_zero_dx_success_stub (v);
      return 1;
    case 0x35:
      emit_get_vector_stub (v);
      return 1;
    case 0x36:
      emit_get_disk_free_stub (v);
      return 1;
    case 0x39:
      emit_syscall_path1 (v, 39);
      return 1;
    case 0x3a:
      emit_syscall_path1 (v, 40);
      return 1;
    case 0x3b:
      emit_syscall_path1 (v, 12);
      return 1;
    case 0x3c:
      emit_create_stub (v);
      return 1;
    case 0x3d:
      emit_open_stub (v);
      return 1;
    case 0x3e:
      emit_close_stub (v);
      return 1;
    case 0x3f:
      emit_readwrite_stub (v, 3);
      return 1;
    case 0x40:
      emit_readwrite_stub (v, 4);
      return 1;
    case 0x41:
      emit_syscall_path1 (v, 10);
      return 1;
    case 0x42:
      emit_lseek_stub (v);
      return 1;
    case 0x43:
      emit_zero_cx_success_stub (v);
      return 1;
    case 0x44:
      emit_zero_dx_success_stub (v);
      return 1;
    case 0x47:
      emit_get_cwd_stub (v);
      return 1;
    case 0x4c:
      emit_exit_stub (v, 1);
      return 1;
    case 0x4d:              /* get child return code */
      emit_success_stub (v);
      return 1;
    case 0x4e:
      emit_find_first_stub (v, rt);
      return 1;
    case 0x4f:
      emit_find_fail_stub (v);
      return 1;
    case 0x48:
      emit_alloc_stub (v, rt);
      return 1;
    case 0x49:
    case 0x4a:
      emit_success_stub (v);
      return 1;
    case 0x54:
      emit_success_stub (v);
      return 1;
    case 0x56:
      emit_rename_stub (v);
      return 1;
    case 0x57:
      emit_file_time_stub (v);
      return 1;
    case 0x62:
      emit_get_psp_stub (v);
      return 1;
    default:
      emit_fail_stub (v);
      return 1;
    }
}

static int
emit_mouse_stub_for_fn (struct byte_vec *v, uint8_t fn)
{
  (void) fn;
  emit_mouse_stub (v);
  return 1;
}

static int
emit_stub_for_interrupt (struct byte_vec *v, uint8_t intr, uint8_t fn,
                         const struct runtime_info *rt)
{
  switch (intr)
    {
    case 0x10:
      return emit_bios_video_stub_for_fn (v, fn, rt);
    case 0x16:
      return emit_bios_keyboard_stub_for_fn (v, fn);
    case 0x1a:
      return emit_bios_clock_stub_for_fn (v, fn);
    case 0x21:
      return emit_stub_for_fn (v, fn, rt);
    case 0x33:
      return emit_mouse_stub_for_fn (v, fn);
    default:
      return 0;
    }
}

static void
emit_install_int21_vector (struct byte_vec *v, uint16_t handler_off)
{
  emit8 (v, 0x1e);          /* push ds */
  emit8 (v, 0x50);          /* push ax */
  emit8 (v, 0x31);
  emit8 (v, 0xc0);          /* xor ax, ax */
  emit8 (v, 0x8e);
  emit8 (v, 0xd8);          /* mov ds, ax */
  emit8 (v, 0xfa);          /* cli */
  emit8 (v, 0xc7);
  emit8 (v, 0x06);
  emit16 (v, 0x0084);
  emit16 (v, handler_off);  /* IVT[21h].offset */
  emit8 (v, 0x8c);
  emit8 (v, 0xc8);          /* mov ax, cs */
  emit8 (v, 0xa3);
  emit16 (v, 0x0086);       /* IVT[21h].segment */
  emit8 (v, 0xfb);          /* sti */
  emit8 (v, 0x58);          /* pop ax */
  emit8 (v, 0x1f);          /* pop ds */
}

static uint16_t
append_int21_interrupt_handler (struct byte_vec *text,
                                const struct runtime_info *rt)
{
  static const uint8_t fns[] = {
    0x00, 0x01, 0x02, 0x06, 0x07, 0x08, 0x09, 0x0b,
    0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
    0x15, 0x19, 0x1a, 0x1b, 0x1c, 0x21, 0x22, 0x23,
    0x25, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
    0x2e, 0x2f, 0x30, 0x33, 0x35, 0x36, 0x39, 0x3a,
    0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42,
    0x43, 0x44, 0x47, 0x48, 0x49, 0x4a, 0x4c, 0x4d,
    0x4e, 0x4f, 0x54, 0x56, 0x57, 0x62
  };
  struct fixup
  {
    size_t call_rel;
    size_t finish_rel;
  };
  struct fixup fixups[sizeof (fns) / sizeof (fns[0])];
  size_t start = text->len;
  size_t unsupported;
  size_t finish;
  size_t done_jmp;
  size_t clear;
  size_t i;

  if (start > ELKS_MAX16)
    die ("text segment grew beyond 64 KiB while adding int 21h handler");

  emit8 (text, 0x55);       /* push bp */
  emit8 (text, 0x89);
  emit8 (text, 0xe5);       /* mov bp, sp */

  for (i = 0; i < sizeof (fns) / sizeof (fns[0]); i++)
    {
      emit8 (text, 0x80);
      emit8 (text, 0xfc);
      emit8 (text, fns[i]); /* cmp ah, imm8 */
      emit8 (text, 0x75);
      emit8 (text, 0x06);   /* jne over call+jmp */
      emit8 (text, 0xe8);   /* call DOS function adapter */
      fixups[i].call_rel = text->len;
      emit16 (text, 0);
      emit8 (text, 0xe9);   /* jmp finish */
      fixups[i].finish_rel = text->len;
      emit16 (text, 0);
    }

  unsupported = text->len;
  (void) unsupported;
  emit8 (text, 0xb8);
  emit16 (text, 1);         /* unsupported DOS function */
  emit8 (text, 0xf9);       /* stc */

  finish = text->len;
  emit8 (text, 0x73);       /* jnc clear */
  emit8 (text, 0x06);
  emit8 (text, 0x83);
  emit8 (text, 0x4e);
  emit8 (text, 0x06);
  emit8 (text, 0x01);       /* or word [bp+6], 1 */
  emit8 (text, 0xeb);
  done_jmp = text->len;
  emit8 (text, 0);
  clear = text->len;
  emit8 (text, 0x83);
  emit8 (text, 0x66);
  emit8 (text, 0x06);
  emit8 (text, 0xfe);       /* and word [bp+6], 0xfffe */
  text->data[done_jmp] = (uint8_t) (text->len - (done_jmp + 1u));
  emit8 (text, 0x5d);       /* pop bp */
  emit8 (text, 0xcf);       /* iret */

  for (i = 0; i < sizeof (fns) / sizeof (fns[0]); i++)
    {
      size_t stub = text->len;
      uint16_t rel;

      if (!emit_stub_for_fn (text, fns[i], rt))
        die ("internal int 21h handler dispatch table mismatch");
      if (stub > ELKS_MAX16 || text->len > ELKS_MAX16)
        die ("text segment grew beyond 64 KiB while adding int 21h handler");

      rel = (uint16_t) (stub - (fixups[i].call_rel + 2u));
      put16 (text->data + fixups[i].call_rel, rel);
      rel = (uint16_t) (finish - (fixups[i].finish_rel + 2u));
      put16 (text->data + fixups[i].finish_rel, rel);
    }

  (void) clear;
  return (uint16_t) start;
}

static void
record_unsupported (struct patch_stats *stats, uint32_t offset, uint8_t intr,
                    int known, uint8_t fn)
{
  stats->unsupported++;
  if (stats->first_len < sizeof (stats->first) / sizeof (stats->first[0]))
    {
      stats->first[stats->first_len].offset = offset;
      stats->first[stats->first_len].intr = intr;
      stats->first[stats->first_len].known = known;
      stats->first[stats->first_len].fn = fn;
      stats->first_len++;
    }
}

static void
report_unsupported (const struct patch_stats *stats)
{
  unsigned i;

  for (i = 0; i < stats->first_len; i++)
    {
      if (stats->first[i].known)
        fprintf (stderr,
                 "msdos2elks: unsupported int %02xh AH=%02x"
                 " at text offset %04x\n",
                 stats->first[i].intr,
                 stats->first[i].fn, (unsigned) stats->first[i].offset);
      else if (stats->first[i].intr == 0x10
               || stats->first[i].intr == 0x16
               || stats->first[i].intr == 0x1a
               || stats->first[i].intr == 0x21)
        fprintf (stderr,
                 "msdos2elks: unsupported int %02xh with non-adjacent"
                 " or dynamic AH at text offset %04x\n",
                 stats->first[i].intr, (unsigned) stats->first[i].offset);
      else
        fprintf (stderr,
                 "msdos2elks: unsupported interrupt %02xh at text offset %04x\n",
                 stats->first[i].intr, (unsigned) stats->first[i].offset);
    }
  if (stats->unsupported > stats->first_len)
    fprintf (stderr, "msdos2elks: plus %u more unsupported interrupt sites\n",
             stats->unsupported - stats->first_len);
}

static void
patch_call (struct byte_vec *text, size_t start, size_t len, size_t target,
            int al_known, uint8_t al)
{
  uint16_t rel;
  size_t i;

  if (al_known && len == 5u)
    {
      rel = (uint16_t) (target - (start + 5u));
      text->data[start] = 0xb0;         /* mov al, imm8 */
      text->data[start + 1u] = al;
      text->data[start + 2u] = 0xe8;    /* call rel16 */
      put16 (text->data + start + 3u, rel);
      return;
    }

  rel = (uint16_t) (target - (start + 3u));
  text->data[start] = 0xe8;
  put16 (text->data + start + 1u, rel);
  for (i = 3u; i < len; i++)
    text->data[start + i] = 0x90;
}

static int
range_has_text_reloc (const struct reloc_vec *rels, size_t start, size_t end)
{
  size_t i;

  if (!rels)
    return 0;
  for (i = 0; i < rels->len; i++)
    if (rels->data[i].vaddr >= start && rels->data[i].vaddr < end)
      return 1;
  return 0;
}

static size_t
modrm_instruction_len (const uint8_t *p, size_t avail, size_t imm)
{
  uint8_t modrm;
  uint8_t mod;
  uint8_t rm;
  size_t len;

  if (avail < 1u)
    return 0;
  modrm = p[0];
  mod = (uint8_t) (modrm >> 6);
  rm = (uint8_t) (modrm & 7u);
  len = 1u + imm;
  if (mod == 0 && rm == 6)
    len += 2u;
  else if (mod == 1)
    len += 1u;
  else if (mod == 2)
    len += 2u;
  return len <= avail ? len : 0;
}

static size_t
movable_instruction_len (const uint8_t *p, size_t avail)
{
  uint8_t op;
  uint8_t reg;

  if (avail == 0)
    return 0;
  op = p[0];

  if (op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e
      || op == 0xf2 || op == 0xf3)
    {
      size_t n = movable_instruction_len (p + 1u, avail - 1u);
      return n ? n + 1u : 0;
    }

  if ((op >= 0x50 && op <= 0x5f) || (op >= 0x40 && op <= 0x4f)
      || op == 0x06 || op == 0x07 || op == 0x0e || op == 0x16
      || op == 0x17 || op == 0x1e || op == 0x1f || op == 0x27
      || op == 0x2f || op == 0x37 || op == 0x3f || op == 0x90
      || op == 0x98 || op == 0x99 || op == 0x9c || op == 0x9d
      || op == 0xa4 || op == 0xa5 || op == 0xaa || op == 0xab
      || op == 0xac || op == 0xad || op == 0xae || op == 0xaf
      || op == 0xf8 || op == 0xf9 || op == 0xfa || op == 0xfb
      || op == 0xfc || op == 0xfd)
    return 1;

  if (op >= 0xb0 && op <= 0xb7)
    return avail >= 2u ? 2u : 0;
  if (op >= 0xb8 && op <= 0xbf)
    return avail >= 3u ? 3u : 0;

  switch (op)
    {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3a: case 0x3b:
    case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8a: case 0x8b:
    case 0x8c: case 0x8d: case 0x8e: case 0x8f:
    case 0xc4: case 0xc5:
    case 0xd0: case 0xd1: case 0xd2: case 0xd3:
      {
        size_t n = modrm_instruction_len (p + 1u, avail - 1u, 0);
        return n ? n + 1u : 0;
      }

    case 0x04: case 0x0c: case 0x14: case 0x1c:
    case 0x24: case 0x2c: case 0x34: case 0x3c:
    case 0xa8:
      return avail >= 2u ? 2u : 0;
    case 0x05: case 0x0d: case 0x15: case 0x1d:
    case 0x25: case 0x2d: case 0x35: case 0x3d:
    case 0xa9:
      return avail >= 3u ? 3u : 0;

    case 0xa0: case 0xa1: case 0xa2: case 0xa3:
      return avail >= 3u ? 3u : 0;
    case 0xc6:
      {
        size_t n = modrm_instruction_len (p + 1u, avail - 1u, 1);
        return n ? n + 1u : 0;
      }
    case 0xc7:
      {
        size_t n = modrm_instruction_len (p + 1u, avail - 1u, 2);
        return n ? n + 1u : 0;
      }
    case 0x80: case 0x82: case 0x83:
      {
        size_t n = modrm_instruction_len (p + 1u, avail - 1u, 1);
        return n ? n + 1u : 0;
      }
    case 0x81:
      {
        size_t n = modrm_instruction_len (p + 1u, avail - 1u, 2);
        return n ? n + 1u : 0;
      }
    case 0xf6:
      if (avail < 2u)
        return 0;
      reg = (uint8_t) ((p[1] >> 3) & 7u);
      {
        size_t n = modrm_instruction_len (p + 1u, avail - 1u,
                                          reg <= 1u ? 1u : 0);
        return n ? n + 1u : 0;
      }
    case 0xf7:
      if (avail < 2u)
        return 0;
      reg = (uint8_t) ((p[1] >> 3) & 7u);
      {
        size_t n = modrm_instruction_len (p + 1u, avail - 1u,
                                          reg <= 1u ? 2u : 0);
        return n ? n + 1u : 0;
      }
    case 0xfe:
      {
        size_t n = modrm_instruction_len (p + 1u, avail - 1u, 0);
        return n ? n + 1u : 0;
      }
    case 0xff:
      if (avail < 2u)
        return 0;
      reg = (uint8_t) ((p[1] >> 3) & 7u);
      if (reg >= 2u && reg <= 5u)
        return 0;
      {
        size_t n = modrm_instruction_len (p + 1u, avail - 1u, 0);
        return n ? n + 1u : 0;
      }
    default:
      return 0;
    }
}

static int
range_is_movable_code (const uint8_t *data, size_t from, size_t to)
{
  size_t pos = from;

  while (pos < to)
    {
      size_t n = movable_instruction_len (data + pos, to - pos);
      if (n == 0)
        {
          uint8_t op = data[pos];
          size_t target;

          if (((op >= 0x70 && op <= 0x7f) || op == 0xeb
               || (op >= 0xe0 && op <= 0xe3))
              && pos + 1u < to)
            {
              target = (size_t) ((int32_t) pos + 2
                                 + (int8_t) data[pos + 1u]);
              if (target >= from && target <= to)
                n = 2u;
            }
        }
      if (n == 0 || pos + n > to)
        return 0;
      pos += n;
    }
  return pos == to;
}

static int
target_in_range (size_t target, size_t from, size_t to)
{
  return target >= from && target < to;
}

static size_t scan_instruction_len (const uint8_t *p, size_t avail);

static int
has_external_branch_target (const uint8_t *data, size_t len, size_t body_from,
                            size_t body_to, size_t whole_from,
                            size_t whole_to)
{
  size_t i;

  for (i = 0; i < len; )
    {
      uint8_t op = data[i];
      size_t insn_len = scan_instruction_len (data + i, len - i);
      size_t target;

      if (insn_len == 0)
        insn_len = 1u;
      if (i >= whole_from && i < whole_to)
        {
          i += insn_len;
          continue;
        }
      if ((op >= 0x70 && op <= 0x7f) || op == 0xeb
          || (op >= 0xe0 && op <= 0xe3))
        {
          if (i + 1u >= len)
            {
              i += insn_len;
              continue;
            }
          target = (size_t) ((int32_t) i + 2
                             + (int8_t) data[i + 1u]);
          if (target_in_range (target, body_from, body_to))
            return 1;
        }
      else if (op == 0xe8 || op == 0xe9)
        {
          int16_t rel;

          if (i + 2u >= len)
            {
              i += insn_len;
              continue;
            }
          rel = (int16_t) get16 (data + i + 1u);
          target = (size_t) ((int32_t) i + 3 + rel);
          if (target_in_range (target, body_from, body_to))
            return 1;
        }
      i += insn_len;
    }
  return 0;
}

static void
patch_compacted_call (struct byte_vec *text, size_t start, size_t body_start,
                      size_t int_off, size_t target, int al_known, uint8_t al)
{
  size_t body_len = int_off - body_start;
  size_t out = start;
  uint16_t rel;

  memmove (text->data + out, text->data + body_start, body_len);
  out += body_len;
  if (al_known)
    {
      text->data[out++] = 0xb0;         /* mov al, imm8 */
      text->data[out++] = al;
    }
  rel = (uint16_t) (target - (out + 3u));
  text->data[out++] = 0xe8;
  put16 (text->data + out, rel);
  out += 2u;
  while (out < int_off + 2u)
    text->data[out++] = 0x90;
}

static int
find_compact_call_candidate (const struct byte_vec *text, size_t original_len,
                             size_t int_off, const struct reloc_vec *rels,
                             size_t *start_out, size_t *body_out,
                             uint8_t *fn_out, int *al_known_out,
                             uint8_t *al_out)
{
  size_t limit;
  size_t k;

  limit = int_off > 48u ? int_off - 48u : 0;
  for (k = int_off; k-- > limit; )
    {
      size_t body_start;
      int al_known = 0;
      uint8_t al = 0;
      uint8_t fn;

      if (text->data[k] == 0xb4 && k + 2u <= int_off)
        {
          body_start = k + 2u;
          fn = text->data[k + 1u];
        }
      else if (text->data[k] == 0xb8 && k + 3u <= int_off)
        {
          body_start = k + 3u;
          al = text->data[k + 1u];
          fn = text->data[k + 2u];
          al_known = 1;
        }
      else
        continue;

      if (!range_is_movable_code (text->data, body_start, int_off))
        continue;
      if (range_has_text_reloc (rels, body_start, int_off))
        continue;
      if (has_external_branch_target (text->data, original_len, body_start,
                                      int_off, k, int_off + 2u))
        continue;

      *start_out = k;
      *body_out = body_start;
      *fn_out = fn;
      *al_known_out = al_known;
      *al_out = al;
      return 1;
    }
  return 0;
}

static size_t
scan_instruction_len (const uint8_t *p, size_t avail)
{
  uint8_t op;
  uint8_t reg;
  size_t n;

  if (avail == 0)
    return 0;
  op = p[0];
  if (op == 0xf0 || op == 0xf2 || op == 0xf3 || op == 0x26
      || op == 0x2e || op == 0x36 || op == 0x3e)
    {
      n = scan_instruction_len (p + 1u, avail - 1u);
      return n ? n + 1u : 1u;
    }

  n = movable_instruction_len (p, avail);
  if (n)
    return n;

  if ((op >= 0x70 && op <= 0x7f) || op == 0xeb
      || (op >= 0xe0 && op <= 0xe3) || op == 0xcd
      || op == 0xce || op == 0xd4 || op == 0xd5
      || (op >= 0xe4 && op <= 0xe7))
    return avail >= 2u ? 2u : 1u;
  if (op == 0xe8 || op == 0xe9 || op == 0xc2 || op == 0xca
      || op == 0x68)
    return avail >= 3u ? 3u : 1u;
  if (op == 0x9a || op == 0xea)
    return avail >= 5u ? 5u : 1u;
  if (op == 0xc3 || op == 0xcb || op == 0xcc || op == 0xcf
      || op == 0xd6 || op == 0xd7 || (op >= 0xec && op <= 0xef)
      || op == 0xf4 || op == 0xf5)
    return 1u;
  if (op == 0x6a)
    return avail >= 2u ? 2u : 1u;
  if (op == 0x60 || op == 0x61 || (op >= 0x6c && op <= 0x6f)
      || op == 0xc9)
    return 1u;
  if (op == 0x62)
    {
      n = modrm_instruction_len (p + 1u, avail - 1u, 0);
      return n ? n + 1u : 1u;
    }
  if (op == 0x69 || op == 0x6b)
    {
      n = modrm_instruction_len (p + 1u, avail - 1u, op == 0x69 ? 2u : 1u);
      return n ? n + 1u : 1u;
    }
  if (op == 0xc0 || op == 0xc1)
    {
      n = modrm_instruction_len (p + 1u, avail - 1u, 1u);
      return n ? n + 1u : 1u;
    }
  if (op == 0xc8)
    return avail >= 4u ? 4u : 1u;
  if (op >= 0xd8 && op <= 0xdf)
    {
      if (avail < 2u)
        return 1u;
      reg = (uint8_t) ((p[1] >> 3) & 7u);
      (void) reg;
      n = modrm_instruction_len (p + 1u, avail - 1u, 0);
      return n ? n + 1u : 1u;
    }

  return 1u;
}

static int
patchable_interrupt_bank (uint8_t intr)
{
  switch (intr)
    {
    case 0x10:
      return 0;
    case 0x16:
      return 1;
    case 0x1a:
      return 2;
    case 0x21:
      return 3;
    case 0x33:
      return 4;
    default:
      return -1;
    }
}

static int
is_reported_interrupt (uint8_t intr)
{
  switch (intr)
    {
    case 0x13:              /* BIOS disk */
    case 0x14:              /* BIOS serial */
    case 0x15:              /* BIOS services */
    case 0x17:              /* BIOS printer */
    case 0x20:              /* DOS terminate */
    case 0x29:              /* DOS fast console output */
    case 0x2f:              /* DOS multiplex */
    case 0x33:              /* mouse */
      return 1;
    default:
      return 0;
    }
}

static void
patch_dos_io (struct byte_vec *text, struct patch_stats *stats,
              const struct runtime_info *rt, const struct reloc_vec *text_relocs)
{
  uint32_t stubs[5][256];
  uint8_t *covered;
  size_t original_len;
  size_t i;
  unsigned bank;

  original_len = text->len;
  covered = (uint8_t *) calloc (original_len ? original_len : 1u, 1u);
  if (!covered)
    die ("out of memory");
  for (bank = 0; bank < 5u; bank++)
    for (i = 0; i < 256u; i++)
      stubs[bank][i] = UINT32_MAX;

  for (i = 0; i + 1u < original_len; )
    {
      uint8_t intr;
      size_t insn_len;
      size_t start = 0;
      size_t len = 0;
      uint8_t fn = 0;
      uint8_t al = 0;
      int al_known = 0;
      int compact = 0;
      size_t body_start = 0;
      int sbank;
      size_t j;

      insn_len = scan_instruction_len (text->data + i, original_len - i);
      if (insn_len == 0)
        insn_len = 1u;
      if (text->data[i] != 0xcd)
        {
          i += insn_len;
          continue;
        }
      intr = text->data[i + 1u];
      sbank = patchable_interrupt_bank (intr);
      if (sbank < 0)
        {
          if (intr == 0x20)
            {
              i += insn_len;
              continue;
            }
          if (is_reported_interrupt (intr))
            record_unsupported (stats, (uint32_t) i, intr, 0, 0);
          i += insn_len;
          continue;
        }

      if (covered[i] || covered[i + 1u])
        {
          i += insn_len;
          continue;
        }

      if (intr == 0x33 && i >= 3u && text->data[i - 3u] == 0xb8)
        {
          start = i - 3u;
          len = 5u;
          fn = 0;
        }
      else if (intr == 0x33 && i >= 2u
               && ((text->data[i - 2u] == 0x31
                    && text->data[i - 1u] == 0xc0)
                   || (text->data[i - 2u] == 0x29
                       && text->data[i - 1u] == 0xc0)))
        {
          start = i - 2u;
          len = 4u;
          fn = 0;
        }
      else if (i >= 2u && text->data[i - 2u] == 0xb4)
        {
          start = i - 2u;
          len = 4u;
          fn = text->data[i - 1u];
        }
      else if (i >= 3u && text->data[i - 3u] == 0xb8)
        {
          start = i - 3u;
          len = 5u;
          al = text->data[i - 2u];
          al_known = 1;
          fn = text->data[i - 1u];
        }
      else if (find_compact_call_candidate (text, original_len, i,
                                            text_relocs, &start, &body_start,
                                            &fn, &al_known, &al))
        {
          len = i + 2u - start;
          compact = 1;
        }
      else
        {
          if (intr == 0x21)
            {
              stats->dynamic_int21 = 1;
              i += insn_len;
              continue;
            }
          if (intr == 0x10 || intr == 0x16 || intr == 0x1a || intr == 0x33)
            {
              i += insn_len;
              continue;
            }
          record_unsupported (stats, (uint32_t) i, intr, 0, 0);
          i += insn_len;
          continue;
        }

      for (j = start; j < start + len; j++)
        {
          if (covered[j])
            {
              record_unsupported (stats, (uint32_t) i, intr, 1, fn);
              start = len = 0;
              break;
            }
        }
      if (!len)
        {
          i += insn_len;
          continue;
        }

      if (stubs[sbank][fn] == UINT32_MAX)
        {
          size_t stub_off = text->len;
          if (!emit_stub_for_interrupt (text, intr, fn, rt))
            {
              if (intr == 0x10 || intr == 0x16 || intr == 0x1a || intr == 0x33)
                {
                  i += insn_len;
                  continue;
                }
              record_unsupported (stats, (uint32_t) i, intr, 1, fn);
              i += insn_len;
              continue;
            }
          if (stub_off > ELKS_MAX16 || text->len > ELKS_MAX16)
            die ("text segment grew beyond 64 KiB while adding ELKS stubs");
          stubs[sbank][fn] = (uint32_t) stub_off;
        }

      if (compact)
        patch_compacted_call (text, start, body_start, i, stubs[sbank][fn],
                              al_known, al);
      else
        patch_call (text, start, len, stubs[sbank][fn], al_known, al);
      for (j = start; j < start + len; j++)
        covered[j] = 1;
      stats->patched++;
      i += insn_len;
    }

  free (covered);
}

static void
patch_com_segment_setup (struct byte_vec *text, struct patch_stats *stats)
{
  size_t i;

  for (i = COM_ORG; i + 1u < text->len; i++)
    {
      if (text->data[i] == 0x0e && text->data[i + 1u] == 0x1f)
        {
          text->data[i] = 0x90;         /* push cs; pop ds -> nop; nop */
          text->data[i + 1u] = 0x90;
          stats->com_segfix++;
        }
      else if (text->data[i] == 0x0e && text->data[i + 1u] == 0x07)
        {
          text->data[i] = 0x1e;         /* push cs; pop es -> push ds; pop es */
          stats->com_segfix++;
        }
      else if (text->data[i] == 0x2e
               && (text->data[i + 1u] == 0x89
                   || text->data[i + 1u] == 0x8b
                   || text->data[i + 1u] == 0x8e
                   || text->data[i + 1u] == 0xa1
                   || text->data[i + 1u] == 0xa3
                   || text->data[i + 1u] == 0xc7
                   || text->data[i + 1u] == 0xff))
        {
          text->data[i] = 0x3e;         /* cs: data access -> ds: */
          stats->com_segfix++;
        }
      else if (text->data[i] == 0x8c
               && (text->data[i + 1u] & 0xf8u) == 0xc8u)
        {
          uint8_t reg = (uint8_t) (text->data[i + 1u] & 7u);
          size_t end = i + 32u < text->len ? i + 32u : text->len;
          size_t j;

          for (j = i + 2u; j + 1u < end; j++)
            if (text->data[j] == 0x8e
                && text->data[j + 1u] == (uint8_t) (0xd8u | reg))
              {
                text->data[i + 1u] = (uint8_t) (0xd8u | reg);
                stats->com_segfix++;
                break;
              }
        }
      else if (i + 3u < text->len
               && text->data[i] == 0x8c && text->data[i + 1u] == 0xc8
               && text->data[i + 2u] == 0x8e && text->data[i + 3u] == 0xd8)
        {
          text->data[i] = 0x90;         /* mov ax,cs; mov ds,ax -> nops */
          text->data[i + 1u] = 0x90;
          text->data[i + 2u] = 0x90;
          text->data[i + 3u] = 0x90;
          stats->com_segfix++;
          i += 3u;
        }
      else if (i + 3u < text->len
               && text->data[i] == 0x8c && text->data[i + 1u] == 0xc8
               && text->data[i + 2u] == 0x8e && text->data[i + 3u] == 0xc0)
        {
          text->data[i] = 0x8c;         /* mov ax,cs; mov es,ax */
          text->data[i + 1u] = 0xd8;    /* -> mov ax,ds; mov es,ax */
          stats->com_segfix++;
          i += 3u;
        }
      else if (i + 3u < text->len
               && text->data[i] == 0x1e
               && ((text->data[i + 1u] == 0x31 && text->data[i + 2u] == 0xc0)
                   || (text->data[i + 1u] == 0x33
                       && text->data[i + 2u] == 0xc0))
               && text->data[i + 3u] == 0x50)
        {
          text->data[i] = 0x0e;         /* push ds; xor ax,ax; push ax */
          stats->com_segfix++;          /* -> push cs; xor ax,ax; push ax */
          i += 3u;
        }
    }
}

static void
patch_mz_stack_setup (struct byte_vec *text, struct patch_stats *stats)
{
  size_t i;

  for (i = 0; i + 17u < text->len; i++)
    {
      uint8_t reg;
      uint8_t sub;
      uint8_t movss;

      if (text->data[i] != 0x8c || text->data[i + 1u] != 0xd3)
        continue;           /* mov bx, ss */

      sub = text->data[i + 3u];
      movss = text->data[i + 14u];
      if (text->data[i + 2u] == 0x2b && sub == 0xd8 && movss == 0xd8)
        reg = 0;            /* ax */
      else if (text->data[i + 2u] == 0x2b && sub == 0xda && movss == 0xd2)
        reg = 2;            /* dx */
      else
        continue;
      (void) reg;

      if (text->data[i + 4u] != 0xd1 || text->data[i + 5u] != 0xe3
          || text->data[i + 6u] != 0xd1 || text->data[i + 7u] != 0xe3
          || text->data[i + 8u] != 0xd1 || text->data[i + 9u] != 0xe3
          || text->data[i + 10u] != 0xd1 || text->data[i + 11u] != 0xe3
          || text->data[i + 12u] != 0xfa
          || text->data[i + 13u] != 0x8e
          || text->data[i + 15u] != 0x03 || text->data[i + 16u] != 0xe3
          || text->data[i + 17u] != 0xfb)
        continue;

      memset (text->data + i, 0x90, 18u);
      stats->stackfix++;
      i += 17u;
    }
}

static void
patch_dos_stack_switches (struct byte_vec *text, struct patch_stats *stats)
{
  size_t i;

  for (i = 0; i + 5u < text->len; i++)
    {
      uint8_t modrm;
      size_t len;

      if (text->data[i] != 0xfa || text->data[i + 1u] != 0x8e)
        continue;
      modrm = text->data[i + 2u];
      if ((modrm & 0xf8u) != 0xd0u)     /* mov ss, r16 */
        continue;

      if (text->data[i + 3u] == 0x8b
          && (text->data[i + 4u] & 0xf8u) == 0xe0u
          && i + 5u < text->len && text->data[i + 5u] == 0xfb)
        len = 6u;                       /* mov sp, r16 */
      else if (text->data[i + 3u] == 0xbc
               && i + 6u < text->len && text->data[i + 6u] == 0xfb)
        len = 7u;                       /* mov sp, imm16 */
      else
        continue;

      memset (text->data + i, 0x90, len);
      stats->stackfix++;
      i += len - 1u;
    }
}

static void
append_com_argv_startup (struct image *img, int install_int21,
                         uint16_t int21_handler)
{
  static const uint8_t prefix[] = {
    0x55,                   /* push bp */
    0x89, 0xe5,             /* mov bp, sp */
    0x50,                   /* push ax */
    0x53,                   /* push bx */
    0x51,                   /* push cx */
    0x52,                   /* push dx */
    0x56,                   /* push si */
    0x57,                   /* push di */
    0x06,                   /* push es */
    0x1e,                   /* push ds */
    0x07,                   /* pop es; stosb writes the PSP in data */
    0xbf, 0x81, 0x00,       /* mov di, 0081h */
    0x31, 0xc9,             /* xor cx, cx */
    0x8b, 0x56, 0x02,       /* mov dx, [bp+2]; argc */
    0x83, 0xfa, 0x01,       /* cmp dx, 1 */
    0x76, 0x31,             /* jbe done */
    0x4a,                   /* dec dx; remaining argv count */
    0x8d, 0x76, 0x06,       /* lea si, [bp+6]; argv[1] */
    0xb0, 0x20,             /* DOS tails normally begin with a space */
    0xaa,                   /* stosb */
    0x41,                   /* inc cx */
    0x36, 0x8b, 0x1c,       /* arg_loop: mov bx, ss:[si] */
    0x09, 0xdb,             /* or bx, bx */
    0x74, 0x22,             /* jz done */
    0x36, 0x8a, 0x07,       /* copy_loop: mov al, ss:[bx] */
    0x08, 0xc0,             /* or al, al */
    0x74, 0x0a,             /* jz arg_done */
    0x83, 0xf9, 0x7e,       /* cmp cx, 126 */
    0x73, 0x16,             /* jae done */
    0xaa,                   /* stosb */
    0x43,                   /* inc bx */
    0x41,                   /* inc cx */
    0xeb, 0xef,             /* jmp copy_loop */
    0x83, 0xc6, 0x02,       /* arg_done: add si, 2 */
    0x4a,                   /* dec dx */
    0x74, 0x0b,             /* jz done */
    0x83, 0xf9, 0x7e,       /* cmp cx, 126 */
    0x73, 0x06,             /* jae done */
    0xb0, 0x20,             /* mov al, ' ' */
    0xaa,                   /* stosb */
    0x41,                   /* inc cx */
    0xeb, 0xd7,             /* jmp arg_loop */
    0x88, 0x0e, 0x80, 0x00, /* done: mov [0080h], cl */
    0xb0, 0x0d,             /* mov al, 0dh */
    0xaa,                   /* stosb */
    0x58,                   /* discard old es; leave es == ds for COM */
    0x5f,                   /* pop di */
    0x5e,                   /* pop si */
    0x5a,                   /* pop dx */
    0x59,                   /* pop cx */
    0x5b,                   /* pop bx */
    0x58,                   /* pop ax */
    0x5d,                   /* pop bp */
    0x0e,                   /* DOS far-ret exit segment */
    0xff, 0x36, 0x02, 0x00  /* DOS near-ret exit word from PSP+2 */
  };
  size_t start = img->text.len;
  uint16_t rel;

  if (install_int21)
    emit_install_int21_vector (&img->text, int21_handler);
  vec_append (&img->text, prefix, sizeof (prefix));
  if (start > ELKS_MAX16 || img->text.len + 3u > ELKS_MAX16)
    die ("text segment grew beyond 64 KiB while adding COM argv startup");

  emit8 (&img->text, 0xe9);        /* jmp COM_ORG */
  rel = (uint16_t) (COM_ORG - (img->text.len + 2u));
  emit16 (&img->text, rel);
  img->entry = (uint16_t) start;
}

static void
install_com_return_exit (struct image *img)
{
  size_t exit_off;
  uint16_t rel;

  if (img->text.len < 3u)
    die ("COM text segment is too small for return-exit trampoline");
  exit_off = img->text.len;
  emit_exit_stub (&img->text, 0);
  if (exit_off > ELKS_MAX16 || img->text.len > ELKS_MAX16)
    die ("text segment grew beyond 64 KiB while adding COM return exit");

  img->text.data[0] = 0xe9;        /* ret to 0000h -> native exit */
  rel = (uint16_t) (exit_off - 3u);
  put16 (img->text.data + 1u, rel);
}

static void
append_mz_argv_startup (struct image *img, uint16_t original_entry,
                        int install_int21, uint16_t int21_handler)
{
  static const uint8_t prefix[] = {
    0x55,                   /* push bp */
    0x89, 0xe5,             /* mov bp, sp */
    0x50,                   /* push ax */
    0x53,                   /* push bx */
    0x51,                   /* push cx */
    0x52,                   /* push dx */
    0x56,                   /* push si */
    0x57,                   /* push di */
    0x16,                   /* push ss */
    0x07,                   /* pop es */
    0xbf, 0x81, 0x00,       /* mov di, 0081h */
    0x31, 0xc9,             /* xor cx, cx */
    0x8b, 0x56, 0x02,       /* mov dx, [bp+2]; argc */
    0x83, 0xfa, 0x01,       /* cmp dx, 1 */
    0x76, 0x31,             /* jbe done */
    0x4a,                   /* dec dx */
    0x8d, 0x76, 0x06,       /* lea si, [bp+6]; argv[1] */
    0xb0, 0x20,             /* command tails begin with a space */
    0xaa,                   /* stosb */
    0x41,                   /* inc cx */
    0x36, 0x8b, 0x1c,       /* arg_loop: mov bx, ss:[si] */
    0x09, 0xdb,             /* or bx, bx */
    0x74, 0x22,             /* jz done */
    0x36, 0x8a, 0x07,       /* copy_loop: mov al, ss:[bx] */
    0x08, 0xc0,             /* or al, al */
    0x74, 0x0a,             /* jz arg_done */
    0x83, 0xf9, 0x7e,       /* cmp cx, 126 */
    0x73, 0x16,             /* jae done */
    0xaa,                   /* stosb */
    0x43,                   /* inc bx */
    0x41,                   /* inc cx */
    0xeb, 0xef,             /* jmp copy_loop */
    0x83, 0xc6, 0x02,       /* arg_done: add si, 2 */
    0x4a,                   /* dec dx */
    0x74, 0x0b,             /* jz done */
    0x83, 0xf9, 0x7e,       /* cmp cx, 126 */
    0x73, 0x06,             /* jae done */
    0xb0, 0x20,             /* mov al, ' ' */
    0xaa,                   /* stosb */
    0x41,                   /* inc cx */
    0xeb, 0xd7,             /* jmp arg_loop */
    0x26, 0x88, 0x0e, 0x80, 0x00, /* done: mov es:[0080h], cl */
    0xb0, 0x0d,             /* mov al, 0dh */
    0xaa,                   /* stosb */
    0x5f,                   /* pop di */
    0x5e,                   /* pop si */
    0x5a,                   /* pop dx */
    0x59,                   /* pop cx */
    0x5b,                   /* pop bx */
    0x58,                   /* pop ax */
    0x16,                   /* push ss */
    0x1f,                   /* pop ds */
    0x16,                   /* push ss */
    0x07,                   /* pop es */
    0x5d                    /* pop bp */
  };
  size_t start = img->text.len;
  uint16_t rel;

  if (install_int21)
    emit_install_int21_vector (&img->text, int21_handler);
  vec_append (&img->text, prefix, sizeof (prefix));
  if (start > ELKS_MAX16 || img->text.len + 3u > ELKS_MAX16)
    die ("text segment grew beyond 64 KiB while adding MZ argv startup");

  emit8 (&img->text, 0xe9);        /* jmp original MZ entry */
  rel = (uint16_t) (original_entry - (img->text.len + 2u));
  emit16 (&img->text, rel);
  img->entry = (uint16_t) start;
}

static void
init_image_memory (struct image *img, const struct options *opts)
{
  img->stack = opts->stack;
  img->heap = opts->heap;
  img->bss = opts->bss;
}

static uint16_t
align_para (uint32_t bytes)
{
  return (uint16_t) ((bytes + 15u) >> 4);
}

static void
append_runtime_state_to_data (struct byte_vec *data, uint16_t heap,
                              uint16_t stack, uint16_t bss,
                              struct runtime_info *rt)
{
  uint32_t runtime_end;
  uint32_t limit_bytes;
  uint16_t next_para;
  uint16_t limit_para;

  if (data->len + 8u > ELKS_MAX16)
    die ("data segment is too large for converter runtime state");

  rt->heap_next_off = (uint16_t) data->len;
  rt->heap_limit_off = (uint16_t) (data->len + 2u);
  rt->dta_off_off = (uint16_t) (data->len + 4u);
  rt->video_mode_off = (uint16_t) (data->len + 6u);
  emit16 (data, 0);
  emit16 (data, 0);
  emit16 (data, 0x80);
  emit16 (data, 0x0003);

  runtime_end = (uint32_t) data->len;
  next_para = align_para (runtime_end + bss);

  if (heap >= ELKS_MAX_HEAP)
    {
      if (stack >= ELKS_MAX_HEAP)
        limit_bytes = runtime_end + bss;
      else
        limit_bytes = ELKS_MAX_HEAP - stack;
    }
  else
    {
      uint32_t heap_bytes = heap ? heap : 4096u;
      limit_bytes = runtime_end + bss + heap_bytes;
    }

  if (limit_bytes > ELKS_MAX16)
    limit_bytes = ELKS_MAX16;
  limit_para = (uint16_t) (limit_bytes >> 4);
  if (limit_para < next_para)
    limit_para = next_para;

  put16 (data->data + rt->heap_next_off, next_para);
  put16 (data->data + rt->heap_limit_off, limit_para);
}

static void
append_runtime_state (struct image *img, struct runtime_info *rt)
{
  append_runtime_state_to_data (&img->data, img->heap, img->stack, img->bss,
                                rt);
}

static void
convert_com (const uint8_t *input, size_t input_len, const struct options *opts,
             struct image *img, struct patch_stats *stats)
{
  struct runtime_info rt;
  uint32_t low_mem;
  uint32_t reserve;
  uint32_t used;
  uint32_t avail;

  if (input_len + COM_ORG > ELKS_MAX16)
    die ("COM image is too large for an ELKS 16-bit segment");

  init_image_memory (img, opts);
  img->entry = COM_ORG;

  vec_append_zeros (&img->text, COM_ORG);
  vec_append (&img->text, input, input_len);

  vec_append_zeros (&img->data, COM_ORG);
  img->data.data[0] = 0xcd;
  img->data.data[1] = 0x20;
  img->data.data[0x80] = 0;
  img->data.data[0x81] = '\r';
  vec_append (&img->data, input, input_len);

  low_mem = opts->bss_set ? img->bss : COM_DEFAULT_BSS;
  reserve = img->stack;
  if (opts->heap_set && img->heap < ELKS_MAX_HEAP)
    reserve += img->heap;
  used = (uint32_t) img->data.len + 6u;
  if (reserve >= ELKS_MAX_HEAP || used >= ELKS_MAX_HEAP - reserve)
    die ("COM image leaves no room for runtime memory");
  avail = ELKS_MAX_HEAP - reserve - used;
  if (low_mem > avail)
    {
      if (opts->bss_set)
        die ("COM bss request leaves no room for stack/heap");
      low_mem = avail;
    }
  vec_append_zeros (&img->data, low_mem);
  img->bss = 0;

  append_runtime_state (img, &rt);
  patch_com_segment_setup (&img->text, stats);
  patch_dos_stack_switches (&img->text, stats);
  patch_dos_io (&img->text, stats, &rt, NULL);
  if (stats->dynamic_int21)
    {
      uint16_t handler = append_int21_interrupt_handler (&img->text, &rt);

      install_com_return_exit (img);
      append_com_argv_startup (img, 1, handler);
    }
  else
    {
      install_com_return_exit (img);
      append_com_argv_startup (img, 0, 0);
    }
}

static void
read_mz_header (const uint8_t *input, size_t input_len, struct mz_header *h)
{
  if (input_len < 28u)
    die ("MZ input is too small");

  h->magic = get16 (input + 0);
  h->cblp = get16 (input + 2);
  h->cp = get16 (input + 4);
  h->crlc = get16 (input + 6);
  h->cparhdr = get16 (input + 8);
  h->minalloc = get16 (input + 10);
  h->maxalloc = get16 (input + 12);
  h->ss = get16 (input + 14);
  h->sp = get16 (input + 16);
  h->csum = get16 (input + 18);
  h->ip = get16 (input + 20);
  h->cs = get16 (input + 22);
  h->lfarlc = get16 (input + 24);
  h->ovno = get16 (input + 26);

  if (h->magic != MZ_MAGIC && h->magic != ZM_MAGIC)
    die ("input is not an MZ executable");
}

static int
has_ascii (const uint8_t *p, size_t len, const char *needle)
{
  size_t n = strlen (needle);
  size_t i;

  if (n == 0 || n > len)
    return 0;
  for (i = 0; i + n <= len; i++)
    if (memcmp (p + i, needle, n) == 0)
      return 1;
  return 0;
}

static void
reject_known_mz_packers (const uint8_t *input, size_t input_len,
                         const uint8_t *image, size_t image_len,
                         const struct mz_header *h)
{
  uint32_t entry = ((uint32_t) h->cs << 4) + h->ip;
  size_t scan_len = input_len < 256u ? input_len : 256u;

  if (has_ascii (input, scan_len, "LZ91")
      || has_ascii (input, scan_len, "LZ09"))
    die ("MZ executable appears to be LZEXE packed; unpack it before conversion");

  if (has_ascii (input, input_len, "PKLITE"))
    die ("MZ executable appears to be PKLITE packed; unpack it before conversion");

  if (has_ascii (input, input_len, "PKWARE Data Compression Library"))
    die ("MZ executable appears to be a compressed installer/SFX; extract it before conversion");

  if (entry + 4u <= image_len
      && image[entry] == 0x0e && image[entry + 1u] == 0x1f
      && image[entry + 2u] == 0x8b && image[entry + 3u] == 0x0e)
    die ("MZ executable appears to be EXEPACK/LZ-style packed; unpack it before conversion");

  if (entry + 5u <= image_len
      && image[entry] == 0x06 && image[entry + 1u] == 0x0e
      && image[entry + 2u] == 0x1f && image[entry + 3u] == 0x8b
      && image[entry + 4u] == 0x0e)
    die ("MZ executable appears to be EXEPACK/LZ-style packed; unpack it before conversion");
}

static size_t
mz_file_size (const struct mz_header *h, size_t input_len)
{
  uint32_t size;

  if (h->cp == 0)
    return input_len;

  size = ((uint32_t) h->cp - 1u) * 512u;
  size += h->cblp ? h->cblp : 512u;
  if (size > input_len)
    die ("MZ header file size is larger than input file");
  return (size_t) size;
}

static unsigned
mz_data_segment_load_score (const uint8_t *image, size_t image_len,
                            uint32_t loc)
{
  uint8_t op;
  uint8_t reg;
  uint8_t modrm;
  uint8_t sreg;

  if (loc < 1u || loc + 3u >= image_len)
    return 0;

  op = image[loc - 1u];
  if (op < 0xb8 || op > 0xbf || image[loc + 2u] != 0x8e)
    return 0;

  reg = (uint8_t) (op - 0xb8u);
  modrm = image[loc + 3u];
  if ((modrm & 0xc7u) != (uint8_t) (0xc0u | reg))
    return 0;

  sreg = (uint8_t) ((modrm >> 3) & 3u);
  if (sreg == 3u)           /* DS */
    return 2;
  if (sreg == 0u)           /* ES */
    return 1;
  return 0;
}

static uint16_t
guess_mz_data_para (const uint8_t *image, size_t image_len,
                    const uint8_t *file, const struct mz_header *h,
                    uint16_t code_para)
{
  uint16_t best = h->ss;
  uint16_t load_best = h->ss;
  unsigned best_count = 0;
  unsigned load_best_count = 0;
  uint16_t chosen = h->ss;
  uint16_t i;

  for (i = 0; i < h->crlc; i++)
    {
      size_t rpos = (size_t) h->lfarlc + (size_t) i * 4u;
      uint16_t off = get16 (file + rpos);
      uint16_t seg = get16 (file + rpos + 2u);
      uint32_t loc = ((uint32_t) seg << 4) + off;
      uint16_t val;
      uint16_t j;
      unsigned count = 0;
      unsigned load_count = 0;

      if (loc + 1u >= image_len)
        continue;
      val = get16 (image + loc);
      if (val == code_para)
        continue;

      for (j = 0; j < h->crlc; j++)
        {
          size_t rpos2 = (size_t) h->lfarlc + (size_t) j * 4u;
          uint16_t off2 = get16 (file + rpos2);
          uint16_t seg2 = get16 (file + rpos2 + 2u);
          uint32_t loc2 = ((uint32_t) seg2 << 4) + off2;

          if (loc2 + 1u < image_len && get16 (image + loc2) == val)
            {
              count++;
              load_count += mz_data_segment_load_score (image, image_len, loc2);
            }
        }
      if ((uint32_t) val * 16u < image_len && load_count > load_best_count)
        {
          load_best_count = load_count;
          load_best = val;
        }
      if (count > best_count)
        {
          best_count = count;
          best = val;
        }
    }

  if ((uint32_t) chosen * 16u > image_len || chosen == code_para)
    chosen = load_best_count ? load_best : (best_count ? best : h->ss);

  return chosen;
}

static uint16_t
mz_heap_from_minalloc (uint16_t minalloc)
{
  uint32_t bytes = (uint32_t) minalloc << 4;

  if (bytes >= ELKS_MAX_HEAP)
    return ELKS_MAX_HEAP;
  return (uint16_t) bytes;
}

static struct mz_segmap
map_mz_segment_value (uint16_t val, uint16_t code_para,
                      uint16_t data_para, size_t text_len, size_t data_len)
{
  struct mz_segmap m;
  uint32_t delta;

  m.section = MZ_SEC_NONE;
  m.delta = 0;

  if (val >= code_para)
    {
      delta = ((uint32_t) val - code_para) << 4;
      if (delta < text_len)
        {
          m.section = MZ_SEC_TEXT;
          m.delta = delta;
          return m;
        }
    }

  if (val >= data_para)
    {
      delta = ((uint32_t) val - data_para) << 4;
      if (delta < data_len)
        {
          m.section = MZ_SEC_DATA;
          m.delta = delta;
          return m;
        }
    }

  return m;
}

static int
reloc_site_is_far_call_or_jump (const uint8_t *section, size_t loc)
{
  if (loc < 3u)
    return 0;
  return section[loc - 3u] == 0x9a || section[loc - 3u] == 0xea;
}

static int
reloc_site_is_split_far_pointer (const uint8_t *section, size_t loc)
{
  if (loc < 4u)
    return 0;

  return section[loc - 4u] >= 0xb8 && section[loc - 4u] <= 0xbf
         && section[loc - 1u] >= 0xb8 && section[loc - 1u] <= 0xbf;
}

static int
decode_mz_imm_store_at_start (const uint8_t *section, size_t section_len,
                              size_t start, struct mz_imm_store *store)
{
  uint8_t modrm;
  uint8_t mod;
  uint8_t rm;
  size_t disp_len;
  size_t disp_pos;
  size_t imm_pos;
  int32_t disp;

  if (start + 4u > section_len || section[start] != 0xc7)
    return 0;

  modrm = section[start + 1u];
  if ((modrm & 0x38u) != 0 || (modrm & 0xc0u) == 0xc0u)
    return 0;

  mod = (uint8_t) (modrm >> 6);
  rm = (uint8_t) (modrm & 7u);
  disp_len = 0;
  disp = 0;
  disp_pos = start + 2u;

  if (mod == 0 && rm == 6u)
    {
      disp_len = 2;
      if (disp_pos + 1u >= section_len)
        return 0;
      disp = get16 (section + disp_pos);
    }
  else if (mod == 1)
    {
      disp_len = 1;
      if (disp_pos >= section_len)
        return 0;
      disp = (int8_t) section[disp_pos];
    }
  else if (mod == 2)
    {
      disp_len = 2;
      if (disp_pos + 1u >= section_len)
        return 0;
      disp = get16 (section + disp_pos);
    }

  imm_pos = start + 2u + disp_len;
  if (imm_pos + 1u >= section_len)
    return 0;

  store->start = start;
  store->imm = imm_pos;
  store->modrm = modrm;
  store->disp = disp;
  return 1;
}

static int
decode_mz_imm_store_at_imm (const uint8_t *section, size_t section_len,
                            size_t loc, struct mz_imm_store *store)
{
  size_t start;
  size_t min_start;

  min_start = loc > 6u ? loc - 6u : 0;
  for (start = min_start; start <= loc && start + 4u <= section_len; start++)
    if (decode_mz_imm_store_at_start (section, section_len, start, store)
        && store->imm == loc)
      return 1;

  return 0;
}

static int
same_mz_store_base (const struct mz_imm_store *a,
                    const struct mz_imm_store *b)
{
  return (a->modrm & 0xc7u) == (b->modrm & 0xc7u);
}

static void
adjust_mz_far_offset_word (uint8_t *section, size_t loc, uint32_t delta)
{
  uint32_t off;

  if (delta == 0)
    return;
  if (delta > ELKS_MAX16)
    die ("MZ segmented relocation offset is outside ELKS flat window");

  off = get16 (section + loc) + delta;
  if (off > ELKS_MAX16)
    die ("MZ far relocation offset overflows 16 bits after flattening");
  put16 (section + loc, (uint16_t) off);
}

static int
adjust_mz_stored_far_pointer (uint8_t *section, size_t section_len,
                              size_t loc, uint32_t delta)
{
  struct mz_imm_store seg_store;
  struct mz_imm_store off_store;
  size_t start;
  size_t end;

  if (delta == 0
      || !decode_mz_imm_store_at_imm (section, section_len, loc, &seg_store))
    return 0;

  start = seg_store.start > 16u ? seg_store.start - 16u : 0;
  end = seg_store.start + 16u < section_len
        ? seg_store.start + 16u : section_len;

  for (; start < end; start++)
    {
      if (start == seg_store.start)
        continue;
      if (!decode_mz_imm_store_at_start (section, section_len, start,
                                         &off_store))
        continue;
      if (same_mz_store_base (&seg_store, &off_store)
          && off_store.disp + 2 == seg_store.disp)
        {
          adjust_mz_far_offset_word (section, off_store.imm, delta);
          return 1;
        }
    }

  return 0;
}

static int
mz_opcode_uses_modrm (uint8_t op)
{
  switch (op)
    {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3a: case 0x3b:
    case 0x62: case 0x69: case 0x6b:
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8a: case 0x8b:
    case 0x8c: case 0x8d: case 0x8e: case 0x8f:
    case 0xc0: case 0xc1: case 0xc4: case 0xc5:
    case 0xc6: case 0xc7:
    case 0xd0: case 0xd1: case 0xd2: case 0xd3:
    case 0xd8: case 0xd9: case 0xda: case 0xdb:
    case 0xdc: case 0xdd: case 0xde: case 0xdf:
    case 0xf6: case 0xf7: case 0xfe: case 0xff:
      return 1;
    default:
      return 0;
    }
}

static int
adjust_mz_direct_ref (uint8_t *section, size_t start, size_t len,
                      uint32_t delta)
{
  size_t p = start;
  size_t end = start + len;
  int seg_override = -1;
  uint8_t op;

  while (p < end)
    {
      switch (section[p])
        {
        case 0x26:
          seg_override = 0;     /* ES */
          break;
        case 0x2e:
          seg_override = 1;     /* CS */
          break;
        case 0x36:
          seg_override = 2;     /* SS */
          break;
        case 0x3e:
          seg_override = 3;     /* DS */
          break;
        case 0xf0:
        case 0xf2:
        case 0xf3:
          break;
        default:
          goto opcode;
        }
      p++;
    }

 opcode:
  if (p >= end)
    return 0;
  if (seg_override >= 0 && seg_override != 3)
    return 0;

  op = section[p];
  if (op >= 0xa0 && op <= 0xa3)
    {
      if (p + 2u >= end)
        return 0;
      adjust_mz_far_offset_word (section, p + 1u, delta);
      return 1;
    }

  if (mz_opcode_uses_modrm (op) && p + 3u < end
      && (section[p + 1u] & 0xc7u) == 0x06u)
    {
      adjust_mz_far_offset_word (section, p + 2u, delta);
      return 1;
    }

  return 0;
}

static int
mz_instruction_transfers_control (const uint8_t *section, size_t pos,
                                  size_t len)
{
  uint8_t op;

  if (len == 0)
    return 1;
  while (len > 1u
         && (section[pos] == 0xf0 || section[pos] == 0xf2
             || section[pos] == 0xf3 || section[pos] == 0x26
             || section[pos] == 0x2e || section[pos] == 0x36
             || section[pos] == 0x3e))
    {
      pos++;
      len--;
    }

  op = section[pos];
  if (op == 0xcd || op == 0xe8 || op == 0xe9 || op == 0xea
      || op == 0x9a || op == 0xc2 || op == 0xc3
      || op == 0xca || op == 0xcb || op == 0xcf)
    return 1;
  if (op == 0xff && len >= 2u)
    {
      uint8_t reg = (uint8_t) ((section[pos + 1u] >> 3) & 7u);

      if (reg >= 2u && reg <= 5u)
        return 1;
    }
  return 0;
}

static int
adjust_mz_ds_subsegment_block (uint8_t *section, size_t section_len,
                               size_t loc, uint32_t delta)
{
  uint8_t reg;
  size_t pos;
  size_t limit;
  unsigned adjusted = 0;

  if (delta == 0 || loc < 1u || loc + 3u >= section_len)
    return 0;
  if (section[loc - 1u] < 0xb8 || section[loc - 1u] > 0xbf)
    return 0;

  reg = (uint8_t) (section[loc - 1u] - 0xb8u);
  if (section[loc + 2u] != 0x8e || section[loc + 3u] != 0xd8u + reg)
    return 0;

  pos = loc + 4u;
  limit = section_len - pos > 96u ? pos + 96u : section_len;
  while (pos < limit)
    {
      size_t insn_len;

      if (section[pos] == 0x1f)          /* pop ds */
        return adjusted != 0;
      insn_len = scan_instruction_len (section + pos, section_len - pos);
      if (insn_len == 0 || pos + insn_len > section_len)
        return 0;
      if (mz_instruction_transfers_control (section, pos, insn_len))
        return 0;
      adjusted += adjust_mz_direct_ref (section, pos, insn_len, delta);
      pos += insn_len;
    }

  return 0;
}

static int
try_adjust_mz_far_offset (uint8_t *section, size_t section_len, size_t loc,
                          uint32_t delta, int require_code_pattern,
                          enum mz_section target_section)
{
  size_t off_loc;

  if (delta == 0)
    return 1;
  if (loc < 2u)
    return 0;

  if (reloc_site_is_far_call_or_jump (section, loc))
    off_loc = loc - 2u;
  else if (reloc_site_is_split_far_pointer (section, loc))
    off_loc = loc - 3u;
  else if (require_code_pattern && target_section == MZ_SEC_DATA
           && adjust_mz_ds_subsegment_block (section, section_len, loc, delta))
    return 1;
  else if (require_code_pattern
           && adjust_mz_stored_far_pointer (section, section_len, loc, delta))
    return 1;
  else if (!require_code_pattern)
    off_loc = loc - 2u;
  else
    return 0;

  adjust_mz_far_offset_word (section, off_loc, delta);
  return 1;
}

static void
adjust_mz_far_offset (uint8_t *section, size_t section_len, size_t loc,
                      uint32_t delta, int require_code_pattern,
                      enum mz_section target_section)
{
  if (!try_adjust_mz_far_offset (section, section_len, loc, delta,
                                 require_code_pattern, target_section))
    {
      if (loc < 2u)
        die ("MZ segmented relocation offset is outside ELKS flat window");
      die ("unsupported MZ segment-only code relocation");
    }
}

static int
ne_find_segment_for_phys (const struct image *img, uint32_t phys)
{
  unsigned i;

  for (i = 0; i < img->ne_nsegs; i++)
    if (phys >= img->ne_seg[i].mz_base
        && phys < img->ne_seg[i].mz_base + img->ne_seg[i].mz_len)
      return (int) i;
  return -1;
}

static int
ne_find_segment_for_segval (const struct image *img, uint16_t val)
{
  return ne_find_segment_for_phys (img, (uint32_t) val << 4);
}

static uint16_t
ne_local_offset (const struct ne_seg_image *seg, uint32_t phys)
{
  uint32_t off = phys - seg->mz_base;

  if (off > ELKS_MAX16)
    die ("NE local offset exceeds 16 bits");
  return (uint16_t) off;
}

static unsigned
ne_add_segment (struct image *img, uint32_t mz_base, const uint8_t *src,
                size_t len, uint16_t flags)
{
  struct ne_seg_image *seg;

  if (img->ne_nsegs >= NE_MAX_SEGS)
    die ("MZ needs more segments than ELKS OS/2 loader can hold");
  if (len == 0 || len > ELKS_MAX16)
    die ("MZ segment cannot be represented as an ELKS OS/2 segment");

  seg = &img->ne_seg[img->ne_nsegs];
  seg->mz_base = mz_base;
  seg->mz_len = (uint32_t) len;
  seg->flags = flags | NESEG_PRELOAD;
  seg->min_alloc = (uint16_t) len;
  vec_append (&seg->bytes, src, len);
  img->ne_nsegs++;
  return img->ne_nsegs - 1u;
}

static void
append_ne_mz_startup (struct image *img, unsigned startup_seg,
                      unsigned target_seg, uint16_t target_off,
                      int install_int21, uint16_t int21_handler)
{
  static const uint8_t prefix[] = {
    0x55, 0x89, 0xe5,       /* push bp; mov bp, sp */
    0x50, 0x53, 0x51, 0x52, /* save ax,bx,cx,dx */
    0x56, 0x57,             /* save si,di */
    0x16, 0x07,             /* es = ss */
    0xbf, 0x81, 0x00,       /* di = command tail text */
    0x31, 0xc9,             /* cx = tail length */
    0x8b, 0x56, 0x02,       /* dx = argc */
    0x83, 0xfa, 0x01, 0x76, 0x31,
    0x4a, 0x8d, 0x76, 0x06,
    0xb0, 0x20, 0xaa, 0x41,
    0x36, 0x8b, 0x1c, 0x09, 0xdb, 0x74, 0x22,
    0x36, 0x8a, 0x07, 0x08, 0xc0, 0x74, 0x0a,
    0x83, 0xf9, 0x7e, 0x73, 0x16,
    0xaa, 0x43, 0x41, 0xeb, 0xef,
    0x83, 0xc6, 0x02, 0x4a, 0x74, 0x0b,
    0x83, 0xf9, 0x7e, 0x73, 0x06,
    0xb0, 0x20, 0xaa, 0x41, 0xeb, 0xd7,
    0x26, 0x88, 0x0e, 0x80, 0x00,
    0xb0, 0x0d, 0xaa,
    0x5f, 0x5e, 0x5a, 0x59, 0x5b, 0x58,
    0x16, 0x1f,             /* ds = ss */
    0x16, 0x07,             /* es = ss */
    0x5d                    /* pop bp */
  };
  struct ne_seg_image *seg = &img->ne_seg[startup_seg];
  size_t start = seg->bytes.len;
  size_t chain;

  if (install_int21)
    emit_install_int21_vector (&seg->bytes, int21_handler);
  vec_append (&seg->bytes, prefix, sizeof (prefix));
  if (seg->bytes.len + 5u > ELKS_MAX16)
    die ("NE startup segment grew beyond 64 KiB");

  emit8 (&seg->bytes, 0xea);       /* far jmp original MZ entry */
  chain = seg->bytes.len;
  emit16 (&seg->bytes, target_off);
  emit16 (&seg->bytes, 0);
  ne_reloc_add (&seg->rels, (uint16_t) chain, NEFIXSRC_FARADDR,
                NEFIXFLG_INTERNAL, (uint8_t) (target_seg + 1u),
                target_off);
  seg->flags |= NESEG_RELOCINFO;
  img->entry = (uint16_t) start;
}

static int
mz_ne_needed (uint32_t data_base, size_t text_len, size_t data_len)
{
  return data_base > ELKS_MAX16 || text_len > ELKS_MAX16
         || data_len > ELKS_MAX16;
}

static void
cap_ne_auto_heap (struct image *img, unsigned data_seg,
                  const struct options *opts)
{
  uint32_t base = (uint32_t) img->ne_seg[data_seg].min_alloc
                  + img->stack + NE_ARG_SLACK;
  uint32_t max_heap;

  if (base >= ELKS_MAX_HEAP)
    {
      if (opts->stack_set)
        die ("NE auto data plus requested stack exceeds 64 KiB");
      img->stack = ELKS_DEFAULT_STACK;
      base = (uint32_t) img->ne_seg[data_seg].min_alloc
             + img->stack + NE_ARG_SLACK;
      if (base >= ELKS_MAX_HEAP)
        die ("NE auto data leaves no room for stack or argv");
    }

  max_heap = ELKS_MAX_HEAP - base;
  if (img->heap > max_heap)
    {
      if (opts->heap_set)
        die ("NE requested heap exceeds 64 KiB auto-data segment");
      img->heap = (uint16_t) max_heap;
    }
}

static void
convert_mz_ne (const uint8_t *input, size_t input_len,
               const struct options *opts, struct image *img,
               struct patch_stats *stats, const struct mz_header *h,
               size_t exe_size, uint16_t code_para, uint16_t data_para)
{
  struct runtime_info rt;
  const uint8_t *image = input + (size_t) h->cparhdr * 16u;
  size_t image_len = exe_size - (size_t) h->cparhdr * 16u;
  uint32_t code_base = (uint32_t) code_para << 4;
  uint32_t data_base = (uint32_t) data_para << 4;
  uint32_t code_end = data_base > code_base && data_base < image_len
                      ? data_base : (uint32_t) image_len;
  size_t data_len = data_base < image_len ? image_len - data_base : 0;
  unsigned data_seg;
  int entry_seg;
  uint32_t pos;
  uint16_t i;

  (void) input_len;
  img->os2_ne = 1;
  init_image_memory (img, opts);
  if (!opts->heap_set)
    img->heap = mz_heap_from_minalloc (h->minalloc);
  if (!opts->stack_set)
    {
      img->stack = (h->sp != 0 && h->sp <= 32768u) ? h->sp : ELKS_DOSISH_STACK;
      if (img->stack < ELKS_DOSISH_STACK)
        img->stack = ELKS_DOSISH_STACK;
    }

  if (code_base >= image_len || code_end <= code_base)
    die ("MZ code segment starts beyond the load image");

  pos = code_base;
  while (pos < code_end)
    {
      size_t len = code_end - pos;

      if (len > NE_CODE_CHUNK)
        len = NE_CODE_CHUNK;
      ne_add_segment (img, pos, image + pos, len, NESEG_CODE);
      pos += (uint32_t) len;
    }

  if (data_len == 0)
    {
      static const uint8_t zero = 0;
      data_seg = ne_add_segment (img, data_base, &zero, 1u, NESEG_DATA);
    }
  else
    data_seg = ne_add_segment (img, data_base, image + data_base, data_len,
                               NESEG_DATA);
  img->ne_auto_data = data_seg;

  append_runtime_state_to_data (&img->ne_seg[data_seg].bytes, img->heap,
                                img->stack, img->bss, &rt);
  data_len = img->ne_seg[data_seg].bytes.len;
  if (data_len + img->bss > ELKS_MAX16)
    die ("NE data segment is too large");
  img->ne_seg[data_seg].min_alloc = (uint16_t) (data_len + img->bss);
  if (h->ss == data_para && h->sp > img->ne_seg[data_seg].min_alloc)
    img->ne_seg[data_seg].min_alloc = h->sp;
  cap_ne_auto_heap (img, data_seg, opts);

  for (i = 0; i < h->crlc; i++)
    {
      size_t rpos = (size_t) h->lfarlc + (size_t) i * 4u;
      uint16_t off = get16 (input + rpos);
      uint16_t segv = get16 (input + rpos + 2u);
      uint32_t loc = ((uint32_t) segv << 4) + off;
      uint16_t val;
      int place;
      int target;
      uint16_t local;
      uint32_t target_phys;
      uint32_t delta;
      enum mz_section target_section;

      if (loc + 1u >= image_len)
        die ("MZ relocation points outside the load image");
      place = ne_find_segment_for_phys (img, loc);
      if (place < 0)
        die ("MZ relocation is outside selected NE segments");
      val = get16 (image + loc);
      target = ne_find_segment_for_segval (img, val);
      if (target < 0)
        continue;

      local = ne_local_offset (&img->ne_seg[place], loc);
      target_phys = (uint32_t) val << 4;
      delta = target_phys - img->ne_seg[target].mz_base;
      target_section = (img->ne_seg[target].flags & NESEG_DATA)
                       ? MZ_SEC_DATA : MZ_SEC_TEXT;
      if (!try_adjust_mz_far_offset (img->ne_seg[place].bytes.data,
                                     img->ne_seg[place].bytes.len, local,
                                     delta, place != (int) data_seg,
                                     target_section))
        continue;

      ne_reloc_add (&img->ne_seg[place].rels, local, NEFIXSRC_SEGMENT,
                    NEFIXFLG_INTERNAL, (uint8_t) (target + 1), 0);
      reloc_add (&img->ne_seg[place].guards, local, 0);
      img->ne_seg[place].flags |= NESEG_RELOCINFO;
    }

  entry_seg = ne_find_segment_for_phys (img, code_base + h->ip);
  if (entry_seg < 0)
    die ("MZ entry point is outside selected NE code segments");
  img->ne_entry_seg = 0;

  for (i = 0; i < img->ne_nsegs; i++)
    if (!(img->ne_seg[i].flags & NESEG_DATA))
      {
        patch_dos_io (&img->ne_seg[i].bytes, stats, &rt,
                      &img->ne_seg[i].guards);
        if (img->ne_seg[i].bytes.len > ELKS_MAX16)
          die ("NE code segment grew beyond 64 KiB while adding stubs");
        img->ne_seg[i].mz_len = (uint32_t) img->ne_seg[i].bytes.len;
        img->ne_seg[i].min_alloc = (uint16_t) img->ne_seg[i].bytes.len;
      }

  if (stats->dynamic_int21)
    {
      uint16_t handler = append_int21_interrupt_handler (&img->ne_seg[0].bytes,
                                                         &rt);

      append_ne_mz_startup (img, 0, (unsigned) entry_seg,
                            ne_local_offset (&img->ne_seg[entry_seg],
                                             code_base + h->ip),
                            1, handler);
    }
  else
    append_ne_mz_startup (img, 0, (unsigned) entry_seg,
                          ne_local_offset (&img->ne_seg[entry_seg],
                                           code_base + h->ip),
                          0, 0);
  if (img->ne_seg[0].bytes.len > ELKS_MAX16)
    die ("NE startup segment grew beyond 64 KiB");
  img->ne_seg[0].mz_len = (uint32_t) img->ne_seg[0].bytes.len;
  img->ne_seg[0].min_alloc = (uint16_t) img->ne_seg[0].bytes.len;

  if (opts->verbose)
    fprintf (stderr,
             "msdos2elks: MZ emitted OS/2 NE cs=%04x ds=%04x segs=%u"
             " heap=%u stack=%u\n",
             code_para, data_para, img->ne_nsegs, img->heap, img->stack);
}

static void
convert_mz (const uint8_t *input, size_t input_len, const struct options *opts,
            struct image *img, struct patch_stats *stats)
{
  struct mz_header h;
  struct runtime_info rt;
  size_t exe_size;
  size_t header_size;
  size_t image_len;
  const uint8_t *image;
  uint16_t code_para;
  uint16_t data_para;
  uint32_t code_base;
  uint32_t data_base;
  size_t text_len;
  size_t data_len;
  uint16_t i;

  read_mz_header (input, input_len, &h);
  exe_size = mz_file_size (&h, input_len);
  header_size = (size_t) h.cparhdr * 16u;
  if (header_size >= exe_size)
    die ("MZ executable has no load image");
  if ((size_t) h.lfarlc + (size_t) h.crlc * 4u > header_size)
    die ("MZ relocation table extends beyond the header");

  image = input + header_size;
  image_len = exe_size - header_size;
  reject_known_mz_packers (input, input_len, image, image_len, &h);

  code_para = opts->mz_code_set ? opts->mz_code_seg : h.cs;
  data_para = opts->mz_data_set
              ? opts->mz_data_seg
              : guess_mz_data_para (image, image_len, input, &h, code_para);

  code_base = (uint32_t) code_para << 4;
  data_base = (uint32_t) data_para << 4;
  if (code_base >= image_len)
    die ("MZ code segment starts beyond the load image");

  if (data_base > code_base && data_base < image_len)
    text_len = (size_t) (data_base - code_base);
  else
    text_len = image_len - code_base;
  if ((uint32_t) h.ip >= text_len)
    die ("MZ entry point is outside the selected text segment");
  data_len = data_base < image_len ? image_len - data_base : 0;

  if (opts->mz_output == MZ_OUT_OS2
      || (opts->mz_output == MZ_OUT_AUTO
          && mz_ne_needed (data_base, text_len, data_len)))
    {
      convert_mz_ne (input, input_len, opts, img, stats, &h, exe_size,
                     code_para, data_para);
      return;
    }

  if (text_len == 0 || text_len > ELKS_MAX16 || data_len > ELKS_MAX16)
    die ("MZ text or data segment is too large for flat ELKS a.out; "
         "use the default --mz-output=os2 with CONFIG_EXEC_OS2=y");

  init_image_memory (img, opts);
  img->entry = h.ip;
  if (!opts->heap_set)
    img->heap = mz_heap_from_minalloc (h.minalloc);
  if (!opts->stack_set)
    img->stack = (h.sp != 0 && h.sp <= 32768u) ? h.sp : ELKS_DOSISH_STACK;

  vec_append (&img->text, image + code_base, text_len);
  if (data_len)
    vec_append (&img->data, image + data_base, data_len);
  if (h.ss == data_para && h.sp > img->data.len && h.sp <= 32768u)
    vec_append_zeros (&img->data, h.sp - img->data.len);
  append_runtime_state (img, &rt);
  patch_mz_stack_setup (&img->text, stats);
  patch_dos_stack_switches (&img->text, stats);

  for (i = 0; i < h.crlc; i++)
    {
      size_t rpos = (size_t) h.lfarlc + (size_t) i * 4u;
      uint16_t off = get16 (input + rpos);
      uint16_t seg = get16 (input + rpos + 2u);
      uint32_t loc = ((uint32_t) seg << 4) + off;
      uint16_t val;
      uint16_t sym;
      struct mz_segmap m;

      if (loc + 1u >= image_len)
        die ("MZ relocation points outside the load image");
      val = get16 (image + loc);
      m = map_mz_segment_value (val, code_para, data_para, text_len, data_len);
      if (m.section == MZ_SEC_DATA)
        sym = ELKS_S_DATA;
      else if (m.section == MZ_SEC_TEXT)
        sym = ELKS_S_TEXT;
      else
        {
          fprintf (stderr,
                   "msdos2elks: MZ relocation %u has unsupported segment value %04x"
                   " (code %04x, data %04x)\n",
                   (unsigned) i, val, code_para, data_para);
          die ("unsupported MZ segmented relocation");
        }

      if (loc >= code_base && loc + 1u < code_base + text_len)
        {
          size_t tloc = loc - code_base;

          adjust_mz_far_offset (img->text.data, img->text.len, tloc,
                                m.delta, 1, m.section);
          reloc_add (&img->trel, tloc, sym);
        }
      else if (loc >= data_base && loc + 1u < data_base + data_len)
        {
          size_t dloc = loc - data_base;

          adjust_mz_far_offset (img->data.data, img->data.len, dloc,
                                m.delta, 0, m.section);
          reloc_add (&img->drel, dloc, sym);
        }
      else
        die ("MZ relocation is outside selected ELKS text/data segments");
    }

  patch_dos_io (&img->text, stats, &rt, &img->trel);
  if (stats->dynamic_int21)
    {
      uint16_t handler = append_int21_interrupt_handler (&img->text, &rt);

      append_mz_argv_startup (img, h.ip, 1, handler);
    }
  else
    append_mz_argv_startup (img, h.ip, 0, 0);

  if (opts->verbose)
    fprintf (stderr,
             "msdos2elks: MZ cs=%04x ds=%04x text=%u data=%u heap=%u stack=%u\n",
             code_para, data_para, (unsigned) img->text.len,
             (unsigned) img->data.len, img->heap, img->stack);
}

static void
check_image_limits (const struct image *img)
{
  uint32_t base;

  if (img->text.len == 0 || img->text.len > ELKS_MAX16)
    die ("ELKS text segment size is invalid");
  if (img->data.len > ELKS_MAX16)
    die ("ELKS data segment size is too large");

  base = (uint32_t) img->data.len + img->bss + img->stack;
  if (base > ELKS_MAX_HEAP)
    die ("ELKS data+bss+stack memory exceeds the 16-bit segment limit");
  if (img->heap != 0 && img->heap < ELKS_MAX_HEAP
      && base + img->heap > ELKS_MAX_HEAP)
    die ("ELKS data+bss+stack+heap memory exceeds the 16-bit segment limit");
}

static void
write_relocs (struct byte_vec *out, const struct reloc_vec *rels)
{
  size_t i;

  for (i = 0; i < rels->len; i++)
    {
      emit32 (out, rels->data[i].vaddr);
      emit16 (out, rels->data[i].sym);
      emit16 (out, ELKS_R_SEGWORD);
    }
}

static void
write_ne_relocs (struct byte_vec *out, const struct ne_reloc_vec *rels)
{
  size_t i;

  emit16 (out, (uint16_t) rels->len);
  for (i = 0; i < rels->len; i++)
    {
      emit8 (out, rels->data[i].src_type);
      emit8 (out, rels->data[i].flags);
      emit16 (out, rels->data[i].src_chain);
      emit8 (out, rels->data[i].segment);
      emit8 (out, 0);
      emit16 (out, rels->data[i].offset);
    }
}

static void
vec_align_zeros (struct byte_vec *out, size_t align)
{
  size_t pad;

  if (align == 0)
    return;
  pad = (align - (out->len % align)) % align;
  while (pad--)
    emit8 (out, 0);
}

static void
write_os2_ne (const char *path, const struct image *img)
{
  FILE *fp;
  struct byte_vec out = { 0, 0, 0 };
  uint16_t segtab_off = NE_HDR_SIZE;
  uint16_t table_abs = NE_MZ_STUB_SIZE + NE_HDR_SIZE;
  uint16_t after_table = (uint16_t) (table_abs + img->ne_nsegs * 8u);
  size_t segtab_pos;
  size_t file_size;
  unsigned i;

  if (!img->ne_nsegs || img->ne_nsegs > NE_MAX_SEGS)
    die ("invalid NE segment count");
  if (img->ne_auto_data >= img->ne_nsegs || img->ne_entry_seg >= img->ne_nsegs)
    die ("invalid NE entry or data segment");

  emit16 (&out, MZ_MAGIC);
  emit16 (&out, 0);            /* patched final-page byte count */
  emit16 (&out, 0);            /* patched page count */
  emit16 (&out, 0);            /* no DOS relocations */
  emit16 (&out, 4);            /* 64-byte MZ stub/header */
  emit16 (&out, 0);
  emit16 (&out, 0xffff);
  emit16 (&out, 0);
  emit16 (&out, 0);
  emit16 (&out, 0);
  emit16 (&out, 0);
  emit16 (&out, 0);
  emit16 (&out, NE_MZ_STUB_SIZE);
  emit16 (&out, 0);
  while (out.len < 0x3cu)
    emit8 (&out, 0);
  emit32 (&out, NE_MZ_STUB_SIZE);
  while (out.len < NE_MZ_STUB_SIZE)
    emit8 (&out, 0);

  emit16 (&out, 0x454eu);      /* NE */
  emit8 (&out, 5);
  emit8 (&out, 0);
  emit16 (&out, after_table - NE_MZ_STUB_SIZE);
  emit16 (&out, 0);            /* no entry table */
  emit32 (&out, 0);
  emit8 (&out, 0x02);          /* multiple-data program */
  emit8 (&out, 0);
  emit16 (&out, (uint16_t) (img->ne_auto_data + 1u));
  emit16 (&out, img->heap);
  emit16 (&out, img->stack);
  emit16 (&out, img->entry);
  emit16 (&out, (uint16_t) (img->ne_entry_seg + 1u));
  emit16 (&out, 0);
  emit16 (&out, (uint16_t) (img->ne_auto_data + 1u));
  emit16 (&out, (uint16_t) img->ne_nsegs);
  emit16 (&out, 0);
  emit16 (&out, 0);
  emit16 (&out, segtab_off);
  emit16 (&out, after_table - NE_MZ_STUB_SIZE);
  emit16 (&out, after_table - NE_MZ_STUB_SIZE);
  emit16 (&out, after_table - NE_MZ_STUB_SIZE);
  emit16 (&out, after_table - NE_MZ_STUB_SIZE);
  emit32 (&out, 0);
  emit16 (&out, 0);
  emit16 (&out, 4);            /* segment offsets are paragraph units */
  emit16 (&out, 0);
  emit8 (&out, 1);             /* OS/2 target */
  emit8 (&out, 0);
  emit16 (&out, 0);
  emit16 (&out, 0);
  emit16 (&out, 0);
  emit8 (&out, 0);
  emit8 (&out, 0);

  if (out.len != table_abs)
    die ("internal NE header size mismatch");

  segtab_pos = out.len;
  for (i = 0; i < img->ne_nsegs * 8u; i++)
    emit8 (&out, 0);

  for (i = 0; i < img->ne_nsegs; i++)
    {
      const struct ne_seg_image *seg = &img->ne_seg[i];
      size_t entry = segtab_pos + i * 8u;
      uint16_t flags = seg->flags;

      if (seg->rels.len)
        flags |= NESEG_RELOCINFO;
      if (seg->bytes.len == 0 || seg->bytes.len > ELKS_MAX16)
        die ("invalid NE segment size");
      if (seg->min_alloc == 0)
        die ("invalid NE segment allocation");

      vec_align_zeros (&out, 16u);
      if ((out.len >> 4) > ELKS_MAX16)
        die ("NE segment file offset exceeds loader format");
      put16 (out.data + entry, (uint16_t) (out.len >> 4));
      put16 (out.data + entry + 2u, (uint16_t) seg->bytes.len);
      put16 (out.data + entry + 4u, flags);
      put16 (out.data + entry + 6u, seg->min_alloc);
      vec_append (&out, seg->bytes.data, seg->bytes.len);
      if (seg->rels.len)
        write_ne_relocs (&out, &seg->rels);
    }

  file_size = out.len;
  put16 (out.data + 2u, (uint16_t) (file_size & 0x1ffu));
  put16 (out.data + 4u, (uint16_t) ((file_size + 511u) >> 9));

  fp = fopen (path, "wb");
  if (!fp)
    die_errno (path);
  if (fwrite (out.data, 1, out.len, fp) != out.len)
    die_errno (path);
  if (fclose (fp) != 0)
    die_errno (path);
  if (chmod (path, 0755) != 0)
    die_errno (path);
  free (out.data);
}

static void
write_elks (const char *path, const struct image *img)
{
  FILE *fp;
  struct byte_vec out = { 0, 0, 0 };
  int have_reloc = img->trel.len || img->drel.len;

  if (img->os2_ne)
    {
      write_os2_ne (path, img);
      return;
    }

  check_image_limits (img);

  emit32 (&out, ELKS_SPLITID);
  emit8 (&out, have_reloc ? ELKS_RELOC_HDR_SIZE : ELKS_HDR_SIZE);
  emit8 (&out, 0);
  emit16 (&out, 1);           /* version: chmem is heap size */
  emit32 (&out, (uint32_t) img->text.len);
  emit32 (&out, (uint32_t) img->data.len);
  emit32 (&out, img->bss);
  emit32 (&out, img->entry);
  emit16 (&out, img->heap);
  emit16 (&out, img->stack);
  emit32 (&out, 0);           /* symbols */

  if (have_reloc)
    {
      emit32 (&out, (uint32_t) img->trel.len * 8u);
      emit32 (&out, (uint32_t) img->drel.len * 8u);
      emit32 (&out, 0);       /* text base */
      emit32 (&out, 0);       /* data base */
    }

  vec_append (&out, img->text.data, img->text.len);
  vec_append (&out, img->data.data, img->data.len);
  write_relocs (&out, &img->trel);
  write_relocs (&out, &img->drel);

  fp = fopen (path, "wb");
  if (!fp)
    die_errno (path);
  if (fwrite (out.data, 1, out.len, fp) != out.len)
    die_errno (path);
  if (fclose (fp) != 0)
    die_errno (path);
  if (chmod (path, 0755) != 0)
    die_errno (path);
  free (out.data);
}

static void
free_image (struct image *img)
{
  unsigned i;

  free (img->text.data);
  free (img->data.data);
  free (img->trel.data);
  free (img->drel.data);
  for (i = 0; i < img->ne_nsegs; i++)
    {
      free (img->ne_seg[i].bytes.data);
      free (img->ne_seg[i].guards.data);
      free (img->ne_seg[i].rels.data);
    }
}

static int
is_zip_archive (const uint8_t *input, size_t input_len)
{
  if (input_len < 4u || input[0] != 'P' || input[1] != 'K')
    return 0;
  return (input[2] == 0x03 && input[3] == 0x04)
         || (input[2] == 0x05 && input[3] == 0x06)
         || (input[2] == 0x07 && input[3] == 0x08);
}

int
main (int argc, char **argv)
{
  struct options opts;
  struct image img;
  struct patch_stats stats;
  uint8_t *input;
  size_t input_len;
  int is_mz;

  parse_options (argc, argv, &opts);
  input = read_file (opts.input, &input_len);
  memset (&img, 0, sizeof (img));
  memset (&stats, 0, sizeof (stats));

  if (opts.format != FMT_COM && is_zip_archive (input, input_len))
    die ("input appears to be a ZIP/SFX archive; extract it before conversion");

  is_mz = input_len >= 2u
          && (get16 (input) == MZ_MAGIC || get16 (input) == ZM_MAGIC);
  if (opts.format == FMT_EXE || (opts.format == FMT_AUTO && is_mz))
    convert_mz (input, input_len, &opts, &img, &stats);
  else
    convert_com (input, input_len, &opts, &img, &stats);

  if (stats.unsupported)
    {
      report_unsupported (&stats);
      if (!opts.partial)
        die ("conversion stopped; use --partial to keep unsupported DOS sites");
    }

      write_elks (opts.output, &img);
  if (opts.verbose)
    {
      if (img.os2_ne)
        fprintf (stderr,
                 "msdos2elks: patched=%u unsupported=%u dynamic-int21=%u"
                 " com-segment-fixes=%u stack-fixes=%u os2-ne-segs=%u\n",
                 stats.patched, stats.unsupported,
                 stats.dynamic_int21 ? 1u : 0u, stats.com_segfix,
                 stats.stackfix, img.ne_nsegs);
      else
        fprintf (stderr,
                 "msdos2elks: patched=%u unsupported=%u dynamic-int21=%u"
                 " com-segment-fixes=%u stack-fixes=%u text=%u data=%u"
                 " trel=%u drel=%u\n",
                 stats.patched, stats.unsupported,
                 stats.dynamic_int21 ? 1u : 0u, stats.com_segfix,
                 stats.stackfix,
                 (unsigned) img.text.len, (unsigned) img.data.len,
                 (unsigned) img.trel.len, (unsigned) img.drel.len);
    }

  free_image (&img);
  free (input);
  return stats.unsupported ? 2 : 0;
}
