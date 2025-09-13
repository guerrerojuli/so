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

#include "shmADT.h"
#include "game_state.h"
#include "game_sync.h"
#include "constants.h"

// Estructura para almacenar los argumentos parseados
typedef struct {
    unsigned int width;
    unsigned int height;
    unsigned int delay;
    unsigned int timeout;
    unsigned int seed;
    char *view_path; // binario de la view
    char **player_paths; // binarios de los jugadores
    int player_count;
} MasterArgs;

// Estructura para almacenar los recursos del juego (IPC, etc.)
typedef struct {
    ShmADT state_shm;
    GameState *state;
    ShmADT sync_shm;
    GameSync *sync;
    pid_t *player_pids; // PIDs de los jugadores
    pid_t view_pid; // PID de la view
    int *player_pipes; // Array de file descriptors para los extremos de lectura
} GameResources;


static bool launch_children(const MasterArgs *args, GameResources *res) {
    char width_str[16]; // para pasarle el ancho y alto al jugador y view
    char height_str[16];
    snprintf(width_str, sizeof(width_str), "%u", args->width);
    snprintf(height_str, sizeof(height_str), "%u", args->height);

    // Lanzar jugadores
    for (int i = 0; i < args->player_count; i++) {
        int pipe_fds[2];
        if (pipe(pipe_fds) == -1) {
            perror("Error al crear pipe");
            return false;
        }

        pid_t pid = fork();
        if (pid == -1) {
            perror("Error en fork para jugador");
            return false;
        }

        if (pid == 0) { // Proceso hijo (jugador)
            close(pipe_fds[R_END]); // El jugador no lee del pipe - R_END = 0
            if (dup2(pipe_fds[W_END], STDOUT_FILENO) == -1) {
                perror("Error en dup2 para jugador");
                exit(EXIT_FAILURE);
            }
            close(pipe_fds[W_END]); // no necesito mas el original

            char *argv[] = {args->player_paths[i], width_str, height_str, NULL};
            execv(args->player_paths[i], argv);
            fprintf(stderr, "Error al ejecutar %s: %s\\n", args->player_paths[i], strerror(errno));
            exit(EXIT_FAILURE);
        }

        // Proceso padre (master)
        close(pipe_fds[W_END]); // El master no escribe en el pipe - W_END = 1
        res->player_pipes[i] = pipe_fds[R_END];
        res->player_pids[i] = pid;
    }

    // Lanzar vista (si existe)
    if (args->view_path) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("Error en fork para vista");
            return false; // Aquí deberíamos limpiar los jugadores ya creados
        }
        if (pid == 0) { // Proceso hijo (vista)
            char *argv[] = {args->view_path, width_str, height_str, NULL};
            execv(args->view_path, argv);
            fprintf(stderr, "Error al ejecutar %s: %s\\n", args->view_path, strerror(errno));
            exit(EXIT_FAILURE);
        }
        res->view_pid = pid;
    }

    return true;
}

static void init_game_state(const MasterArgs *args, GameResources *res) {
    srand(args->seed);
    
    GameState *state = res->state;
    state->width = args->width;
    state->height = args->height;
    state->player_count = args->player_count;
    state->finished = false;

    // Inicializar el tablero con recompensas aleatorias
    for (unsigned int i = 0; i < state->width * state->height; i++) {
        state->board[i] = 1 + (rand() % 9); // Recompensas entre 1 y 9
    }

    // Inicializar y posicionar a los jugadores
    for (int i = 0; i < args->player_count; i++) {
        Player *p = &state->players[i];
        p->pid = res->player_pids[i];
        snprintf(p->name, sizeof(p->name), "player %d", i);
        p->score = 0;
        p->valid_move_requests = 0;
        p->invalid_move_requests = 0;
        p->blocked = false;

        // Lógica de spawn simple: en las esquinas y puntos intermedios - REVISAR LUEGO
        switch(i) {
            case 0: p->x = 0; p->y = 0; break;
            case 1: p->x = state->width - 1; p->y = state->height - 1; break;
            case 2: p->x = 0; p->y = state->height - 1; break;
            case 3: p->x = state->width - 1; p->y = 0; break;
            default: // Posiciones aleatorias para más jugadores, evitando bordes
                p->x = 1 + (rand() % (state->width - 2));
                p->y = 1 + (rand() % (state->height - 2));
                break;
        }
        // Marcar la celda de spawn como ocupada por el jugador, según el enunciado (-id).
        state->board[p->y * state->width + p->x] = -(i);
    }
}


static void cleanup_game_resources(GameResources *res, int player_count) {
    if (res->player_pipes) {
        for (int i = 0; i < player_count; i++) {
            if (res->player_pipes[i] != 0) {
                close(res->player_pipes[i]);
            }
        }
        free(res->player_pipes);
    }
     if (res->player_pids) {
        free(res->player_pids);
    }
    if (res->state_shm) {
        destroy_shm(res->state_shm);
    }
    if (res->sync_shm) {
        destroy_shm(res->sync_shm);
    }
}


static void print_usage(const char *exec_name) {
    fprintf(stderr, "Uso: %s [-w width] [-h height] [-d delay] [-t timeout] [-s seed] [-v view_path] -p player1 [player2 ...]\\n", exec_name);
}

