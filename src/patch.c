#include "internal.h"

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

  if (stats->unsupported_video)
    {
      if (stats->first_video_mode_known)
        fprintf (stderr,
                 "msdos2elks: unsupported VGA/non-CGA-EGA-MDA video mode"
                 " %02xh at text offset %04x\n",
                 stats->first_video_mode,
                 (unsigned) stats->first_video_offset);
      else
        fprintf (stderr,
                 "msdos2elks: unsupported dynamic BIOS video mode/service"
                 " AH=%02xh at text offset %04x; refusing possible VGA app\n",
                 stats->first_video_fn,
                 (unsigned) stats->first_video_offset);
    }

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
record_unsupported_video (struct patch_stats *stats, uint32_t off,
                          uint8_t fn, uint8_t mode, int mode_known)
{
  if (stats->unsupported_video == 0)
    {
      stats->first_video_offset = off;
      stats->first_video_fn = fn;
      stats->first_video_mode = mode;
      stats->first_video_mode_known = mode_known;
    }
  stats->unsupported_video++;
  record_unsupported (stats, off, 0x10, 1, fn);
}

static int
bgi_optional_video_probe (const struct patch_stats *stats, uint8_t fn,
                          uint8_t mode, int mode_known)
{
  if (!stats->allow_bgi_multimode_video)
    return 0;
  if (fn == 0x30)
    return 1;
  if (fn != 0x00 || !mode_known)
    return 0;
  return mode == 0x11u || mode == 0x30u || mode == 0x40u;
}

static int
dos_keyboard_function (uint8_t fn)
{
  return fn == 0x01u || fn == 0x07u || fn == 0x08u || fn == 0x0bu
         || fn == 0x0cu;
}

