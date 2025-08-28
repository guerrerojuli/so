#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <ncurses.h>

#include "game_state.h"
#include "game_sync.h"
#include "headers/shmADT.h"

static volatile sig_atomic_t stop_requested = 0;

static void handle_sigint(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void print_board(const GameState *state)
{
    int start_y = 1;
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
    int start_y = (int)state->height + 3;
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

/* Usa implementación común en game_sync.c */

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        errno = EINVAL;
        perror("view: invalid usage");
        return 1;
    }

    unsigned long width = strtoul(argv[1], NULL, 10);
    unsigned long height = strtoul(argv[2], NULL, 10);
    if (width == 0 || height == 0)
    {
        errno = EINVAL;
        perror("view: invalid dimensions");
        return 1;
    }

    size_t map_size = GAME_STATE_MAP_SIZE(width, height);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    ShmADT state_shm = open_shm(GAME_STATE_SHM_NAME, map_size, O_RDONLY, 0600, PROT_READ);
    GameState *state = (GameState *)get_shm_pointer(state_shm);

    ShmADT sync_shm = open_shm(GAME_SYNC_SHM_NAME, sizeof(GameSync), O_RDWR, 0600, PROT_READ | PROT_WRITE);
    GameSync *sync = (GameSync *)get_shm_pointer(sync_shm);

    if (!getenv("TERM"))
        setenv("TERM", "xterm-256color", 1);
    initscr();
    cbreak();
    noecho();
    curs_set(0);

    while (!stop_requested)
    {
        if (sem_wait(&sync->view_update_ready) == -1)
        {
            if (errno == EINTR)
                continue;
            perror("view: sem_wait(view_update_ready)");
            break;
        }
        game_sync_reader_enter(sync);
        clear();
        mvprintw(0, 0, "==== JUEGO ====");
        print_board(state);
        print_players(state);
        mvprintw((int)state->height + 3 + (int)state->player_count + 2, 0,
                 "finished=%s", state->finished ? "true" : "false");
        refresh();

        game_sync_reader_exit(sync);

        if (sem_post(&sync->view_print_done) == -1)
        {
            perror("view: sem_post(view_print_done)");
            break;
        }
    }

    endwin();
    close_shm(sync_shm);
    close_shm(state_shm);
    return 0;
}
