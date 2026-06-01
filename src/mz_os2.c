#include "internal.h"

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

static int mz_instruction_transfers_control (const uint8_t *section,
                                             size_t pos, size_t len);

static uint16_t
mz_stack_alias_para (const uint8_t *image, size_t image_len,
                     const uint8_t *file, const struct mz_header *h,
                     uint16_t fallback)
{
  uint16_t best = fallback;
  uint16_t i;

  /*
   * Several DOS C runtimes relocate a paragraph constant into a general
   * register, then move that register into SS after some setup.  The constant
   * can be below the MZ header SS; the runtime is deliberately creating a
   * lower stack/data alias so large positive offsets reach the same physical
   * area.  NE fixups can relocate a segment base but cannot add a paragraph
   * delta, so choose that lower paragraph as the generated auto-data base.
   */
  for (i = 0; i < h->crlc; i++)
    {
      size_t rpos = (size_t) h->lfarlc + (size_t) i * 4u;
      uint16_t off = get16 (file + rpos);
      uint16_t seg = get16 (file + rpos + 2u);
      uint32_t loc32 = ((uint32_t) seg << 4) + off;
      size_t loc;
      size_t pos;
      size_t limit;
      uint8_t reg;
      uint16_t val;

      if (loc32 < 1u || loc32 + 1u >= image_len)
        continue;
      loc = (size_t) loc32;
      if (image[loc - 1u] < 0xb8 || image[loc - 1u] > 0xbfu)
        continue;
      val = get16 (image + loc);
      if ((uint32_t) val * 16u >= image_len || val >= best)
        continue;

      reg = (uint8_t) (image[loc - 1u] - 0xb8u);
      pos = loc + 2u;
      limit = pos + 64u < image_len ? pos + 64u : image_len;
      while (pos + 1u < limit)
        {
          size_t len;

          if (image[pos] == 0x8e
              && image[pos + 1u] == (uint8_t) (0xd0u | reg))
            {
              best = val;
              break;
            }
          if (mz_instruction_transfers_control (image, pos,
                                                limit - pos > 6u
                                                ? 6u : limit - pos))
            break;
          len = scan_instruction_len (image + pos, limit - pos);
          pos += len ? len : 1u;
        }
    }

  return best;
}

static uint16_t
mz_code_para_covering_relocs (const uint8_t *image, size_t image_len,
                              const uint8_t *file,
                              const struct mz_header *h,
                              uint16_t code_para)
{
  uint32_t code_base = (uint32_t) code_para << 4;
  uint16_t i;

  (void) image;

  /*
   * The MZ header CS:IP is the entry point, not necessarily the first byte of
   * code in the load image.  Some linkers place relocated startup code before
   * the entry paragraph.  If we start the generated text segment at CS, those
   * relocation records fall outside every selected NE segment and conversion
   * fails.  Move the text base down to cover such relocated words; the entry
   * jump is still computed from the original MZ CS:IP later.
   */
  for (i = 0; i < h->crlc; i++)
    {
      size_t rpos = (size_t) h->lfarlc + (size_t) i * 4u;
      uint16_t off = get16 (file + rpos);
      uint16_t seg = get16 (file + rpos + 2u);
      uint32_t loc = ((uint32_t) seg << 4) + off;

      if (loc + 1u < image_len && loc < code_base)
        code_base = loc & ~(uint32_t) 0x0fu;
    }

  return (uint16_t) (code_base >> 4);
}

static uint16_t
mz_heap_from_minalloc (uint16_t minalloc)
{
  uint32_t bytes = (uint32_t) minalloc << 4;

  if (bytes >= ELKS_MAX_HEAP)
    return ELKS_MAX_HEAP;
  return (uint16_t) bytes;
}

static uint16_t
mz_heap_from_allocs (uint16_t minalloc, uint16_t maxalloc)
{
  /*
   * DOS MZ headers describe the extra memory requested after the loaded
   * image.  A maxalloc value of ffffh is the normal DOS spelling for
   * "give this program as much conventional memory as possible".  Many
   * games and graphics libraries then build private stacks or arenas near
   * that top-of-memory value.  Keep that DOS contract for OS/2 NE output so
   * ELKS allocates a high auto-data ceiling instead of a tiny minalloc-only
   * stack area.
   */
  if (maxalloc == 0xffffu)
    return ELKS_MAX_HEAP;

  if (maxalloc > minalloc)
    minalloc = maxalloc;

  return mz_heap_from_minalloc (minalloc);
}

