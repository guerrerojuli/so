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
    char *view_path;
    char **player_paths;
    int player_count;
} MasterArgs;

// Estructura para almacenar los recursos del juego (IPC, etc.)
typedef struct {
    ShmADT state_shm;
    GameState *state;
    ShmADT sync_shm;
    GameSync *sync;
} GameResources;


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

static void cleanup_game_resources(GameResources *res) {
    if (res->state_shm) {
        destroy_shm(res->state_shm);
    }
    if (res->sync_shm) {
        destroy_shm(res->sync_shm);
    }
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

    GameResources resources = {0};
    if (!init_game_resources(&args, &resources)) {
        fprintf(stderr, "Error: No se pudieron inicializar los recursos del juego.\n");
        // La limpieza ya se hizo dentro de init_game_resources en caso de fallo
        return EXIT_FAILURE;
    }
    
    printf("Recursos del juego inicializados correctamente.\n");

    // TODO: Fase 2 - Lanzar procesos hijos
    // TODO: Fase 3 - Bucle principal del juego
    
    printf("Limpiando recursos...\n");
    cleanup_game_resources(&resources);
    printf("Recursos limpiados. Saliendo.\n");

    return EXIT_SUCCESS;
}