static void
patch_call (struct byte_vec *text, size_t start, size_t len, size_t target,
            int al_known, uint8_t al)
{
  uint16_t rel;
  size_t i;

  if (al_known && len >= 5u)
    {
      rel = (uint16_t) (target - (start + 5u));
      text->data[start] = 0xb0;         /* mov al, imm8 */
      text->data[start + 1u] = al;
      text->data[start + 2u] = 0xe8;    /* call rel16 */
      put16 (text->data + start + 3u, rel);
      for (i = 5u; i < len; i++)
        text->data[start + i] = 0x90;
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
      || op == 0x64 || op == 0x65 || op == 0x66 || op == 0x67
      || op == 0xf2 || op == 0xf3)
    {
      if (op == 0x66 && avail >= 2u && p[1] == 0x68)
        return avail >= 6u ? 6u : 0;
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
      || op == 0x2e || op == 0x36 || op == 0x3e
      || op == 0x64 || op == 0x65 || op == 0x66 || op == 0x67)
    {
      if (op == 0x66 && avail >= 2u && p[1] == 0x68)
        return avail >= 6u ? 6u : 1u;
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

static int
likely_explicit_unsupported_interrupt (const uint8_t *data, size_t off)
{
  if (off >= 2u && data[off - 2u] == 0xb4)
    return 1;
  if (off >= 3u && data[off - 3u] == 0xb8)
    return 1;
  return 0;
}

static void
patch_bda_keyboard_status_checks (struct byte_vec *text,
                                  struct patch_stats *stats,
                                  const struct runtime_info *rt,
                                  uint32_t *status_stub,
                                  uint8_t *covered, size_t original_len)
{
  static const uint8_t pattern[] =
  {
    0x33, 0xdb,             /* xor bx, bx */
    0x8e, 0xc3,             /* mov es, bx */
    0xbb, 0x1a, 0x04,       /* mov bx, 041ah */
    0x26, 0x8b, 0x07,       /* mov ax, es:[bx] */
    0x33, 0xdb,             /* xor bx, bx */
    0x8e, 0xc3,             /* mov es, bx */
    0xbb, 0x1c, 0x04,       /* mov bx, 041ch */
    0x26, 0x3b, 0x07,       /* cmp ax, es:[bx] */
    0x75, 0x04,             /* jne key */
    0x33, 0xc0,             /* xor ax, ax */
    0xeb, 0x03,             /* jmp done */
    0xb8, 0x01, 0x00,       /* key: mov ax, 1 */
    0xc3                    /* done: ret */
  };
  size_t i;

  for (i = 0; i + sizeof (pattern) <= original_len; i++)
    {
      size_t j;
      uint16_t rel;

      if (memcmp (text->data + i, pattern, sizeof (pattern)) != 0)
        continue;
      for (j = 0; j < sizeof (pattern); j++)
        if (covered[i + j])
          break;
      if (j != sizeof (pattern))
        continue;

      if (*status_stub == UINT32_MAX)
        {
          size_t stub_off = text->len;

          emit_bios_keyboard_stub_for_fn (text, 0x01, rt);
          if (stub_off > ELKS_MAX16 || text->len > ELKS_MAX16)
            die ("text segment grew beyond 64 KiB while adding keyboard stub");
          *status_stub = (uint32_t) stub_off;
        }

      rel = (uint16_t) (*status_stub - (i + 3u));
      text->data[i] = 0xe8;         /* call status_stub */
      put16 (text->data + i + 1u, rel);
      text->data[i + 3u] = 0x09;    /* or ax, ax */
      text->data[i + 4u] = 0xc0;
      text->data[i + 5u] = 0x74;    /* jz no_key */
      text->data[i + 6u] = 0x04;
      text->data[i + 7u] = 0xb8;    /* mov ax, 1 */
      put16 (text->data + i + 8u, 1);
      text->data[i + 10u] = 0xc3;   /* ret */
      text->data[i + 11u] = 0x31;   /* no_key: xor ax, ax */
      text->data[i + 12u] = 0xc0;
      text->data[i + 13u] = 0xc3;   /* ret */
      memset (text->data + i + 14u, 0x90, sizeof (pattern) - 14u);
      for (j = 0; j < sizeof (pattern); j++)
        covered[i + j] = 1;
      stats->patched++;
      stats->bios_keyboard_input = 1;
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

  patch_bda_keyboard_status_checks (text, stats, rt, &stubs[1][0x01],
                                    covered, original_len);
  if (stats->allow_bgi_multimode_video)
    {
      size_t stub_off = text->len;

      emit_bios_no_vga_info_stub (text);
      if (stub_off > ELKS_MAX16 || text->len > ELKS_MAX16)
        die ("text segment grew beyond 64 KiB while adding video stub");
      stubs[0][0x30] = (uint32_t) stub_off;
    }

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
          if (is_reported_interrupt (intr)
              && (intr == 0x29
                  || likely_explicit_unsupported_interrupt (text->data, i)))
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
      else if (i >= 4u && text->data[i - 4u] == 0xb0
               && text->data[i - 2u] == 0xb4)
        {
          start = i - 4u;
          len = 6u;
          al = text->data[i - 3u];
          al_known = 1;
          fn = text->data[i - 1u];
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
          if (intr == 0x16)
            {
              stats->bios_keyboard_input = 1;
              stats->dynamic_int16 = 1;
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

      if (intr == 0x10 && fn == 0x00)
        {
          if (!al_known || !bios_video_mode_is_ega_mda_supported (al))
            {
              if (bgi_optional_video_probe (stats, fn, al, al_known))
                {
                  i += insn_len;
                  continue;
                }
              record_unsupported_video (stats, (uint32_t) i, fn, al,
                                        al_known);
              i += insn_len;
              continue;
            }
        }
      else if (intr == 0x10 && fn == 0x30)
        {
          if (!bgi_optional_video_probe (stats, fn, 0, 0))
            {
              record_unsupported_video (stats, (uint32_t) i, fn, 0, 0);
              i += insn_len;
              continue;
            }
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
      if (intr == 0x16 || (intr == 0x21 && dos_keyboard_function (fn)))
        stats->bios_keyboard_input = 1;
      i += insn_len;
    }

  free (covered);
}

static void
