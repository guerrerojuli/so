// player.c (versión mínima, sin modularizar)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <semaphore.h>
#include "game_state.h"
#include "game_sync.h"

// -------------------- Tipos compartidos: usar headers --------------------

// -------------------- util mínima --------------------

static inline void die(const char *msg)
{
  if (errno)
    fprintf(stderr, "%s (errno=%d: %s)\n", msg, errno, strerror(errno));
  else
    fprintf(stderr, "%s\n", msg);
  exit(EXIT_FAILURE);
}

static inline int board_index(const GameState *state, int x, int y)
{
  return y * state->width + x;
}
static inline bool in_bounds(const GameState *state, int x, int y)
{
  return x >= 0 && y >= 0 && x < state->width && y < state->height;
}
static inline bool is_free_cell(int v) { return v >= 1 && v <= 9; }

// Direcciones 0..7 (arriba y horario)
static const int DX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
static const int DY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

// -------------------- RW-lock (solo lectura desde el player) --------------------
// Lectores-escritor sin inanición del escritor.
// Como player sólo lee, usamos enter/exit de lector.

/* RW-lock lector: usar implementación de game_sync.c */

// -------------------- IA mínima (greedy local) --------------------

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
  return bestd; // -1 si no hay movimientos válidos
}

// -------------------- main --------------------

int main(int argc, char **argv)
{
  // Opcional: el enunciado dice que recibe width/height; no los usamos en la lógica.
  (void)argv;
  if (argc < 3)
    die("player: uso: player <width> <height>\n");

  // Si el master muere y cierra el pipe, evitamos morir por SIGPIPE:
  signal(SIGPIPE, SIG_IGN);

  // Adjuntar a /game_state (sólo lectura; reintentos breves por si el master aún no creó el SHM)
  int fd_state = -1;
  for (int i = 0; i < 2000; i++) // hasta ~2s
  {
    fd_state = shm_open(GAME_STATE_SHM_NAME, O_RDONLY, 0600);
    if (fd_state >= 0)
      break;
    if (errno != ENOENT)
      break;
    usleep(1000);
  }
  if (fd_state < 0)
    die("player shm_open state");
  struct stat state_shm_stat;
  if (fstat(fd_state, &state_shm_stat) < 0)
    die("player fstat state");
  GameState *state = mmap(NULL, state_shm_stat.st_size, PROT_READ, MAP_SHARED, fd_state, 0);
  if (state == MAP_FAILED)
    die("player mmap state");
  close(fd_state);

  // Adjuntar a /game_sync (mismo esquema de reintentos)
  int fd_sync = -1;
  for (int i = 0; i < 2000; i++)
  {
    fd_sync = shm_open(GAME_SYNC_SHM_NAME, O_RDWR, 0600);
    if (fd_sync >= 0)
      break;
    if (errno != ENOENT)
      break;
    usleep(1000);
  }
  if (fd_sync < 0)
    die("player shm_open sync");
  GameSync *sync_area = mmap(NULL, sizeof(GameSync), PROT_READ | PROT_WRITE, MAP_SHARED, fd_sync, 0);
  if (sync_area == MAP_FAILED)
    die("player mmap sync");
  close(fd_sync);

  // Encontrar mi índice por PID (espera breve hasta que el master lo setee)
  pid_t mypid = getpid();
  unsigned me = 0;
  for (;;)
  {
    bool found = false;
    game_sync_reader_enter(sync_area);
    unsigned player_count_snapshot = state->player_count;
    for (me = 0; me < player_count_snapshot; me++)
    {
      if (state->players[me].pid == mypid)
      {
        found = true;
        break;
      }
    }
    bool finished_snapshot = state->finished;
    game_sync_reader_exit(sync_area);
    if (finished_snapshot)
      goto done; // si ya terminó, salir
    if (found)
      break;
    usleep(1000); // 1ms para no hacer busy-wait
  }

  // Bucle principal: un movimiento por permiso del master (G[me])
  while (1)
  {
    // Esperar permiso para ENVIAR UN movimiento
    sem_wait(&sync_area->player_can_move[me]);

    // ¿Ya terminó el juego?
    game_sync_reader_enter(sync_area);
    bool finished_snapshot = state->finished;
    game_sync_reader_exit(sync_area);
    if (finished_snapshot)
      break;

    // Elegir dirección (snapshot de lectura mínima)
    game_sync_reader_enter(sync_area);
    int chosen_dir = choose_direction(state, me);
    game_sync_reader_exit(sync_area);

    // Sin movimientos válidos -> cerrar stdout (EOF) y terminar
    if (chosen_dir < 0)
    {
      close(1);
      break;
    }

    // Enviar 1 byte con la dirección (0..7) por stdout (fd=1)
    unsigned char dir = (unsigned char)chosen_dir;
    ssize_t w = write(1, &dir, 1);
    if (w != 1)
    {
      // Si el master cerró el pipe, salimos
      break;
    }
    // Importante: NO hacer sem_post(G[me]); eso lo hace el master cuando procesa
  }

done:
  munmap(sync_area, sizeof *sync_area);
  munmap(state, state_shm_stat.st_size);
  return 0;
}
