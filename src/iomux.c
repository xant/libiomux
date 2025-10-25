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

#include <sys/resource.h>

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

#define MUTEX_LOCK(_iom) if (_iom->lock && __builtin_expect(pthread_mutex_lock((_iom->lock)) != 0, 0)) { abort(); }
#define MUTEX_UNLOCK(_iom) if (_iom->lock && __builtin_expect(pthread_mutex_unlock((_iom->lock)) != 0, 0)) { abort(); }

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

void iomux_run(iomux_t *iomux, struct timeval *tv_default);

int iomux_hangup = 0;

typedef struct _iomux_output_chunk_s {
    unsigned char *data;
    int len;
    int free;
    int offset;
    TAILQ_ENTRY(_iomux_output_chunk_s) next;
} iomux_output_chunk_t;

//! \brief iomux connection strucure
typedef struct _iomux_connection_s {
    int fd;
    uint32_t flags;
    iomux_callbacks_t cbs;
    unsigned char *inbuf;
    int output_len;
    TAILQ_HEAD(, _iomux_output_chunk_s) output_queue;

    int bufsize;
    int eof;
    int inlen;
    struct timeval expire_time;
    TAILQ_ENTRY(_iomux_connection_s) next;
#if defined(HAVE_KQUEUE)
    int16_t kfilters[2];
    struct kevent event[2];
#endif
} iomux_connection_t;

//! \brief iomux timeout structure
typedef struct _iomux_timeout {
    iomux_timeout_id_t id;
    struct timeval expire_time;
    void (*cb)(iomux_t *iomux, void *priv);
    void *priv;
    iomux_timeout_free_context_cb free_ctx_cb;
} iomux_timeout_t;

//! \brief IOMUX base structure
struct _iomux {
    iomux_connection_t **connections;
    TAILQ_HEAD(, _iomux_connection_s) connections_list;
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

    int emfile_fd;

    pthread_mutex_t *lock;
};

static void set_error(iomux_t *iomux, char *fmt, ...) {
    va_list arg;
    va_start(arg, fmt);
    vsnprintf(iomux->error, sizeof(iomux->error), fmt, arg);
    va_end(arg);
}


#define IOMUX_FLUSH_MAXRETRIES 5    //!< Maximum number of iterations for flushing the output buffer

static void
iomux_timeout_destroy(iomux_timeout_t *timeout)
{
    if (timeout->free_ctx_cb)
        timeout->free_ctx_cb(timeout->priv);
    free(timeout);
    
}

iomux_t *
iomux_create(int bufsize, int threadsafe)
{
    iomux_t *iomux = (iomux_t *)calloc(1, sizeof(iomux_t));

    if (!iomux) {
        fprintf(stderr, "Error allocating iomux");
        return NULL;
    }

    iomux->bufsize = (bufsize > 0) ? bufsize : IOMUX_CONNECTION_BUFSIZE_DEFAULT;

    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
        iomux->maxconnections = rlim.rlim_cur;
    } else {
        fprintf(stderr, "Can't get the max number of filedescriptors: %s\n",
                strerror(errno));
        iomux->maxconnections = IOMUX_CONNECTIONS_MAX_DEFAULT;
    }

#if defined(HAVE_EPOLL)
    iomux->efd = epoll_create1(0);
    if (iomux->efd == -1) {
        fprintf(stderr, "Errors creating the epoll descriptor : %s\n", strerror(errno));
        free(iomux);
        return NULL;
    }
    iomux->events = calloc(1, sizeof(struct epoll_event) * iomux->maxconnections);
    if (!iomux->events) {
        fprintf(stderr, "Errors creating the events array : %s\n", strerror(errno));
        iomux_destroy(iomux);
        return NULL;
    }
#elif defined(HAVE_KQUEUE)
    iomux->kfd = kqueue();
    if (iomux->kfd == -1) {
        fprintf(stderr, "Errors creating the kqueue descriptor : %s\n", strerror(errno));
        free(iomux);
        return NULL;
    }
    iomux->events = calloc(1, sizeof(struct kevent) * (iomux->maxconnections * 2));
    if (!iomux->events) {
        fprintf(stderr, "Errors creating the events array : %s\n", strerror(errno));
        iomux_destroy(iomux);
        return NULL;
    }
