#include "internal.h"

struct pklite_code
{
  uint16_t bits;
  uint8_t nbits;
  int value;
};

struct pklite_bits
{
  const uint8_t *input;
  size_t len;
  size_t pos;
  uint16_t bits;
  unsigned remain;
};

struct pklite_reloc
{
  uint16_t off;
  uint16_t seg;
};

struct pklite_reloc_vec
{
  struct pklite_reloc *data;
  size_t len;
  size_t cap;
};

static const struct pklite_code pklite_count_large[] =
{
  { 0x02, 2, 2 }, { 0x03, 2, 3 }, { 0x00, 3, 4 },
  { 0x02, 4, 5 }, { 0x03, 4, 6 }, { 0x04, 4, 7 },
  { 0x0a, 5, 8 }, { 0x0b, 5, 9 }, { 0x0c, 5, 10 },
  { 0x1a, 6, 11 }, { 0x1b, 6, 12 }, { 0x1c, 6, -1 },
  { 0x3a, 7, 13 }, { 0x3b, 7, 14 }, { 0x3c, 7, 15 },
  { 0x7a, 8, 16 }, { 0x7b, 8, 17 }, { 0x7c, 8, 18 },
  { 0x0fa, 9, 19 }, { 0x0fb, 9, 20 }, { 0x0fc, 9, 21 },
  { 0x0fd, 9, 22 }, { 0x0fe, 9, 23 }, { 0x0ff, 9, 24 }
};

static const struct pklite_code pklite_count_small[] =
{
  { 0x02, 3, 2 }, { 0x00, 2, 3 }, { 0x04, 3, 4 },
  { 0x05, 3, 5 }, { 0x0c, 4, 6 }, { 0x0d, 4, 7 },
  { 0x0e, 4, 8 }, { 0x0f, 4, 9 }, { 0x03, 3, -1 }
};

static const struct pklite_code pklite_offset_code[] =
{
  { 0x01, 1, 0 }, { 0x00, 4, 1 }, { 0x01, 4, 2 },
  { 0x04, 5, 3 }, { 0x05, 5, 4 }, { 0x06, 5, 5 },
  { 0x07, 5, 6 }, { 0x10, 6, 7 }, { 0x11, 6, 8 },
  { 0x12, 6, 9 }, { 0x13, 6, 10 }, { 0x14, 6, 11 },
  { 0x15, 6, 12 }, { 0x16, 6, 13 }, { 0x2e, 7, 14 },
  { 0x2f, 7, 15 }, { 0x30, 7, 16 }, { 0x31, 7, 17 },
  { 0x32, 7, 18 }, { 0x33, 7, 19 }, { 0x34, 7, 20 },
  { 0x35, 7, 21 }, { 0x36, 7, 22 }, { 0x37, 7, 23 },
  { 0x38, 7, 24 }, { 0x39, 7, 25 }, { 0x3a, 7, 26 },
  { 0x3b, 7, 27 }, { 0x3c, 7, 28 }, { 0x3d, 7, 29 },
  { 0x3e, 7, 30 }, { 0x3f, 7, 31 }
};

static int
pklite_has_ascii (const uint8_t *p, size_t len, const char *needle)
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

static int
pklite_apply_lemmings_cga_patch (struct byte_vec *body)
{
  static const uint8_t loader_patch[] = { 0xe8, 0xc5, 0x04 };
  size_t loader_off = 0x00e6u;
  int patched = 0;

  if (body->len < loader_off + sizeof (loader_patch))
    return 0;
  if (memcmp (body->data + loader_off, loader_patch,
              sizeof (loader_patch)) != 0)
    return 0;
  if (!pklite_has_ascii (body->data, body->len,
                         "PC Lemmings Machine Type Selection Screen")
      || !pklite_has_ascii (body->data, body->len, "cgamain.dat"))
    return 0;

  memset (body->data + loader_off, 0x90, sizeof (loader_patch));
  patched++;

  return patched;
}

static size_t
pklite_mz_file_size (const struct mz_header *h, size_t input_len)
{
  uint32_t size;

  if (h->cp == 0)
    return input_len;

  size = ((uint32_t) h->cp - 1u) * 512u;
  size += h->cblp ? h->cblp : 512u;
  if (size > input_len)
    die ("PKLITE MZ header file size is larger than input file");
  return (size_t) size;
}

