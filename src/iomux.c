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
#include <sys/timerfd.h>
#elif defined(HAVE_KQUEUE)
#include <sys/event.h>
#endif

#include "bsd_queue.h"

#include "iomux.h"

#define IOMUX_CONNECTIONS_MAX (1<<16)
// 1MB default connection bufsize
#define IOMUX_CONNECTION_BUFSIZE (1<<16)
#define IOMUX_CONNECTION_SERVER (1)

int iomux_hangup = 0;

//! \brief iomux connection strucure
typedef struct __iomux_connection {
    uint32_t flags;
    iomux_callbacks_t cbs;
    unsigned char outbuf[IOMUX_CONNECTION_BUFSIZE];
    int eof;
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
    struct timeval wait_time;
    TAILQ_ENTRY(__iomux_timeout) timeout_list;
    void (*cb)(iomux_t *iomux, void *priv);
    void *priv;
#if defined(HAVE_EPOLL)
    int timerfd;
#endif
} iomux_timeout_t;

//! \brief IOMUX base structure
struct __iomux {
    iomux_connection_t *connections[IOMUX_CONNECTIONS_MAX];
    int maxfd;
    int minfd;
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
    struct epoll_event events[IOMUX_CONNECTIONS_MAX];
    int efd; 
    iomux_timeout_t *timeouts_fd[IOMUX_CONNECTIONS_MAX];
#elif defined(HAVE_KQUEUE)
    struct kevent events[IOMUX_CONNECTIONS_MAX*2];
    int kfd;
#endif
    TAILQ_HEAD(, __iomux_timeout) timeouts;
    int last_timeout_id;
};

static void set_error(iomux_t *iomux, char *fmt, ...) {
    va_list arg;
    va_start(arg, fmt);
    vsnprintf(iomux->error, sizeof(iomux->error), fmt, arg);
    va_end(arg);
}


#define IOMUX_FLUSH_MAXRETRIES 5    //!< Maximum number of iterations for flushing the output buffer

static void iomux_handle_timeout(iomux_t *iomux, void *priv);

iomux_t *iomux_create(void)
{
    iomux_t *iomux = (iomux_t *)calloc(1, sizeof(iomux_t));

    if (!iomux) {
        fprintf(stderr, "Error allocating iomux");
        return NULL;
    }

    TAILQ_INIT(&iomux->timeouts);

#if defined(HAVE_EPOLL)
    iomux->efd = epoll_create1(0);
    if (iomux->efd == -1) {
        fprintf(stderr, "Errors creating the epoll descriptor : %s\n", strerror(errno));
        free(iomux);
        return NULL;
    }
#elif defined(HAVE_KQUEUE)
    iomux->kfd = kqueue();
    if (iomux->kfd == -1) {
        fprintf(stderr, "Errors creating the kqueue descriptor : %s\n", strerror(errno));
        free(iomux);
        return NULL;
    }
#endif

    iomux->last_timeout_id = 0;

    return iomux;
}

