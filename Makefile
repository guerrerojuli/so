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
OBJS_COMMON := game_sync.o

.PHONY: all clean format

all: $(BINS)

view: view.o $(OBJS_COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS_COMMON) $(LIBS_VIEW)

player: player.o $(OBJS_COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS_COMMON) $(LIBS_PLAYER)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(BINS) *.o

format:
	@command -v clang-format >/dev/null 2>&1 && clang-format -i *.c *.h || echo "clang-format no encontrado; omitiendo formato"