static size_t
pklite_decompressor_len (uint16_t ver_code, const uint8_t *input,
                         size_t exe_size, size_t code_off)
{
  switch (ver_code)
    {
    case 0x0100: case 0x0103: case 0x0105: case 0x010c:
    case 0x010d: case 0x010e: case 0x010f:
      return 0x1d0u;
    case 0x1103: case 0x110c: case 0x110d:
      return 0x1e0u;
    case 0x110e: case 0x110f:
      return 0x200u;
    case 0x2100: case 0x2103: case 0x2105: case 0x210a:
    case 0x210c: case 0x210d: case 0x210e: case 0x210f:
      return 0x290u;
    case 0x3103:
      return 0x2a0u;
    case 0x310c: case 0x310d:
      return 0x290u;
    case 0x310e: case 0x310f:
      return 0x2c0u;
    case 0x0132: case 0x0201: case 0x2132: case 0x2201:
      if (code_off + 0x4au > exe_size)
        die ("PKLITE decompressor is truncated");
      return (size_t) ((((uint32_t) get16 (input + code_off + 0x48u) << 1)
                        + 0x62u) & ~(uint32_t) 0x0fu);
    default:
      die ("unsupported PKLITE version");
    }
  return 0;
}

static void
pklite_bits_load (struct pklite_bits *r)
{
  if (r->pos + 2u > r->len)
    die ("truncated PKLITE bit stream");
  r->bits = get16 (r->input + r->pos);
  r->pos += 2u;
  r->remain = 16u;
}

static void
pklite_bits_init (struct pklite_bits *r, const uint8_t *input, size_t len,
                  size_t pos)
{
  r->input = input;
  r->len = len;
  r->pos = pos;
  r->bits = 0;
  r->remain = 0;
  pklite_bits_load (r);
}

static uint8_t
pklite_read_byte (struct pklite_bits *r)
{
  if (r->pos >= r->len)
    die ("truncated PKLITE byte stream");
  return r->input[r->pos++];
}

static unsigned
pklite_read_bit (struct pklite_bits *r)
{
  unsigned bit = r->bits & 1u;

  r->bits >>= 1;
  if (--r->remain == 0)
    pklite_bits_load (r);
  return bit;
}

static int
pklite_read_code (struct pklite_bits *r, const struct pklite_code *table,
                  size_t table_len)
{
  uint16_t bits = 0;
  unsigned nbits;
  size_t i;

  for (nbits = 1u; nbits <= 16u; nbits++)
    {
      bits = (uint16_t) ((bits << 1) | pklite_read_bit (r));
      for (i = 0; i < table_len; i++)
        if (table[i].nbits == nbits && table[i].bits == bits)
          return table[i].value;
    }

  die ("bad PKLITE prefix code");
  return 0;
}

static void
pklite_reloc_add (struct pklite_reloc_vec *v, uint16_t off, uint16_t seg)
{
  struct pklite_reloc *p;
  size_t ncap;

  if (v->len == UINT16_MAX)
    die ("PKLITE relocation table is too large");
  if (v->len == v->cap)
    {
      ncap = v->cap ? v->cap * 2u : 32u;
      if (ncap < v->cap)
        die ("PKLITE relocation capacity overflow");
      p = (struct pklite_reloc *) realloc (v->data, ncap * sizeof (*v->data));
      if (!p)
        die ("out of memory");
      v->data = p;
      v->cap = ncap;
    }

  v->data[v->len].off = off;
  v->data[v->len].seg = seg;
  v->len++;
}

static int
pklite_parse_long_relocs (const uint8_t *input, size_t exe_size, size_t pos,
                          struct pklite_reloc_vec *rels,
                          size_t *footer_pos)
{
  uint16_t seg = 0;

  memset (rels, 0, sizeof (*rels));
  for (;;)
    {
      uint16_t count;
      uint16_t i;

      if (pos + 2u > exe_size)
        goto fail;
      count = get16 (input + pos);
      pos += 2u;
      if (count == 0xffffu)
        break;
      if (pos + (size_t) count * 2u > exe_size)
        goto fail;
      for (i = 0; i < count; i++)
        {
          pklite_reloc_add (rels, get16 (input + pos), seg);
          pos += 2u;
        }
      seg = (uint16_t) (seg + 0x0fffu);
    }

  if (pos + 8u > exe_size)
    goto fail;
  *footer_pos = pos;
  return 1;

fail:
  free (rels->data);
  memset (rels, 0, sizeof (*rels));
  return 0;
}

