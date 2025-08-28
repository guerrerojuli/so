UNAME_S := $(shell uname -s)

CC ?= cc
CSTD ?= -std=c11
WARN ?= -Wall -Wextra -Wpedantic
OPT  ?= -O2
DBG  ?= -g
CFLAGS ?= $(CSTD) $(WARN) $(OPT) $(DBG)

LIBS_COMMON :=
LIBS_VIEW   := -lncurses
LIBS_PLAYER :=

ifeq ($(UNAME_S),Linux)
  LIBS_COMMON += -pthread -lrt
else ifeq ($(UNAME_S),Darwin)
  LIBS_COMMON += -pthread
endif

BINS := view player
OBJS_COMMON := game_sync.o utils/shmADT.o

.PHONY: all clean format

all: $(BINS)

view: view.o $(OBJS_COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS_COMMON) $(LIBS_VIEW)

player: player.o $(OBJS_COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS_COMMON) $(LIBS_PLAYER)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

utils/shmADT.o: utils/shmADT.c headers/shmADT.h
	$(CC) $(CFLAGS) -c utils/shmADT.c -o utils/shmADT.o

clean:
	rm -f $(BINS) *.o utils/*.o

format:
	@command -v clang-format >/dev/null 2>&1 && clang-format -i *.c *.h || echo "clang-format no encontrado; omitiendo formato"


