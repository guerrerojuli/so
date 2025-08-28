#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <semaphore.h>
#include "game_state.h"
#include "game_sync.h"
#include "headers/shmADT.h"

static inline int board_index(const GameState *state, int x, int y)
{
  return y * state->width + x;
}
static inline bool in_bounds(const GameState *state, int x, int y)
{
  return x >= 0 && y >= 0 && x < (int)state->width && y < (int)state->height;
}
static inline bool is_free_cell(int v) { return v >= 1 && v <= 9; }

static const int DX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
static const int DY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

static bool find_player_index_by_pid(const GameState *state, GameSync *sync,
                                     pid_t pid, unsigned *out_index,
                                     bool *out_finished_now)
{
  game_sync_reader_enter(sync);
  unsigned player_count_snapshot = state->player_count;
  if (player_count_snapshot > MAX_PLAYERS)
    player_count_snapshot = MAX_PLAYERS;
  for (unsigned i = 0; i < player_count_snapshot; i++)
  {
    if (state->players[i].pid == pid)
    {
      bool finished_snapshot = state->finished;
      game_sync_reader_exit(sync);
      if (out_index)
        *out_index = i;
      if (out_finished_now)
        *out_finished_now = finished_snapshot;
      return true;
    }
  }
  bool finished_snapshot = state->finished;
  game_sync_reader_exit(sync);
  if (out_finished_now)
    *out_finished_now = finished_snapshot;
  return false;
}

static int choose_direction(const GameState *state, unsigned me)
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

int main(int argc, char **argv)
{
  if (argc != 3)
  {
    errno = EINVAL;
    perror("player: invalid usage");
    return 1;
  }

  unsigned long width = strtoul(argv[1], NULL, 10);
  unsigned long height = strtoul(argv[2], NULL, 10);
  if (width == 0 || height == 0)
  {
    errno = EINVAL;
    perror("player: invalid dimensions");
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);

  size_t map_size = GAME_STATE_MAP_SIZE(width, height);

  ShmADT state_shm = open_shm(GAME_STATE_SHM_NAME, map_size, O_RDONLY, 0600, PROT_READ);
  GameState *state = (GameState *)get_shm_pointer(state_shm);

  ShmADT sync_shm = open_shm(GAME_SYNC_SHM_NAME, sizeof(GameSync), O_RDWR, 0600, PROT_READ | PROT_WRITE);
  GameSync *sync_area = (GameSync *)get_shm_pointer(sync_shm);

  pid_t mypid = getpid();
  unsigned me = 0;
  bool finished_now = false;
  bool found = find_player_index_by_pid(state, sync_area, mypid, &me, &finished_now);
  if (!found)
  {
    errno = ENOENT;
    perror("player: pid not registered");
  }
  else if (!finished_now)
  {
    while (1)
    {
      if (sem_wait(&sync_area->player_can_move[me]) == -1)
      {
        if (errno == EINTR)
          continue;
        perror("player: sem_wait(player_can_move)");
        break;
      }

      int chosen_dir = -1;
      game_sync_reader_enter(sync_area);
      bool finished_now = state->finished;
      if (!finished_now)
        chosen_dir = choose_direction(state, me);
      game_sync_reader_exit(sync_area);
      if (finished_now)
        break;

      if (chosen_dir < 0)
      {
        close(1);
        break;
      }

      unsigned char dir = (unsigned char)chosen_dir;
      ssize_t w = write(1, &dir, 1);
      if (w != 1)
      {
        if (errno != 0)
          perror("player: write(stdout)");
        break;
      }
    }
  }
  close_shm(sync_shm);
  close_shm(state_shm);
  return 0;
}
