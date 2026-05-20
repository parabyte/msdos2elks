#include "internal.h"

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
  emit8 (v, 0x50);          /* push ax */
  emit8 (v, 0x1e);          /* push ds */
  emit8 (v, 0x88);
  emit8 (v, 0xc3);          /* bl = al */
  emit8 (v, 0x30);
  emit8 (v, 0xff);          /* bh = 0 */
  emit8 (v, 0xd1);
  emit8 (v, 0xe3);          /* bx *= 2 */
  emit8 (v, 0xd1);
  emit8 (v, 0xe3);          /* bx *= 4 */
  emit8 (v, 0x31);
  emit8 (v, 0xc0);          /* ax = 0 */
  emit8 (v, 0x8e);
  emit8 (v, 0xd8);          /* ds = IVT segment */
  emit8 (v, 0xc4);
  emit8 (v, 0x1f);          /* les bx, [bx] */
  emit8 (v, 0x1f);          /* pop ds */
  emit8 (v, 0x58);          /* pop ax */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_set_vector_stub (struct byte_vec *v)
{
  emit8 (v, 0x50);          /* push ax */
  emit8 (v, 0x53);          /* push bx */
  emit8 (v, 0x06);          /* push es */
  emit8 (v, 0x88);
  emit8 (v, 0xc3);          /* bl = al */
  emit8 (v, 0x30);
  emit8 (v, 0xff);          /* bh = 0 */
  emit8 (v, 0xd1);
  emit8 (v, 0xe3);          /* bx *= 2 */
  emit8 (v, 0xd1);
  emit8 (v, 0xe3);          /* bx *= 4 */
  emit8 (v, 0x31);
  emit8 (v, 0xc0);          /* ax = 0 */
  emit8 (v, 0x8e);
  emit8 (v, 0xc0);          /* es = IVT segment */
  emit8 (v, 0xfa);          /* cli */
  emit8 (v, 0x26);
  emit8 (v, 0x89);
  emit8 (v, 0x17);          /* es:[bx] = dx */
  emit8 (v, 0x26);
  emit8 (v, 0x8c);
  emit8 (v, 0x5f);
  emit8 (v, 0x02);          /* es:[bx+2] = ds */
  emit8 (v, 0xfb);          /* sti */
  emit8 (v, 0x07);          /* pop es */
  emit8 (v, 0x5b);          /* pop bx */
  emit8 (v, 0x58);          /* pop ax */
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
emit_get_allocation_info_stub (struct byte_vec *v,
                               const struct runtime_info *rt)
{
  emit8 (v, 0xb8);
  emit16 (v, 1);            /* AL = sectors per cluster */
  emit8 (v, 0xb9);
  emit16 (v, 512);          /* bytes per sector */
  emit8 (v, 0xba);
  emit16 (v, 0x1000);       /* clusters */
  emit8 (v, 0xbb);
  emit16 (v, rt->media_id_off);
  emit8 (v, 0x16);
  emit8 (v, 0x1f);          /* DS:BX = media ID byte */
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
  size_t fmem_pos;
  size_t overlarge_pos;
  size_t query_pos;
  size_t ja_pos;
  size_t fail_pos;
  size_t sys_fail_pos;

  emit8 (v, 0x53);          /* push bx */
  emit8 (v, 0x51);          /* push cx */
  emit8 (v, 0x52);          /* push dx */
  emit8 (v, 0x8b);
  emit8 (v, 0x16);
  emit16 (v, rt->heap_base_seg_off);
  emit8 (v, 0x83);
  emit8 (v, 0xfa);
  emit8 (v, 0xff);          /* cmp dx, 0ffffh */
  emit8 (v, 0x74);          /* je fmemalloc */
  fmem_pos = v->len;
  emit8 (v, 0);
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
  emit8 (v, 0x8b);
  emit8 (v, 0x16);
  emit16 (v, rt->heap_base_seg_off);
  emit8 (v, 0x09);
  emit8 (v, 0xd2);          /* or dx, dx */
  emit8 (v, 0x75);
  emit8 (v, 0x02);          /* jnz have_base */
  emit8 (v, 0x8c);
  emit8 (v, 0xda);          /* dx = ds */
  emit8 (v, 0x01);
  emit8 (v, 0xd0);          /* ax += allocation base segment */
  emit8 (v, 0x5a);
  emit8 (v, 0x59);
  emit8 (v, 0x5b);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
  fail_pos = v->len;
  emit8 (v, 0x89);
  emit8 (v, 0xc3);          /* bx = largest available block */
  emit8 (v, 0xb8);
  emit16 (v, 8);            /* insufficient memory */
  emit8 (v, 0x5a);
  emit8 (v, 0x59);
  emit8 (v, 0x83);
  emit8 (v, 0xc4);
  emit8 (v, 0x02);          /* discard saved bx */
  emit8 (v, 0xf9);
  emit8 (v, 0xc3);
  v->data[ja_pos] = (uint8_t) (fail_pos - (ja_pos + 1u));

  v->data[fmem_pos] = (uint8_t) (v->len - (fmem_pos + 1u));
  emit8 (v, 0x83);
  emit8 (v, 0xfb);
  emit8 (v, 0xff);          /* cmp bx, 0ffffh */
  emit8 (v, 0x74);          /* je query_largest */
  query_pos = v->len;
  emit8 (v, 0);
  emit8 (v, 0x3b);
  emit8 (v, 0x1e);
  emit16 (v, rt->heap_limit_off);       /* cmp bx, largest advertised */
  emit8 (v, 0x77);                      /* ja overlarge_probe */
  overlarge_pos = v->len;
  emit8 (v, 0);
  emit8 (v, 0xc7);
  emit8 (v, 0x06);
  emit16 (v, rt->heap_next_off);
  emit16 (v, 0);            /* clear output segment word */
  emit8 (v, 0xb9);
  emit16 (v, rt->heap_next_off);
  emit8 (v, 0xb8);
  emit16 (v, 82);           /* fmemalloc */
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit8 (v, 0x85);
  emit8 (v, 0xc0);          /* test ax, ax */
  emit8 (v, 0x78);          /* js sys_fail */
  sys_fail_pos = v->len;
  emit8 (v, 0);
  emit8 (v, 0xa1);
  emit16 (v, rt->heap_next_off);        /* ax = allocated segment */
  emit8 (v, 0x5a);
  emit8 (v, 0x59);
  emit8 (v, 0x5b);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
  v->data[sys_fail_pos] = (uint8_t) (v->len - (sys_fail_pos + 1u));
  emit8 (v, 0x8b);
  emit8 (v, 0x1e);
  emit16 (v, rt->heap_limit_off);
  emit8 (v, 0xb8);
  emit16 (v, 8);            /* insufficient memory */
  emit8 (v, 0x5a);
  emit8 (v, 0x59);
  emit8 (v, 0x83);
  emit8 (v, 0xc4);
  emit8 (v, 0x02);          /* discard saved bx */
  emit8 (v, 0xf9);
  emit8 (v, 0xc3);

  v->data[overlarge_pos] = (uint8_t) (v->len - (overlarge_pos + 1u));
  emit8 (v, 0xb8);
  emit16 (v, 0x1000u);      /* transient probe segment, not a real arena */
  emit8 (v, 0x5a);
  emit8 (v, 0x59);
  emit8 (v, 0x5b);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);

  v->data[query_pos] = (uint8_t) (v->len - (query_pos + 1u));
  emit8 (v, 0x8b);
  emit8 (v, 0x1e);
  emit16 (v, rt->heap_limit_off);
  emit8 (v, 0xb8);
  emit16 (v, 8);            /* DOS reports largest block with CF set */
  emit8 (v, 0x5a);
  emit8 (v, 0x59);
  emit8 (v, 0x83);
  emit8 (v, 0xc4);
  emit8 (v, 0x02);          /* discard saved bx */
  emit8 (v, 0xf9);
  emit8 (v, 0xc3);
}

