/*
 * msdos2elks - host-side MS-DOS COM/MZ to ELKS converter.
 *
 * The implementation is split under src/ by responsibility.  The module
 * files are included here as one translation unit so small binary patching
 * helpers can stay file-local while the source remains readable.
 */

#include "src/internal.h"

#include "src/common.c"
#include "src/cli.c"
#include "src/dos_bios_io.c"
#include "src/patch.c"
#include "src/startup.c"
#include "src/com.c"
#include "src/mz_os2.c"
#include "src/output.c"
#include "src/main.c"