static int
pklite_parse_short_relocs (const uint8_t *input, size_t exe_size, size_t pos,
                           struct pklite_reloc_vec *rels,
                           size_t *footer_pos)
{
  memset (rels, 0, sizeof (*rels));
  for (;;)
    {
      uint8_t count;
      uint16_t seg;
      uint8_t i;

      if (pos >= exe_size)
        goto fail;
      count = input[pos++];
      if (count == 0)
        break;
      if (pos + 2u + (size_t) count * 2u > exe_size)
        goto fail;
      seg = get16 (input + pos);
      pos += 2u;
      for (i = 0; i < count; i++)
        {
          pklite_reloc_add (rels, get16 (input + pos), seg);
          pos += 2u;
        }
    }

  if (pos + 8u > exe_size)
    goto fail;
  *footer_pos = pos;
  return 1;

fail:
  free (rels->data);
  memset (rels, 0, sizeof (*rels));
  return 0;
}

static void
pklite_decode_relocs (const uint8_t *input, size_t exe_size, size_t pos,
                      int prefer_long, struct pklite_reloc_vec *rels,
                      size_t *footer_pos)
{
  struct pklite_reloc_vec first;
  struct pklite_reloc_vec second;
  size_t first_footer = 0;
  size_t second_footer = 0;
  int first_ok;
  int second_ok;

  first_ok = prefer_long
             ? pklite_parse_long_relocs (input, exe_size, pos, &first,
                                         &first_footer)
             : pklite_parse_short_relocs (input, exe_size, pos, &first,
                                          &first_footer);
  if (first_ok && first_footer + 8u == exe_size)
    {
      *rels = first;
      *footer_pos = first_footer;
      return;
    }

  second_ok = prefer_long
              ? pklite_parse_short_relocs (input, exe_size, pos, &second,
                                           &second_footer)
              : pklite_parse_long_relocs (input, exe_size, pos, &second,
                                          &second_footer);
  if (second_ok && second_footer + 8u == exe_size)
    {
      free (first.data);
      *rels = second;
      *footer_pos = second_footer;
      return;
    }

  if (first_ok)
    {
      free (second.data);
      *rels = first;
      *footer_pos = first_footer;
      return;
    }
  if (second_ok)
    {
      *rels = second;
      *footer_pos = second_footer;
      return;
    }

  die ("bad PKLITE relocation table");
}

static void
pklite_emit_revealed_mz (struct byte_vec *exe, const struct byte_vec *body,
                         const struct pklite_reloc_vec *rels,
                         uint8_t minor, uint8_t major, uint16_t ss,
                         uint16_t sp, uint16_t cs, uint16_t ip)
{
  size_t extra_len = (major & 0x10u) ? 2u : 0;
  size_t off_reloc = 0x1cu + extra_len;
  size_t header_len = (off_reloc + rels->len * 4u + 15u) & ~(size_t) 15u;
  size_t image_paras = (body->len + 15u) >> 4;
  size_t stack_paras = (((uint32_t) ss << 4) + sp + 15u) >> 4;
  size_t minalloc = stack_paras > image_paras ? stack_paras - image_paras : 0;
  size_t total = header_len + body->len;
  size_t i;

  if (header_len > UINT16_MAX || total > 0x1ffffffu)
    die ("revealed PKLITE executable is too large");

  vec_append_zeros (exe, header_len);
  exe->data[0] = 'M';
  exe->data[1] = 'Z';
  put16 (exe->data + 2u, (uint16_t) (total & 0x1ffu));
  put16 (exe->data + 4u, (uint16_t) ((total + 0x1ffu) >> 9));
  put16 (exe->data + 6u, (uint16_t) rels->len);
  put16 (exe->data + 8u, (uint16_t) (header_len >> 4));
  put16 (exe->data + 10u, minalloc > UINT16_MAX
         ? UINT16_MAX : (uint16_t) minalloc);
  put16 (exe->data + 12u, UINT16_MAX);
  put16 (exe->data + 14u, ss);
  put16 (exe->data + 16u, sp);
  put16 (exe->data + 18u, 0);
  put16 (exe->data + 20u, ip);
  put16 (exe->data + 22u, cs);
  put16 (exe->data + 24u, (uint16_t) off_reloc);
  put16 (exe->data + 26u, 0);
  if (extra_len)
    {
      exe->data[0x1cu] = minor;
      exe->data[0x1du] = major;
    }

  for (i = 0; i < rels->len; i++)
    {
      size_t pos = off_reloc + i * 4u;

      put16 (exe->data + pos, rels->data[i].off);
      put16 (exe->data + pos + 2u, rels->data[i].seg);
    }

  vec_append (exe, body->data, body->len);
}

