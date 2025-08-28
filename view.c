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
static int colors_ok = 0;
static const short BASE_COLORS[] = {COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE};
static const int NUM_BASE_COLORS = (int)(sizeof(BASE_COLORS) / sizeof(BASE_COLORS[0]));

static inline short player_color_pair(unsigned int idx)
{
    if (!colors_ok)
        return 0;
    return (short)(1 + (idx % (unsigned int)MAX_PLAYERS));
}

static void handle_sigint(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void draw_box(int y, int x, int height, int width, const char *title)
{
    if (height < 2 || width < 2)
        return;

    mvaddch(y, x, ACS_ULCORNER);
    mvhline(y, x + 1, ACS_HLINE, width - 2);
    mvaddch(y, x + width - 1, ACS_URCORNER);

    mvvline(y + 1, x, ACS_VLINE, height - 2);
    mvvline(y + 1, x + width - 1, ACS_VLINE, height - 2);

    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);

    if (title && *title)
    {
        int len = (int)strlen(title);
        int pos_x = x + 2;
        if (pos_x + len < x + width - 1)
        {
            attron(A_BOLD);
            mvprintw(y, pos_x, "%s", title);
            attroff(A_BOLD);
        }
    }
}

static void print_board(const GameState *state, const int *owner_map, const int *head_map)
{
    int start_y = 1;
    int cell_w = 5;
    int inner_h = (int)state->height;
    int inner_w = (int)state->width * cell_w;
    char title[64];
    snprintf(title, sizeof(title), "Board %ux%u", state->width, state->height);
    draw_box(start_y, 0, inner_h + 2, inner_w + 2, title);

    for (unsigned int row = 0; row < state->height; ++row)
    {
        for (unsigned int col = 0; col < state->width; ++col)
        {
            int idx = (int)(row * state->width + col);
            int cell = state->board[idx];
            int owner = owner_map ? owner_map[idx] : -1;
            int head_owner = head_map ? head_map[idx] : -1;
            short pair = owner >= 0 ? player_color_pair((unsigned int)owner) : 0;
            int y = start_y + 1 + (int)row;
            int x = 1 + (int)col * cell_w;

            if (head_owner >= 0)
            {
                short head_pair = player_color_pair((unsigned int)head_owner);
                if (head_pair)
                    attron(COLOR_PAIR(head_pair) | A_BOLD);
                mvprintw(y, x, "[%3d]", cell);
                if (head_pair)
                    attroff(COLOR_PAIR(head_pair) | A_BOLD);
            }
            else
            {
                if (pair)
                    attron(COLOR_PAIR(pair));
                mvprintw(y, x, " %3d ", cell);
                if (pair)
                    attroff(COLOR_PAIR(pair));
            }
        }
    }
}

static void print_players(const GameState *state)
{
    int start_y = (int)state->height + 3;
    int list_rows = (int)state->player_count;
    int box_w = COLS - 2;
    if (box_w < 10)
        box_w = 10;
    char title[64];
    snprintf(title, sizeof(title), "Players (%u)", state->player_count);
    draw_box(start_y, 0, list_rows + 2, box_w, title);

    for (unsigned int i = 0; i < state->player_count && i < MAX_PLAYERS; ++i)
    {
        const Player *p = &state->players[i];
        short pair = player_color_pair(i);
        if (pair)
            attron(COLOR_PAIR(pair));
        mvprintw(start_y + 1 + (int)i, 1,
                 "- #%u name=%s score=%u pos=(%u,%u) valid=%u invalid=%u blocked=%s",
                 i,
                 p->name,
                 p->score,
                 (unsigned)p->x,
                 (unsigned)p->y,
                 p->valid_move_requests,
                 p->invalid_move_requests,
                 p->blocked ? "true" : "false");
        if (pair)
            attroff(COLOR_PAIR(pair));
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

    size_t cells = (size_t)state->width * (size_t)state->height;
    int *owner_map = (int *)malloc(cells * sizeof(int));
    int *head_map = (int *)malloc(cells * sizeof(int));
    if (owner_map == NULL || head_map == NULL)
    {
        perror("view: malloc(owner_map/head_map)");
        close_shm(sync_shm);
        close_shm(state_shm);
        return 1;
    }
    for (size_t i = 0; i < cells; ++i)
        owner_map[i] = -1;
    for (size_t i = 0; i < cells; ++i)
        head_map[i] = -1;

    if (!getenv("TERM"))
        setenv("TERM", "xterm-256color", 1);
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    if (has_colors())
    {
        start_color();
        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            short fg = BASE_COLORS[i % NUM_BASE_COLORS];
            init_pair((short)(i + 1), fg, COLOR_BLACK);
        }
        colors_ok = 1;
    }

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
        attron(A_BOLD);
        mvprintw(0, 0, "==== JUEGO ====");
        attroff(A_BOLD);
        for (size_t i = 0; i < cells; ++i)
            head_map[i] = -1;
        for (unsigned int i = 0; i < state->player_count && i < MAX_PLAYERS; ++i)
        {
            unsigned int px = state->players[i].x;
            unsigned int py = state->players[i].y;
            if (px < state->width && py < state->height)
            {
                owner_map[py * state->width + px] = (int)i; /* persist visited */
                head_map[py * state->width + px] = (int)i;  /* current head */
            }
        }
        print_board(state, owner_map, head_map);
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
    free(owner_map);
    free(head_map);
    close_shm(sync_shm);
    close_shm(state_shm);
    return 0;
}
