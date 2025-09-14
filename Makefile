UNAME_S := $(shell uname -s)

CC ?= cc
CSTD ?= -std=c11
WARN ?= -Wall -Wextra -Wpedantic
OPT  ?= -O2
DBG  ?= -g
CFLAGS ?= $(CSTD) $(WARN) $(OPT) $(DBG) -Iinclude -D_DEFAULT_SOURCE

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

.PHONY: all clean format vg-master vg-view vg-player vg-game

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

# =========================
# Valgrind configuration
# =========================

# Allow overriding from the CLI: make vg-master VG_FLAGS="..."
VG ?= valgrind
VG_FLAGS ?= --leak-check=full --show-leak-kinds=all --track-origins=yes \
            --trace-children=yes --track-fds=yes --num-callers=25 \
            --errors-for-leak-kinds=definite,possible --error-exitcode=123 \
            --log-file=valgrind-%p.log

# Default scenario parameters (override with W=, H=, N=, DELAY=, TIMEOUT=, SEED=)
W ?= 10
H ?= 8
N ?= 2
DELAY ?= 0
TIMEOUT ?= 2
SEED ?= 1

# Expand to N copies of ./player
PLAYERS := $(foreach i,$(shell seq 1 $(N)),./player)

# Run master under Valgrind with a small demo setup
vg-master: all
	$(VG) $(VG_FLAGS) ./master -w $(W) -h $(H) -d $(DELAY) -t $(TIMEOUT) -s $(SEED) -v ./view -p $(PLAYERS)

# Run view alone under Valgrind
vg-view: view
	$(VG) $(VG_FLAGS) ./view $(W) $(H)

# Run one player under Valgrind
vg-player: player
	$(VG) $(VG_FLAGS) ./player $(W) $(H)

# Alias: run the whole game via master
vg-game: vg-master
