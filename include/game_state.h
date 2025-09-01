#ifndef GAME_STATE_H
#define GAME_STATE_H

#include <stdbool.h>
#include <sys/types.h>
#include "constants.h"

typedef struct
{
    char name[16];
    unsigned int score;
    unsigned int invalid_move_requests;
    unsigned int valid_move_requests;
    unsigned short x, y;
    pid_t pid; /* not to be printed by view */
    bool blocked;
} Player;

typedef struct
{
    unsigned short width;
    unsigned short height;
    unsigned int player_count;
    Player players[MAX_PLAYERS];
    bool finished;
    int board[]; /* row-major: row-0, row-1, ..., row-(height-1) */
} GameState;

#define GAME_STATE_MAP_SIZE(w, h) (sizeof(GameState) + (size_t)(w) * (size_t)(h) * sizeof(int))

#endif /* GAME_STATE_H */
