#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

#include "game_state.h"
#include <stdbool.h>

// Function prototypes to be moved from player.c
int choose_direction(const GameState *state, unsigned me);
bool in_bounds(const GameState *state, int x, int y);

#endif // GAME_LOGIC_H