static void
emit_free_stub (struct byte_vec *v, const struct runtime_info *rt)
{
  size_t success_from_not_fmem;
  size_t success_from_fake;
  size_t fail_pos;

  emit8 (v, 0x53);          /* push bx */
  emit8 (v, 0x52);          /* push dx */
  emit8 (v, 0x8b);
  emit8 (v, 0x16);
  emit16 (v, rt->heap_base_seg_off);
  emit8 (v, 0x83);
  emit8 (v, 0xfa);
  emit8 (v, 0xff);          /* cmp dx, 0ffffh */
  emit8 (v, 0x75);          /* jne success */
  success_from_not_fmem = v->len;
  emit8 (v, 0);
  emit8 (v, 0x06);          /* push es */
  emit8 (v, 0x5b);          /* bx = segment to free */
  emit8 (v, 0x81);
  emit8 (v, 0xfb);
  emit16 (v, 0x1000u);
  emit8 (v, 0x74);          /* fake probe segment */
  success_from_fake = v->len;
  emit8 (v, 0);
  emit8 (v, 0xb8);
  emit16 (v, 83);           /* fmemfree */
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit8 (v, 0x85);
  emit8 (v, 0xc0);
  emit8 (v, 0x78);          /* js fail */
  fail_pos = v->len;
  emit8 (v, 0);
  v->data[success_from_not_fmem] =
    (uint8_t) (v->len - (success_from_not_fmem + 1u));
  v->data[success_from_fake] = (uint8_t) (v->len - (success_from_fake + 1u));
  emit8 (v, 0x5a);
  emit8 (v, 0x5b);
  emit8 (v, 0x31);
  emit8 (v, 0xc0);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
  v->data[fail_pos] = (uint8_t) (v->len - (fail_pos + 1u));
  emit8 (v, 0xf7);
  emit8 (v, 0xd8);
  emit8 (v, 0x5a);
  emit8 (v, 0x5b);
  emit8 (v, 0xf9);
  emit8 (v, 0xc3);
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
emit_read_stub (struct byte_vec *v, const struct runtime_info *rt)
{
  size_t far_pos;
  size_t loop_pos;
  size_t done_from_empty;
  size_t chunk_ok_from_jbe;
  size_t error_from_js;
  size_t done_from_zero;
  size_t done_from_short;
  size_t loop_from_jmp;
  size_t done_pos;
  size_t error_pos;
  uint16_t rel;

  emit8 (v, 0x50);          /* push ax */
  emit8 (v, 0x52);          /* push dx */
  emit8 (v, 0x8c);
  emit8 (v, 0xd8);          /* ax = ds */
  emit8 (v, 0x8c);
  emit8 (v, 0xd2);          /* dx = ss */
  emit8 (v, 0x39);
  emit8 (v, 0xd0);          /* cmp ax, dx */
  emit8 (v, 0x5a);
  emit8 (v, 0x58);
  emit8 (v, 0x75);          /* jne far_path */
  far_pos = v->len;
  emit8 (v, 0);
  emit_readwrite_stub (v, 3);

  v->data[far_pos] = (uint8_t) (v->len - (far_pos + 1u));
  emit8 (v, 0x53);          /* push bx */
  emit8 (v, 0x51);          /* push cx */
  emit8 (v, 0x52);          /* push dx */
  emit8 (v, 0x56);          /* push si */
  emit8 (v, 0x57);          /* push di */
  emit8 (v, 0x55);          /* push bp */
  emit8 (v, 0x1e);          /* push ds */
  emit8 (v, 0x06);          /* push es */
  emit8 (v, 0x1e);
  emit8 (v, 0x07);          /* es = destination ds */
  emit8 (v, 0x89);
  emit8 (v, 0xd7);          /* di = destination offset */
  emit8 (v, 0x31);
  emit8 (v, 0xed);          /* bp = total bytes read */

  loop_pos = v->len;
  emit8 (v, 0x09);
  emit8 (v, 0xc9);          /* or cx, cx */
  emit8 (v, 0x74);          /* jz done */
  done_from_empty = v->len;
  emit8 (v, 0);
  emit8 (v, 0x89);
  emit8 (v, 0xca);          /* dx = remaining */
  emit8 (v, 0x81);
  emit8 (v, 0xfa);
  emit16 (v, 512u);         /* cmp dx, 512 */
  emit8 (v, 0x76);          /* jbe chunk_ok */
  chunk_ok_from_jbe = v->len;
  emit8 (v, 0);
  emit8 (v, 0xba);
  emit16 (v, 512u);         /* dx = chunk */
  v->data[chunk_ok_from_jbe] =
    (uint8_t) (v->len - (chunk_ok_from_jbe + 1u));

  emit8 (v, 0x51);          /* push remaining */
  emit8 (v, 0x52);          /* push chunk */
  emit8 (v, 0x53);          /* push fd */
  emit8 (v, 0x06);          /* push destination segment */
  emit8 (v, 0x57);          /* push destination offset */
  emit8 (v, 0x55);          /* push total */
  emit8 (v, 0xb9);
  emit16 (v, rt->io_buf_off);
  emit8 (v, 0x16);
  emit8 (v, 0x1f);          /* ds = ss */
  emit8 (v, 0xb8);
  emit16 (v, 3);            /* read */
  emit8 (v, 0xcd);
  emit8 (v, 0x80);
  emit8 (v, 0x5d);          /* pop total */
  emit8 (v, 0x5f);          /* pop destination offset */
  emit8 (v, 0x07);          /* pop destination segment */
  emit8 (v, 0x5b);          /* pop fd */
  emit8 (v, 0x5a);          /* pop chunk */
  emit8 (v, 0x59);          /* pop remaining */
  emit8 (v, 0x85);
  emit8 (v, 0xc0);          /* test ax, ax */
  emit8 (v, 0x78);          /* js error */
  error_from_js = v->len;
  emit8 (v, 0);
  emit8 (v, 0x09);
  emit8 (v, 0xc0);          /* or ax, ax */
  emit8 (v, 0x74);          /* jz done */
  done_from_zero = v->len;
  emit8 (v, 0);
  emit8 (v, 0x51);          /* push remaining */
  emit8 (v, 0x50);          /* push actual */
  emit8 (v, 0x89);
  emit8 (v, 0xc1);          /* cx = actual */
  emit8 (v, 0xbe);
  emit16 (v, rt->io_buf_off);
  emit8 (v, 0x16);
  emit8 (v, 0x1f);          /* ds = ss */
  emit8 (v, 0xfc);          /* cld */
  emit8 (v, 0xf3);
  emit8 (v, 0xa4);          /* rep movsb */
  emit8 (v, 0x58);          /* pop actual */
  emit8 (v, 0x59);          /* pop remaining */
  emit8 (v, 0x01);
  emit8 (v, 0xc5);          /* bp += actual */
  emit8 (v, 0x29);
  emit8 (v, 0xc1);          /* remaining -= actual */
  emit8 (v, 0x39);
  emit8 (v, 0xd0);          /* cmp ax, chunk */
  emit8 (v, 0x72);          /* jb done */
  done_from_short = v->len;
  emit8 (v, 0);
  emit8 (v, 0xeb);          /* jmp loop */
  loop_from_jmp = v->len;
  emit8 (v, 0);

  done_pos = v->len;
  emit8 (v, 0x89);
  emit8 (v, 0xe8);          /* ax = total */
  emit8 (v, 0x07);
  emit8 (v, 0x1f);
  emit8 (v, 0x5d);
  emit8 (v, 0x5f);
  emit8 (v, 0x5e);
  emit8 (v, 0x5a);
  emit8 (v, 0x59);
  emit8 (v, 0x5b);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);

  error_pos = v->len;
  emit8 (v, 0x09);
  emit8 (v, 0xed);          /* or bp, bp */
  emit8 (v, 0x75);          /* jnz done */
  rel = (uint16_t) (done_pos - (v->len + 1u));
  emit8 (v, (uint8_t) rel);
  emit8 (v, 0xf7);
  emit8 (v, 0xd8);          /* ax = positive errno */
  emit8 (v, 0x07);
  emit8 (v, 0x1f);
  emit8 (v, 0x5d);
  emit8 (v, 0x5f);
  emit8 (v, 0x5e);
  emit8 (v, 0x5a);
  emit8 (v, 0x59);
  emit8 (v, 0x5b);
  emit8 (v, 0xf9);
  emit8 (v, 0xc3);

  v->data[done_from_empty] = (uint8_t) (done_pos - (done_from_empty + 1u));
  v->data[error_from_js] = (uint8_t) (error_pos - (error_from_js + 1u));
  v->data[done_from_zero] = (uint8_t) (done_pos - (done_from_zero + 1u));
  v->data[done_from_short] = (uint8_t) (done_pos - (done_from_short + 1u));
  v->data[loop_from_jmp] = (uint8_t) (loop_pos - (loop_from_jmp + 1u));
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
emit_read_char_stub (struct byte_vec *v, int echo, int bios_key)
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
  if (bios_key)
    {
      emit8 (v, 0x30);
      emit8 (v, 0xe4);      /* ah = 0 for unknown scan code */
      emit8 (v, 0x3c);
      emit8 (v, 0x0a);      /* map ELKS newline to DOS carriage return */
      emit8 (v, 0x75);
      emit8 (v, 0x02);
      emit8 (v, 0xb0);
      emit8 (v, 0x0d);
      emit8 (v, 0x3c);
      emit8 (v, 0x0d);
      emit8 (v, 0x75);
      emit8 (v, 0x02);
      emit8 (v, 0xb4);
      emit8 (v, 0x1c);      /* Enter scan code */
    }
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
emit_bios_set_video_mode_stub (struct byte_vec *v, const struct runtime_info *rt)
{
  emit8 (v, 0xa2);
  emit16 (v, rt->video_mode_off);  /* mov [video_mode], al */
  emit8 (v, 0xb4);
  emit8 (v, 0x00);                 /* mov ah, 00h */
  emit8 (v, 0xcd);
  emit8 (v, 0x10);                 /* let BIOS switch real hardware */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_bios_video_passthrough_stub (struct byte_vec *v, uint8_t fn)
{
  emit8 (v, 0xb4);
  emit8 (v, fn);
  emit8 (v, 0xcd);
  emit8 (v, 0x10);
  emit8 (v, 0xc3);
}

static void
emit_bios_passthrough_stub (struct byte_vec *v, uint8_t intr, uint8_t fn)
{
  emit8 (v, 0xb4);
  emit8 (v, fn);
  emit8 (v, 0xcd);
  emit8 (v, intr);
  emit8 (v, 0xc3);
}

static void
emit_bios_get_display_combination_stub (struct byte_vec *v)
{
  emit8 (v, 0x31);
  emit8 (v, 0xc0);          /* AL != 1Ah: no VGA/MCGA display-combo BIOS. */
  emit8 (v, 0x31);
  emit8 (v, 0xdb);
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_bios_set_palette_stub (struct byte_vec *v)
{
  emit8 (v, 0xb4);
  emit8 (v, 0x0b);                 /* mov ah, 0Bh */
  emit8 (v, 0xcd);
  emit8 (v, 0x10);                 /* let BIOS update CGA palette state */
  emit8 (v, 0xf8);
  emit8 (v, 0xc3);
}

static void
emit_bios_write_pixel_stub (struct byte_vec *v)
{
  emit_bios_video_passthrough_stub (v, 0x0c);
}

static void
emit_bios_read_pixel_stub (struct byte_vec *v)
{
  emit_bios_video_passthrough_stub (v, 0x0d);
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
      (void) rt;
      emit_bios_video_passthrough_stub (v, fn);
      return 1;
    case 0x0b:              /* set palette/background */
      emit_bios_set_palette_stub (v);
      return 1;
    case 0x03:              /* get cursor position and size */
      (void) rt;
      emit_bios_video_passthrough_stub (v, 0x03);
      return 1;
    case 0x08:              /* read character/attribute at cursor */
      (void) rt;
      emit_bios_video_passthrough_stub (v, 0x08);
      return 1;
    case 0x09:              /* write character/attribute at cursor */
    case 0x0a:              /* write character at cursor */
      (void) rt;
      emit_bios_video_passthrough_stub (v, fn);
      return 1;
    case 0x0c:              /* write graphics pixel */
      (void) rt;
      emit_bios_write_pixel_stub (v);
      return 1;
    case 0x0d:              /* read graphics pixel */
      (void) rt;
      emit_bios_read_pixel_stub (v);
      return 1;
    case 0x0e:              /* teletype output */
      (void) rt;
      emit_bios_video_passthrough_stub (v, 0x0e);
      return 1;
    case 0x0f:              /* get current video mode */
      (void) rt;
      emit_bios_video_passthrough_stub (v, 0x0f);
      return 1;
    case 0x12:              /* EGA alternate select/query */
      (void) rt;
      emit_bios_video_passthrough_stub (v, 0x12);
      return 1;
    case 0x1a:              /* get display combination code */
      emit_bios_get_display_combination_stub (v);
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
      emit_read_char_stub (v, 0, 1);
      return 1;
    case 0x01:              /* check key */
    case 0x11:              /* enhanced check key */
      emit8 (v, 0x31);
      emit8 (v, 0xc0);      /* ax = 0 */
      emit8 (v, 0x39);
      emit8 (v, 0xc0);      /* ZF set: no key pending */
      emit8 (v, 0xc3);
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
      emit_bios_passthrough_stub (v, 0x1a, 0x00);
      return 1;
    case 0x01:              /* set timer ticks */
      emit_bios_passthrough_stub (v, 0x1a, 0x01);
      return 1;
    case 0x02:              /* get RTC time, BCD */
      emit_bios_passthrough_stub (v, 0x1a, 0x02);
      return 1;
    case 0x04:              /* get RTC date, BCD */
      emit_bios_passthrough_stub (v, 0x1a, 0x04);
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
      emit_read_char_stub (v, 1, 0);
      return 1;
    case 0x02:
      emit_write_char_stub (v);
      return 1;
    case 0x06:
      emit_direct_console_stub (v);
      return 1;
    case 0x07:
    case 0x08:
      emit_read_char_stub (v, 0, 0);
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
      emit_get_allocation_info_stub (v, rt);
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
      emit_set_vector_stub (v);
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
      emit_read_stub (v, rt);
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
    case 0x4a:
      emit_success_stub (v);
      return 1;
    case 0x49:
      emit_free_stub (v, rt);
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
