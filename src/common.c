#include "internal.h"

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

static int
bios_video_mode_is_text (uint8_t mode)
{
  mode &= 0x7fu;            /* bit 7 requests no clear on EGA/VGA BIOSes. */
  return mode <= 0x03u || mode == 0x07u;
}

static int
bios_video_mode_needs_console_lock (uint8_t mode)
{
  /*
   * BIOS modes 00h-03h are CGA text modes and 07h is MDA text mode on
   * IBM PC/XT-class display hardware.  Other mode numbers are graphics
   * modes or adapter-specific extensions.  A converted DOS program using
   * those modes may draw directly through B000h/B800h/A000h video memory,
   * so the generated runtime asks ELKS to stop console painting while the
   * program owns the display.  Unsupported adapter-specific modes are still
   * handed to the ROM BIOS; the converter does not emulate or validate them.
   */
  return !bios_video_mode_is_text (mode);
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