static bool parse_args(int argc, char **argv, MasterArgs *args) {
    // Valores por defecto
    args->width = DEFAULT_WIDTH;
    args->height = DEFAULT_HEIGHT;
    args->delay = DEFAULT_DELAY;
    args->timeout = DEFAULT_TIMEOUT;
    args->seed = time(NULL);
    args->view_path = NULL;
    args->player_paths = NULL;
    args->player_count = 0;

    int opt;
    bool players_found = false;
    while ((opt = getopt(argc, argv, "w:h:d:t:s:v:p:")) != -1 && !players_found) {
        switch (opt) {
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
                // optind es el índice del siguiente argumento a procesar.
                // A partir de aquí son todas rutas de players.
                optind--;
                args->player_count = argc - optind;
                if (args->player_count > MAX_PLAYERS) {
                    fprintf(stderr, "Error: Se pueden tener como máximo %d jugadores.\\n", MAX_PLAYERS);
                    return false;
                }
                args->player_paths = &argv[optind];
                players_found = true; // Salimos del bucle después de esto
                break;
            default:
                print_usage(argv[0]);
                return false;
        }
    }

    if (args->player_count == 0) {
        fprintf(stderr, "Error: Se debe especificar al menos un jugador con -p.\\n");
        print_usage(argv[0]);
        return false;
    }

    if (args->width < MIN_WIDTH || args->height < MIN_HEIGHT) {
        fprintf(stderr, "Error: El ancho y alto mínimos son %d y %d.\\n", MIN_WIDTH, MIN_HEIGHT);
        return false;
    }

    return true;
}

static bool init_game_resources(const MasterArgs *args, GameResources *res) {
    // Crear memoria compartida para sincronización
    res->sync_shm = create_shm(GAME_SYNC_SHM_NAME, sizeof(GameSync), O_RDWR | O_CREAT | O_EXCL, 0666, PROT_READ | PROT_WRITE);
    if (res->sync_shm == NULL) {
        perror("Error al crear la SHM de sincronización");
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
    for (int i = 0; i < args->player_count; i++) {
        sem_init(&res->sync->player_can_move[i], 1, (i == 0) ? 1 : 0); // El primer jugador puede empezar
    }

    // Crear memoria compartida para el estado del juego
    size_t state_size = GAME_STATE_MAP_SIZE(args->width, args->height);
    res->state_shm = create_shm(GAME_STATE_SHM_NAME, state_size, O_RDWR | O_CREAT | O_EXCL, 0666, PROT_READ | PROT_WRITE);
    if (res->state_shm == NULL) {
        perror("Error al crear la SHM del estado del juego");
        destroy_shm(res->sync_shm); // Limpiar recurso anterior
        return false;
    }
    res->state = get_shm_pointer(res->state_shm);

    return true;
}


// Hago un par de prints para ver que este funcionando bien la config del master
int main(int argc, char **argv)
{
    MasterArgs args;
    if (!parse_args(argc, argv, &args)) {
        return EXIT_FAILURE;
    }

    printf("Configuración del juego:\n");
    printf("  Ancho: %u\n", args.width);
    printf("  Alto: %u\n", args.height);
    printf("  Jugadores: %d\n", args.player_count);
    for(int i = 0; i < args.player_count; i++) {
        printf("    - %s\n", args.player_paths[i]);
    }

    GameResources resources = {0}; // Inicializa todos los campos a 0/NULL
    resources.player_pipes = (int *)calloc(args.player_count, sizeof(int));
    resources.player_pids = (pid_t *)calloc(args.player_count, sizeof(pid_t));
    if (!resources.player_pipes || !resources.player_pids) {
        perror("Error al reservar memoria para recursos de hijos");
        cleanup_game_resources(&resources, args.player_count);
        return EXIT_FAILURE;
    }

    if (!init_game_resources(&args, &resources)) {
        fprintf(stderr, "Error: No se pudieron inicializar los recursos del juego.\\n");
        cleanup_game_resources(&resources, args.player_count);
        return EXIT_FAILURE;
    }
    printf("Recursos del juego inicializados correctamente.\\n");

    if (!launch_children(&args, &resources)) {
        fprintf(stderr, "Error: No se pudieron lanzar los procesos hijos.\\n");
        cleanup_game_resources(&resources, args.player_count);
        return EXIT_FAILURE;
    }
    printf("Procesos hijos lanzados correctamente.\\n");

    printf("Inicializando estado del juego...\\n");
    init_game_state(&args, &resources);
    printf("Estado del juego inicializado.\\n");

    // TODO: Fase 3 - Bucle principal del juego
    // Espera simple a que los hijos terminen (temporal)
    printf("Esperando a que los procesos hijos terminen...\\n");
    for (int i = 0; i < args.player_count; i++) {
        waitpid(resources.player_pids[i], NULL, 0);
    }
    if (resources.view_pid != 0) {
        waitpid(resources.view_pid, NULL, 0);
    }
    
    printf("Limpiando recursos...\\n");
    cleanup_game_resources(&resources, args.player_count);
    printf("Recursos limpiados. Saliendo.\\n");

    return EXIT_SUCCESS;
}
