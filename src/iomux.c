/**
 * \file
 *
 * \brief I/O multiplexer
 *
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdarg.h>

#if defined(HAVE_EPOLL)
#include <sys/epoll.h>
#elif defined(HAVE_KQUEUE)
#include <sys/event.h>
#endif

#define __USE_UNIX98
#include <pthread.h>

#include "bsd_queue.h"

#include "iomux.h"
#include "bh.h"

#define IOMUX_CONNECTIONS_MAX_DEFAULT (1<<13) // defaults to 8192
// 1MB default connection bufsize
#define IOMUX_CONNECTION_BUFSIZE_DEFAULT (1<<13) // defaults to 8192
#define IOMUX_CONNECTION_SERVER (1)

#define MUTEX_LOCK(__iom) if (__iom->lock) pthread_mutex_lock(__iom->lock);
#define MUTEX_UNLOCK(__iom) if (__iom->lock) pthread_mutex_unlock(__iom->lock);

void iomux_run(iomux_t *iomux, struct timeval *tv_default);

int iomux_hangup = 0;

//! \brief iomux connection strucure
typedef struct __iomux_connection {
    uint32_t flags;
    iomux_callbacks_t cbs;
    unsigned char *inbuf;
    unsigned char *outbuf;
    int bufsize;
    int eof;
    int inlen;
    int outlen;
    iomux_timeout_id_t timeout_id;
#if defined(HAVE_KQUEUE)
    int16_t kfilters[2];
    struct kevent event[2];
#endif
} iomux_connection_t;

//! \brief iomux timeout structure
typedef struct __iomux_timeout {
    iomux_timeout_id_t id;
    struct timeval expire_time;
    TAILQ_ENTRY(__iomux_timeout) timeout_list;
    void (*cb)(iomux_t *iomux, void *priv);
    void *priv;
} iomux_timeout_t;

//! \brief IOMUX base structure
struct __iomux {
    iomux_connection_t **connections;
    int maxfd;
    int minfd;
    int bufsize;
    int maxconnections;
    int leave;

    iomux_cb_t loop_next_cb;
    void *loop_next_priv;
    iomux_cb_t loop_end_cb;
    void *loop_end_priv;
    iomux_cb_t hangup_cb;
    void *hangup_priv;

    char error[2048];

    struct timeval last_timeout_check;

#if defined(HAVE_EPOLL)
    struct epoll_event *events;
    int efd; 
#elif defined(HAVE_KQUEUE)
    struct kevent *events;
    int kfd;
#endif
    bh_t *timeouts;
    int last_timeout_id;

    int num_fds;

    pthread_mutex_t *lock;
};

static void set_error(iomux_t *iomux, char *fmt, ...) {
    va_list arg;
    va_start(arg, fmt);
    vsnprintf(iomux->error, sizeof(iomux->error), fmt, arg);
    va_end(arg);
}


#define IOMUX_FLUSH_MAXRETRIES 5    //!< Maximum number of iterations for flushing the output buffer

static void iomux_handle_timeout(iomux_t *iomux, void *priv);

iomux_t *
iomux_create(int max_connections, int bufsize, int threadsafe)
{
    iomux_t *iomux = (iomux_t *)calloc(1, sizeof(iomux_t));

    if (!iomux) {
        fprintf(stderr, "Error allocating iomux");
        return NULL;
    }

    iomux->bufsize = (bufsize > 0) ? bufsize : IOMUX_CONNECTION_BUFSIZE_DEFAULT;
    iomux->maxconnections = (max_connections > 0) ? max_connections : IOMUX_CONNECTIONS_MAX_DEFAULT;

#if defined(HAVE_EPOLL)
    iomux->efd = epoll_create1(0);
    if (iomux->efd == -1) {
        fprintf(stderr, "Errors creating the epoll descriptor : %s\n", strerror(errno));
        free(iomux);
        return NULL;
    }
    iomux->events = calloc(1, sizeof(struct epoll_event) * iomux->maxconnections);
#elif defined(HAVE_KQUEUE)
    iomux->kfd = kqueue();
    if (iomux->kfd == -1) {
        fprintf(stderr, "Errors creating the kqueue descriptor : %s\n", strerror(errno));
        free(iomux);
        return NULL;
    }
    iomux->events = calloc(1, sizeof(struct kevent) * (iomux->maxconnections * 2));
#endif

    iomux->connections = calloc(1, sizeof(iomux_connection_t *) * iomux->maxconnections);

    iomux->timeouts = bh_create();

    if (threadsafe) {
        iomux->lock = malloc(sizeof(pthread_mutex_t));
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(iomux->lock, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    iomux->last_timeout_id = 0;

    return iomux;
}

int
iomux_add(iomux_t *iomux, int fd, iomux_callbacks_t *cbs)
{
    iomux_connection_t *connection = NULL;

    if (fd >= maxconnections)
        return 0;

    MUTEX_LOCK(iomux);

    if (fd < 0) {
        set_error(iomux, "fd %d is invalid", fd);
        MUTEX_UNLOCK(iomux);
        return 0;
    } else if (fd >= iomux->maxconnections) {
        set_error(iomux, "fd %d exceeds max fd %d", fd, iomux->maxconnections);
        MUTEX_UNLOCK(iomux);
        return 0;
    }

    if (iomux->connections[fd]) {
        set_error(iomux, "filedescriptor %d already added", fd);
        MUTEX_UNLOCK(iomux);
        return 0;
    }
    if (!cbs) {
        set_error(iomux, "no callbacks have been specified, skipping filedescriptor %d", fd);
        MUTEX_UNLOCK(iomux);
        return 0;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);
    connection = (iomux_connection_t *)calloc(1, sizeof(iomux_connection_t));
    if (connection) {

#if defined(HAVE_EPOLL)
        struct epoll_event event;
        bzero(&event, sizeof(event));
        event.data.fd = fd;
        event.events = EPOLLIN;
        if (cbs->mux_output)
            event.events |= EPOLLOUT;
        int rc = epoll_ctl(iomux->efd, EPOLL_CTL_ADD, fd, &event);
        if (rc == -1) {
            fprintf(stderr, "Errors adding fd %d to epoll instance %d : %s\n", 
                    fd, iomux->efd, strerror(errno));
            free(connection);
            MUTEX_UNLOCK(iomux);
            return 0;
        }

#elif defined(HAVE_KQUEUE)
        connection->kfilters[0] = EVFILT_READ;
        connection->kfilters[1] = EVFILT_WRITE;

        EV_SET(&connection->event[0], fd, connection->kfilters[0], EV_ADD | EV_ONESHOT, 0, 0, 0);
        EV_SET(&connection->event[1], fd, connection->kfilters[1], EV_DELETE | EV_ONESHOT, 0, 0, 0);
#endif

        if (fd > iomux->maxfd)
            iomux->maxfd = fd;
        if (fd < iomux->minfd)
            iomux->minfd = fd;
        
        memcpy(&connection->cbs, cbs, sizeof(connection->cbs));

        connection->inbuf = malloc(iomux->bufsize);
        connection->outbuf = malloc(iomux->bufsize);
        connection->bufsize = iomux->bufsize;

        iomux->connections[fd] = connection;
        iomux->num_fds++;
        while (!iomux->connections[iomux->minfd] && iomux->minfd != iomux->maxfd)
            iomux->minfd++;

        MUTEX_UNLOCK(iomux);
        return 1;
    }
    MUTEX_UNLOCK(iomux);
    return 0;
}

int
iomux_remove(iomux_t *iomux, int fd)
{
    MUTEX_LOCK(iomux);

    if (!iomux->connections[fd]) {
        MUTEX_UNLOCK(iomux);
        return 0;
    }
    iomux_unschedule(iomux, iomux->connections[fd]->timeout_id);

#if defined(HAVE_EPOLL)
    struct epoll_event event;
    bzero(&event, sizeof(event));
    event.data.fd = fd;

    // NOTE: events might be NULL but on linux kernels < 2.6.9 
    //       it was required to be non-NULL even if ignored
    event.events = EPOLLIN | EPOLLOUT;

    // NOTE: if the fd has been already closed epoll_ctl would return an error
    epoll_ctl(iomux->efd, EPOLL_CTL_DEL, fd, &event);
#elif defined(HAVE_KQUEUE)
    int i;
    for (i = 0; i < 2; i++) {
        EV_SET(&iomux->connections[fd]->event[i], fd, iomux->connections[fd]->kfilters[i], EV_DELETE | EV_ONESHOT, 0, 0, 0);
    }
#endif
    free(iomux->connections[fd]->inbuf);
    free(iomux->connections[fd]->outbuf);
    free(iomux->connections[fd]);
    iomux->connections[fd] = NULL;
    iomux->num_fds--;

    if (iomux->maxfd == fd)
        while (iomux->maxfd > 0 && !iomux->connections[iomux->maxfd])
            iomux->maxfd--;

    if (iomux->minfd == fd) {
        if (iomux->minfd < iomux->maxfd)
            while (iomux->minfd != iomux->maxfd && !iomux->connections[iomux->minfd])
                iomux->minfd++;
        else
            iomux->minfd = iomux->maxfd;
    }
    MUTEX_UNLOCK(iomux);
    return 1;
}

iomux_timeout_id_t
iomux_schedule(iomux_t *iomux, struct timeval *tv, iomux_cb_t cb, void *priv)
{
    iomux_timeout_t *timeout;

    if (!tv || !cb)
        return 0;

    MUTEX_LOCK(iomux);

    struct timeval now;
    gettimeofday(&now, NULL);
    if (iomux->last_timeout_check.tv_sec == 0)
        memcpy(&iomux->last_timeout_check, &now, sizeof(struct timeval));

    timeout = (iomux_timeout_t *)calloc(1, sizeof(iomux_timeout_t));
    timeradd(&now, tv, &timeout->expire_time);
    timeout->cb = cb;
    timeout->priv = priv;
    uint64_t expire =  (timeout->expire_time.tv_sec * 1000) + (timeout->expire_time.tv_usec/1000);
    timeout->id = (expire << 8) | (++iomux->last_timeout_id % 256);

    // keep the list sorted in ascending order
    bh_insert(iomux->timeouts, timeout->id, timeout, sizeof(iomux_timeout_t));
    MUTEX_UNLOCK(iomux);
    return timeout->id;
}

iomux_timeout_id_t
iomux_reschedule(iomux_t *iomux, iomux_timeout_id_t id, struct timeval *tv, iomux_cb_t cb, void *priv)
{
    iomux_unschedule(iomux, id);
    return iomux_schedule(iomux, tv, cb, priv);
}

typedef struct {
    iomux_t *iomux;
    iomux_cb_t cb;
    void *priv;
    int count;
} iomux_binheap_iterator_arg_t;

static int
iomux_binheap_iterator_callback(bh_t *bh, uint64_t key, void *value, size_t vlen, void *priv)
{
    iomux_binheap_iterator_arg_t *arg = (iomux_binheap_iterator_arg_t *)priv;
    iomux_timeout_t *timeout = (iomux_timeout_t *)value;

    if (arg->cb == timeout->cb && arg->priv == timeout->priv) {
        free(timeout);
        arg->count++;
        return -1;
    }
    return 1;
}

int
iomux_unschedule_all(iomux_t *iomux, iomux_cb_t cb, void *priv)
{
    MUTEX_LOCK(iomux);

    iomux_binheap_iterator_arg_t arg = { iomux, cb, priv, 0 };
    bh_foreach(iomux->timeouts, iomux_binheap_iterator_callback, &arg);

    MUTEX_UNLOCK(iomux);

    return arg.count;
}

int
iomux_unschedule(iomux_t *iomux, iomux_timeout_id_t id)
{
    if (!id)
        return 0;

    MUTEX_LOCK(iomux);
    void *timeout_ptr = NULL;
    if (bh_delete(iomux->timeouts, id, &timeout_ptr, NULL) != 0) {
        MUTEX_UNLOCK(iomux);
        return 0;
    }

    if (timeout_ptr)
        free(timeout_ptr);

    MUTEX_UNLOCK(iomux);
    return 1;
}

static void
iomux_handle_timeout(iomux_t *iomux, void *priv)
{
    int fd = (long int)priv;

    if (iomux->connections[fd]) {
        iomux_callbacks_t *cbs = &iomux->connections[fd]->cbs;
        if (cbs->mux_timeout) {
            cbs->mux_timeout(iomux, fd, cbs->priv);
        }
    }
}

iomux_timeout_id_t
iomux_set_timeout(iomux_t *iomux, int fd, struct timeval *tv)
{
    MUTEX_LOCK(iomux);
    if (!iomux->connections[fd]) {
        MUTEX_UNLOCK(iomux);
        return 0;
    }

    if (!tv) {
        (void) iomux_unschedule(iomux, iomux->connections[fd]->timeout_id);
        MUTEX_UNLOCK(iomux);
        return 0;
    }

    MUTEX_UNLOCK(iomux);
    return iomux_reschedule(iomux, iomux->connections[fd]->timeout_id, tv, iomux_handle_timeout, (void *)(long int)fd);
}

int
iomux_listen(iomux_t *iomux, int fd)
{
    MUTEX_LOCK(iomux);
    if (!iomux->connections[fd]) {
        set_error(iomux, "%s: No connections for fd %d", __FUNCTION__, fd);
        MUTEX_UNLOCK(iomux);
        return 0;
    }
    assert(iomux->connections[fd]->cbs.mux_connection);

    if (listen(fd, -1) != 0) {
        set_error(iomux, "%s: Error listening on fd %d: %s", __FUNCTION__, fd, strerror(errno));
        MUTEX_UNLOCK(iomux);
        return 0;
    }

    iomux->connections[fd]->flags = iomux->connections[fd]->flags | IOMUX_CONNECTION_SERVER;

    MUTEX_UNLOCK(iomux);
    return 1;
}

void
iomux_loop_next_cb(iomux_t *iomux, iomux_cb_t cb, void *priv)
{
    iomux->loop_next_cb = cb;
    iomux->loop_next_priv = priv;
}

void
iomux_loop_end_cb(iomux_t *iomux, iomux_cb_t cb, void *priv)
{
    iomux->loop_end_cb = cb;
    iomux->loop_end_priv = priv;
}

void
iomux_hangup_cb(iomux_t *iomux, iomux_cb_t cb, void *priv)
{
    iomux->hangup_cb = cb;
    iomux->hangup_priv = priv;
}

// NOTE - this MUST be called while the lock is NOT retained
static void
iomux_accept_connections_fd(iomux_t *iomux,
                            int fd,
                            iomux_connection_callback_t mux_connection,
                            void *priv)
{
    int newfd;
    struct sockaddr_in peer;
    socklen_t socklen = sizeof(struct sockaddr);
    // if it is, accept all pending connections and add them to the mux
    while ((newfd = accept(fd, (struct sockaddr *)&peer, &socklen)) >= 0) {
        if (mux_connection)
            mux_connection(iomux, newfd, priv);
    }
}

static void 
iomux_read_fd(iomux_t *iomux, int fd, iomux_input_callback_t mux_input, void *priv)
{
    MUTEX_LOCK(iomux);
    iomux_connection_t *conn = iomux->connections[fd];

    if (conn->inlen >= conn->bufsize) {
        MUTEX_UNLOCK(iomux);
        return;
    }

    int rb = read(fd, conn->inbuf + conn->inlen, conn->bufsize - conn->inlen);

    if (rb == -1) {
        if (errno != EINTR && errno != EAGAIN) {
            // don't output warnings if the filedescriptor has been closed
            // without informing the iomux or if the connection has been
            // dropped by the peer
            if (errno != EBADF && errno != ECONNRESET)
                fprintf(stderr, "read on fd %d failed: %s\n", fd, strerror(errno));
            iomux_close(iomux, fd);
        }
    } else if (rb == 0) {
         iomux_close(iomux, fd);
    } else {
        conn->inlen += rb;
         if (mux_input) {
             int len = conn->inlen;
             unsigned char *buf = malloc(len);
             memcpy(buf, conn->inbuf, len);
             int mb = mux_input(iomux, fd, buf, len, priv);
             if (iomux->connections[fd] == conn && iomux->connections[fd]->inlen == conn->inlen)
             {
                 if (mb == conn->inlen) {
                     conn->inlen = 0;
                 } else if (mb) {
                     memcpy(conn->inbuf, buf + mb, len - mb);
                     conn->inlen -= mb;
                 }
             }
             free(buf);
         }
    }
    MUTEX_UNLOCK(iomux);
}

// NOTE - this MUST be called while the lock is NOT retained
static void
iomux_write_fd(iomux_t *iomux, int fd, void *priv)
{
    MUTEX_LOCK(iomux);

    if (!iomux->connections[fd] || !iomux->connections[fd]->outlen) {
#if defined(HAVE_EPOLL)
            MUTEX_UNLOCK(iomux);
            // let's unregister this fd from EPOLLOUT events (seems nothing needs to be sent anymore)
            struct epoll_event event;
            bzero(&event, sizeof(event));
            event.data.fd = fd;
            event.events = EPOLLIN;

            int rc = epoll_ctl(iomux->efd, EPOLL_CTL_MOD, fd, &event);
            if (rc == -1) {
                fprintf(stderr, "Errors modifying fd %d on epoll instance %d : %s\n", 
                        fd, iomux->efd, strerror(errno));
            }
#elif defined(HAVE_KQUEUE)
        EV_SET(&iomux->connections[fd]->event[1], fd, iomux->connections[fd]->kfilters[1], EV_DELETE | EV_ONESHOT, 0, 0, 0);
        MUTEX_UNLOCK(iomux);
#endif
        return; 
    }

    char *outbuf = (char *)iomux->connections[fd]->outbuf;
    int outlen = iomux->connections[fd]->outlen;

    MUTEX_UNLOCK(iomux);

    int wb = write(fd, outbuf, outlen);
    if (wb <= 0) {
        if (errno != EINTR && errno != EAGAIN) {
            fprintf(stderr, "write on fd %d failed: %s\n", fd, strerror(errno));
            iomux_close(iomux, fd);
        }
    } else {
        MUTEX_LOCK(iomux);
        iomux->connections[fd]->outlen -= wb;
        if (iomux->connections[fd]->outlen) { // shift data if we didn't write it all at once
            memmove(iomux->connections[fd]->outbuf, &iomux->connections[fd]->outbuf[wb], iomux->connections[fd]->outlen);
        } else {
#if defined(HAVE_EPOLL)
            // let's unregister this fd from EPOLLOUT events (seems nothing needs to be sent anymore)
            struct epoll_event event = { 0 };
            event.data.fd = fd;
            event.events = EPOLLIN;

            int rc = epoll_ctl(iomux->efd, EPOLL_CTL_MOD, fd, &event);
            if (rc == -1) {
                fprintf(stderr, "Errors modifying fd %d on epoll instance %d : %s\n", 
                        fd, iomux->efd, strerror(errno));
            }
#elif defined(HAVE_KQUEUE)
            EV_SET(&iomux->connections[fd]->event[1], fd, iomux->connections[fd]->kfilters[1], EV_DELETE | EV_ONESHOT, 0, 0, 0);
#endif
        }
        MUTEX_UNLOCK(iomux);
    }
}

static struct timeval *
iomux_adjust_timeout(iomux_t *iomux, struct timeval *tv_default)
{
    static __thread struct timeval tv = { 0, 0 };
    iomux_timeout_t *timeout = NULL;
    void *timeout_ptr = NULL;
    uint64_t key = 0;
    bh_minimum(iomux->timeouts, &key, &timeout_ptr, NULL);
    timeout = (iomux_timeout_t *)timeout_ptr;

    if (!timeout)
        return tv_default;

    struct timeval wait_time = { 0, 0 };
    struct timeval now;
    gettimeofday(&now, NULL);
    timersub(&timeout->expire_time, &now, &wait_time);
    if (tv_default && timeout) {
        if (timercmp(&wait_time, tv_default, >))
            memcpy(&tv, tv_default, sizeof(struct timeval));
        else
            memcpy(&tv, &wait_time, sizeof(struct timeval));
    } else if (timeout) {
        memcpy(&tv, &wait_time, sizeof(struct timeval));
    } else if (tv_default) {
        memcpy(&tv, tv_default, sizeof(struct timeval));
    } else {
        return NULL;
    }
    return &tv;
}

// NOTE - this MUST be called while the lock is NOT retained
void
iomux_run_timeouts(iomux_t *iomux)
{
    iomux_timeout_t *timeout = NULL;

    MUTEX_LOCK(iomux);

    struct timeval now;
    gettimeofday(&now, NULL);

    void *timeout_ptr = NULL;
    while (bh_delete_minimum(iomux->timeouts, &timeout_ptr, NULL) == 0) {
        timeout = (iomux_timeout_t *)timeout_ptr;
        if (timercmp(&now, &timeout->expire_time, <)) {
            bh_insert(iomux->timeouts,
                      timeout->id,
                      timeout,
                      sizeof(iomux_timeout_t));
            break;
        }
        // run expired timeouts
        MUTEX_UNLOCK(iomux);
        timeout->cb(iomux, timeout->priv);
        MUTEX_LOCK(iomux);
        free(timeout);
    }

    MUTEX_UNLOCK(iomux);
}

void
iomux_loop(iomux_t *iomux, struct timeval *tv)
{
    struct timeval tv_default = { 0, 20000 };
    while (!iomux->leave) {
        if (iomux->loop_next_cb)
            iomux->loop_next_cb(iomux, iomux->loop_end_priv);

        iomux_run(iomux, tv ? tv : &tv_default);

        if (iomux_hangup && iomux->hangup_cb)
            iomux->hangup_cb(iomux, iomux->hangup_priv);
    }

    if (iomux->loop_end_cb)
        iomux->loop_end_cb(iomux, iomux->loop_end_priv);

    iomux->leave = 0;
}

void
iomux_end_loop(iomux_t *iomux)
{
    iomux->leave = 1;
}

int
iomux_write(iomux_t *iomux, int fd, const void *buf, int len)
{
    MUTEX_LOCK(iomux);

    if (!iomux->connections[fd])
        return 0;

    int free_space = iomux->connections[fd]->bufsize - iomux->connections[fd]->outlen;
    int wlen = (len > free_space)?free_space:len;

    if (wlen) {
#if defined(HAVE_EPOLL)
        struct epoll_event event;
        bzero(&event, sizeof(event));
        event.data.fd = fd;
        event.events = EPOLLIN | EPOLLOUT;

        int rc = epoll_ctl(iomux->efd, EPOLL_CTL_MOD, fd, &event);
        if (rc == -1) {
            MUTEX_UNLOCK(iomux);
            if (errno == EBADF) {
                iomux_close(iomux, fd);
            } else {
                fprintf(stderr, "Errors modifying fd %d to epoll instance %d : %s\n", 
                        fd, iomux->efd, strerror(errno));
            }
            return 0;
        }
#elif defined(HAVE_KQUEUE)
        EV_SET(&iomux->connections[fd]->event[1], fd, iomux->connections[fd]->kfilters[1], EV_ADD | EV_ONESHOT, 0, 0, 0);
#endif
        memcpy(iomux->connections[fd]->outbuf+iomux->connections[fd]->outlen,
                buf, wlen);
        iomux->connections[fd]->outlen += wlen;
    }

    MUTEX_UNLOCK(iomux);
    return wlen;
}

int
iomux_close(iomux_t *iomux, int fd)
{
    MUTEX_LOCK(iomux);

    iomux_connection_t *conn = iomux->connections[fd];
    if (!conn) { // fd is not registered within iomux
        MUTEX_UNLOCK(iomux);
        return 0;
    }

    if (fcntl(fd, F_GETFD, 0) != -1 && conn->outlen) { // there is pending data
        int retries = 0;
        while (conn->outlen && retries <= IOMUX_FLUSH_MAXRETRIES) {
            int wb = write(fd, conn->outbuf, conn->outlen);
            if (wb == -1) {
                if (errno == EINTR || errno == EAGAIN)
                    retries++;
                else
                    break;
            } else if (wb == 0) {
                fprintf(stderr, "%s: closing filedescriptor %d with %db pending data\n", __FUNCTION__, fd, conn->outlen);
                break;
            } else {
                conn->outlen -= wb;
            }
        }
    }

    void (*mux_eof)(iomux_t *, int, void *) = conn->cbs.mux_eof;
    void *priv = conn->cbs.priv;

    iomux_remove(iomux, fd);

    if(mux_eof)
        mux_eof(iomux, fd, priv);

    MUTEX_UNLOCK(iomux);

    return 1;
}

int
iomux_isempty(iomux_t *iomux)
{
    int fd;
    MUTEX_LOCK(iomux);
    for (fd = iomux->minfd; fd <= iomux->maxfd; fd++) {
        if (iomux->connections[fd]) {
            MUTEX_UNLOCK(iomux);
            return 0;
        }
    }
    MUTEX_UNLOCK(iomux);
    return 1;
}

int iomux_write_buffer(iomux_t *iomux, int fd)
{
    int len = 0;
    MUTEX_LOCK(iomux);
    if (iomux->connections[fd])
        len = iomux->connections[fd]->bufsize - iomux->connections[fd]->outlen;
    MUTEX_UNLOCK(iomux);
    return len;
}

void
iomux_destroy(iomux_t *iomux)
{
    iomux_clear(iomux);
#if defined(HAVE_EPOLL)
    close(iomux->efd);
#elif defined(HAVE_KQUEUE)
    close(iomux->kfd);
#endif
    if (iomux->lock) {
        pthread_mutex_destroy(iomux->lock);
        free(iomux->lock);
    }
    bh_destroy(iomux->timeouts);
    free(iomux->connections);
#if defined(HAVE_EPOLL) || defined(HAVE_KQUEUE)
    free(iomux->events);
#endif
    free(iomux);
}

void
iomux_clear(iomux_t *iomux)
{
    int fd;
    iomux_timeout_t *timeout = NULL;

    MUTEX_LOCK(iomux);
    for (fd = iomux->maxfd; fd >= iomux->minfd; fd--) {
        if (iomux->connections[fd]) {
            iomux_close(iomux, fd);
        }
    }

    void *timeout_ptr = NULL;
    while (bh_delete_minimum(iomux->timeouts, &timeout_ptr, NULL) == 0) {
        timeout = (iomux_timeout_t *)timeout_ptr;
        free(timeout);
    }
    MUTEX_UNLOCK(iomux);
}

iomux_callbacks_t *
iomux_callbacks(iomux_t *iomux, int fd)
{
    if (iomux->connections[fd])
        return &iomux->connections[fd]->cbs;
    return NULL;
}

int iomux_num_fds(iomux_t *iomux)
{
    int num_fds = 0;
    MUTEX_LOCK(iomux);
    num_fds = iomux->num_fds;
    MUTEX_UNLOCK(iomux);
    return num_fds;
}

#if defined(HAVE_KQUEUE)
void
iomux_run(iomux_t *iomux, struct timeval *tv_default)
{
    int i;
    struct timespec ts;

    MUTEX_LOCK(iomux);

    int n = 0;
    for (i = iomux->minfd; i <= iomux->maxfd; i++) {
        if (!iomux->connections[i])
            continue;
        if (iomux->connections[i]->cbs.mux_output) {
            int maxlen = iomux->connections[i]->bufsize - iomux->connections[i]->outlen;
            unsigned char data[maxlen];
            int len = maxlen;
            iomux->connections[i]->cbs.mux_output(iomux, i, data, &len,
                                                  iomux->connections[i]->cbs.priv);

            if (!iomux->connections[i])
                continue;

            if (len) {
                memmove(iomux->connections[i]->outbuf + iomux->connections[i]->outlen, data, len);
                iomux->connections[i]->outlen += len;
                memcpy(&iomux->events[n], &iomux->connections[i]->event, 2 * sizeof(struct kevent));
                n += 2;
            } else {
                memmove(&iomux->events[n], &iomux->connections[i]->event, sizeof(struct kevent));
                n++;
            }
        } else if (iomux->connections[i]->outlen) {
            memmove(&iomux->events[n], &iomux->connections[i]->event, 2 * sizeof(struct kevent));
            n += 2;
        } else {
            memmove(&iomux->events[n], &iomux->connections[i]->event, sizeof(struct kevent));
            n++;
        }
    }

    struct timeval *tv = iomux_adjust_timeout(iomux, tv_default);
    if (tv) {
        ts.tv_sec = tv->tv_sec;
        ts.tv_nsec = tv->tv_usec * 1000;
    }

    MUTEX_UNLOCK(iomux);
    int cnt = 0;
    if (n > 0) {
        cnt = kevent(iomux->kfd, iomux->events, n, iomux->events, iomux->maxconnections * 2, tv ? &ts : NULL);
    } else if (tv) {
        struct timeval tv_select = { 0, 0 };
        if (tv)
            memcpy(&tv_select, tv, sizeof(tv_select));
        // there are no filedescriptor in the mux, we just want to wait until next timeout expires
        select(0, NULL, NULL, NULL, tv ? &tv_select : NULL);
    }
    MUTEX_LOCK(iomux);

    if (cnt == -1) {
        fprintf(stderr, "kevent returned error : %s\n", strerror(errno));
    } else if (cnt > 0) {
        for (i = 0; i < cnt; i++) {
            struct kevent *event = &iomux->events[i];
            int fd = event->ident;
            iomux_connection_t *conn = iomux->connections[fd];
            if (!conn) {
                // TODO - Error Messages
                continue;
            }

            if (event->filter == EVFILT_READ) {
                if ((iomux->connections[fd]->flags&IOMUX_CONNECTION_SERVER) == (IOMUX_CONNECTION_SERVER) && event->data)
                {
                    while(event->data--) {
                        iomux_connection_callback_t mux_connection = iomux->connections[fd]->cbs.mux_connection;
                        void * priv = iomux->connections[fd]->cbs.priv;
                        iomux_accept_connections_fd(iomux, fd, mux_connection, priv);
                    }
                } else {
                    iomux_input_callback_t mux_input = iomux->connections[fd]->cbs.mux_input;
                    void * priv = iomux->connections[fd]->cbs.priv;
                    iomux_read_fd(iomux, fd, mux_input, priv);
                }
            }

            if (event->flags & EV_EOF) {
                iomux_close(iomux, fd);
                continue;
            }

            if (event->filter == EVFILT_WRITE) {
                void * priv = iomux->connections[fd]->cbs.priv;
                iomux_write_fd(iomux, fd, priv);
            }
        }
    }
    MUTEX_UNLOCK(iomux);
    iomux_run_timeouts(iomux);
}

#elif defined(HAVE_EPOLL)

void
iomux_run(iomux_t *iomux, struct timeval *tv_default)
{
    int fd;

    MUTEX_LOCK(iomux);

    int i;
    for (i = iomux->minfd; i <= iomux->maxfd; i++) {
        if (!iomux->connections[i])
            continue;
        if (iomux->connections[i]->cbs.mux_output) {
            int maxlen = iomux->connections[i]->bufsize - iomux->connections[i]->outlen;
            unsigned char data[maxlen];
            int len = maxlen;
            iomux->connections[i]->cbs.mux_output(iomux, i, data, &len,
                                                  iomux->connections[i]->cbs.priv);

            // NOTE: the output callback might have removed the fd from the mux
            if (!iomux->connections[i])
                continue;

            if (len) {
                memmove(iomux->connections[i]->outbuf + iomux->connections[i]->outlen, data, len);
                iomux->connections[i]->outlen += len;
            }
        } 
        if (iomux->connections[i]->outlen) {
            struct epoll_event event;
            bzero(&event, sizeof(event));
            event.data.fd = i;
            event.events = EPOLLIN | EPOLLOUT;

            int rc = epoll_ctl(iomux->efd, EPOLL_CTL_MOD, i, &event);
            if (rc == -1) {
                fprintf(stderr, "Errors modifying fd %d to epoll instance %d : %s\n", 
                        i, iomux->efd, strerror(errno));
            }
        } 
    }



    MUTEX_UNLOCK(iomux);

    // shrink the timeout if we have timers expiring earlier
    struct timeval *tv = iomux_adjust_timeout(iomux, tv_default);
    int epoll_waiting_time = tv ? ((tv->tv_sec * 1000) + (tv->tv_usec / 1000)) : -1;

    int num_fds = iomux->maxfd - iomux->minfd + 1;
    int n = 0;
    if (num_fds > 0) {
        n = epoll_wait(iomux->efd, iomux->events, num_fds, epoll_waiting_time);
    } else if (tv) {
        struct timeval tv_select = { 0, 0 };
        if (tv)
            memcpy(&tv_select, tv, sizeof(tv_select));
        // there are no filedescriptor in the mux, we just want to wait until next timeout expires
        select(0, NULL, NULL, NULL, tv ? &tv_select : NULL);
    }

    MUTEX_LOCK(iomux);

    for (i = 0; i < n; i++) {
        if ((iomux->events[i].events & EPOLLHUP))
        {
            iomux_close(iomux, iomux->events[i].data.fd);
            continue;
        } else if ((iomux->events[i].events & EPOLLERR)) {
            if (errno != EINPROGRESS) {
                fprintf (stderr, "epoll error on fd %d: %s\n",
                        iomux->events[i].data.fd, strerror(errno));
                iomux_close(iomux, iomux->events[i].data.fd);
            }
            continue;
        }

        fd  = iomux->events[i].data.fd;
        iomux_connection_t *conn = iomux->connections[fd];
        if (conn) {
            iomux_connection_callback_t mux_connection = conn->cbs.mux_connection;
            iomux_input_callback_t mux_input = conn->cbs.mux_input;
            void *priv = conn->cbs.priv;

            if ((conn->flags&IOMUX_CONNECTION_SERVER) == (IOMUX_CONNECTION_SERVER))
            {
                iomux_accept_connections_fd(iomux, fd, mux_connection, priv);
            } else {
                if (iomux->events[i].events & EPOLLIN || iomux->events[i].events & EPOLLPRI)
                {
                    iomux_read_fd(iomux, fd, mux_input, priv);
                }

                if (!iomux->connections[fd]) // connection has been closed/removed
                    continue;

                if (iomux->events[i].events & EPOLLOUT) {
                    iomux_write_fd(iomux, fd, priv);
                } 
            }
        }
    }
    MUTEX_UNLOCK(iomux);
    iomux_run_timeouts(iomux);
}

#else

void
iomux_run(iomux_t *iomux, struct timeval *tv_default)
{
    int fd;
    fd_set rin[iomux->maxconnections/1024], rout[iomux->maxconnections/1024];
    int maxfd = iomux->minfd;;

    memset(&rin, 0, sizeof(rin));
    memset(&rout, 0, sizeof(rout));

    MUTEX_LOCK(iomux);

    for (fd = iomux->minfd; fd <= iomux->maxfd; fd++) {
        if (iomux->connections[fd])  {
            iomux_connection_t *conn = iomux->connections[fd];
            // always register managed fds for reading (even if 
            // no mux_input callbacks is present) to detect EOF.
            if (conn->cbs.mux_output) {
                int maxsize = iomux->connections[fd]->bufsize - iomux->connections[fd]->outlen;
                unsigned char data[maxsize];
                int len = maxsize;
                iomux->connections[fd]->cbs.mux_output(iomux, fd, data, &len,
                                                      iomux->connections[fd]->cbs.priv);
                if (!iomux->connections[fd])
                    continue;

                if (len) {
                    memmove(iomux->connections[fd]->outbuf + iomux->connections[fd]->outlen, data, len);
                    iomux->connections[fd]->outlen += len;
                }
            }

            FD_SET(fd, &rin[0]);
            if (fd > maxfd)
                maxfd = fd;

            if (conn->outlen) {
                // output pending data
                FD_SET(fd, &rout[0]);
                if (fd > maxfd)
                    maxfd = fd;
            }
        }
    }

    // NOTE: some select() implementations update the timeout with
    //       the unslept time (possibly 0 if no events happened)
    //       and we don't want to modify the timeout provided to us
    //       as argument, hence we make a copy here and we pass the
    //       copy to select()
    struct timeval tv_select = { 0, 0 };

    // shrink the timeout if we have timers expiring earlier
    struct timeval *tv = iomux_adjust_timeout(iomux, tv_default);
    if (tv)
        memcpy(&tv_select, tv, sizeof(tv_select));

    MUTEX_UNLOCK(iomux);
    int rc = select(maxfd+1, &rin[0], &rout[0], NULL, tv ? &tv_select : NULL);
    MUTEX_LOCK(iomux);
    switch (rc) {
    case -1:
        if (errno == EINTR) {
            MUTEX_UNLOCK(iomux);
            return;
        }
        if (errno == EAGAIN) {
            MUTEX_UNLOCK(iomux);
            return;
        }
        else if (errno == EBADF) {
            // there is some bad filedescriptor among the managed ones
            // probably the user called close() on the filedescriptor
            // without informing the iomux
            for (fd = iomux->minfd; fd <= iomux->maxfd; fd++) {
                if (iomux->connections[fd] && fcntl(fd, F_GETFD, 0) == -1) {
                    iomux_close(iomux, fd);
                }
            }
        }
        set_error(iomux, "select(): %s", strerror(errno));
        break;
    case 0:
        break;
    default:
        for (fd = iomux->minfd; fd <= iomux->maxfd; fd++) {
            iomux_connection_t *conn = iomux->connections[fd];
            if (conn) {
                iomux_connection_callback_t mux_connection = conn->cbs.mux_connection;
                iomux_input_callback_t mux_input = conn->cbs.mux_input;
                void *priv = conn->cbs.priv;
                if (FD_ISSET(fd, &rin[0])) {
                    // check if this is a listening socket
                    if ((iomux->connections[fd]->flags&IOMUX_CONNECTION_SERVER) == (IOMUX_CONNECTION_SERVER)) {
                        iomux_accept_connections_fd(iomux, fd, mux_connection, priv);
                    } else {
                        iomux_read_fd(iomux, fd, mux_input, priv);
                    }
                }
                if (!iomux->connections[fd]) // connection has been closed/removed
                    continue;

                if (FD_ISSET(fd, &rout[0])) {
                    iomux_write_fd(iomux, fd, priv);
                }
            }
        }
    }

    MUTEX_UNLOCK(iomux);
    iomux_run_timeouts(iomux);
}

#endif


