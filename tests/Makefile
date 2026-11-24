CC = cc
CPPFLAGS =
CFLAGS = -O2 -g -std=gnu11 -Wall -Wextra -Wpedantic
LDFLAGS =
LDLIBS =
PYTHON = python3

PROGRAMS = unit fake clocks
BACKENDS = ../bcm2835.c ../rp1.c ../rockchip.c ../k3.c ../mxc.c

.PHONY: all check clean
all: $(PROGRAMS)

$(PROGRAMS): %: %.c ../ppsbias.c $(BACKENDS) ../backend.h
	$(CC) $(CPPFLAGS) -I.. $(CFLAGS) $(LDFLAGS) -o $@ $< $(BACKENDS) $(LDLIBS) -lm

check: all
	./unit
	./clocks
	$(PYTHON) checkcli.py
	$(PYTHON) fake.py

clean:
	rm -f $(PROGRAMS)
