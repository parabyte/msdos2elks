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

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

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