static int
mz_entry_loads_ds_from_cs (const uint8_t *image, size_t image_len,
                           uint32_t entry)
{
  size_t pos;
  size_t end;
  uint8_t cs_regs = 0;

  if (entry >= image_len)
    return 0;

  pos = (size_t) entry;
  end = pos + 64u;
  if (end > image_len)
    end = image_len;

  while (pos + 1u < end)
    {
      uint8_t op = image[pos];
      uint8_t modrm;
      uint8_t reg;
      size_t len;

      if (op == 0x0e && image[pos + 1u] == 0x1f)
        return 1;           /* push cs; pop ds */

      if ((op >= 0x70 && op <= 0x7fu) || op == 0xeb
          || (op >= 0xe0 && op <= 0xe3)
          || op == 0xe8 || op == 0xe9 || op == 0xea || op == 0xcd
          || op == 0xc2 || op == 0xc3 || op == 0xca || op == 0xcb)
        return 0;

      if ((op == 0x8c || op == 0x8e) && pos + 1u < end)
        {
          modrm = image[pos + 1u];
          if ((modrm & 0xc0u) == 0xc0u)
            {
              reg = (uint8_t) (modrm & 7u);
              if (op == 0x8c && ((modrm >> 3) & 3u) == 1u)
                cs_regs |= (uint8_t) (1u << reg);
              else if (op == 0x8e && ((modrm >> 3) & 3u) == 3u
                       && (cs_regs & (uint8_t) (1u << reg)))
                return 1;   /* mov reg, cs; ...; mov ds, reg */
            }
          pos += 2u;
          continue;
        }

      len = scan_instruction_len (image + pos, end - pos);
      pos += len ? len : 1u;
    }

  return 0;
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
adjust_mz_direct_ref_for_segment (uint8_t *section, size_t start, size_t len,
                                  uint32_t delta, uint8_t sreg)
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
  if (sreg == 3u)
    {
      if (seg_override >= 0 && seg_override != 3)
        return 0;
    }
  else if (seg_override != (int) sreg)
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
mz_instruction_reloads_segment (const uint8_t *section, size_t pos,
                                size_t len, uint8_t sreg)
{
  uint8_t op;
  uint8_t modrm;

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
  if ((op == 0x07 && sreg == 0u)
      || (op == 0x17 && sreg == 2u)
      || (op == 0x1f && sreg == 3u))
    return 1;
  if ((op == 0xc4 && sreg == 0u) || (op == 0xc5 && sreg == 3u))
    return 1;
  if (op != 0x8e || len < 2u)
    return 0;

  modrm = section[pos + 1u];
  return (modrm & 0xc0u) == 0xc0u && ((modrm >> 3) & 3u) == sreg;
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
adjust_mz_subsegment_block (uint8_t *section, size_t section_len,
                            size_t loc, uint32_t delta)
{
  uint8_t reg;
  uint8_t modrm;
  uint8_t sreg;
  size_t pos;
  size_t limit;
  unsigned adjusted = 0;

  if (delta == 0 || loc < 1u || loc + 3u >= section_len)
    return 0;
  if (section[loc - 1u] < 0xb8 || section[loc - 1u] > 0xbf)
    return 0;

  reg = (uint8_t) (section[loc - 1u] - 0xb8u);
  if (section[loc + 2u] != 0x8e)
    return 0;
  modrm = section[loc + 3u];
  if ((modrm & 0xc7u) != (uint8_t) (0xc0u | reg))
    return 0;
  sreg = (uint8_t) ((modrm >> 3) & 3u);
  if (sreg == 1u)
    return 0;

  pos = loc + 4u;
  limit = section_len - pos > 96u ? pos + 96u : section_len;
  while (pos < limit)
    {
      size_t insn_len;

      insn_len = scan_instruction_len (section + pos, section_len - pos);
      if (insn_len == 0 || pos + insn_len > section_len)
        return 0;
      if (mz_instruction_reloads_segment (section, pos, insn_len, sreg))
        return adjusted != 0;
      if (mz_instruction_transfers_control (section, pos, insn_len))
        return 0;
      adjusted += adjust_mz_direct_ref_for_segment (section, pos, insn_len,
                                                    delta, sreg);
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

  (void) target_section;

  if (delta == 0)
    return 1;
  if (loc < 2u)
    return 0;

  if (reloc_site_is_far_call_or_jump (section, loc))
    off_loc = loc - 2u;
  else if (reloc_site_is_split_far_pointer (section, loc))
    off_loc = loc - 3u;
  else if (require_code_pattern
           && adjust_mz_subsegment_block (section, section_len, loc, delta))
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

static unsigned
ne_add_psp_segment (struct image *img)
{
  uint8_t psp[256];

  /*
   * DOS runs MZ EXE entry code with DS and ES pointing at a 256-byte
   * Program Segment Prefix, not at the program's data segment.  The command
   * tail at PSP:0080h is filled by the generated startup code from ELKS'
   * argv stack.  Keep this as a real NE data segment so larger EXE runtimes
   * can save the PSP segment without trampling initialized data at offsets
   * below 0100h.
   */
  memset (psp, 0, sizeof (psp));
  psp[0] = 0xcd;             /* INT 20h, the traditional PSP signature. */
  psp[1] = 0x20;
  return ne_add_segment (img, 0x110000u, psp, sizeof (psp), NESEG_DATA);
}

static unsigned
ne_add_env_segment (struct image *img)
{
  uint8_t env[64];
  static const uint8_t path_env[] = "PATH=.";
  static const uint8_t argv0[] = "C:\\PROGRAM.EXE";
  size_t pos;

  /*
   * DOS stores a segment pointer to the process environment at PSP:002Ch.
   * A valid empty environment starts with a double NUL.  DOS 3.x and later
   * then store a word count and the program path; older C runtimes use that
   * tail while building argv[0], and some report a fatal environment error if
   * it is missing.
   */
  memset (env, 0, sizeof (env));
  pos = 0;
  memcpy (env + pos, path_env, sizeof (path_env));
  pos += sizeof (path_env);
  env[pos++] = 0;
  env[pos++] = 1;
  env[pos++] = 0;
  memcpy (env + pos, argv0, sizeof (argv0));
  return ne_add_segment (img, 0x111000u, env, sizeof (env), NESEG_DATA);
}

static uint16_t align_para (uint32_t bytes);

static void
emit_ne_mov_ax_segment (struct ne_seg_image *seg, unsigned target_seg)
{
  size_t imm;

  emit8 (&seg->bytes, 0xb8);        /* mov ax, segment */
  imm = seg->bytes.len;
  emit16 (&seg->bytes, 0);
  ne_reloc_add (&seg->rels, (uint16_t) imm, NEFIXSRC_SEGMENT,
                NEFIXFLG_INTERNAL, (uint8_t) (target_seg + 1u), 0);
  seg->flags |= NESEG_RELOCINFO;
}

static void
emit_ne_set_es_to_segment (struct ne_seg_image *seg, unsigned target_seg)
{
  emit_ne_mov_ax_segment (seg, target_seg);
  emit8 (&seg->bytes, 0x8e);
  emit8 (&seg->bytes, 0xc0);        /* mov es, ax */
}

static void
emit_ne_set_ds_es_to_segment (struct ne_seg_image *seg, unsigned target_seg)
{
  emit8 (&seg->bytes, 0x50);        /* preserve DOS entry AX */
  emit_ne_mov_ax_segment (seg, target_seg);
  emit8 (&seg->bytes, 0x8e);
  emit8 (&seg->bytes, 0xd8);        /* mov ds, ax */
  emit8 (&seg->bytes, 0x8e);
  emit8 (&seg->bytes, 0xc0);        /* mov es, ax */
  emit8 (&seg->bytes, 0x58);
}

static uint16_t
ne_auto_top_para (const struct image *img, unsigned data_seg)
{
  uint32_t paras;

  if (img->heap >= ELKS_MAX_HEAP)
    return 0x0fffu;

  /*
   * ELKS' OS/2 loader allocates the auto data segment as minalloc plus the
   * NE stack, argument copy space, and heap.  Use the same paragraph-scale
   * accounting for the PSP top-of-memory word, rounded up at the byte total
   * boundary.  If the heap is zero the loader substitutes its small default
   * heap; 4 KiB matches the converter's flat-image default reserve.
   */
  paras = align_para (img->ne_seg[data_seg].min_alloc);
  paras += align_para ((uint32_t) img->stack + NE_ARG_SLACK
                       + (img->heap ? img->heap : 4096u));
  if (paras > 0x0fffu)
    paras = 0x0fffu;
  return (uint16_t) paras;
}

static int
mz_dos_alloc_request_is_free_probe (const uint8_t *image, size_t image_len,
                                    size_t fn_pos)
{
  size_t pos;
  size_t end;
  int saw_call = 0;

  end = fn_pos + 40u < image_len ? fn_pos + 40u : image_len;
  for (pos = fn_pos + 2u; pos + 1u < end; pos++)
    if (image[pos] == 0xcd && image[pos + 1u] == 0x21)
      {
        pos += 2u;
        saw_call = 1;
        break;
      }
  if (!saw_call)
    return 0;

  for (; pos + 3u < end; pos++)
    {
      if (image[pos] == 0xb4 && image[pos + 1u] == 0x48)
        return 0;
      if (image[pos] == 0xb4 && image[pos + 1u] == 0x49
          && image[pos + 2u] == 0xcd && image[pos + 3u] == 0x21)
        return 1;
    }

  return 0;
}

static uint16_t
mz_image_max_dos_alloc_request (const uint8_t *image, size_t image_len)
{
  uint16_t max = 0;
  size_t i;

  for (i = 0; i + 3u < image_len; i++)
    {
      if (image[i] == 0xb4 && image[i + 1u] == 0x48
          && image[i + 2u] == 0xcd && image[i + 3u] == 0x21)
        max = max ? max : 1;
      if (i + 4u < image_len && image[i] == 0xb8
          && image[i + 2u] == 0x48 && image[i + 3u] == 0xcd
          && image[i + 4u] == 0x21)
        max = max ? max : 1;
      if (image[i] == 0xb4 && image[i + 1u] == 0x48)
        {
          size_t j;

          for (j = i + 2u; j + 4u < image_len && j < i + 12u; j++)
            if (image[j] == 0xbb)
              {
                uint16_t request = get16 (image + j + 1u);

                if (request != 0xffffu
                    && !mz_dos_alloc_request_is_free_probe (image, image_len,
                                                            i)
                    && request > max)
                  max = request;
                break;
              }
        }
    }
  return max;
}

static void
ne_enable_conventional_dos_heap (struct image *img, unsigned runtime_seg,
                                 struct runtime_info *rt, uint16_t max_request)
{
  struct ne_seg_image *seg = &img->ne_seg[runtime_seg];
  uint16_t largest;

  largest = max_request;
  if (largest < 0x7fffu)
    largest = 0x7fffu;

  /*
   * Some DOS games allocate a conventional-memory arena far larger than one
   * 64 KiB NE segment and then address it with real-mode segment arithmetic.
   * Prefer a kernel-owned fmemalloc arena so ELKS will release it with the
   * process.  Reads into subsegments still use the runtime bounce buffer
   * because ELKS validates syscall buffers against exact segment bases.
   */
  put16 (seg->bytes.data + rt->heap_next_off, 0);
  put16 (seg->bytes.data + rt->heap_limit_off, largest);
  put16 (seg->bytes.data + rt->heap_base_seg_off, 0xffffu);
}

static void
ne_enable_far_dos_heap (struct image *img, unsigned runtime_seg,
                        struct runtime_info *rt)
{
  struct ne_seg_image *seg;

  /*
   * DOS AH=48h returns a segment that programs may immediately load into DS
   * and dereference.  ELKS validates user segment bases, so returning an
   * arbitrary paragraph inside the auto-data segment is not enough.  Use
   * kernel-owned far-memory blocks for dynamic DOS allocations; each returned
   * paragraph is then an actual user segment base and can safely be used like
   * a real DOS allocation block.
   */
  seg = &img->ne_seg[runtime_seg];
  put16 (seg->bytes.data + rt->heap_next_off, 0);
  put16 (seg->bytes.data + rt->heap_limit_off, 0x7fffu);
  put16 (seg->bytes.data + rt->heap_base_seg_off, 0xffffu);
}

static void
append_ne_mz_startup (struct image *img, unsigned startup_seg,
                      unsigned target_seg, uint16_t target_off,
                      int install_int21, uint16_t int21_handler,
                      const struct runtime_info *rt,
                      int raw_keyboard, int direct_video, int install_int16,
                      uint16_t int16_handler,
                      uint16_t psp_top_para, int separate_psp,
                      unsigned psp_seg, unsigned psp_top_seg,
                      unsigned env_seg, uint16_t initial_sp)
{
  static const uint8_t prefix[] = {
    0x55, 0x89, 0xe5,       /* push bp; mov bp, sp */
    0x50, 0x53, 0x51, 0x52, /* save ax,bx,cx,dx */
    0x56, 0x57              /* save si,di */
  };
  static const uint8_t copy_tail[] = {
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
  if (install_int16)
    emit_install_int16_vector (&seg->bytes, int16_handler);

  vec_append (&seg->bytes, prefix, sizeof (prefix));
  if (separate_psp)
    emit_ne_set_es_to_segment (seg, psp_seg);
  else
    {
      emit8 (&seg->bytes, 0x16);
      emit8 (&seg->bytes, 0x07);        /* es = ss */
    }
  if (psp_top_para)
    {
      if (separate_psp)
        emit_ne_mov_ax_segment (seg, psp_top_seg);
      else
        {
          emit8 (&seg->bytes, 0x8c);
          emit8 (&seg->bytes, 0xc8);    /* ax = cs */
        }
      emit8 (&seg->bytes, 0x05);
      emit16 (&seg->bytes, psp_top_para);
      emit8 (&seg->bytes, 0x26);
      emit8 (&seg->bytes, 0xa3);
      emit16 (&seg->bytes, 0x0002);     /* es:[PSP top] = base + top */
      emit8 (&seg->bytes, 0x31);
      emit8 (&seg->bytes, 0xc0);
      emit8 (&seg->bytes, 0x26);
      emit8 (&seg->bytes, 0xa3);
      emit16 (&seg->bytes, 0x002c);     /* es:[environment] = 0 */
    }
  if (separate_psp)
    {
      emit_ne_mov_ax_segment (seg, env_seg);
      emit8 (&seg->bytes, 0x26);
      emit8 (&seg->bytes, 0xa3);
      emit16 (&seg->bytes, 0x002c);     /* es:[environment] = empty block */
    }
  vec_append (&seg->bytes, copy_tail, sizeof (copy_tail));
  if (direct_video)
    emit_save_initial_video_mode (&seg->bytes, rt);
  if (raw_keyboard)
    emit_stdin_raw_mode (&seg->bytes, rt);
  if (separate_psp)
    emit_ne_set_ds_es_to_segment (seg, psp_seg);
  if (initial_sp)
    {
      emit8 (&seg->bytes, 0xbc);        /* mov sp, DOS header SP */
      emit16 (&seg->bytes, initial_sp);
    }
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

  if (img->heap >= ELKS_MAX_HEAP)
    return;

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
  uint32_t entry_phys = ((uint32_t) h->cs << 4) + h->ip;
  uint32_t code_end = data_base > code_base && data_base < image_len
                      ? data_base : (uint32_t) image_len;
  size_t data_len = data_base < image_len ? image_len - data_base : 0;
  unsigned data_seg;
  unsigned psp_seg = 0;
  unsigned psp_top_seg = 0;
  unsigned env_seg = 0;
  int entry_seg;
  int runtime_in_code;
  int separate_psp = 0;
  uint16_t dos_alloc_max;
  uint32_t pos;
  uint16_t psp_top_para = 0;
  uint16_t i;

  (void) input_len;
  img->os2_ne = 1;
  init_image_memory (img, opts);
  dos_alloc_max = mz_image_max_dos_alloc_request (image, image_len);
  if (!opts->heap_set)
    img->heap = mz_heap_from_allocs (h->minalloc, h->maxalloc);
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

  runtime_in_code = data_base >= image_len && image_len <= ELKS_MAX16
                    && mz_entry_loads_ds_from_cs (image, image_len,
                                                  entry_phys);
  if (runtime_in_code)
    {
      enum
      {
        /*
         * Single-segment MZ programs often set DS and SS to CS and use
         * the PSP "top of memory" word to choose a private stack/heap
         * ceiling.  The converter appends runtime state and interrupt
         * stubs above that ceiling, so leave a full 8 KiB tail for those
         * generated helpers.  This keeps the NE segment comfortably below
         * the 64 KiB loader edge while still advertising a DOS-like
         * contiguous memory area below the runtime.
         */
        MZ_CODE_RUNTIME_TAIL = 8192u
      };
      struct ne_seg_image *seg = &img->ne_seg[0];
      uint32_t reserve = 0;
      uint32_t max_reserve = 0;

      if (seg->bytes.len < ELKS_MAX16)
        {
          max_reserve = ELKS_MAX16 - (uint32_t) seg->bytes.len;
          if (max_reserve > MZ_CODE_RUNTIME_TAIL)
            max_reserve -= MZ_CODE_RUNTIME_TAIL;
          else
            max_reserve = 0;
        }
      reserve = max_reserve;
      if (reserve)
        vec_append_zeros (&seg->bytes, (size_t) reserve);
      psp_top_para = align_para ((uint32_t) seg->bytes.len);
      append_runtime_state_to_data (&seg->bytes, img->heap, img->stack,
                                    img->bss, &rt);
      if (dos_alloc_max > 0x0fffu)
        ne_enable_conventional_dos_heap (img, 0, &rt, dos_alloc_max);
      else if (dos_alloc_max)
        ne_enable_far_dos_heap (img, 0, &rt);
    }
  else
    {
      append_runtime_state_to_data (&img->ne_seg[data_seg].bytes, img->heap,
                                    img->stack, img->bss, &rt);
      if (dos_alloc_max > 0x0fffu)
        ne_enable_conventional_dos_heap (img, data_seg, &rt, dos_alloc_max);
      else if (dos_alloc_max)
        ne_enable_far_dos_heap (img, data_seg, &rt);
    }

  data_len = img->ne_seg[data_seg].bytes.len;
  if (data_len + img->bss > ELKS_MAX16)
    die ("NE data segment is too large");
  img->ne_seg[data_seg].min_alloc = (uint16_t) (data_len + img->bss);
  if (h->ss == data_para && h->sp > img->ne_seg[data_seg].min_alloc)
    img->ne_seg[data_seg].min_alloc = h->sp;
  cap_ne_auto_heap (img, data_seg, opts);

  if (!runtime_in_code)
    for (i = 0; i < img->ne_nsegs; i++)
      if (!(img->ne_seg[i].flags & NESEG_DATA))
        {
          patch_mz_stack_setup (&img->ne_seg[i].bytes, stats);
          patch_dos_stack_switches (&img->ne_seg[i].bytes, stats);
        }

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

  entry_seg = ne_find_segment_for_phys (img, entry_phys);
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

  if (stats->bios_keyboard_input)
    stats->dynamic_int16 = 1;

  if (!runtime_in_code)
    {
      separate_psp = 1;
      env_seg = ne_add_env_segment (img);
      psp_seg = ne_add_psp_segment (img);
      psp_top_seg = data_seg;
      psp_top_para = ne_auto_top_para (img, data_seg);
    }

  if (stats->dynamic_int21 || stats->dynamic_int16)
    {
      uint16_t int21_handler = stats->dynamic_int21
        ? append_int21_interrupt_handler (&img->ne_seg[0].bytes, &rt) : 0;
      uint16_t int16_handler = stats->dynamic_int16
        ? append_int16_interrupt_handler (&img->ne_seg[0].bytes, &rt) : 0;

      append_ne_mz_startup (img, 0, (unsigned) entry_seg,
                            ne_local_offset (&img->ne_seg[entry_seg],
                                             entry_phys),
                            stats->dynamic_int21, int21_handler,
                            &rt,
                            stats->bios_keyboard_input,
                            stats->direct_video_output,
                            stats->dynamic_int16, int16_handler,
                            psp_top_para, separate_psp, psp_seg, psp_top_seg,
                            env_seg, h->sp);
    }
  else
    append_ne_mz_startup (img, 0, (unsigned) entry_seg,
                          ne_local_offset (&img->ne_seg[entry_seg],
                                           entry_phys),
                          0, 0, &rt, stats->bios_keyboard_input,
                          stats->direct_video_output, 0, 0,
                          psp_top_para, separate_psp, psp_seg, psp_top_seg,
                          env_seg, h->sp);
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
  uint32_t entry_phys;
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

  code_para = opts->mz_code_set
              ? opts->mz_code_seg
              : mz_code_para_covering_relocs (image, image_len, input, &h,
                                              h.cs);
  entry_phys = ((uint32_t) h.cs << 4) + h.ip;
  data_para = opts->mz_data_set
              ? opts->mz_data_seg
              : guess_mz_data_para (image, image_len, input, &h, code_para);
  if (!opts->mz_data_set)
    data_para = mz_stack_alias_para (image, image_len, input, &h, data_para);
  if (!opts->mz_data_set && data_para == h.ss && h.sp != 0
      && ((uint32_t) h.ss << 4) >= image_len)
    {
      /*
       * If the MZ stack segment starts beyond the initialized image, keep the
       * generated auto-data segment there.  Older C runtimes commonly use a
       * separate uninitialized stack/PSP area while addressing initialized
       * constants through relocated code-segment values.  Treating an earlier
       * relocated constant segment as auto-data gives ELKS a tiny break value
       * and makes those runtimes trip stack checks before they can process
       * their DOS command tail.
       */
      data_para = h.ss;
    }
  if (!opts->mz_data_set
      && image_len <= ELKS_MAX16
      && mz_entry_loads_ds_from_cs (image, image_len, entry_phys))
    data_para = align_para ((uint32_t) image_len);

  code_base = (uint32_t) code_para << 4;
  data_base = (uint32_t) data_para << 4;
  if (code_base >= image_len)
    die ("MZ code segment starts beyond the load image");

  if (data_base > code_base && data_base < image_len)
    text_len = (size_t) (data_base - code_base);
  else
    text_len = image_len - code_base;
  if (entry_phys < code_base
      || entry_phys - code_base >= (uint32_t) text_len)
    die ("MZ entry point is outside the selected text segment");
  data_len = data_base < image_len ? image_len - data_base : 0;

  if (opts->mz_output == MZ_OUT_OS2
      || (opts->mz_output == MZ_OUT_AUTO
          && (image_len > ELKS_MAX16
              || mz_ne_needed (data_base, text_len, data_len))))
    {
      convert_mz_ne (input, input_len, opts, img, stats, &h, exe_size,
                     code_para, data_para);
      return;
    }

  if (text_len == 0 || text_len > ELKS_MAX16 || data_len > ELKS_MAX16)
    die ("MZ text or data segment is too large for flat ELKS a.out; "
         "use the default --mz-output=os2 with CONFIG_EXEC_OS2=y");

  init_image_memory (img, opts);
  img->entry = (uint16_t) (entry_phys - code_base);
  if (!opts->heap_set)
    img->heap = mz_heap_from_allocs (h.minalloc, h.maxalloc);
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
  if (stats->bios_keyboard_input)
    stats->dynamic_int16 = 1;
  if (stats->dynamic_int21 || stats->dynamic_int16)
    {
      uint16_t int21_handler = stats->dynamic_int21
        ? append_int21_interrupt_handler (&img->text, &rt) : 0;
      uint16_t int16_handler = stats->dynamic_int16
        ? append_int16_interrupt_handler (&img->text, &rt) : 0;

      append_mz_argv_startup (img, h.ip, stats->dynamic_int21, int21_handler,
                              &rt,
                              stats->bios_keyboard_input,
                              stats->direct_video_output,
                              stats->dynamic_int16, int16_handler);
    }
  else
    append_mz_argv_startup (img, h.ip, 0, 0, &rt,
                            stats->bios_keyboard_input,
                            stats->direct_video_output, 0, 0);

  if (opts->verbose)
    fprintf (stderr,
             "msdos2elks: MZ cs=%04x ds=%04x text=%u data=%u heap=%u stack=%u\n",
             code_para, data_para, (unsigned) img->text.len,
             (unsigned) img->data.len, img->heap, img->stack);
}
