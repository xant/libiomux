#ifndef NO_PTHREAD

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <pthread.h>

#include "bsd_queue.h"
#include "iomux.h"

typedef struct _iomtee_fd_s {
    int fd;
    int rofx;
    TAILQ_ENTRY(_iomtee_fd_s) next;
} iomtee_fd_t;

struct _iomtee_s {
    iomux_t *iomux;
    TAILQ_HEAD(, _iomtee_fd_s) fds;
    char buf[65535];
    int wofx;
    int blen;
    int pipe[2];
    pthread_t th;
    int leave;
};

static int iomtee_write_buffer(iomtee_t *tee, void *data, int len)
{
    int min_rofx = tee->wofx;
    // XXX - inefficient
    iomtee_fd_t *tfd, *tmp;
    TAILQ_FOREACH_SAFE(tfd, &tee->fds, next, tmp) {
        if (tfd->fd >= 0 && tfd->rofx < min_rofx)
            min_rofx = tfd->rofx;
    }

    int free_space = abs(tee->wofx - min_rofx);
    int write_len = free_space > len ? len : free_space;
    if (tee->wofx <= min_rofx) {
        memcpy(&tee->buf[tee->wofx], data, write_len);
        tee->wofx += write_len;
    } else {
        int diff = sizeof(tee->buf) - tee->wofx;
        memcpy(&tee->buf[tee->wofx], data, diff);
        memcpy(&tee->buf[0], data + diff, write_len - diff);
        tee->wofx = write_len - diff;
    }
    return write_len;
}

static int
iomtee_read_buffer(iomtee_t *tee, iomtee_fd_t *tfd, void *out, int len)
{
    int free_space = abs(tee->wofx - tfd->rofx);
    int read_len = free_space < len ? free_space : len;
    if (tee->wofx > tfd->rofx) {
        memcpy(out, &tee->buf[tfd->rofx], read_len);
        tfd->rofx += read_len;
    } else {
        int diff = sizeof(tee->buf) - tfd->rofx;
        memcpy(out, &tee->buf[tfd->rofx], diff);
        memcpy(out+diff, &tee->buf[0], read_len - diff);
        tfd->rofx = read_len - diff;
    }
    return read_len;
}

static int
iomtee_output(iomux_t *iomux,
              int fd,
              unsigned char **out,
              int *len,
              void *priv)
{
    iomtee_t *tee = (iomtee_t *)priv;
    iomtee_fd_t *tfd = NULL;
    iomtee_fd_t *iter, *tmp;

    TAILQ_FOREACH_SAFE(iter, &tee->fds, next, tmp) {
        if (iter->fd == fd) {
            tfd = iter;
            break;
        }
    }

    if (!tfd) {
        // TODO - Error Messages
        *len = 0;
        return IOMUX_OUTPUT_MODE_NONE;
    }
    
    *len = abs(tee->wofx - tfd->rofx);
    *out = malloc(*len);
    int rb = iomtee_read_buffer(tee, tfd, *out, *len);
    if (rb != *len)
        *len = rb;
    return IOMUX_OUTPUT_MODE_FREE;
}

static int
iomtee_input(iomux_t *iomux, int fd, unsigned char *data, int len, void *priv)
{
    iomtee_t *tee = (iomtee_t *)priv;
    int min_write = len;
    iomtee_fd_t *tee_fd, *tmp;
    TAILQ_FOREACH_SAFE(tee_fd, &tee->fds, next, tmp) {
        if (tee_fd->fd == -1)
            continue; // skip closed receivers
        int wb = iomux_write(iomux, tee_fd->fd, data, len, IOMUX_OUTPUT_MODE_COPY);
        if (wb < len) {
            if (wb < min_write)
                min_write = wb;
        }
    }
    
    if (min_write < len) {
        int wb = iomtee_write_buffer(tee, data + min_write, len - min_write);
        if (wb < len - min_write) {
            // Buffer Underrun
            // TODO - Error Messages
        }
    }
    return len;
}