int
iomux_add(iomux_t *iomux, int fd, iomux_callbacks_t *cbs)
{
    iomux_connection_t *connection = NULL;

    if (fd < 0) {
        set_error(iomux, "fd %d is invalid", fd);
        return 0;
    } else if (fd >= IOMUX_CONNECTIONS_MAX) {
        set_error(iomux, "fd %d exceeds max fd %d", fd, IOMUX_CONNECTIONS_MAX);
        return 0;
    }

    if (iomux->connections[fd]) {
        set_error(iomux, "filedescriptor %d already added", fd);
        return 0;
    }
    if (!cbs) {
        set_error(iomux, "no callbacks have been specified, skipping filedescriptor %d", fd);
        return 0;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);
    connection = (iomux_connection_t *)calloc(1, sizeof(iomux_connection_t));
    if (connection) {

#if defined(HAVE_EPOLL)
        struct epoll_event event = { 0 };
        event.data.fd = fd;
        event.events = EPOLLIN;
        if (connection->cbs.mux_output)
            event.events = event.events | EPOLLOUT;

        int rc = epoll_ctl(iomux->efd, EPOLL_CTL_ADD, fd, &event);
        if (rc == -1) {
            fprintf(stderr, "Errors adding fd %d to epoll instance %d : %s\n", 
                    fd, iomux->efd, strerror(errno));
            free(connection);
            return 0;;
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
        iomux->connections[fd] = connection;
        while (!iomux->connections[iomux->minfd] && iomux->minfd != iomux->maxfd)
            iomux->minfd++;

        return 1;
    }
    return 0;
}

void
iomux_remove(iomux_t *iomux, int fd)
{
    iomux_unschedule(iomux, iomux->connections[fd]->timeout_id);

#if defined(HAVE_EPOLL)
    struct epoll_event event = { 0 };
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
    free(iomux->connections[fd]);
    iomux->connections[fd] = NULL;

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
}

iomux_timeout_id_t
iomux_schedule(iomux_t *iomux, struct timeval *tv, iomux_cb_t cb, void *priv)
{
    iomux_timeout_t *timeout, *timeout2;

    if (!tv || !cb)
        return 0;

    if (iomux->last_timeout_check.tv_sec == 0)
        gettimeofday(&iomux->last_timeout_check, NULL);

    timeout = (iomux_timeout_t *)calloc(1, sizeof(iomux_timeout_t));
    memcpy(&timeout->wait_time, tv, sizeof(struct timeval));
    timeout->cb = cb;
    timeout->priv = priv;
    timeout->id = ++iomux->last_timeout_id;

#if defined(HAVE_EPOLL)
    timeout->timerfd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    if (timeout->timerfd == -1) {
        fprintf(stderr, "Errors creating the timer descriptor : %s\n", strerror(errno));
        free(timeout);
        return 0;
    }
    struct epoll_event event = { 0 };

    event.data.fd = timeout->timerfd;

    event.events = EPOLLIN | EPOLLONESHOT; 

    const struct itimerspec its = { { 0, 0 }, { timeout->wait_time.tv_sec, timeout->wait_time.tv_usec * 1000 } };
    int rc = timerfd_settime(timeout->timerfd, 0, &its, NULL);
    if (rc != 0) {
        fprintf(stderr, "Errors setting the timer on timerfd %d on epoll instance %d : %s\n", 
                timeout->timerfd, iomux->efd, strerror(errno));
        close(timeout->timerfd);
        free(timeout);
        return 0;
    }

    iomux->timeouts_fd[timeout->timerfd] = timeout;

    rc = epoll_ctl(iomux->efd, EPOLL_CTL_ADD, timeout->timerfd, &event);
    if (rc == -1) {
        fprintf(stderr, "Errors adding timeout %d on epoll instance %d : %s\n", 
                timeout->id, iomux->efd, strerror(errno));
        close(timeout->timerfd);
        iomux->timeouts_fd[timeout->timerfd] = NULL;
        free(timeout);
        return 0;
    }
#endif

    // keep the list sorted in ascending order
    TAILQ_FOREACH(timeout2, &iomux->timeouts, timeout_list) {
        if ((tv->tv_sec == timeout2->wait_time.tv_sec &&  tv->tv_usec < timeout2->wait_time.tv_usec) ||
                tv->tv_sec < timeout2->wait_time.tv_sec)
        {
            TAILQ_INSERT_BEFORE(timeout2, timeout, timeout_list);
            return timeout->id;
        }
    }
    TAILQ_INSERT_TAIL(&iomux->timeouts, timeout, timeout_list);

    return timeout->id;
}

iomux_timeout_id_t
iomux_reschedule(iomux_t *iomux, iomux_timeout_id_t id, struct timeval *tv, iomux_cb_t cb, void *priv)
{
    iomux_unschedule(iomux, id);
    return iomux_schedule(iomux, tv, cb, priv);
}

int
iomux_unschedule_all(iomux_t *iomux, iomux_cb_t cb, void *priv)
{           
    iomux_timeout_t *timeout, *timeout_tmp;
    int count = 0;

    TAILQ_FOREACH_SAFE(timeout, &iomux->timeouts, timeout_list, timeout_tmp) {
        if (cb == timeout->cb && priv == timeout->priv) {
#if defined(HAVE_EPOLL)
            iomux->timeouts_fd[timeout->timerfd] = NULL;
            close(timeout->timerfd);
#endif
            TAILQ_REMOVE(&iomux->timeouts, timeout, timeout_list);
            free(timeout);
            count++;
        }
    }
    return count;
}

int
iomux_unschedule(iomux_t *iomux, iomux_timeout_id_t id)
{
    iomux_timeout_t *timeout, *timeout_tmp;

    if (!id)
        return 0;

    TAILQ_FOREACH_SAFE(timeout, &iomux->timeouts, timeout_list, timeout_tmp) {
        if (id == timeout->id) {
#if defined(HAVE_EPOLL)
            iomux->timeouts_fd[timeout->timerfd] = NULL;
            close(timeout->timerfd);
#endif
            TAILQ_REMOVE(&iomux->timeouts, timeout, timeout_list);
            free(timeout);
            break;
        }
    }

    return 1;
}

static void
iomux_handle_timeout(iomux_t *iomux, void *priv)
{
    int fd = (long int)priv;

    if (iomux->connections[fd]) {
        iomux_callbacks_t *cbs = &iomux->connections[fd]->cbs;
        if (cbs->mux_timeout)
            cbs->mux_timeout(iomux, fd, cbs->priv);
    }
}

iomux_timeout_id_t
iomux_set_timeout(iomux_t *iomux, int fd, struct timeval *tv)
{
    if (!iomux->connections[fd])
        return 0;

    if (!tv) {
        (void) iomux_unschedule(iomux, iomux->connections[fd]->timeout_id);
        return 0;
    } else {
        return iomux_reschedule(iomux, iomux->connections[fd]->timeout_id, tv, iomux_handle_timeout, (void *)(long int)fd);
    }
}

int
iomux_listen(iomux_t *iomux, int fd)
{
    if (!iomux->connections[fd]) {
        set_error(iomux, "%s: No connections for fd %d", __FUNCTION__, fd);
        return 0;
    }
    assert(iomux->connections[fd]->cbs.mux_connection);

    if (listen(fd, -1) != 0) {
        set_error(iomux, "%s: Error listening on fd %d: %s", __FUNCTION__, fd, strerror(errno));
        return 0;
    }

    iomux->connections[fd]->flags = iomux->connections[fd]->flags | IOMUX_CONNECTION_SERVER;

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

static void
iomux_update_timeouts(iomux_t *iomux)
{
    iomux_timeout_t *timeout = NULL;
    struct timeval diff = { 0, 0 };
    struct timeval now;

    gettimeofday(&now, NULL);
    if (iomux->last_timeout_check.tv_sec)
        timersub(&now, &iomux->last_timeout_check, &diff);

    memcpy(&iomux->last_timeout_check, &now, sizeof(struct timeval));

    // update timeouts' waiting time
    TAILQ_FOREACH(timeout, &iomux->timeouts, timeout_list)
        timersub(&timeout->wait_time, &diff, &timeout->wait_time);
}

static void
iomux_accept_connections_fd(iomux_t *iomux, int fd)
{
    iomux_callbacks_t *cbs =  &iomux->connections[fd]->cbs;
    int newfd;
    struct sockaddr_in peer;
    socklen_t socklen = sizeof(struct sockaddr);
    // if it is, accept all pending connections and add them to the mux
    while ((newfd = accept(fd, (struct sockaddr *)&peer, &socklen)) >= 0) {
        cbs->mux_connection(iomux, newfd, cbs->priv);
    }
}

static void 
iomux_read_fd(iomux_t *iomux, int fd)
{
    iomux_callbacks_t *cbs =  &iomux->connections[fd]->cbs;
    char inbuf[IOMUX_CONNECTION_BUFSIZE];
    int rb = read(fd, inbuf, sizeof(inbuf));
    if (rb == -1) {
        if (errno != EINTR && errno != EAGAIN) {
            // don't output warnings if the filedescriptor has been closed
            // without informing the iomux and we detect it only now
            if (errno != EBADF)
                fprintf(stderr, "read on fd %d failed: %s\n", fd, strerror(errno));
            iomux_close(iomux, fd);
        }
    } else if (rb == 0) {
         iomux_close(iomux, fd);
    } else {
         if (cbs->mux_input)
             cbs->mux_input(iomux, fd, inbuf, rb, cbs->priv);
    }
}

static void
iomux_write_fd(iomux_t *iomux, int fd)
{
    iomux_callbacks_t *cbs =  &iomux->connections[fd]->cbs;
    if (!iomux->connections[fd]->outlen && cbs->mux_output) {
        cbs->mux_output(iomux, fd, cbs->priv);
    }

    // note that the fd might have been closed by the mux_output callback
    // so we need to check for its presence again
    if (!iomux->connections[fd] || !iomux->connections[fd]->outlen) {
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
        return; 
    }

    int wb = write(fd, iomux->connections[fd]->outbuf, iomux->connections[fd]->outlen);
    if (wb == -1) {
        if (errno != EINTR || errno != EAGAIN) {
            fprintf(stderr, "write on fd %d failed: %s\n", fd, strerror(errno));
            iomux_close(iomux, fd);
        }
    } else if (wb == 0) {
        iomux_close(iomux, fd);
    } else {
        iomux->connections[fd]->outlen -= wb;
        if (iomux->connections[fd]->outlen) { // shift data if we didn't write it all at once
            memmove(iomux->connections[fd]->outbuf, &iomux->connections[fd]->outbuf[wb], iomux->connections[fd]->outlen);
        } else if (!cbs->mux_output) {
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
    }
}

#ifndef HAVE_EPOLL
static struct timeval *
iomux_adjust_timeout(iomux_t *iomux, struct timeval *tv_default)
{
    struct timeval *tv = NULL;
    iomux_timeout_t *timeout = NULL;

    timeout = TAILQ_FIRST(&iomux->timeouts);
    if (tv_default && timeout) {
        if (timercmp(&timeout->wait_time, tv_default, >))
            tv = tv_default;
        else
            tv = &timeout->wait_time;
    } else if (timeout) {
        tv = &timeout->wait_time;
    } else if (tv_default) {
        tv = tv_default;
    } else {
        tv = NULL;
    }
    return tv;
}

void
iomux_run_timeouts(iomux_t *iomux)
{
    iomux_timeout_t *timeout = NULL;

    // run expired timeouts
    struct timeval diff = { 0, 0 };
    struct timeval now;

    gettimeofday(&now, NULL);
    if (iomux->last_timeout_check.tv_sec)
        timersub(&now, &iomux->last_timeout_check, &diff);
    else
        memcpy(&diff, &now, sizeof(now));

    iomux_update_timeouts(iomux);

    memset(&diff, 0, sizeof(diff));
    while ((timeout = TAILQ_FIRST(&iomux->timeouts)) && timercmp(&timeout->wait_time, &diff, <=)) {
        TAILQ_REMOVE(&iomux->timeouts, timeout, timeout_list);
        timeout->cb(iomux, timeout->priv);
        free(timeout);
    }
}

#endif


#if defined(HAVE_KQUEUE)
void
iomux_run(iomux_t *iomux, struct timeval *tv_default)
{
    int i;
    struct timespec ts;

    int n = 0;
    for (i = iomux->minfd; i <= iomux->maxfd; i++) {
        if (!iomux->connections[i])
            continue;
        if (iomux->connections[i]->outlen || iomux->connections[i]->cbs.mux_output) {
            memcpy(&iomux->events[n], &iomux->connections[i]->event, 2 * sizeof(struct kevent));
            n += 2;
        } else {
            memcpy(&iomux->events[n], &iomux->connections[i]->event, sizeof(struct kevent));
            n++;
        }
    }

    struct timeval *tv = iomux_adjust_timeout(iomux, tv_default);
    if (tv) {
        ts.tv_sec = tv->tv_sec;
        ts.tv_nsec = tv->tv_usec * 1000;
    }

    int cnt = kevent(iomux->kfd, iomux->events, n, iomux->events, IOMUX_CONNECTIONS_MAX * 2, tv ? &ts : NULL);

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
                if ((iomux->connections[fd]->flags&IOMUX_CONNECTION_SERVER) == (IOMUX_CONNECTION_SERVER) && event->data) {
                    while(event->data--)
                        iomux_accept_connections_fd(iomux, fd);
                } else {
                    iomux_read_fd(iomux, fd);
                }
            }

            if (event->flags & EV_EOF) {
                iomux_close(iomux, fd);
                continue;
            }

            if (event->filter == EVFILT_WRITE) {
                iomux_write_fd(iomux, fd);
            }
        }
    }
    iomux_run_timeouts(iomux);
}

