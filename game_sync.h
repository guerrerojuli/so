#ifndef GAME_SYNC_H
#define GAME_SYNC_H

#include <semaphore.h>
#include "game_state.h"

#define GAME_SYNC_SHM_NAME "/game_sync"

typedef struct
{
    sem_t view_update_ready;            /* master -> view: state changed */
    sem_t view_print_done;              /* view -> master: printing completed */
    sem_t master_starvation_guard;      /* mutex: protect master access to state */
    sem_t state_mutex;                  /* mutex for game state */
    sem_t readers_count_mutex;          /* mutex for readers_count */
    unsigned int readers_count;         /* number of views/players reading state */
    sem_t player_can_move[MAX_PLAYERS]; /* per-player movement slot */
} GameSync;

/* Reader-side of fair RW-lock used by view/player (writers handled by master) */
void game_sync_reader_enter(GameSync *sync);
void game_sync_reader_exit(GameSync *sync);

/* Writer-side of fair RW-lock used by master */
void game_sync_writer_enter(GameSync *sync);
void game_sync_writer_exit(GameSync *sync);

/* Securely executes a callback with read-only access to the game state */
void game_sync_run_as_reader(GameSync *sync, GameState *state, void (*callback)(GameState *state, void *context), void *context);

/* Securely executes a callback with write access to the game state */
void game_sync_run_as_writer(GameSync *sync, GameState *state, void (*callback)(GameState *state, void *context), void *context);


#endif /* GAME_SYNC_H */
