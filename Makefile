CC = cc
CPPFLAGS =
CFLAGS = -O2 -g -std=gnu11 -Wall -Wextra -Wpedantic
LDFLAGS =
LDLIBS =
export CC CPPFLAGS CFLAGS LDFLAGS LDLIBS
PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
INSTALL = install

PROGRAMS = ppsbias ppsecho

.PHONY: all clean install uninstall check
all: $(PROGRAMS)

ppsbias: ppsbias.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS) -lm

ppsecho: ppsecho.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

check: all
	$(MAKE) -C tests check

install: all
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 755 $(PROGRAMS) "$(DESTDIR)$(BINDIR)/"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/ppsbias" "$(DESTDIR)$(BINDIR)/ppsecho"

clean:
	rm -f $(PROGRAMS)
	$(MAKE) -C tests clean