#elif defined(HAVE_EPOLL)

void
iomux_run(iomux_t *iomux, struct timeval *tv_default)
{
    int fd;

    struct timeval *tv = tv_default;

    int epoll_waiting_time = tv ? ((tv->tv_sec * 1000) + (tv->tv_usec / 1000)) : -1;
    int num_fds = iomux->maxfd - iomux->minfd + 1;
    int n = epoll_wait(iomux->efd, iomux->events, num_fds, epoll_waiting_time);
    int i;
    for (i = 0; i < n; i++) {
        if ((iomux->events[i].events & EPOLLHUP))
        {
            iomux_close(iomux, iomux->events[i].data.fd);
            continue;
        } else if ((iomux->events[i].events & EPOLLERR)) {
            fprintf (stderr, "epoll error on fd %d\n", iomux->events[i].data.fd);
            iomux_close(iomux, iomux->events[i].data.fd);
            continue;
        }

        fd  = iomux->events[i].data.fd;
        iomux_connection_t *conn = iomux->connections[fd];
        iomux_timeout_t *timeout = iomux->timeouts_fd[fd];
        if (conn) {
            if ((conn->flags&IOMUX_CONNECTION_SERVER) == (IOMUX_CONNECTION_SERVER))
            {
                iomux_accept_connections_fd(iomux, fd);
            } else {
                if (iomux->events[i].events & EPOLLIN || iomux->events[i].events & EPOLLPRI)
                {
                    iomux_read_fd(iomux, fd);
                }

                if (!iomux->connections[fd]) // connection has been closed/removed
                    continue;

                if (iomux->events[i].events& EPOLLOUT) {
                    iomux_write_fd(iomux, fd);
                }
            }
        } else if (timeout) {
            TAILQ_REMOVE(&iomux->timeouts, timeout, timeout_list);
            timeout->cb(iomux, timeout->priv);
            close(timeout->timerfd);
            free(timeout);
            iomux->timeouts_fd[fd] = NULL;
        }
    }
    iomux_update_timeouts(iomux);
}

