CC      := gcc
CFLAGS  := -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -g -pthread -Isrc
LDFLAGS := -pthread

SRC := src/server.c src/http.c src/auth.c src/store.c src/state.c src/json.c
OBJ := $(SRC:src/%.c=build/%.o)
BIN := bin/restapi

.PHONY: all clean run check slides

all: $(BIN)

$(BIN): $(OBJ)
	mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.c src/http.h src/auth.h src/store.h src/state.h src/json.h
	mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build bin

run: $(BIN)
	./$(BIN)

check: $(BIN)
	bash scripts/check.sh

slides:
	cd presentation && xelatex -interaction=nonstopmode slides.tex && xelatex -interaction=nonstopmode slides.tex && rm -f slides.aux slides.log slides.nav slides.out slides.snm slides.toc slides.vrb
