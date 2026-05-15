PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
CC     ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
EXE := lsldb

ifeq ($(OS),Windows_NT)
  EXE := lsldb.exe
endif

.PHONY: all clean install test

all: $(EXE)

$(EXE): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c src/lsldb.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(EXE)

install: $(EXE)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(EXE) $(DESTDIR)$(BINDIR)/$(EXE)
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 0644 man/$(EXE).1 $(DESTDIR)$(PREFIX)/share/man/man1/$(EXE).1

test: $(EXE)
	@sh tests/run_tests.sh ./$(EXE)
