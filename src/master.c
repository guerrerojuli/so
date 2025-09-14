#define _POSIX_C_SOURCE 200809L // para usar getopt
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <time.h>
#include <sys/select.h>
#include <math.h>
#include <stdarg.h>
#include "shmADT.h"
#include "game_state.h"
#include "game_sync.h"
#include "constants.h"

// Shared direction vectors and common constants
#define NUM_DIRECTIONS 8
#define COORD_BUF_LEN 16
#define BOARD_INDEX(state, X, Y) ((Y) * (state)->width + (X))
static const int DIR_DX[NUM_DIRECTIONS] = {0, 1, 1, 1, 0, -1, -1, -1};
static const int DIR_DY[NUM_DIRECTIONS] = {-1, -1, 0, 1, 1, 1, 0, -1};
static const double SPAWN_RADIUS_DIVISOR = 3;

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Estructura para almacenar los argumentos parseados
typedef struct
{
    unsigned int width;
    unsigned int height;
    unsigned int delay;
    unsigned int timeout;
    unsigned int seed;
    char *view_path;
    char *player_paths[MAX_PLAYERS];
    int player_count;
} MasterArgs;

// Estructura para almacenar los recursos del juego (IPC, etc.)
typedef struct
{
    ShmADT state_shm;
    GameState *state;
    ShmADT sync_shm;
    GameSync *sync;
    pid_t *player_pids;   // PIDs de los jugadores
    pid_t view_pid;       // PID de la view
    int *player_pipes;    // Array de file descriptors para los extremos de lectura
    int *player_statuses; // Exit statuses de jugadores (para impresión posterior)
    int view_status;      // Exit status de la vista
} GameResources;

