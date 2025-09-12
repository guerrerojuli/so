#define _POSIX_C_SOURCE 200809L // para strdup
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include "shmADT.h"

struct ShmCDT
{
        char *name;
        size_t size;
        int fd;
        void *shmaddr;
};

ShmADT create_shm(const char *restrict name, size_t size, int open_flag, int mode, int prot)
{
        ShmADT new_shm = malloc(sizeof(struct ShmCDT));
        if (new_shm == NULL)
        {
                return NULL;
        }

        new_shm->fd = shm_open(name, open_flag, mode);
        if (new_shm->fd == -1)
        {
                free(new_shm);
                return NULL;
        }

        if (open_flag != O_RDONLY)
        {
                if (-1 == ftruncate(new_shm->fd, size))
                {
                        close(new_shm->fd);
                        shm_unlink(name);
                        free(new_shm);
                        return NULL;
                }
        }

        new_shm->shmaddr = mmap(NULL, size, prot, MAP_SHARED, new_shm->fd, 0);
        if (new_shm->shmaddr == MAP_FAILED)
        {
                close(new_shm->fd);
                shm_unlink(name);
                free(new_shm);
                return NULL;
        }

        new_shm->name = strdup(name);
        if (new_shm->name == NULL)
        {
                munmap(new_shm->shmaddr, size);
                close(new_shm->fd);
                shm_unlink(name);
                free(new_shm);
                return NULL;
        }

        new_shm->size = size;

        return new_shm;
}

int destroy_shm(ShmADT shm)
{
        if (shm == NULL)
        {
                errno = EINVAL;
                return -1;
        }

        int ret = 0;
        if (-1 == munmap(shm->shmaddr, shm->size))
        {
                ret = -1;
        }

        if (-1 == close(shm->fd))
        {
                ret = -1;
        }

        if (-1 == shm_unlink(shm->name))
        {
                ret = -1;
        }

        free(shm->name);
        free(shm);
        return ret;
}

ShmADT open_shm(const char *restrict name, size_t size, int open_flag, int mode, int prot)
{
        ShmADT opened_shm = malloc(sizeof(struct ShmCDT));
        if (opened_shm == NULL)
        {
                return NULL;
        }

        opened_shm->fd = shm_open(name, open_flag, mode);
        if (opened_shm->fd == -1)
        {
                free(opened_shm);
                return NULL;
        }

        opened_shm->shmaddr = mmap(NULL, size, prot, MAP_SHARED, opened_shm->fd, 0);
        if (opened_shm->shmaddr == MAP_FAILED)
        {
                close(opened_shm->fd);
                free(opened_shm);
                return NULL;
        }

        opened_shm->name = strdup(name);
        if (opened_shm->name == NULL)
        {
                munmap(opened_shm->shmaddr, size);
                close(opened_shm->fd);
                free(opened_shm);
                return NULL;
        }

        opened_shm->size = size;

        return opened_shm;
}

int close_shm(ShmADT shm)
{
        if (shm == NULL)
        {
                errno = EINVAL;
                return -1;
        }

        int ret = 0;
        if (-1 == munmap(shm->shmaddr, shm->size))
        {
                ret = -1;
        }

        if (-1 == close(shm->fd))
        {
                ret = -1;
        }

        free(shm->name);
        free(shm);
        return ret;
}

void *get_shm_pointer(ShmADT shm)
{
        if (shm == NULL)
        {
                errno = EINVAL;
                return NULL;
        }

        return shm->shmaddr;
}