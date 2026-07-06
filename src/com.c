#include "internal.h"

static int
com_has_ascii (const uint8_t *p, size_t len, const char *needle)
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
reject_known_com_packers (const uint8_t *input, size_t input_len)
{
  /*
   * A packed COM file still starts at offset 0100h, but that entry belongs to
   * the unpacking stub rather than to the DOS program the converter needs to
   * inspect.  Do not try to run or reveal that loader.  Reject obvious packer
   * signatures and require a plain XT-era COM image as input.
   */
  if (com_has_ascii (input, input_len, "UPX!"))
    die ("COM executable appears to be UPX packed; provide a plain DOS executable");
  if (com_has_ascii (input, input_len, "PKLITE"))
    die ("COM executable appears to be PKLITE packed; provide a plain DOS executable");
}

static void
init_com_psp (struct byte_vec *data)
{
  size_t i;

  /*
   * COM programs start with DS pointing at the Program Segment Prefix.  DOS C
   * runtimes use the PSP job file table at 0018h to discover inherited
   * handles.  Populate the standard DOS handles 0..4 and mark the remaining
   * slots unused so stdout and stderr are not mistaken for stdin or closed
   * handles.
   */
  data->data[0] = 0xcd;
  data->data[1] = 0x20;
  for (i = 0; i < 20u; i++)
    data->data[0x18u + i] = (i < 5u) ? (uint8_t) i : 0xffu;
  put16 (data->data + 0x32u, 20u);
  put16 (data->data + 0x34u, 0x18u);
  put16 (data->data + 0x36u, 0);
  data->data[0x80] = 0;
  data->data[0x81] = '\r';
}

void
convert_com (const uint8_t *input, size_t input_len,
             const struct options *opts, struct image *img,
             struct patch_stats *stats)
{
  struct runtime_info rt;
  uint32_t low_mem;
  uint32_t reserve;
  uint32_t heap_reserve;
  uint32_t used;
  uint32_t avail;

  if (input_len + COM_ORG > ELKS_MAX16)
    die ("COM image is too large for an ELKS 16-bit segment");
  reject_known_com_packers (input, input_len);

  init_image_memory (img, opts);
  img->entry = COM_ORG;

  vec_append_zeros (&img->text, COM_ORG);
  vec_append (&img->text, input, input_len);

  vec_append_zeros (&img->data, COM_ORG);
  init_com_psp (&img->data);
  vec_append (&img->data, input, input_len);

  low_mem = opts->bss_set ? img->bss : COM_DEFAULT_BSS;
  /*
   * ELKS copies argv/environ data near the initial user stack during exec.
   * Keep a small fixed slack area above the requested stack so large COM
   * images still exec with ordinary command-line arguments.
   */
  reserve = (uint32_t) img->stack + ELKS_ARG_SLACK;
  if (img->heap == 0)
    heap_reserve = ELKS_DEFAULT_HEAP;
  else if (img->heap < ELKS_MAX_HEAP)
    heap_reserve = img->heap;
  else
    heap_reserve = 0;
  reserve += heap_reserve;
  /*
   * Default COM scratch memory is deliberately generous for small programs,
   * but large real-mode COM tools still need room for the converter runtime
   * state and the ELKS stack.  Account for the exact runtime-state bytes here
   * before deciding how much default scratch space can fit.  Explicit
   * --bss=BYTES remains a hard request and fails instead of being reduced.
   */
  used = (uint32_t) img->data.len + RUNTIME_STATE_SIZE;
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
  patch_com_stack_pointer_setup (&img->text, stats);
  patch_dos_io (&img->text, stats, &rt, NULL);
  if (stats->dynamic_int21 || stats->dynamic_int16)
    {
      uint16_t int21_handler = stats->dynamic_int21
        ? append_int21_interrupt_handler (&img->text, &rt) : 0;
      uint16_t int16_handler = stats->dynamic_int16
        ? append_int16_interrupt_handler (&img->text, &rt) : 0;

      install_com_return_exit (img, &rt);
      append_com_argv_startup (img, stats->dynamic_int21, int21_handler,
                               &rt,
                               stats->bios_keyboard_input,
                               stats->graphics_output,
                               stats->dynamic_int16, int16_handler,
                               opts->startup_video_mode_set,
                               opts->startup_video_mode);
    }
  else
    {
      install_com_return_exit (img, &rt);
      append_com_argv_startup (img, 0, 0, &rt, stats->bios_keyboard_input,
                               stats->graphics_output, 0, 0,
                               opts->startup_video_mode_set,
                               opts->startup_video_mode);
    }
}
