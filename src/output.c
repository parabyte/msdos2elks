#include "internal.h"

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
  /*
   * ELKS' OS/2 NE loader treats ffffh as "maximum heap".  The converter uses
   * ELKS_MAX_HEAP internally as the 64 KiB-minus-paragraph sentinel because
   * that value is also safe for flat a.out accounting.
   */
  emit16 (&out, img->heap >= ELKS_MAX_HEAP ? 0xffffu : img->heap);
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

void
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

void
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
