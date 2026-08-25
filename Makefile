STD     := gnu11

CFLAGS  ?= -O2
override CFLAGS += -std=$(STD) -Wall -Wextra
LDFLAGS ?=
LDLIBS  :=

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin
DESTDIR ?=

all: icd

icd: icd.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ icd.c $(LDLIBS)

install: icd
	install -Dm755 icd $(DESTDIR)$(BINDIR)/icd

clean:
	rm -f icd

.PHONY: all install clean
