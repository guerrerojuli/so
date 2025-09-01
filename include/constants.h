#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <ncurses.h>

// From game_state.h
#define MAX_PLAYERS 9
#define GAME_STATE_SHM_NAME "/game_state"

// From game_sync.h
#define GAME_SYNC_SHM_NAME "/game_sync"

// From view.c
static const short BASE_COLORS[] = {COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE};
static const int NUM_BASE_COLORS = (int)(sizeof(BASE_COLORS) / sizeof(BASE_COLORS[0]));

#endif // CONSTANTS_H
