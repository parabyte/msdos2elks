#include "internal.h"

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