#endif


    iomux->connections = calloc(1, sizeof(iomux_connection_t *) * iomux->maxconnections);
    if (!iomux->connections) {
        fprintf(stderr, "Errors creating the connections array : %s\n", strerror(errno));
        iomux_destroy(iomux);
        return NULL;
    }
    TAILQ_INIT(&iomux->connections_list);

    iomux->timeouts = bh_create((bh_free_value_callback_t)iomux_timeout_destroy);
    if (!iomux->timeouts) {
        fprintf(stderr, "Errors creating the internal binheap to store timeouts\n");
        iomux_destroy(iomux);
        return NULL;
    }

    if (threadsafe) {
        iomux->lock = malloc(sizeof(pthread_mutex_t));
        if (!iomux->lock) {
            fprintf(stderr, "Can't create the iomux internal mutex : %s\n", strerror(errno));
            iomux_destroy(iomux);
            return NULL;
        }
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        if (pthread_mutex_init(iomux->lock, &attr) != 0) {
            fprintf(stderr, "Can't initialize the iomux internal mutex : %s\n", strerror(errno));
            iomux_destroy(iomux);
            return NULL;
        }
        pthread_mutexattr_destroy(&attr);
    }
    iomux->last_timeout_id = 0;

    // NOTE : we save this file descriptor to mitigate accept() EMFILE errors
    //        (which might be leading to inifinite loops)
    //        Basically we keep a spare file descriptor that we close to get
    //        below the EMFILE limit to then accept() and immediately close()
    //        all pending connections
    iomux->emfile_fd = open("/", O_CLOEXEC);
    if (iomux->emfile_fd < 0) {
        fprintf(stderr, "Can't open the internal emfile : %s\n", strerror(errno));
        iomux_destroy(iomux);
        return NULL;
    }
    return iomux;
}

