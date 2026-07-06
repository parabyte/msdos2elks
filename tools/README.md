# Tools

Optional helpers for local testing and batch conversion.

```sh
make check
make smoke-xt
make smoke-xt-graphics
```

```sh
tools/convert-directory.sh INPUT_DIR OUTPUT_DIR
tools/explain-conversion.sh PROGRAM.EXE
```

Runtime ELKS/QEMU helpers are available for manual validation:

```sh
tools/elks-runtime-smoke.sh ELKS_BOOT_FLOPPY APP_FLOPPY OUTPUT_DIR
tools/elks-runtime-batch.sh ELKS_BOOT_FLOPPY APP_FLOPPY OUTPUT_DIR
```
