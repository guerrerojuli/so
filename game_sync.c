 #include <semaphore.h>
 
typedef struct {
     sem_t view_update_ready; // El máster le indica a la vista que hay cambios por imprimir
     sem_t view_print_done; // La vista le indica al máster que terminó de imprimir
     sem_t master_starvation_guard; // Mutex para evitar inanición del máster al acceder al estado
     sem_t state_mutex; // Mutex para el estado del juego
     sem_t readers_count_mutex; // Mutex para la siguiente variable
     unsigned int readers_count; // Cantidad de jugadores leyendo el estado
     sem_t player_can_move[9]; // Le indican a cada jugador que puede enviar 1 movimiento
 } GameSync;