static int
pklite_reveal_mz (const uint8_t *input, size_t input_len, uint8_t **out,
                  size_t *out_len, int verbose)
{
  struct mz_header h;
  struct pklite_bits bits;
  struct byte_vec body;
  struct byte_vec exe;
  struct pklite_reloc_vec rels;
  size_t exe_size;
  size_t code_off;
  size_t comp_off;
  size_t footer_pos;
  uint8_t minor;
  uint8_t major;
  uint16_t ver_code;
  int large;
  int extra;
  int lemmings_patch;

  if (input_len < 0x40u || get16 (input) != MZ_MAGIC)
    return 0;
  if (!pklite_has_ascii (input, input_len < 512u ? input_len : 512u,
                         "PKLITE"))
    return 0;

  memset (&h, 0, sizeof (h));
  h.cblp = get16 (input + 2u);
  h.cp = get16 (input + 4u);
  h.crlc = get16 (input + 6u);
  h.cparhdr = get16 (input + 8u);
  h.lfarlc = get16 (input + 24u);
  exe_size = pklite_mz_file_size (&h, input_len);
  code_off = (size_t) h.cparhdr * 16u;
  if (code_off >= exe_size || h.lfarlc < 0x1eu)
    die ("bad PKLITE MZ header");

  minor = input[0x1cu];
  major = input[0x1du];
  ver_code = (uint16_t) (((uint16_t) major << 8) | minor);
  large = (major & 0x20u) != 0;
  extra = (major & 0x10u) != 0;
  comp_off = code_off + pklite_decompressor_len (ver_code, input, exe_size,
                                                 code_off);
  if (comp_off >= exe_size)
    die ("bad PKLITE compressed data offset");

  memset (&body, 0, sizeof (body));
  memset (&exe, 0, sizeof (exe));
  pklite_bits_init (&bits, input, exe_size, comp_off);
  for (;;)
    {
      if (pklite_read_bit (&bits))
        {
          const struct pklite_code *count_table;
          size_t count_len;
          int count;
          uint16_t off;
          uint16_t off_hi = 0;
          uint8_t off_lo;
          size_t copy_from;
          size_t i;

          count_table = large ? pklite_count_large : pklite_count_small;
          count_len = large
                      ? sizeof (pklite_count_large) / sizeof (pklite_count_large[0])
                      : sizeof (pklite_count_small) / sizeof (pklite_count_small[0]);
          count = pklite_read_code (&bits, count_table, count_len);
          if (count < 0)
            {
              uint8_t code = pklite_read_byte (&bits);

              if (code == 0xfeu)
                {
                  if (!large)
                    die ("unsupported PKLITE uncompressed region");
                  continue;
                }
              if (code == 0xffu)
                break;
              if (code == 0xfdu && large)
                die ("unsupported PKLITE uncompressed region");
              count = (int) code + (large ? 25 : 10);
            }

          if (count != 2)
            off_hi = (uint16_t) pklite_read_code
              (&bits, pklite_offset_code,
               sizeof (pklite_offset_code) / sizeof (pklite_offset_code[0]));
          off_lo = pklite_read_byte (&bits);
          off = (uint16_t) (off_lo | (off_hi << 8));
          copy_from = body.len >= off ? body.len - off : SIZE_MAX;
          for (i = 0; i < (size_t) count; i++)
            {
              uint8_t b = 0;
              size_t src = copy_from == SIZE_MAX ? SIZE_MAX : copy_from + i;

              if (src < body.len)
                b = body.data[src];
              emit8 (&body, b);
              if (body.len > 16u * 1024u * 1024u)
                die ("PKLITE output is too large");
            }
        }
      else
        {
          uint8_t b = pklite_read_byte (&bits);

          if (extra)
            b ^= (uint8_t) bits.remain;
          emit8 (&body, b);
        }
    }

  pklite_decode_relocs (input, exe_size, bits.pos, extra, &rels, &footer_pos);
  lemmings_patch = pklite_apply_lemmings_cga_patch (&body);
  pklite_emit_revealed_mz (&exe, &body, &rels, minor, major,
                           get16 (input + footer_pos),
                           get16 (input + footer_pos + 2u),
                           get16 (input + footer_pos + 4u),
                           get16 (input + footer_pos + 6u));

  if (verbose)
    fprintf (stderr,
             "msdos2elks: revealed PKLITE MZ version %u.%02u"
             " large=%u extra=%u image=%u relocs=%u\n",
             (unsigned) (major & 0x0fu), (unsigned) minor,
             large ? 1u : 0u, extra ? 1u : 0u,
             (unsigned) body.len, (unsigned) rels.len);
  if (verbose && lemmings_patch)
    fprintf (stderr,
             "msdos2elks: applied Lemmings CGA loader compatibility patch\n");

  free (body.data);
  free (rels.data);
  *out = exe.data;
  *out_len = exe.len;
  return 1;
}
