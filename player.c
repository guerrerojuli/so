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

#define MAX_PLAYERS 9
#define SHM_STATE "/game_state"
#define SHM_SYNC "/game_sync"

// -------------------- Tipos compartidos (deben matchear al master) --------------------

typedef struct
{
  char name[16]; // visible (no usamos acá)
  unsigned int score;
  unsigned int invalids;
  unsigned int valids;
  unsigned short x, y; // posición actual
  pid_t pid;           // usado para identificar "me"
  bool blocked;        // lo setea el master al ver EOF
} player_state_t;

typedef struct
{
  unsigned short width;
  unsigned short height;
  unsigned int player_count;
  player_state_t players[MAX_PLAYERS];
  bool finished;
  int board[]; // flexible array: tamaño = width*height
} game_state_t;

typedef struct
{
  sem_t A;              // master -> view (no lo usamos aquí)
  sem_t B;              // view -> master (no lo usamos aquí)
  sem_t C;              // turnstile (RW-lock)
  sem_t D;              // resource (RW-lock)
  sem_t E;              // mutex readers count (RW-lock)
  unsigned int F;       // readers count (RW-lock)
  sem_t G[MAX_PLAYERS]; // permiso por jugador: 1 movimiento a la vez
} game_sync_t;

// -------------------- util mínima --------------------

static inline void die(const char *msg)
{
  if (errno)
    fprintf(stderr, "%s (errno=%d: %s)\n", msg, errno, strerror(errno));
  else
    fprintf(stderr, "%s\n", msg);
  exit(EXIT_FAILURE);
}

static inline int idx(const game_state_t *st, int x, int y)
{
  return y * st->width + x;
}
static inline bool in_bounds(const game_state_t *st, int x, int y)
{
  return x >= 0 && y >= 0 && x < st->width && y < st->height;
}
static inline bool is_free_cell(int v) { return v >= 1 && v <= 9; }

// Direcciones 0..7 (arriba y horario)
static const int DX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
static const int DY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

// -------------------- RW-lock (solo lectura desde el player) --------------------
// Lectores-escritor sin inanición del escritor.
// Como player sólo lee, usamos enter/exit de lector.

static inline void rw_reader_enter(game_sync_t *s)
{
  sem_wait(&s->C); // pasar por el torniquete
  sem_post(&s->C);
  sem_wait(&s->E); // proteger F
  s->F++;
  if (s->F == 1)
    sem_wait(&s->D); // primer lector bloquea a escritores
  sem_post(&s->E);
}
static inline void rw_reader_exit(game_sync_t *s)
{
  sem_wait(&s->E);
  s->F--;
  if (s->F == 0)
    sem_post(&s->D); // último lector libera recurso
  sem_post(&s->E);
}

// -------------------- IA mínima (greedy local) --------------------

static int choose_dir(const game_state_t *ST, unsigned me)
{
  int bestd = -1, bestv = -1;
  int x = ST->players[me].x, y = ST->players[me].y;
  for (int d = 0; d < 8; d++)
  {
    int nx = x + DX[d], ny = y + DY[d];
    if (!in_bounds(ST, nx, ny))
      continue;
    int v = ST->board[idx(ST, nx, ny)];
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
  if (argc < 3)
    die("player: uso: player <width> <height>\n");

  // Si el master muere y cierra el pipe, evitamos morir por SIGPIPE:
  signal(SIGPIPE, SIG_IGN);

  // Adjuntar a /game_state (sólo lectura; reintentos breves por si el master aún no creó el SHM)
  int fd_state = -1;
  for (int i = 0; i < 2000; i++) // hasta ~2s
  {
    fd_state = shm_open(SHM_STATE, O_RDONLY, 0600);
    if (fd_state >= 0)
      break;
    if (errno != ENOENT)
      break;
    usleep(1000);
  }
  if (fd_state < 0)
    die("player shm_open state");
  struct stat st;
  if (fstat(fd_state, &st) < 0)
    die("player fstat state");
  game_state_t *ST = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd_state, 0);
  if (ST == MAP_FAILED)
    die("player mmap state");
  close(fd_state);

  // Adjuntar a /game_sync (mismo esquema de reintentos)
  int fd_sync = -1;
  for (int i = 0; i < 2000; i++)
  {
    fd_sync = shm_open(SHM_SYNC, O_RDWR, 0600);
    if (fd_sync >= 0)
      break;
    if (errno != ENOENT)
      break;
    usleep(1000);
  }
  if (fd_sync < 0)
    die("player shm_open sync");
  game_sync_t *SY = mmap(NULL, sizeof(game_sync_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd_sync, 0);
  if (SY == MAP_FAILED)
    die("player mmap sync");
  close(fd_sync);

  // Encontrar mi índice por PID (espera breve hasta que el master lo setee)
  pid_t mypid = getpid();
  unsigned me = 0;
  for (;;)
  {
    bool found = false;
    rw_reader_enter(SY);
    unsigned pc = ST->player_count;
    for (me = 0; me < pc; me++)
    {
      if (ST->players[me].pid == mypid)
      {
        found = true;
        break;
      }
    }
    bool fin = ST->finished;
    rw_reader_exit(SY);
    if (fin)
      goto done; // si ya terminó, salir
    if (found)
      break;
    usleep(1000); // 1ms para no hacer busy-wait
  }

  // Bucle principal: un movimiento por permiso del master (G[me])
  while (1)
  {
    // Esperar permiso para ENVIAR UN movimiento
    sem_wait(&SY->G[me]);

    // ¿Ya terminó el juego?
    rw_reader_enter(SY);
    bool fin = ST->finished;
    rw_reader_exit(SY);
    if (fin)
      break;

    // Elegir dirección (snapshot de lectura mínima)
    rw_reader_enter(SY);
    int d = choose_dir(ST, me);
    rw_reader_exit(SY);

    // Sin movimientos válidos -> cerrar stdout (EOF) y terminar
    if (d < 0)
    {
      close(1);
      break;
    }

    // Enviar 1 byte con la dirección (0..7) por stdout (fd=1)
    unsigned char dir = (unsigned char)d;
    ssize_t w = write(1, &dir, 1);
    if (w != 1)
    {
      // Si el master cerró el pipe, salimos
      break;
    }
    // Importante: NO hacer sem_post(G[me]); eso lo hace el master cuando procesa
  }

done:
  munmap(SY, sizeof *SY);
  munmap(ST, st.st_size);
  return 0;
}
