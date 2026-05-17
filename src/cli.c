#include "internal.h"

static uint16_t
parse_u16 (const char *s, const char *what)
{
  char *end;
  unsigned long v;

  errno = 0;
  v = strtoul (s, &end, 0);
  if (errno || *end || v > ELKS_MAX16)
    {
      fprintf (stderr, "msdos2elks: bad %s '%s'\n", what, s);
      exit (1);
    }
  return (uint16_t) v;
}

static int
parse_prefixed_arg (const char *arg, const char *name, const char **value)
{
  size_t n = strlen (name);

  if (strncmp (arg, name, n) == 0 && arg[n] == '=')
    {
      *value = arg + n + 1u;
      return 1;
    }
  return 0;
}

static void
usage (FILE *out)
{
  fprintf (out,
           "usage: msdos2elks [OPTIONS] INPUT OUTPUT\n"
           "\n"
           "Options:\n"
           "  --format=auto|com|exe\n"
           "  --stack=BYTES\n"
           "  --heap=BYTES\n"
           "  --bss=BYTES\n"
           "  --mz-output=os2|aout|auto\n"
           "  --mz-code-seg=PARA\n"
           "  --mz-data-seg=PARA\n"
           "  --partial\n"
           "  --verbose\n");
}

static void
parse_options (int argc, char **argv, struct options *opts)
{
  int i;
  const char *value;

  memset (opts, 0, sizeof (*opts));
  opts->format = FMT_AUTO;
  opts->mz_output = MZ_OUT_OS2;
  opts->stack = ELKS_DEFAULT_STACK;
  opts->heap = 0;

  for (i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--help") == 0 || strcmp (argv[i], "-h") == 0)
        {
          usage (stdout);
          exit (0);
        }
      else if (strcmp (argv[i], "--partial") == 0)
        opts->partial = 1;
      else if (strcmp (argv[i], "--verbose") == 0)
        opts->verbose = 1;
      else if (parse_prefixed_arg (argv[i], "--format", &value))
        {
          if (strcmp (value, "auto") == 0)
            opts->format = FMT_AUTO;
          else if (strcmp (value, "com") == 0)
            opts->format = FMT_COM;
          else if (strcmp (value, "exe") == 0 || strcmp (value, "mz") == 0)
            opts->format = FMT_EXE;
          else
            die ("--format must be auto, com, or exe");
        }
      else if (parse_prefixed_arg (argv[i], "--stack", &value))
        {
          opts->stack = parse_u16 (value, "stack size");
          opts->stack_set = 1;
        }
      else if (parse_prefixed_arg (argv[i], "--heap", &value))
        {
          opts->heap = parse_u16 (value, "heap size");
          opts->heap_set = 1;
        }
      else if (parse_prefixed_arg (argv[i], "--bss", &value))
        {
          opts->bss = parse_u16 (value, "bss size");
          opts->bss_set = 1;
        }
      else if (parse_prefixed_arg (argv[i], "--mz-output", &value))
        {
          if (strcmp (value, "os2") == 0 || strcmp (value, "ne") == 0)
            opts->mz_output = MZ_OUT_OS2;
          else if (strcmp (value, "aout") == 0
                   || strcmp (value, "minix") == 0)
            opts->mz_output = MZ_OUT_AOUT;
          else if (strcmp (value, "auto") == 0)
            opts->mz_output = MZ_OUT_AUTO;
          else
            die ("--mz-output must be os2, aout, or auto");
        }
      else if (parse_prefixed_arg (argv[i], "--mz-code-seg", &value))
        {
          opts->mz_code_seg = parse_u16 (value, "MZ code segment");
          opts->mz_code_set = 1;
        }
      else if (parse_prefixed_arg (argv[i], "--mz-data-seg", &value))
        {
          opts->mz_data_seg = parse_u16 (value, "MZ data segment");
          opts->mz_data_set = 1;
        }
      else if (argv[i][0] == '-')
        {
          fprintf (stderr, "msdos2elks: unknown option '%s'\n", argv[i]);
          usage (stderr);
          exit (1);
        }
      else if (!opts->input)
        opts->input = argv[i];
      else if (!opts->output)
        opts->output = argv[i];
      else
        die ("too many file names");
    }

  if (!opts->input || !opts->output)
    {
      usage (stderr);
      exit (1);
    }
}

static uint8_t *
read_file (const char *path, size_t *len_out)
{
  FILE *fp;
  long pos;
  size_t got;
  uint8_t *buf;

  fp = fopen (path, "rb");
  if (!fp)
    die_errno (path);
  if (fseek (fp, 0, SEEK_END) != 0)
    die_errno (path);
  pos = ftell (fp);
  if (pos < 0)
    die_errno (path);
  if (fseek (fp, 0, SEEK_SET) != 0)
    die_errno (path);

  buf = (uint8_t *) malloc ((size_t) pos ? (size_t) pos : 1u);
  if (!buf)
    die ("out of memory");
  got = fread (buf, 1, (size_t) pos, fp);
  if (got != (size_t) pos)
    die_errno (path);
  if (fclose (fp) != 0)
    die_errno (path);

  *len_out = (size_t) pos;
  return buf;
}

