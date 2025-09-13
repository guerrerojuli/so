#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <semaphore.h>
#include "game_state.h"
#include "game_sync.h"
#include "shmADT.h"
#include "player.h"
#include "game_logic.h"

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

typedef struct
{
  unsigned long width;
  unsigned long height;
} PlayerArgs;

typedef struct
{
  ShmADT state_shm;
  GameState *state;
  ShmADT sync_shm;
  GameSync *sync;
} PlayerResources;

static bool parse_args(int argc, char **argv, PlayerArgs *out_args)
{
  if (argc != 3)
  {
    errno = EINVAL;
    fprintf(stderr, "player: invalid usage. Usage: %s <width> <height>\n", argv[0]);
    return false;
  }

  out_args->width = strtoul(argv[1], NULL, 10);
  out_args->height = strtoul(argv[2], NULL, 10);
  if (out_args->width == 0 || out_args->height == 0)
  {
    errno = EINVAL;
    fprintf(stderr,
            "player: invalid dimensions: width=%lu height=%lu (must be > 0)\n",
            out_args->width, out_args->height);
    return false;
  }
  return true;
}

static bool init_resources(const PlayerArgs *args, PlayerResources *out_res)
{
  size_t map_size = GAME_STATE_MAP_SIZE(args->width, args->height);

  out_res->state_shm = open_shm(GAME_STATE_SHM_NAME, map_size, O_RDONLY, 0600, PROT_READ);
  if (out_res->state_shm == NULL)
  {
    fprintf(stderr,
            "player: failed to open shm '%s' (read-only, size=%zu): %s\n",
            GAME_STATE_SHM_NAME, map_size, strerror(errno));
    return false;
  }
  out_res->state = (GameState *)get_shm_pointer(out_res->state_shm);

  out_res->sync_shm = open_shm(GAME_SYNC_SHM_NAME, sizeof(GameSync), O_RDWR, 0600, PROT_READ | PROT_WRITE);
  if (out_res->sync_shm == NULL)
  {
    fprintf(stderr,
            "player: failed to open shm '%s' (read/write, size=%zu): %s\n",
            GAME_SYNC_SHM_NAME, sizeof(GameSync), strerror(errno));
    close_shm(out_res->state_shm);
    return false;
  }
  out_res->sync = (GameSync *)get_shm_pointer(out_res->sync_shm);
  return true;
}

static void cleanup_resources(PlayerResources *res)
{
  // El master es responsable de desvincular la memoria compartida (shm_unlink).
  // Aquí solo cerramos nuestra vista local (munmap/close y liberar wrapper),
  // lo cual es seguro en presencia de shm_unlink del master.
  if (res && res->sync_shm)
    close_shm(res->sync_shm);
  if (res && res->state_shm)
    close_shm(res->state_shm);
}

static void run_player_loop(GameState *state, GameSync *sync)
{
  pid_t mypid = getpid();
  unsigned me = 0;
  bool finished_now = false;
  bool found = find_player_index_by_pid(state, sync, mypid, &me, &finished_now);

  if (!found)
  {
    errno = ENOENT;
    fprintf(stderr, "player: PID %d not registered in GameState: %s\n", (int)mypid, strerror(errno));
    return;
  }

  if (finished_now)
  {
    return;
  }

  while (true)
  {
    if (sem_wait(&sync->player_can_move[me]) == -1)
    {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "player: error in sem_wait(player_can_move[%u]): %s\n", me, strerror(errno));
      break;
    }

    int chosen_dir = -1;
    game_sync_reader_enter(sync);
    finished_now = state->finished;
    if (!finished_now)
      chosen_dir = choose_direction(state, me);
    game_sync_reader_exit(sync);

    if (finished_now)
      break;

    if (chosen_dir < 0)
    {
      close(STDOUT_FILENO);
      break;
    }

    unsigned char dir = (unsigned char)chosen_dir;
    ssize_t w = write(STDOUT_FILENO, &dir, 1);
    if (w != 1)
    {
      if (errno != 0)
        fprintf(stderr, "player: failed to write direction to stdout (pid=%d): %s\n", (int)mypid, strerror(errno));
      break;
    }
  }
}

int main(int argc, char **argv)
{
  PlayerArgs args;
  if (!parse_args(argc, argv, &args))
  {
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);

  PlayerResources res;
  if (!init_resources(&args, &res))
  {
    return 1;
  }

  run_player_loop(res.state, res.sync);

  cleanup_resources(&res);

  return 0;
}
