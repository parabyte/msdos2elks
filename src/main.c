#include "internal.h"

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
  uint8_t *revealed;
  const uint8_t *convert_input;
  size_t input_len;
  size_t convert_len;
  int is_mz;

  parse_options (argc, argv, &opts);
  input = read_file (opts.input, &input_len);
  revealed = NULL;
  convert_input = input;
  convert_len = input_len;
  memset (&img, 0, sizeof (img));
  memset (&stats, 0, sizeof (stats));

  if (opts.format != FMT_COM && is_zip_archive (input, input_len))
    die ("input appears to be a ZIP/SFX archive; extract it before conversion");

  is_mz = input_len >= 2u
          && (get16 (input) == MZ_MAGIC || get16 (input) == ZM_MAGIC);
  if ((opts.format == FMT_EXE || (opts.format == FMT_AUTO && is_mz))
      && pklite_reveal_mz (input, input_len, &revealed, &convert_len,
                           opts.verbose))
    convert_input = revealed;

  if (opts.format == FMT_EXE || (opts.format == FMT_AUTO && is_mz))
    stats.dynamic_int21 = 1;

  if (opts.format == FMT_EXE || (opts.format == FMT_AUTO && is_mz))
    convert_mz (convert_input, convert_len, &opts, &img, &stats);
  else
    convert_com (convert_input, convert_len, &opts, &img, &stats);

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
                 " dynamic-int16=%u bios-keyboard=%u"
                 " direct-video=%u com-segment-fixes=%u stack-fixes=%u"
                 " os2-ne-segs=%u\n",
                 stats.patched, stats.unsupported,
                 stats.dynamic_int21 ? 1u : 0u,
                 stats.dynamic_int16 ? 1u : 0u,
                 stats.bios_keyboard_input ? 1u : 0u,
                 stats.direct_video_output ? 1u : 0u,
                 stats.com_segfix, stats.stackfix, img.ne_nsegs);
      else
        fprintf (stderr,
                 "msdos2elks: patched=%u unsupported=%u dynamic-int21=%u"
                 " dynamic-int16=%u bios-keyboard=%u"
                 " direct-video=%u com-segment-fixes=%u stack-fixes=%u"
                 " text=%u data=%u trel=%u drel=%u\n",
                 stats.patched, stats.unsupported,
                 stats.dynamic_int21 ? 1u : 0u,
                 stats.dynamic_int16 ? 1u : 0u,
                 stats.bios_keyboard_input ? 1u : 0u,
                 stats.direct_video_output ? 1u : 0u,
                 stats.com_segfix, stats.stackfix,
                 (unsigned) img.text.len, (unsigned) img.data.len,
                 (unsigned) img.trel.len, (unsigned) img.drel.len);
    }

  free_image (&img);
  free (revealed);
  free (input);
  return stats.unsupported ? 2 : 0;
}
