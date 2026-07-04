-include config.mk

CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
WARN_CFLAGS ?= -Wall -Wextra -std=c99
LDFLAGS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INSTALL ?= install

PROG = msdos2elks
OBJS = \
	src/common.o \
	src/cli.o \
	src/dos_bios_io.o \
	src/patch.o \
	src/startup.o \
	src/com.o \
	src/mz_os2.o \
	src/output.o \
	src/main.o

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(OBJS): src/internal.h

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) -c -o $@ $<

check: $(PROG)
	./tools/selftest.sh ./$(PROG)

smoke-xt: $(PROG)
	./tools/xt-smoke.sh ./$(PROG)

smoke-xt-graphics: $(PROG)
	./tools/xt-graphics-smoke.sh ./$(PROG)

smoke-elks-runtime:
	./tools/elks-runtime-smoke.sh "$(ELKS_BOOT_IMAGE)" "$(ELKS_APP_IMAGE)" "$(SCREENSHOTS)"

smoke-elks-runtime-batch:
	./tools/elks-runtime-batch.sh "$(ELKS_BOOT_IMAGE)" "$(ELKS_APP_IMAGE)" "$(SCREENSHOTS)"

install: $(PROG)
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 755 $(PROG) "$(DESTDIR)$(BINDIR)/$(PROG)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(PROG)"

clean:
	rm -f $(PROG) $(OBJS) msdos2elks.o

distclean: clean
	rm -f config.mk config.log config.status

.PHONY: all check smoke-xt smoke-xt-graphics smoke-elks-runtime smoke-elks-runtime-batch install uninstall clean distclean
