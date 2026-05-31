#include "internal.h"

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
                         uint16_t int21_handler,
                         const struct runtime_info *rt, int raw_keyboard,
                         int direct_video, int install_int16,
                         uint16_t int16_handler)
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
  if (install_int16)
    emit_install_int16_vector (&img->text, int16_handler);
  vec_append (&img->text, prefix, sizeof (prefix));
  if (raw_keyboard)
    emit_stdin_raw_mode (&img->text, rt);
  (void) direct_video;
  if (start > ELKS_MAX16 || img->text.len + 3u > ELKS_MAX16)
    die ("text segment grew beyond 64 KiB while adding COM argv startup");

  emit8 (&img->text, 0xe9);        /* jmp COM_ORG */
  rel = (uint16_t) (COM_ORG - (img->text.len + 2u));
  emit16 (&img->text, rel);
  img->entry = (uint16_t) start;
}

static void
install_com_return_exit (struct image *img, const struct runtime_info *rt)
{
  size_t exit_off;
  uint16_t rel;

  if (img->text.len < 3u)
    die ("COM text segment is too small for return-exit trampoline");
  exit_off = img->text.len;
  emit_exit_stub (&img->text, 0, rt);
  if (exit_off > ELKS_MAX16 || img->text.len > ELKS_MAX16)
    die ("text segment grew beyond 64 KiB while adding COM return exit");

  img->text.data[0] = 0xe9;        /* ret to 0000h -> native exit */
  rel = (uint16_t) (exit_off - 3u);
  put16 (img->text.data + 1u, rel);
}

static void
append_mz_argv_startup (struct image *img, uint16_t original_entry,
                        int install_int21, uint16_t int21_handler,
                        const struct runtime_info *rt, int raw_keyboard,
                        int direct_video, int install_int16,
                        uint16_t int16_handler)
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
  if (install_int16)
    emit_install_int16_vector (&img->text, int16_handler);
  vec_append (&img->text, prefix, sizeof (prefix));
  if (raw_keyboard)
    emit_stdin_raw_mode (&img->text, rt);
  (void) direct_video;
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

  if (data->len + 529u > ELKS_MAX16)
    die ("data segment is too large for converter runtime state");

  rt->heap_next_off = (uint16_t) data->len;
  rt->heap_limit_off = (uint16_t) (data->len + 2u);
  rt->dta_off_off = (uint16_t) (data->len + 4u);
  rt->video_mode_off = (uint16_t) (data->len + 6u);
  rt->heap_base_seg_off = (uint16_t) (data->len + 8u);
  rt->keyboard_fd_off = (uint16_t) (data->len + 10u);
  rt->keyboard_mode_off = (uint16_t) (data->len + 12u);
  rt->keyboard_pending_off = (uint16_t) (data->len + 14u);
  rt->io_buf_off = (uint16_t) (data->len + 16u);
  rt->media_id_off = (uint16_t) (data->len + 528u);
  emit16 (data, 0);
  emit16 (data, 0);
  emit16 (data, 0x80);
  emit16 (data, 0x0003);
  emit16 (data, 0);
  emit16 (data, 0xffffu);
  emit16 (data, 0);
  emit16 (data, 0);
  vec_append_zeros (data, 512u);
  emit8 (data, 0xf8);

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