#else

void
iomux_run(iomux_t *iomux, struct timeval *tv_default)
{
    int fd;
    fd_set rin, rout;
    int maxfd = iomux->minfd;;

    FD_ZERO(&rin);
    FD_ZERO(&rout);

    for (fd = iomux->minfd; fd <= iomux->maxfd; fd++) {
        if (iomux->connections[fd])  {
            iomux_connection_t *conn = iomux->connections[fd];
            // always register managed fds for reading (even if 
            // no mux_input callbacks is present) to detect EOF.
            FD_SET(fd, &rin);
            if (fd > maxfd)
                maxfd = fd;
            if (conn->outlen || conn->cbs.mux_output) {
                // output pending data
                FD_SET(fd, &rout);
                if (fd > maxfd)
                    maxfd = fd;
            }
        }
    }

    struct timeval *tv = iomux_adjust_timeout(iomux, tv_default);

    switch (select(maxfd+1, &rin, &rout, NULL, tv)) {
    case -1:
        if (errno == EINTR)
            return;
        if (errno == EAGAIN)
            return;
        else if (errno == EBADF) {
            // there is some bad filedescriptor amont the managed ones
            // probably the user called close() on the filedescriptor
            // without informing the iomux
            for (fd = iomux->minfd; fd <= iomux->maxfd; fd++) {
                if (iomux->connections[fd] && fcntl(fd, F_GETFD, 0) == -1)
                    iomux_close(iomux, fd);
            }
        }
        set_error(iomux, "select(): %s", strerror(errno));
        break;
    case 0:
        break;
    default:
        for (fd = iomux->minfd; fd <= iomux->maxfd; fd++) {
            if (iomux->connections[fd]) {
                if (FD_ISSET(fd, &rin)) {
                    // check if this is a listening socket
                    if ((iomux->connections[fd]->flags&IOMUX_CONNECTION_SERVER) == (IOMUX_CONNECTION_SERVER)) {
                        iomux_accept_connections_fd(iomux, fd);
                    } else {
                        iomux_read_fd(iomux, fd);
                    }
                }
                if (!iomux->connections[fd]) // connection has been closed/removed
                    continue;

                if (FD_ISSET(fd, &rout)) {
                    iomux_write_fd(iomux, fd);
                }
            }
        }
    }

    iomux_run_timeouts(iomux);
}

