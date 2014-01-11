#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <pthread.h>
#include "iomux.h"

#define IOMTEE_MAX_FILEDESCRIPTORS 256

typedef struct {
    int fd;
    int rofx;
} iomtee_fd_t;

struct __iomtee_s {
    iomux_t *iomux;
    int num_fds;
    iomtee_fd_t fds[IOMTEE_MAX_FILEDESCRIPTORS];
    char buf[65535];
    int wofx;
    int blen;
    int pipe[2];
    pthread_t th;
};

static int iomtee_write_buffer(iomtee_t *tee, void *data, int len)
{
    int min_rofx = tee->wofx;
    int i;
    // XXX - inefficient
    for (i = 0; i < tee->num_fds; i++) {
        if (tee->fds[i].fd >= 0 && tee->fds[i].rofx < min_rofx)
            min_rofx = tee->fds[i].rofx;
    }

    int free_space = abs(tee->wofx - min_rofx)+1;
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

static int iomtee_read_buffer(iomtee_t *tee, int fd_pos, void *out, int len)
{
    int *rofx = &tee->fds[fd_pos].rofx;
    int free_space = abs(tee->wofx - *rofx);
    int read_len = free_space < len ? free_space : len;
    if (tee->wofx > *rofx) {
        memcpy(out, &tee->buf[*rofx], read_len);
        *rofx += len;
    } else {
        int diff = sizeof(tee->buf) - *rofx;
        memcpy(out, &tee->buf[*rofx], diff);
        memcpy(out+diff, &tee->buf[0], read_len - diff);
        *rofx = read_len - diff;
    }
    return read_len;
}

static void iomtee_output(iomux_t *iomux, int fd, void *priv)
{
    iomtee_t *tee = (iomtee_t *)priv;
    static char buf[65535];
    iomtee_fd_t *tee_fd = NULL;

    int i;
    for (i = 0; i < tee->num_fds; i++) {
        if (tee->fds[i].fd == fd) {
            tee_fd = &tee->fds[i];
            break;
        }
    }

    if (!tee_fd) {
        // TODO - Error Messages
        return;
    }
    
    int write_buffer_size = iomux_write_buffer(iomux, fd);
    int rb = iomtee_read_buffer(tee, i, buf, write_buffer_size);
    int wb = iomux_write(iomux, fd, buf, rb);
    if (wb == rb) {
        iomux_callbacks_t *cbs = iomux_callbacks(iomux, fd);
        cbs->mux_output = NULL;
    }
}

static void iomtee_input(iomux_t *iomux, int fd, void *data, int len, void *priv)
{
    iomtee_t *tee = (iomtee_t *)priv;
    int min_write = len;
    int i;
    for (i = 0; i < tee->num_fds; i++) {
        iomtee_fd_t *tee_fd = &tee->fds[i];
        if (tee_fd->fd == -1)
            continue; // skip closed receivers
        int wb = iomux_write(iomux, tee_fd->fd, data, len);
        if (wb < len) {
            iomux_callbacks_t *cbs = iomux_callbacks(iomux, fd);
            if (cbs)
                cbs->mux_output = iomtee_output;
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
}

static void iomtee_eof(iomux_t *iomux, int fd, void *priv)
{
    iomtee_t *tee = (iomtee_t *)priv;

    if (fd == tee->pipe[0] || fd == tee->pipe[1]) {
        iomux_end_loop(iomux);
        return;
    }

    int i, found = 0, more = 0;
    for (i = 0; i < tee->num_fds; i++) {
        if (tee->fds[i].fd == fd) {
            tee->fds[i].fd = -1;
            found++;
        }
        more++;
        if (found)
            break;
    }

    if (!more)
        iomux_end_loop(iomux);

}

static void *tee_run(void *arg) {
    struct timeval tv = { 0, 50000 };
    iomtee_t *tee = (iomtee_t *)arg;
    for (;;) {
        iomux_run(tee->iomux, &tv);
        pthread_testcancel();
    }
    return NULL;
}

iomtee_t *iomtee_open(int *vfd, int num_fds, ...)
{
    iomtee_t *tee = calloc(1, sizeof(iomtee_t));
    tee->iomux = iomux_create();
    int rc = pipe(tee->pipe);
    if (rc != 0) {
        fprintf(stderr, "Can't create pipe : %s\n", strerror(errno));
        iomux_destroy(tee->iomux);
        free(tee);
    }

    iomux_callbacks_t icbs = {
        .mux_input = iomtee_input,
        .mux_eof = iomtee_eof,
        .priv = tee
    };

    iomux_add(tee->iomux, tee->pipe[0], &icbs);
    
    iomux_callbacks_t ocbs = {
        .mux_eof = iomtee_eof,
        .priv = tee
    };

    int i;
    tee->num_fds = num_fds;
    va_list ap;
    va_start(ap, num_fds);
    for (i = 0; i < num_fds; i++) {
        tee->fds[i].fd = va_arg(ap, int);
        iomux_add(tee->iomux, tee->fds[i].fd, &ocbs);
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

void iomtee_close(iomtee_t *tee)
{
    pthread_cancel(tee->th);
    pthread_join(tee->th, NULL);
    close(tee->pipe[0]);
    close(tee->pipe[1]);
    iomux_destroy(tee->iomux);
    free(tee);
}