int
iomux_add(iomux_t *iomux, int fd, iomux_callbacks_t *cbs)
{
    iomux_connection_t *connection = NULL;

    if (fd >= iomux->maxconnections)
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
        if (!connection->inbuf) {
            set_error(iomux, "Can't allocate memory for the input buffer: %s", strerror(errno));
            MUTEX_UNLOCK(iomux);
            free(connection);
            return 0;
        }
        TAILQ_INIT(&connection->output_queue);
        connection->bufsize = iomux->bufsize;
        connection->fd = fd;

        iomux->connections[fd] = connection;
        iomux->num_fds++;
        while (!iomux->connections[iomux->minfd] && iomux->minfd != iomux->maxfd)
            iomux->minfd++;

        TAILQ_INSERT_TAIL(&iomux->connections_list, connection, next);
        // if we have no emfile_fd saved, let's open one now
        // it could have been previously closed because we
        // reached the EMFILE condition but we were not able
        // to open it again because some other thread go the
        // free filedescriptor before us being able to get it back
        if (iomux->emfile_fd == -1)
            iomux->emfile_fd = open("/", O_CLOEXEC);

        MUTEX_UNLOCK(iomux);
        return 1;
    } else {
        set_error(iomux, "Can't create a new connection :  %s", strerror(errno));
        MUTEX_UNLOCK(iomux);
        return 0;
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
    TAILQ_REMOVE(&iomux->connections_list, iomux->connections[fd], next);
    if (iomux->connections[fd]->inbuf)
        free(iomux->connections[fd]->inbuf);
    iomux_output_chunk_t *chunk = TAILQ_FIRST(&iomux->connections[fd]->output_queue);
    while (chunk) {
        TAILQ_REMOVE(&iomux->connections[fd]->output_queue, chunk, next);
        if (chunk->free) {
            if (iomux->connections[fd]->cbs.mux_free_data)
                iomux->connections[fd]->cbs.mux_free_data(iomux, fd, chunk->data, chunk->len, iomux->connections[fd]->cbs.priv);
            else
                free(chunk->data);
        }
        free(chunk);
        chunk = TAILQ_FIRST(&iomux->connections[fd]->output_queue);
    }
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
iomux_schedule(iomux_t *iomux,
               struct timeval *tv,
               iomux_cb_t cb,
               void *priv,
               iomux_timeout_free_context_cb free_ctx_cb)
{
    iomux_timeout_t *timeout;

    if (!tv || !cb) {
        // TODO - set an error message
        return 0;
    }

    MUTEX_LOCK(iomux);

    struct timeval now;
    gettimeofday(&now, NULL);
    if (iomux->last_timeout_check.tv_sec == 0)
        memcpy(&iomux->last_timeout_check, &now, sizeof(struct timeval));

    timeout = (iomux_timeout_t *)calloc(1, sizeof(iomux_timeout_t));
    if (!timeout) {
        // TODO - set an error message
        MUTEX_UNLOCK(iomux);
        return 0;
    }
    timeradd(&now, tv, &timeout->expire_time);
    timeout->cb = cb;
    timeout->priv = priv;
    timeout->free_ctx_cb = free_ctx_cb;

    uint64_t expire =  (timeout->expire_time.tv_sec * 1000) + (timeout->expire_time.tv_usec/1000);
    timeout->id = (uint64_t)((expire << 8) | (uint8_t)(++iomux->last_timeout_id % 256));

    // keep the list sorted in ascending order
    if (bh_insert(iomux->timeouts, timeout->id, timeout, sizeof(iomux_timeout_t)) != 0) {
        fprintf(stderr, "Can't insert a new node in the binomial heap\n");
        MUTEX_UNLOCK(iomux);
        free(timeout);
        return 0;
    }
    MUTEX_UNLOCK(iomux);
    return timeout->id;
}

iomux_timeout_id_t
iomux_reschedule(iomux_t *iomux,
                 iomux_timeout_id_t id,
                 struct timeval *tv,
                 iomux_cb_t cb,
                 void *priv,
                 iomux_timeout_free_context_cb free_ctx_cb)
{
    iomux_unschedule(iomux, id);
    return iomux_schedule(iomux, tv, cb, priv, free_ctx_cb);
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

/*
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
*/

void
iomux_set_timeout(iomux_t *iomux, int fd, struct timeval *tv)
{
    MUTEX_LOCK(iomux);
    if (!iomux->connections[fd]) {
        MUTEX_UNLOCK(iomux);
        return;
    }

    if (tv) {
        struct timeval now;
        gettimeofday(&now, NULL);
        timeradd(&now, tv, &iomux->connections[fd]->expire_time);
    } else {
        memset(&iomux->connections[fd]->expire_time, 0, sizeof(iomux->connections[fd]->expire_time));
    }
    MUTEX_UNLOCK(iomux);
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
    if (errno == EMFILE || errno == ENFILE) {
        fprintf(stderr, "Maximum number of filedescriptors reached, can't accept new connections!\n");
        MUTEX_LOCK(iomux);
        if (iomux->emfile_fd >= 0) {
            close(iomux->emfile_fd);
            while((newfd = accept(fd, (struct sockaddr *)&peer, &socklen)) >= 0)
                close(newfd); // let's signal clients we are overloaded
            // NOTE: if some other thread got the freed filedescriptor we won't be
            //       able to get it back. If this happens we will try saving the
            //       emfile_fd again when performing other operations
            //       (as iomux_add() and iomux_close())
            iomux->emfile_fd = open("/", O_CLOEXEC);
        }
        MUTEX_UNLOCK(iomux);
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
             int mb = mux_input(iomux, fd, conn->inbuf, len, priv);
             if (iomux->connections[fd] == conn && iomux->connections[fd]->inlen == conn->inlen)
             {
                 if (mb == conn->inlen) {
                     conn->inlen = 0;
                 } else if (mb) {
                     memmove(conn->inbuf, conn->inbuf + mb, len - mb);
                     conn->inlen -= mb;
                 }
             }
         }
    }
    MUTEX_UNLOCK(iomux);
}

static void
iomux_write_fd(iomux_t *iomux, int fd, void *priv)
{
    MUTEX_LOCK(iomux);

    iomux_output_chunk_t *chunk = TAILQ_FIRST(&iomux->connections[fd]->output_queue);
    if (!iomux->connections[fd] || !chunk) {
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

    char *outbuf = (char *)chunk->data + chunk->offset;
    int outlen = chunk->len - chunk->offset;

    MUTEX_UNLOCK(iomux);

    int wb = write(fd, outbuf, outlen);
    if (wb <= 0) {
        if (errno != EINTR && errno != EAGAIN) {
            fprintf(stderr, "write on fd %d failed: %s\n", fd, strerror(errno));
            iomux_close(iomux, fd);
        }
    } else {
        MUTEX_LOCK(iomux);
        outlen -= wb;
        if (!outlen) {
            TAILQ_REMOVE(&iomux->connections[fd]->output_queue, chunk, next); 
            if (chunk->free) {
                if (iomux->connections[fd]->cbs.mux_free_data)
                    iomux->connections[fd]->cbs.mux_free_data(iomux, fd, chunk->data, chunk->len, iomux->connections[fd]->cbs.priv);
                else
                    free(chunk->data);
            }
            free(chunk);
            chunk = TAILQ_FIRST(&iomux->connections[fd]->output_queue);
        } else {
            chunk->offset += wb;
        }
        if (!chunk) {
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
    if (timercmp(&timeout->expire_time, &now, >))
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

    if (!bh_count(iomux->timeouts)) {
        // there are no timeouts scheduled,
        // return early
        MUTEX_UNLOCK(iomux);
        return;
    }

    struct timeval now;
    gettimeofday(&now, NULL);

    void *timeout_ptr = NULL;
    while (bh_delete_minimum(iomux->timeouts, &timeout_ptr, NULL) == 0) {
        timeout = (iomux_timeout_t *)timeout_ptr;
        if (timercmp(&now, &timeout->expire_time, <)) {
            if (bh_insert(iomux->timeouts,
                          timeout->id,
                          timeout,
                          sizeof(iomux_timeout_t)) != 0)
            {
                fprintf(stderr, "%s: Can't insert a new node in the binomial heap\n", __FUNCTION__);
            }
            break;
        }
        // run expired timeouts
        timeout->cb(iomux, timeout->priv);
        iomux_timeout_destroy(timeout);
    }

    MUTEX_UNLOCK(iomux);
}

void
iomux_loop(iomux_t *iomux, struct timeval *tv)
{
    struct timeval tv_default = { 0, 20000 };
    while (!iomux->leave) {
        if (iomux->loop_next_cb)
            iomux->loop_next_cb(iomux, iomux->loop_next_priv);

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
iomux_write(iomux_t *iomux, int fd, unsigned char *buf, int len,  iomux_output_mode_t mode)
{
    iomux_output_chunk_t *chunk = calloc(1, sizeof(iomux_output_chunk_t));
    if (!chunk) {
        set_error(iomux, "%s: Can't allocate memory for the new chunk", __FUNCTION__, strerror(errno));
        return 0;
    }
    chunk->free = (mode != IOMUX_OUTPUT_MODE_NONE);
    if (mode == IOMUX_OUTPUT_MODE_COPY) {
        chunk->data = malloc(len);
        if (!chunk->data) {
            set_error(iomux, "%s: Can't allocate memory for the chunk data", __FUNCTION__, strerror(errno));
            free(chunk);
            return 0;
        }
        memcpy(chunk->data, buf, len);
    } else {
        // TODO - check for unknown output modes
        chunk->data = buf;
    }
    chunk->len = len;

    MUTEX_LOCK(iomux);

    if (!iomux->connections[fd]) {
        MUTEX_UNLOCK(iomux);
        if (chunk->free) {
            free(chunk->data);
        }
        free(chunk);
        return 0;
    }

    TAILQ_INSERT_TAIL(&iomux->connections[fd]->output_queue, chunk, next);

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

    MUTEX_UNLOCK(iomux);
    return len;
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

    iomux_output_chunk_t *chunk = TAILQ_FIRST(&conn->output_queue);
    if (fcntl(fd, F_GETFD, 0) != -1 && chunk) { // there is pending data
        int retries = 0;
        iomux_output_chunk_t *last_chunk = NULL; // XXX - here to silence the static analyzer
        while (chunk && chunk != last_chunk && retries <= IOMUX_FLUSH_MAXRETRIES) {
            int wb = write(fd, chunk->data, chunk->len);
            if (wb == -1) {
                if (errno == EINTR || errno == EAGAIN) {
                    retries++;
                    continue;
                } else {
                    break;
                }
            } else if (wb == 0) {
                fprintf(stderr, "%s: closing filedescriptor %d with pending data\n", __FUNCTION__, fd);
                break;
            }
            TAILQ_REMOVE(&conn->output_queue, chunk, next);
            retries = 0;
            if (chunk->free) {
                if (iomux->connections[fd]->cbs.mux_free_data)
                    iomux->connections[fd]->cbs.mux_free_data(iomux, fd, chunk->data, chunk->len, iomux->connections[fd]->cbs.priv);
                else
                    free(chunk->data);
            }
            free(chunk);
            // the static analyzer reports a false positive because not able
            // to properly understand the TAILQ_REMOVE macro.
            // The extra check against the last_chunk pointer is here just
            // to silence that warning
            last_chunk = chunk;
            chunk = TAILQ_FIRST(&conn->output_queue);
        }
    }

    void (*mux_eof)(iomux_t *, int, void *) = conn->cbs.mux_eof;
    void *priv = conn->cbs.priv;

    iomux_remove(iomux, fd);

    if(mux_eof)
        mux_eof(iomux, fd, priv);


    // if we have no emfile_fd saved, let's open one now
    // it could have been previously closed because we
    // reached the EMFILE condition but we were not able
    // to open it again because some other thread go the
    // free filedescriptor before us being able to get it back
    if (iomux->emfile_fd == -1)
        iomux->emfile_fd = open("/", O_CLOEXEC);

    MUTEX_UNLOCK(iomux);

    return 1;
}

int
iomux_isempty(iomux_t *iomux)
{
    MUTEX_LOCK(iomux);
    int empty = (TAILQ_FIRST(&iomux->connections_list) == NULL);
    MUTEX_UNLOCK(iomux);
    return empty;
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
    close(iomux->emfile_fd);
    free(iomux);
}

void
iomux_clear(iomux_t *iomux)
{
    int fd;
    MUTEX_LOCK(iomux);

    for (fd = iomux->maxfd; fd >= iomux->minfd; fd--) {
        if (iomux->connections[fd]) {
            iomux_close(iomux, fd);
        }
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

int
iomux_set_output_callback(iomux_t *iomux, int fd, iomux_output_callback_t cb)
{
    MUTEX_LOCK(iomux);
    if (!iomux->connections[fd]) {
        MUTEX_UNLOCK(iomux);
        return 0;
    }
    iomux->connections[fd]->cbs.mux_output = cb;
    MUTEX_UNLOCK(iomux);
    return 1;
}

int
iomux_unset_output_callback(iomux_t *iomux, int fd)
{
    MUTEX_LOCK(iomux);
    if (!iomux->connections[fd]) {
        MUTEX_UNLOCK(iomux);
        return 0;
    }
    iomux_output_callback_t prev = iomux->connections[fd]->cbs.mux_output;
    iomux->connections[fd]->cbs.mux_output = NULL;
    MUTEX_UNLOCK(iomux);
    return prev ? 1 : 0;
}

static inline int
iomux_poll_connection(iomux_t *iomux, iomux_connection_t *connection, struct timeval *expire_min, struct timeval *now)
{
    int fd = connection->fd;
    int len = connection->inlen;

    if (len && connection->cbs.mux_input) {
        int mb = connection->cbs.mux_input(iomux, fd, connection->inbuf, len, connection->cbs.priv);
        if (iomux->connections[fd] == connection && iomux->connections[fd]->inlen == connection->inlen)
        {
            if (mb == connection->inlen) {
                connection->inlen = 0;
            } else if (mb) {
                memmove(connection->inbuf, connection->inbuf + mb, len - mb);
                connection->inlen -= mb;
            }
        }
    }

    if (connection->expire_time.tv_sec) {
        if (timercmp(now, &connection->expire_time, >)) {
            memset(&connection->expire_time, 0, sizeof(connection->expire_time));
            if (connection->cbs.mux_timeout) {
                connection->cbs.mux_timeout(iomux, fd, connection->cbs.priv);
                // a timeout routine can remove an fd from the mux, so we need to check for its existance again
                if (!iomux->connections[fd])
                    return -1;
            }
        } else {
            struct timeval expire_time;
            timersub(&connection->expire_time, now, &expire_time);
            if (!expire_min->tv_sec || timercmp(expire_min,  &expire_time, >))
                memcpy(expire_min, &expire_time, sizeof(struct timeval));
        }
    }

    iomux_output_chunk_t *chunk = TAILQ_FIRST(&connection->output_queue);
    if (!chunk && connection->cbs.mux_output) {
        int len = 0;
        unsigned char *data = NULL;
        int mode = connection->cbs.mux_output(iomux, fd, &data, &len, connection->cbs.priv);

        // NOTE: the output callback might have removed the fd from the mux
        if (!iomux->connections[fd]) {
            free(data);
            return -1;
        }

        if (data) {
            chunk = calloc(1, sizeof(iomux_output_chunk_t));
            if (!chunk) {
                set_error(iomux, "%s: Can't allocate memory for the new chunk", __FUNCTION__, strerror(errno));
                free(chunk);
                return 0;
            }
            if (mode == IOMUX_OUTPUT_MODE_COPY) {
                chunk->data = malloc(len);
                if (!chunk->data) {
                    set_error(iomux, "%s: Can't allocate memory for the chunk data", __FUNCTION__, strerror(errno));
                    free(chunk);
                    return 0;
                }
                memcpy(chunk->data, data, len);
            } else {
                chunk->data = data;
            }
            chunk->free = (mode != IOMUX_OUTPUT_MODE_NONE);
            chunk->len = len;
            TAILQ_INSERT_TAIL(&connection->output_queue, chunk, next);
#if defined(HAVE_EPOLL)
            // NOTE: In the epoll implementation we want to register a filedescriptor
            //       for input events only if no data was in the queue but a new chunk
            //       was provided via a mux_output callback
            return 1;
#endif
        }
    }

#if !defined(HAVE_EPOLL)
    // NOTE: Both kqueue and select implementation need to actively
    //       register for output events at each call so we need
    //       to notify back that there is pending data.
    //       In the epoll implementation instead, filedescriptor are
    //       registered for output events only once so if there was
    //       already a chunk in the queue, the filedescriptor was
    //       presumably already registered for output events.
    if (chunk)
        return 1;
#endif

    return 0;
}

#if defined(HAVE_KQUEUE)
void
iomux_run(iomux_t *iomux, struct timeval *tv_default)
{
    int i;
    struct timespec ts;
    struct timeval expire_min = { 0, 0 };

    struct timeval now;
    gettimeofday(&now, NULL);

    MUTEX_LOCK(iomux);

    int n = 0;
    iomux_connection_t *connection = NULL;
    iomux_connection_t *tmp;
    TAILQ_FOREACH_SAFE(connection, &iomux->connections_list, next, tmp) {
        int prc = iomux_poll_connection(iomux, connection, &expire_min, &now);

        switch(prc) {
            case -1:
                continue;
            case 1:
                memcpy(&iomux->events[n], &connection->event, 2 * sizeof(struct kevent));
                n += 2;
                break;
            case 0:
            default:
                memcpy(&iomux->events[n], &connection->event, sizeof(struct kevent));
                n++;
                break;
        }
    }


    if (!tv_default ||
         ((expire_min.tv_sec || expire_min.tv_usec) &&
          tv_default != &expire_min && timercmp(tv_default, &expire_min, >)))
    {
        tv_default = &expire_min;
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

    struct timeval expire_min = { 0, 0 };

    struct timeval now;
    gettimeofday(&now, NULL);

    MUTEX_LOCK(iomux);

    iomux_connection_t *connection = NULL;
    iomux_connection_t *tmp;
    TAILQ_FOREACH_SAFE(connection, &iomux->connections_list, next, tmp) {
        int fd = connection->fd;
        int prc = iomux_poll_connection(iomux, connection, &expire_min, &now);

        switch(prc) {
            case -1:
                continue;
            case 1:
            {
                struct epoll_event event;
                bzero(&event, sizeof(event));
                event.data.fd = fd;
                event.events = EPOLLIN | EPOLLOUT;

                int rc = epoll_ctl(iomux->efd, EPOLL_CTL_MOD, fd, &event);
                if (rc == -1) {
                    fprintf(stderr, "Errors modifying fd %d to epoll instance %d : %s\n",
                            fd, iomux->efd, strerror(errno));
                }
                break;
            }
            case 0:
            default:
                break;
        }
    }

    int num_fds = iomux->num_fds;

    MUTEX_UNLOCK(iomux);

    if (!tv_default ||
         ((expire_min.tv_sec || expire_min.tv_usec) &&
          tv_default != &expire_min && timercmp(tv_default, &expire_min, >)))
    {
        tv_default = &expire_min;
    }

    // shrink the timeout if we have timers expiring earlier
    struct timeval *tv = iomux_adjust_timeout(iomux, tv_default);
    int epoll_waiting_time = tv ? ((tv->tv_sec * 1000) + (tv->tv_usec / 1000)) : -1;

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

    int i;
    for (i = 0; i < n; i++) {
        if ((iomux->events[i].events & EPOLLHUP || iomux->events[i].events & EPOLLRDHUP))
        {
            iomux_close(iomux, iomux->events[i].data.fd);
            continue;
        } else if ((iomux->events[i].events & EPOLLERR)) {
            int error = 0;
            socklen_t errlen = sizeof(error);
            if (getsockopt(iomux->events[i].data.fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen) == 0)
            {
                if (error == EINPROGRESS) // this is not an error
                    continue;

                fprintf (stderr, "epoll error on fd %d: %s\n",
                        iomux->events[i].data.fd, strerror(error));
                iomux_close(iomux, iomux->events[i].data.fd);
            } else {
                fprintf (stderr, "unkown epoll error on fd %d\n", iomux->events[i].data.fd);
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
    int fdset_size = iomux->maxconnections > 1024 ? iomux->maxconnections/1024 : 1;
    fd_set rin[fdset_size], rout[fdset_size];
    int maxfd = iomux->minfd;;

    memset(&rin, 0, sizeof(rin));
    memset(&rout, 0, sizeof(rout));

    struct timeval expire_min = { 0, 0 };

    struct timeval now;
    gettimeofday(&now, NULL);

    MUTEX_LOCK(iomux);

    iomux_connection_t *connection = NULL;
    iomux_connection_t *tmp;
    TAILQ_FOREACH_SAFE(connection, &iomux->connections_list, next, tmp) {
        int fd = connection->fd;
        int prc = iomux_poll_connection(iomux, connection, &expire_min, &now);

        switch(prc) {
            case -1:
                continue;
            case 1:
                // output pending data
                FD_SET(fd, &rout[0]);
                if (fd > maxfd)
                    maxfd = fd;
                // NOTE: no break statement here
                // since we still want to
                // register the fd for input
            case 0:
            default:
                FD_SET(fd, &rin[0]);
                if (fd > maxfd)
                    maxfd = fd;
                break;
        }


    }

    if (!tv_default ||
         ((expire_min.tv_sec || expire_min.tv_usec) &&
          tv_default != &expire_min && timercmp(tv_default, &expire_min, >)))
    {
        tv_default = &expire_min;
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

static int
iomux_binheap_iterator_move_callback(bh_t *bh, uint64_t key, void *value, size_t vlen, void *priv)
{
    iomux_t *dst = (iomux_t *)priv;
    iomux_timeout_t *timeout = (iomux_timeout_t *)value;
    // XXX - ids might overlap ... they should be recomputed
    if (bh_insert(dst->timeouts, timeout->id, timeout, sizeof(iomux_timeout_t)) != 0)
        fprintf(stderr, "%s: Can't insert a new node in the destination binomial heap\n", __FUNCTION__);
    return -1;
}

int
iomux_move(iomux_t *src, iomux_t *dst)
{
    int count = 0;

    MUTEX_LOCK(src);
    MUTEX_LOCK(dst);


    iomux_connection_t *connection = NULL;
    iomux_connection_t *tmp;
    TAILQ_FOREACH_SAFE(connection, &src->connections_list, next, tmp) {

        int fd = connection->fd;

        iomux_callbacks_t cbs;
        memcpy(&cbs, &connection->cbs, sizeof(cbs));

        if (iomux_add(dst, fd, &cbs)) {
            iomux_connection_t *new_connection = dst->connections[fd];
            free(new_connection->inbuf);
            new_connection->inbuf = connection->inbuf;
            new_connection->bufsize = connection->bufsize;
            new_connection->inlen = connection->inlen;
            connection->inbuf = NULL;
            connection->bufsize = 0;
            iomux_output_chunk_t *chunk = NULL;
            iomux_output_chunk_t *ctmp;
            TAILQ_FOREACH_SAFE(chunk, &connection->output_queue, next, ctmp) {
                TAILQ_REMOVE(&connection->output_queue, chunk, next);
                TAILQ_INSERT_TAIL(&new_connection->output_queue, chunk, next);
            }
        }

        iomux_remove(src, connection->fd);
        count++;
    }

    bh_foreach(src->timeouts, iomux_binheap_iterator_move_callback, (void *)dst);

    MUTEX_UNLOCK(src);
    MUTEX_UNLOCK(dst);

    return count;
}