static inline void notify_view(const MasterArgs *args, GameResources *res)
{
    if (!args->view_path)
    {
        return;
    }
    sem_post(&res->sync->view_update_ready);
    sem_wait(&res->sync->view_print_done);
    struct timespec delay = {.tv_sec = args->delay / 1000, .tv_nsec = (args->delay % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}

static inline void lock_writer(GameResources *res)
{
    sem_wait(&res->sync->master_starvation_guard);
    sem_wait(&res->sync->state_mutex);
    sem_post(&res->sync->master_starvation_guard);
}

static inline void unlock_writer(GameResources *res)
{
    sem_post(&res->sync->state_mutex);
}

static inline void finish_game_and_notify(const MasterArgs *args, GameResources *res)
{
    lock_writer(res);
    res->state->finished = true;
    unlock_writer(res);
    notify_view(args, res);
}

static inline void set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags != -1)
    {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

static inline long long monotonic_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

static bool any_player_can_move(const GameState *state)
{
    for (unsigned int i = 0; i < state->player_count; i++)
    {
        const Player *p = &state->players[i];
        if (p->blocked)
            continue;
        for (int m = 0; m < NUM_DIRECTIONS; m++)
        {
            int nx = (int)p->x + DIR_DX[m];
            int ny = (int)p->y + DIR_DY[m];
            if (nx >= 0 && nx < (int)state->width && ny >= 0 && ny < (int)state->height)
            {
                if (state->board[BOARD_INDEX(state, nx, ny)] > 0)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool launch_player(const MasterArgs *args, GameResources *res, int player_index, const char *width_str, const char *height_str)
{
    int pipe_fds[2];
    if (pipe(pipe_fds) == -1)
    {
        perror("pipe creation failed");
        return false;
    }

    // Evitar heredar descriptores a procesos execv (CLOEXEC)
    for (int i = 0; i < 2; i++)
    {
        set_cloexec(pipe_fds[i]);
    }

    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork failed for player");
        return false;
    }

    if (pid == 0)
    {                           // Proceso hijo (jugador)
        close(pipe_fds[R_END]); // El jugador no lee del pipe - R_END = 0
        if (dup2(pipe_fds[W_END], STDOUT_FILENO) == -1)
        {
            perror("dup2 failed for player");
            exit(EXIT_FAILURE);
        }
        close(pipe_fds[W_END]); // no necesito mas el original

        char *argv[] = {args->player_paths[player_index], (char *)width_str, (char *)height_str, NULL};
        execv(args->player_paths[player_index], argv);
        perror("execv player failed");
        exit(EXIT_FAILURE);
    }

    // Proceso padre (master)
    close(pipe_fds[W_END]); // El master no escribe en el pipe - W_END = 1
    res->player_pipes[player_index] = pipe_fds[R_END];
    res->player_pids[player_index] = pid;
    return true;
}

static bool launch_view(const MasterArgs *args, GameResources *res, const char *width_str, const char *height_str)
{
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork failed for view");
        return false; // Aquí deberíamos limpiar los jugadores ya creados
    }
    if (pid == 0)
    { // Proceso hijo (vista)
        char *argv[] = {args->view_path, (char *)width_str, (char *)height_str, NULL};
        execv(args->view_path, argv);
        perror("execv view failed");
        exit(EXIT_FAILURE);
    }
    res->view_pid = pid;
    return true;
}

static bool launch_children(const MasterArgs *args, GameResources *res)
{
    char width_str[COORD_BUF_LEN]; // para pasarle el ancho y alto al jugador y view
    char height_str[COORD_BUF_LEN];
    snprintf(width_str, sizeof(width_str), "%u", args->width);
    snprintf(height_str, sizeof(height_str), "%u", args->height);

    // Lanzar jugadores
    for (int i = 0; i < args->player_count; i++)
    {
        if (!launch_player(args, res, i, width_str, height_str))
        {
            return false;
        }
    }

    // Lanzar vista (si existe)
    if (args->view_path)
    {
        if (!launch_view(args, res, width_str, height_str))
        {
            return false;
        }
    }

    return true;
}

static void init_game_state(const MasterArgs *args, GameResources *res)
{
    srand(args->seed);

    GameState *state = res->state;
    state->width = args->width;
    state->height = args->height;
    state->player_count = args->player_count;
    state->finished = false;

    // Inicializar el tablero con recompensas aleatorias
    for (unsigned int i = 0; i < state->width * state->height; i++)
    {
        state->board[i] = 1 + (rand() % 9); // Recompensas entre 1 y 9
    }

    // state->board[BOARD_INDEX(state, args->width - 1, (args->height / 2))] = 4;
    //  Inicializar jugadores
    for (int i = 0; i < args->player_count; i++)
    {
        Player *p = &state->players[i];
        p->pid = res->player_pids[i];
        p->score = 0;
        p->valid_move_requests = 0;
        p->invalid_move_requests = 0;
        p->blocked = false;

        // Cálculo delíptico alrededor del centro del tablero
        double radius_x = ((double)state->width) / SPAWN_RADIUS_DIVISOR;
        double radius_y = ((double)state->height) / SPAWN_RADIUS_DIVISOR;
        if (radius_x < 1.0)
            radius_x = 1.0;
        if (radius_y < 1.0)
            radius_y = 1.0;
        int center_x = (int)state->width / 2;
        int center_y = (int)state->height / 2;

        double theta = (2.0 * M_PI * (double)i) / (double)state->player_count;
        int tx = center_x + (int)lround(radius_x * cos(theta));
        int ty = center_y + (int)lround(radius_y * sin(theta));
        tx = clampi(tx, 0, (int)state->width - 1);
        ty = clampi(ty, 0, (int)state->height - 1);

        p->x = (unsigned short)tx;
        p->y = (unsigned short)ty;
        // Marcar la celda de spawn como ocupada por el jugador, según el enunciado (-id).
        state->board[BOARD_INDEX(state, p->x, p->y)] = -(i);
    }
}

static void process_player_move(int player_idx, int pipe_fd, const MasterArgs *args, GameResources *res)
{
    unsigned char move;
    ssize_t bytes_read = read(pipe_fd, &move, sizeof(move));

    if (bytes_read <= 0)
    { // EOF o error
        if (bytes_read != 0)
            perror("read from pipe failed");

        // Bloqueamos al jugador para que no se le considere más
        sem_wait(&res->sync->state_mutex);
        res->state->players[player_idx].blocked = true;
        sem_post(&res->sync->state_mutex);

        close(pipe_fd);
        res->player_pipes[player_idx] = -1; // Marcar como cerrado

        // Notificar a la vista del cambio de estado (jugador bloqueado) si existe
        notify_view(args, res);

        return;
    }

    // Adquirir bloqueo de escritor para modificar el estado
    sem_wait(&res->sync->master_starvation_guard);
    sem_wait(&res->sync->state_mutex);
    sem_post(&res->sync->master_starvation_guard);

    GameState *state = res->state;
    Player *player = &state->players[player_idx];
    bool is_valid = false;

    // Calcular nuevas coordenadas (lógica simple, se puede refinar)
    if (move < NUM_DIRECTIONS)
    {
        int nx = player->x + DIR_DX[move];
        int ny = player->y + DIR_DY[move];

        // Validar movimiento
        if (nx >= 0 && nx < state->width && ny >= 0 && ny < state->height &&
            state->board[BOARD_INDEX(state, nx, ny)] > 0)
        {

            is_valid = true;
            int reward = state->board[BOARD_INDEX(state, nx, ny)];
            player->score += reward;
            player->x = nx;
            player->y = ny;
            state->board[BOARD_INDEX(state, nx, ny)] = -(player_idx);
            player->valid_move_requests++;
        }
    }

    if (!is_valid)
    {
        player->invalid_move_requests++;
    }

    // Liberar bloqueo de escritor
    unlock_writer(res);

    // Notificar al jugador correspondiente que su solicitud fue procesada
    sem_post(&res->sync->player_can_move[player_idx]);

    // Notificar a la vista ante cualquier cambio de estado (válido o inválido)
    notify_view(args, res);
}

static void cleanup_game_resources(GameResources *res, int player_count)
{
    // Destruir semáforos antes de liberar la SHM de sincronización
    if (res->sync)
    {
        sem_destroy(&res->sync->view_update_ready);
        sem_destroy(&res->sync->view_print_done);
        sem_destroy(&res->sync->master_starvation_guard);
        sem_destroy(&res->sync->state_mutex);
        sem_destroy(&res->sync->readers_count_mutex);
        for (int i = 0; i < player_count && i < MAX_PLAYERS; i++)
        {
            sem_destroy(&res->sync->player_can_move[i]);
        }
    }

    if (res->player_pipes)
    {
        for (int i = 0; i < player_count; i++)
        {
            if (res->player_pipes[i] >= 0)
            {
                close(res->player_pipes[i]);
                res->player_pipes[i] = -1;
            }
        }
        free(res->player_pipes);
    }
    if (res->player_pids)
    {
        free(res->player_pids);
    }
    if (res->player_statuses)
    {
        free(res->player_statuses);
    }
    if (res->state_shm)
    {
        destroy_shm(res->state_shm);
    }
    if (res->sync_shm)
    {
        destroy_shm(res->sync_shm);
    }
}

static void print_finish_status(const MasterArgs *args, GameResources *res)
{
    if (res->view_pid > 0)
    {
        if (WIFEXITED(res->view_status))
        {
            printf("View exited (%d)\n", WEXITSTATUS(res->view_status));
        }
        else if (WIFSIGNALED(res->view_status))
        {
            printf("View terminated by signal %d\n", WTERMSIG(res->view_status));
        }
    }

    for (int i = 0; i < args->player_count; i++)
    {
        if (res->player_pids[i] > 0)
        {
            if (WIFEXITED(res->player_statuses[i]))
            {
                printf("Player %d (PID %d) exited (%d) with a score of %d / %d / %d.\n", i, res->player_pids[i], WEXITSTATUS(res->player_statuses[i]), res->state->players[i].score, res->state->players[i].valid_move_requests, res->state->players[i].invalid_move_requests);
            }
            else if (WIFSIGNALED(res->player_statuses[i]))
            {
                printf("Player %d (PID %d) terminated by signal %d with a score of %d / %d / %d.\n", i, res->player_pids[i], WTERMSIG(res->player_statuses[i]), res->state->players[i].score, res->state->players[i].valid_move_requests, res->state->players[i].invalid_move_requests);
            }
        }
    }
}

static void print_usage(const char *exec_name)
{
    fprintf(stderr, "Usage: %s [-w width] [-h height] [-d delay] [-t timeout] [-s seed] [-v view_path] -p player1 [player2 ...]\\n", exec_name);
}

static bool parse_args(int argc, char **argv, MasterArgs *args)
{
    // Valores por defecto
    args->width = DEFAULT_WIDTH;
    args->height = DEFAULT_HEIGHT;
    args->delay = DEFAULT_DELAY;
    args->timeout = DEFAULT_TIMEOUT;
    args->seed = time(NULL);
    args->view_path = NULL;
    args->player_count = 0;

    int opt;
    bool players_set = false; // Se usa para aceptar solo el primer -p
    while ((opt = getopt(argc, argv, "w:h:d:t:s:v:p:")) != -1)
    {
        switch (opt)
        {
        case 'w':
            args->width = atoi(optarg);
            break;
        case 'h':
            args->height = atoi(optarg);
            break;
        case 'd':
            args->delay = atoi(optarg);
            break;
        case 't':
            args->timeout = atoi(optarg);
            break;
        case 's':
            args->seed = atoi(optarg);
            break;
        case 'v':
            args->view_path = optarg;
            break;
        case 'p':
            // Aceptamos solo el primer grupo de jugadores (primer -p).
            // Consumimos optarg (primer jugador) y luego todos los argumentos
            // siguientes que no comiencen con '-' como jugadores adicionales.
            if (!players_set)
            {
                players_set = true;
                // Primer jugador proviene de optarg
                if (args->player_count == MAX_PLAYERS)
                {
                    fprintf(stderr, "Error: Maximum number of players is %d.\\n", MAX_PLAYERS);
                    return false;
                }
                args->player_paths[args->player_count++] = optarg;

                // Agregar jugadores adicionales hasta el próximo argumento que parezca opción
                while (optind < argc && argv[optind][0] != '-')
                {
                    if (args->player_count == MAX_PLAYERS)
                    {
                        fprintf(stderr, "Error: Maximum number of players is %d.\\n", MAX_PLAYERS);
                        return false;
                    }
                    args->player_paths[args->player_count++] = argv[optind++];
                }
            }
            else
            {
                // Ignorar -p adicionales: no agregar jugadores y saltar sus argumentos
                // para que getopt pueda seguir procesando opciones posteriores.
                while (optind < argc && argv[optind][0] != '-')
                {
                    optind++;
                }
            }
            break;
        default:
            print_usage(argv[0]);
            return false;
        }
    }

    if (args->player_count == 0)
    {
        fprintf(stderr, "Error: At least one player must be specified with -p.\\n");
        print_usage(argv[0]);
        return false;
    }

    if (args->width < MIN_WIDTH || args->height < MIN_HEIGHT)
    {
        fprintf(stderr, "Error: Minimum width and height are %d and %d.\\n", MIN_WIDTH, MIN_HEIGHT);
        return false;
    }

    return true;
}

static bool init_game_resources(const MasterArgs *args, GameResources *res)
{
    // Crear memoria compartida para sincronización
    res->sync_shm = create_shm(GAME_SYNC_SHM_NAME, sizeof(GameSync), O_RDWR | O_CREAT | O_EXCL, 0666, PROT_READ | PROT_WRITE);
    if (res->sync_shm == NULL)
    {
        perror("create_shm GameSync failed");
        return false;
    }
    res->sync = get_shm_pointer(res->sync_shm);

    // Inicializar semáforos
    sem_init(&res->sync->view_update_ready, 1, 0);
    sem_init(&res->sync->view_print_done, 1, 0);
    sem_init(&res->sync->master_starvation_guard, 1, 1);
    sem_init(&res->sync->state_mutex, 1, 1);
    sem_init(&res->sync->readers_count_mutex, 1, 1);
    res->sync->readers_count = 0;
    for (int i = 0; i < args->player_count; i++)
    {
        sem_init(&res->sync->player_can_move[i], 1, 1); // Cada jugador puede enviar 1 solicitud inicial
    }

    // Crear memoria compartida para el estado del juego
    size_t state_size = GAME_STATE_MAP_SIZE(args->width, args->height);
    res->state_shm = create_shm(GAME_STATE_SHM_NAME, state_size, O_RDWR | O_CREAT | O_EXCL, 0666, PROT_READ | PROT_WRITE);
    if (res->state_shm == NULL)
    {
        perror("create_shm GameState failed");
        destroy_shm(res->sync_shm); // Limpiar recurso anterior
        return false;
    }
    res->state = get_shm_pointer(res->state_shm);

    return true;
}

static bool init_resources(const MasterArgs *args, GameResources *res)
{
    *res = (GameResources){0};

    res->player_pipes = (int *)calloc(args->player_count, sizeof(int));
    res->player_pids = (pid_t *)calloc(args->player_count, sizeof(pid_t));
    res->player_statuses = (int *)calloc(args->player_count, sizeof(int));
    if (!res->player_pipes || !res->player_pids)
    {
        perror("allocating memory for child resources failed");
        cleanup_game_resources(res, args->player_count);
        return false;
    }

    // Inicializar descriptores de pipe a -1 para evitar cierres accidentales de 0
    for (int i = 0; i < args->player_count; i++)
    {
        res->player_pipes[i] = -1;
    }

    if (!init_game_resources(args, res))
    {
        fprintf(stderr, "Error: Game resources could not be initialized.\n");
        cleanup_game_resources(res, args->player_count);
        return false;
    }

    return true;
}

static void print_config(const MasterArgs *args)
{
    printf("width: %u\n", args->width);
    printf("height: %u\n", args->height);
    printf("delay: %u\n", args->delay);
    printf("timeout: %u\n", args->timeout);
    printf("seed: %u\n", args->seed);
    printf("view: %s\n", args->view_path ? args->view_path : "");
    printf("num_players: %d\n", args->player_count);
    for (int i = 0; i < args->player_count; i++)
    {
        printf("  %s\n", args->player_paths[i]);
    }
}

static void init_game(const MasterArgs *args, GameResources *resources)
{
    init_game_state(args, resources);
    notify_view(args, resources);

    int current_player_turn = 0;
    fd_set read_fds;
    int max_fd = 0;
    long long last_valid_move_ms = monotonic_millis();

    while (!resources->state->finished)
    {
        FD_ZERO(&read_fds);
        max_fd = 0; // Recalcular en cada iteración
        int active_players = 0;
        for (int i = 0; i < args->player_count; i++)
        {
            if (!resources->state->players[i].blocked && resources->player_pipes[i] != -1)
            {
                FD_SET(resources->player_pipes[i], &read_fds);
                if (resources->player_pipes[i] > max_fd)
                {
                    max_fd = resources->player_pipes[i];
                }
                active_players++;
            }
        }

        if (active_players == 0)
        {
            // Adquirir lock de escritor para actualizar el estado final
            sem_wait(&resources->sync->master_starvation_guard);
            sem_wait(&resources->sync->state_mutex);
            sem_post(&resources->sync->master_starvation_guard);

            resources->state->finished = true;

            // Liberar lock de escritor
            sem_post(&resources->sync->state_mutex);

            // Notificar a la vista por última vez para que vea finished=true
            sem_post(&resources->sync->view_update_ready);
            sem_wait(&resources->sync->view_print_done);

            break;
        }

        // Timeout basado en último movimiento válido
        long long now_ms = monotonic_millis();
        long long elapsed_ms = now_ms - last_valid_move_ms;
        long long remaining_ms = (long long)args->timeout * 1000LL - elapsed_ms;
        if (remaining_ms <= 0)
        {
            // Set finished bajo lock y notificar vista
            finish_game_and_notify(args, resources);
            break;
        }

        struct timeval timeout;
        timeout.tv_sec = (time_t)(remaining_ms / 1000LL);
        timeout.tv_usec = (suseconds_t)((remaining_ms % 1000LL) * 1000LL);

        int ready_fds = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (ready_fds == -1)
        {
            perror("select failed");
            break;
        }

        if (ready_fds == 0)
        {
            // select agotó el timeout relativo a últimos válidos → finalizar
            finish_game_and_notify(args, resources);
            break;
        }

        // Lógica de Round-Robin
        for (int i = 0; i < args->player_count; i++)
        {
            int player_idx = (current_player_turn + i) % args->player_count;
            int player_pipe = resources->player_pipes[player_idx];

            if (FD_ISSET(player_pipe, &read_fds))
            {
                // Procesar movimiento
                // Capturar conteos previos para detectar si fue válido
                unsigned int prev_valid = resources->state->players[player_idx].valid_move_requests;
                process_player_move(player_idx, player_pipe, args, resources);

                // Si hubo un movimiento válido, actualizar reloj
                if (resources->state->players[player_idx].valid_move_requests > prev_valid)
                {
                    last_valid_move_ms = monotonic_millis();
                }

                // Recalcular jugadores activos después de procesar el movimiento
                int remaining_active = 0;
                for (int p = 0; p < args->player_count; p++)
                {
                    if (!resources->state->players[p].blocked && resources->player_pipes[p] != -1)
                    {
                        remaining_active++;
                    }
                }

                if (remaining_active == 0)
                {
                    // Adquirir lock de escritor para actualizar el estado final y notificar
                    finish_game_and_notify(args, resources);

                    break; // salir del for y luego del while por la condición
                }

                // Finalizar si ningún jugador puede moverse
                if (!any_player_can_move(resources->state))
                {
                    finish_game_and_notify(args, resources);
                    break;
                }

                // Avanzar al siguiente jugador para la próxima ronda
                current_player_turn = (player_idx + 1) % args->player_count;
                break; // Procesar solo un jugador por iteración de select
            }
        }
    }

    if (resources->view_pid > 0)
    {
        int status;
        waitpid(resources->view_pid, &status, 0);
        resources->view_status = status;
    }
    for (int i = 0; i < args->player_count; i++)
    {
        if (resources->player_pids[i] > 0)
        {
            int status;
            waitpid(resources->player_pids[i], &status, 0);
            resources->player_statuses[i] = status;
        }
    }
}

int main(int argc, char **argv)
{
    MasterArgs args;
    if (!parse_args(argc, argv, &args))
        return EXIT_FAILURE;

    print_config(&args);

    GameResources resources;
    if (!init_resources(&args, &resources))
    {
        return EXIT_FAILURE;
    }

    if (!launch_children(&args, &resources))
    {
        fprintf(stderr, "Error: Child processes could not be launched.\n");
        cleanup_game_resources(&resources, args.player_count);
        return EXIT_FAILURE;
    }

    init_game(&args, &resources);

    print_finish_status(&args, &resources);

    cleanup_game_resources(&resources, args.player_count);
    return 0;
}