#endif

void
iomux_loop(iomux_t *iomux, struct timeval *tv_default)
{
    while (!iomux->leave) {
        if (iomux->loop_next_cb)
            iomux->loop_next_cb(iomux, iomux->loop_end_priv);

        iomux_run(iomux, tv_default);

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
    int free_space = IOMUX_CONNECTION_BUFSIZE-iomux->connections[fd]->outlen;
    int wlen = (len > free_space)?free_space:len;

    if (wlen) {
#if defined(HAVE_EPOLL)
        struct epoll_event event = { 0 };
        event.data.fd = fd;
        event.events = EPOLLIN | EPOLLOUT;

        int rc = epoll_ctl(iomux->efd, EPOLL_CTL_MOD, fd, &event);
        if (rc == -1) {
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

    return wlen;
}

void
iomux_close(iomux_t *iomux, int fd)
{
    iomux_connection_t *conn = iomux->connections[fd];
    if (!conn) // fd is not registered within iomux
        return;

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

}

int
iomux_isempty(iomux_t *iomux)
{
    int fd;
    for (fd = iomux->minfd; fd <= iomux->maxfd; fd++) {
        if (iomux->connections[fd]) {
            return 0;
        }
    }
    return 1;
}

int iomux_write_buffer(iomux_t *iomux, int fd)
{
    if (iomux->connections[fd])
        return IOMUX_CONNECTION_BUFSIZE-iomux->connections[fd]->outlen;
    return 0;
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
    free(iomux);
}

void
iomux_clear(iomux_t *iomux)
{
    int fd;
    iomux_timeout_t *timeout, *timeout_tmp;

    for (fd = iomux->maxfd; fd >= iomux->minfd; fd--)
        if (iomux->connections[fd])
            iomux_close(iomux, fd);

    TAILQ_FOREACH_SAFE(timeout, &iomux->timeouts, timeout_list, timeout_tmp) {
#if defined(HAVE_EPOLL)
        iomux->timeouts_fd[timeout->timerfd] = NULL;
        close(timeout->timerfd);
#endif
        TAILQ_REMOVE(&iomux->timeouts, timeout, timeout_list);
        free(timeout);
    }
}

iomux_callbacks_t *
iomux_callbacks(iomux_t *iomux, int fd)
{
    if (iomux->connections[fd])
        return &iomux->connections[fd]->cbs;
    return NULL;
}

