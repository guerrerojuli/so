UNAME_S := $(shell uname -s)

CC ?= cc
CSTD ?= -std=c11
WARN ?= -Wall -Wextra -Wpedantic
OPT  ?= -O2
CFLAGS ?= $(CSTD) $(WARN) $(OPT) -Iinclude -D_DEFAULT_SOURCE

LIBS_COMMON := -lm
LIBS_VIEW   := -lncurses
LIBS_PLAYER :=

ifeq ($(UNAME_S),Linux)
  LIBS_COMMON += -pthread -lrt
else ifeq ($(UNAME_S),Darwin)
  LIBS_COMMON += -pthread
endif

BINS := master view player
OBJS_COMMON := src/utils/game_sync.o src/utils/shmADT.o src/utils/game_logic.o

.PHONY: all clean format

all: $(BINS)

master: src/master.o $(OBJS_COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS_COMMON)

view: src/view.o $(OBJS_COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS_COMMON) $(LIBS_VIEW)

player: src/player.o $(OBJS_COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS_COMMON) $(LIBS_PLAYER)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

src/utils/%.o: src/utils/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(BINS) src/*.o src/utils/*.o

format:
	@command -v clang-format >/dev/null 2>&1 && clang-format -i src/*.c src/headers/*.h || echo "clang-format no encontrado; omitiendo formato"
