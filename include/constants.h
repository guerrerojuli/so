#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <ncurses.h>

// Constantes del juego
#define MAX_PLAYERS 9
#define GAME_STATE_SHM_NAME "/game_state"
#define GAME_SYNC_SHM_NAME "/game_sync"

#define DEFAULT_WIDTH 10
#define DEFAULT_HEIGHT 10
#define DEFAULT_DELAY 200 //ms
#define DEFAULT_TIMEOUT 10 //s
#define DEFAULT_VIEW_PATH NULL
#define MIN_PLAYERS 1
#define MIN_WIDTH 10
#define MIN_HEIGHT 10

#define R_END 0
#define W_END 1

// Colores para la vista
static const short BASE_COLORS[] = {COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE};
static const int NUM_BASE_COLORS = (int)(sizeof(BASE_COLORS) / sizeof(BASE_COLORS[0]));


#endif // CONSTANTS_H
