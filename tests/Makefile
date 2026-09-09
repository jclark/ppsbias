CC = cc
CPPFLAGS =
CFLAGS = -O2 -g -std=gnu11 -Wall -Wextra -Wpedantic
LDFLAGS =
LDLIBS =
PYTHON = python3

PROGRAMS = unit fake

.PHONY: all check clean
all: $(PROGRAMS)

$(PROGRAMS): %: %.c ../ppsbias.c
	$(CC) $(CPPFLAGS) -I.. $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS) -lm

check: all
	./unit
	$(PYTHON) checkcli.py
	$(PYTHON) fake.py

clean:
	rm -f $(PROGRAMS)
