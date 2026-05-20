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
OBJS = msdos2elks.o
MODULES = \
	src/internal.h \
	src/common.c \
	src/cli.c \
	src/dos_bios_io.c \
	src/patch.c \
	src/startup.c \
	src/com.c \
	src/pklite.c \
	src/mz_os2.c \
	src/output.c \
	src/main.c

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

msdos2elks.o: $(MODULES)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) -c -o $@ $<

check: $(PROG)
	./tools/selftest.sh ./$(PROG)

install: $(PROG)
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 755 $(PROG) "$(DESTDIR)$(BINDIR)/$(PROG)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(PROG)"

clean:
	rm -f $(PROG) $(OBJS)

distclean: clean
	rm -f config.mk config.log config.status

.PHONY: all check install uninstall clean distclean
