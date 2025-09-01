#include "game_logic.h"

// Static functions and constants moved from player.c
static inline int board_index(const GameState *state, int x, int y)
{
  return y * state->width + x;
}

bool in_bounds(const GameState *state, int x, int y)
{
  return x >= 0 && y >= 0 && x < (int)state->width && y < (int)state->height;
}

static inline bool is_free_cell(int v) { return v >= 1 && v <= 9; }

static const int DX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
static const int DY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

int choose_direction(const GameState *state, unsigned me)
{
  int bestd = -1, bestv = -1;
  int x = state->players[me].x, y = state->players[me].y;
  for (int d = 0; d < 8; d++)
  {
    int nx = x + DX[d], ny = y + DY[d];
    if (!in_bounds(state, nx, ny))
      continue;
    int v = state->board[board_index(state, nx, ny)];
    if (is_free_cell(v) && v > bestv)
    {
      bestv = v;
      bestd = d;
    }
  }
  return bestd;
}