static void
iomtee_eof(iomux_t *iomux, int fd, void *priv)
{
    iomtee_t *tee = (iomtee_t *)priv;

    if (fd == tee->pipe[0] || fd == tee->pipe[1]) {
        iomux_end_loop(iomux);
        return;
    }


    iomtee_fd_t *tfd, *tmp;
    TAILQ_FOREACH_SAFE(tfd, &tee->fds, next, tmp) {
        if (tfd->fd == fd) {
            tfd->fd = -1;
            TAILQ_REMOVE(&tee->fds, tfd, next);
            free(tfd);
            break;
        }
    }

    if (!TAILQ_FIRST(&tee->fds))
        iomux_end_loop(iomux);


}

static void *tee_run(void *arg) {
    struct timeval tv = { 0, 50000 };
    iomtee_t *tee = (iomtee_t *)arg;
    while (!__sync_fetch_and_add(&tee->leave, 0)) {
        iomux_run(tee->iomux, &tv);
    }
    return NULL;
}

iomtee_t *iomtee_open(int *vfd, int num_fds, ...)
{
    iomtee_t *tee = calloc(1, sizeof(iomtee_t));
    tee->iomux = iomux_create(0, 1);
    int rc = pipe(tee->pipe);
    if (rc != 0) {
        fprintf(stderr, "Can't create pipe : %s\n", strerror(errno));
        iomux_destroy(tee->iomux);
        free(tee);
        return NULL;
    }

    iomux_callbacks_t icbs = {
        .mux_input = iomtee_input,
        .mux_output = iomtee_output,
        .mux_eof = iomtee_eof,
        .priv = tee
    };

    iomux_add(tee->iomux, tee->pipe[0], &icbs);
    
    iomux_callbacks_t ocbs = {
        .mux_eof = iomtee_eof,
        .priv = tee
    };

    TAILQ_INIT(&tee->fds);

    int i;
    va_list ap;
    va_start(ap, num_fds);
    for (i = 0; i < num_fds; i++) {
        int fd = va_arg(ap, int);
        iomtee_fd_t *tfd = calloc(1, sizeof(iomtee_fd_t));
        tfd->fd = fd;
        TAILQ_INSERT_TAIL(&tee->fds, tfd, next);
        iomux_add(tee->iomux, tfd->fd, &ocbs);
    }
    va_end(ap);

    if (pthread_create(&tee->th, NULL, tee_run, tee) != 0) {
        // TODO - Error Messages
        close(tee->pipe[0]);
        close(tee->pipe[1]);
        iomux_destroy(tee->iomux);
        free(tee);
        return NULL;
    }

    if (vfd)
        *vfd = tee->pipe[1];
    return tee;
}

int iomtee_fd(iomtee_t *tee)
{
    return tee->pipe[1];
}

void iomtee_add_fd(iomtee_t *tee, int fd)
{
    iomux_callbacks_t ocbs = {
        .mux_eof = iomtee_eof,
        .priv = tee
    };
    iomtee_fd_t *tfd = calloc(1, sizeof(iomtee_fd_t));
    tfd->fd = fd;
    TAILQ_INSERT_TAIL(&tee->fds, tfd, next);
    iomux_add(tee->iomux, tfd->fd, &ocbs);
}

void iomtee_remove_fd(iomtee_t *tee, int fd)
{
    iomtee_fd_t *tfd, *tmp;
    TAILQ_FOREACH_SAFE(tfd, &tee->fds, next, tmp) {
        if (tfd->fd == fd) {
            TAILQ_REMOVE(&tee->fds, tfd, next);
            iomux_remove(tee->iomux, fd);
            break;
        }
    }
}

void iomtee_close(iomtee_t *tee)
{
    (void)__sync_add_and_fetch(&tee->leave, 1);
    pthread_join(tee->th, NULL);
    close(tee->pipe[0]);
    close(tee->pipe[1]);
    iomux_destroy(tee->iomux);
    iomtee_fd_t *tfd, *tmp;
    TAILQ_FOREACH_SAFE(tfd, &tee->fds, next, tmp) {
        TAILQ_REMOVE(&tee->fds, tfd, next);
        free(tfd);
    }
    free(tee);
}

#endif
