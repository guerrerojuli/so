#include "game_sync.h"

void game_sync_reader_enter(GameSync *s)
{
    /* Pass through turnstile to avoid starving writers (master) */
    sem_wait(&s->master_starvation_guard);
    sem_post(&s->master_starvation_guard);
    /* Reader side of RW-lock */
    sem_wait(&s->readers_count_mutex);
    s->readers_count++;
    if (s->readers_count == 1)
        sem_wait(&s->state_mutex);
    sem_post(&s->readers_count_mutex);
}

void game_sync_reader_exit(GameSync *s)
{
    sem_wait(&s->readers_count_mutex);
    s->readers_count--;
    if (s->readers_count == 0)
        sem_post(&s->state_mutex);
    sem_post(&s->readers_count_mutex);
}

void game_sync_writer_enter(GameSync *s)
{
    /* El escritor (master) indica su intención de escribir.
       Esto bloquea a nuevos lectores que lleguen después de él. */
    sem_wait(&s->master_starvation_guard);
    
    /* Pide acceso exclusivo al estado del juego.
       Este es el mismo semáforo que bloquea el primer lector. */
    sem_wait(&s->state_mutex);

    /* Una vez que tiene el acceso, permite que otros procesos
       puedan pasar por el "turnstile" (la guardia). */
    sem_post(&s->master_starvation_guard);
}

void game_sync_writer_exit(GameSync *s)
{
    /* El escritor libera el acceso exclusivo al estado del juego. */
    sem_post(&s->state_mutex);
}

void game_sync_run_as_reader(GameSync *sync, GameState *state, void (*callback)(GameState *state, void *context), void *context)
{
    game_sync_reader_enter(sync);
    callback(state, context);
    game_sync_reader_exit(sync);
}

void game_sync_run_as_writer(GameSync *sync, GameState *state, void (*callback)(GameState *state, void *context), void *context)
{
    game_sync_writer_enter(sync);
    callback(state, context);
    game_sync_writer_exit(sync);
}