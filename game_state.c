#include "game_state.h"
#include "game_sync.h"
#include <string.h>

/*
 * Internal callback function to initialize the board.
 * This function is executed under a writer lock, ensuring safe access.
 * It's not meant to be called directly from outside this file.
 */
static void _internal_initialize_board(GameState *state, void *context) {
    
    for (unsigned int i = 0; i < state->width * state->height; ++i) {
        // aca hay que poner la incializacion de los valores del tablero
    }
    state->finished = false;
}

/*
 * Public function to initialize the game board.
 * It uses the secure writer function from game_sync to execute the initialization.
 */
void game_state_initialize_board(GameSync *sync, GameState *state) {
    // Call the writer wrapper, passing our internal function as a callback.
    // No context is needed for this operation, so we pass NULL.
    game_sync_run_as_writer(sync, state, _internal_initialize_board, NULL);
}