#ifndef GAME_SYNC_H
#define GAME_SYNC_H

#include <semaphore.h>

#define MAX_PLAYERS 9
#define GAME_SYNC_SHM_NAME "/game_sync_shm"

typedef struct {
    sem_t view_update_ready;     /* master -> view: state changed */
    sem_t view_print_done;       /* view -> master: printing completed */
    sem_t master_starvation_guard; /* mutex: protect master access to state */
    sem_t state_mutex;           /* mutex for game state */
    sem_t readers_count_mutex;   /* mutex for readers_count */
    unsigned int readers_count;  /* number of views/players reading state */
    sem_t player_can_move[MAX_PLAYERS]; /* per-player movement slot */
} GameSync;

#endif /* GAME_SYNC_H */

