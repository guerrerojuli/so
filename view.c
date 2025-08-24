#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <semaphore.h>
#include <ncurses.h>

#include "game_state.h"

/* Debe coincidir con player/master mínimos */
#define SHM_STATE "/game_state"
#define SHM_SYNC "/game_sync"

typedef struct
{
    sem_t A;              /* master -> view */
    sem_t B;              /* view -> master */
    sem_t C;              /* turnstile (RW-lock) */
    sem_t D;              /* resource (RW-lock) */
    sem_t E;              /* readers mutex (RW-lock) */
    unsigned int F;       /* readers count (RW-lock) */
    sem_t G[MAX_PLAYERS]; /* per-player move slot */
} game_sync_t;

static volatile sig_atomic_t stop_requested = 0;

static void handle_sigint(int sig)
{
    (void)sig; // Suppress unused parameter warning
    stop_requested = 1;
}

static void print_board(const GameState *state)
{
    int start_y = 1; /* below the header */
    mvprintw(start_y, 0, "Board %ux%u", state->width, state->height);
    for (unsigned int row = 0; row < state->height; ++row)
    {
        for (unsigned int col = 0; col < state->width; ++col)
        {
            int cell = state->board[row * state->width + col];
            mvprintw(start_y + 1 + (int)row, (int)col * 4, "%3d ", cell);
        }
    }
}

static void print_players(const GameState *state)
{
    int start_y = (int)state->height + 3; /* header + board label + board */
    mvprintw(start_y, 0, "Players (%u)", state->player_count);
    for (unsigned int i = 0; i < state->player_count && i < MAX_PLAYERS; ++i)
    {
        const Player *p = &state->players[i];
        mvprintw(start_y + 1 + (int)i, 0,
                 "- #%u name=%s score=%u pos=(%u,%u) valid=%u invalid=%u blocked=%s",
                 i,
                 p->name,
                 p->score,
                 (unsigned)p->x,
                 (unsigned)p->y,
                 p->valid_move_requests,
                 p->invalid_move_requests,
                 p->blocked ? "true" : "false");
    }
}

static inline void rw_reader_enter(game_sync_t *s)
{
    sem_wait(&s->C);
    sem_post(&s->C);
    sem_wait(&s->E);
    s->F++;
    if (s->F == 1)
        sem_wait(&s->D);
    sem_post(&s->E);
}

static inline void rw_reader_exit(game_sync_t *s)
{
    sem_wait(&s->E);
    s->F--;
    if (s->F == 0)
        sem_post(&s->D);
    sem_post(&s->E);
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "Uso: %s <ancho> <alto>\n", argv[0]);
        return 1;
    }

    // Parse command line arguments
    unsigned long width = strtoul(argv[1], NULL, 10);
    unsigned long height = strtoul(argv[2], NULL, 10);
    if (width == 0 || height == 0)
    {
        fprintf(stderr, "Dimensiones inválidas\n");
        return 1;
    }

    size_t map_size = GAME_STATE_MAP_SIZE(width, height);

    // Handle SIGINT
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    int fd_state = shm_open(SHM_STATE, O_RDONLY, 0600);
    if (fd_state == -1)
    {
        perror("shm_open(GAME_STATE)");
        return 1;
    }
    void *addr_state = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd_state, 0);
    if (addr_state == MAP_FAILED)
    {
        perror("mmap(GAME_STATE)");
        close(fd_state);
        return 1;
    }
    GameState *state = (GameState *)addr_state;

    int fd_sync = shm_open(SHM_SYNC, O_RDWR, 0600);
    if (fd_sync == -1)
    {
        perror("shm_open(GAME_SYNC)");
        munmap(addr_state, map_size);
        close(fd_state);
        return 1;
    }
    void *addr_sync = mmap(NULL, sizeof(game_sync_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd_sync, 0);
    if (addr_sync == MAP_FAILED)
    {
        perror("mmap(GAME_SYNC)");
        munmap(addr_state, map_size);
        close(fd_state);
        close(fd_sync);
        return 1;
    }
    game_sync_t *sync = (game_sync_t *)addr_sync;

    if (!getenv("TERM"))
        setenv("TERM", "xterm-256color", 1);
    initscr();
    cbreak();
    noecho();
    curs_set(0);

    while (!stop_requested)
    {
        if (sem_wait(&sync->A) == -1)
        {
            if (errno == EINTR)
                continue;
            perror("sem_wait(A)");
            break;
        }
        rw_reader_enter(sync);

        /* Read & render state */
        clear();
        mvprintw(0, 0, "==== JUEGO ====");
        print_board(state);
        print_players(state);
        mvprintw((int)state->height + 3 + (int)state->player_count + 2, 0,
                 "finished=%s", state->finished ? "true" : "false");
        refresh();

        rw_reader_exit(sync);

        if (sem_post(&sync->B) == -1)
        {
            perror("sem_post(B)");
            break;
        }
    }

    endwin();
    munmap(addr_sync, sizeof(game_sync_t));
    close(fd_sync);
    munmap(addr_state, map_size);
    close(fd_state);
    return 0;
}
