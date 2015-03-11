#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>


#include <sys/stat.h>
#include <sys/types.h>

#include <libgen.h>

#include "iomux.h"

#include "ut.h"

#define TEST_STRING "CIAO"
#define TEST_SERVER_PORT   6543
#define TEST_CLIENT_PORT   6544

int test_input(iomux_t *mux, int fd, unsigned char *data, int len, void *priv);
void test_timeout(iomux_t *mux, int fd, void *priv);
//void test_eof(iomux_t *mux, int fd, void *priv);
void test_connection(iomux_t *mux, int fd, void *priv);

struct {
    int number;
    char string[256];
} test_context;

int client, server;

iomux_callbacks_t callbacks = 
{ 
    test_input, NULL, test_timeout, NULL, test_connection, (void *)&test_context
};

static int
string2sockaddr(const char *host, int port, struct sockaddr_in *sockaddr)
{
    u_int32_t ip = htonl(INADDR_LOOPBACK);
    errno = EINVAL;

    if (host) {
        char host2[512];
        char *p;
        char *pe;

        strncpy(host2, host, sizeof(host2)-1);
        p = strchr(host2, ':');

        if (p) {                // check for <host>:<port>
            *p = '\0';                // point to port part
            p++;
            port = strtol(p, &pe, 10);        // convert string to number
            if (*pe != '\0') {            // did not match complete string? try as string
#if (defined(__APPLE__) && defined(__MACH__))
                struct servent *e = getservbyname(p, "tcp");
#else
                struct servent *e = NULL, ebuf;
                char buf[1024];
                getservbyname_r(p, "tcp", &ebuf, buf, sizeof(buf), &e);
#endif
                if (!e) {
                    errno = ENOENT;        // to avoid errno == 0 in error case
                    return -1;
                }
                port = ntohs(e->s_port);
            }
        }

        if (strcmp(host2, "*") == 0) {
            ip = INADDR_ANY;
        } else {
            if (!inet_aton(host2, (struct in_addr *)&ip)) {

                struct hostent *e = NULL;
#if (defined(__APPLE__) && defined(__MACH__))
                e = gethostbyname(host2);
#else
                struct hostent ebuf;
                char buf[1024];
                int herrno;
                gethostbyname_r(host2, &ebuf, buf, sizeof(buf), &e, &herrno);
#endif
                if (!e || e->h_addrtype != AF_INET) {
                    errno = ENOENT;        // to avoid errno == 0 in error case
                    return -1;
                }
                ip = ((unsigned long *) (e->h_addr_list[0]))[0];
            }
        }
    }
    if (port == 0)
        return -1;
    else
        port = htons(port);

    bzero(sockaddr, sizeof(struct sockaddr_in));
#ifndef __linux
    sockaddr->sin_len = sizeof(struct sockaddr_in);
#endif
    sockaddr->sin_family = AF_INET;
    sockaddr->sin_addr.s_addr = ip;
    sockaddr->sin_port = port;

    return 0;
}

static int
open_socket(const char *host, int port)
{
    int val = 1;
    struct sockaddr_in sockaddr;
    int sock;
    struct linger ling = {0, 0};

    errno = EINVAL;
    if (host == NULL || strlen(host) == 0 || port == 0)
    return -1;

    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == -1)
        return -1;

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &val,  sizeof(val));
    setsockopt(sock, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));

    if (string2sockaddr(host, port, &sockaddr) == -1
    || bind(sock, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) == -1)
    {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        return -1;
    }

    listen(sock, -1);
    fcntl(sock, F_SETFD, FD_CLOEXEC);

    return sock;
}

static int
open_connection(const char *host, int port, unsigned int timeout)
{
    int val = 1;
    struct sockaddr_in sockaddr;
    int sock;

    errno = EINVAL;
    if (host == NULL || strlen(host) == 0 || port == 0)
    return -1;

    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == -1)
    return -1;

    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &val,  sizeof(val));
    if (timeout > 0) {
    struct timeval tv = { timeout, 0 };
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1
        || setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1)
        fprintf(stderr, "%s:%d: Failed to set timeout to %d\n", host, port, timeout);
    }

    if (string2sockaddr(host, port, &sockaddr) == -1 ||
        connect(sock, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) == -1)
    {
        shutdown(sock, SHUT_RDWR);
        close(sock);
        return -1;
    }

    fcntl(sock, F_SETFD, FD_CLOEXEC);

    return sock;
}




int test_input(iomux_t *mux, int fd, unsigned char *data, int len, void *priv)
{
    struct timeval tv = { 0, 5000 };

    if (len != strlen(TEST_STRING)) {
        ut_failure("len %d should %d", len, strlen(TEST_STRING));
    } else {
        ut_validate_buffer(data, len, TEST_STRING, len);
        iomux_set_timeout(mux, server, &tv); // if this works the event loop will stop
    }
    return len;
}

void test_timeout(iomux_t *mux, int fd, void *priv)
{
    //struct timeval tv = { 1, 0 };
    ut_testing("iomux_end_loop(mux)");
    iomux_end_loop(mux);
}

void test_timeout_nofd(iomux_t *mux, void *priv)
{
    int *cnt = (int *)priv;
    (*cnt)++;
    iomux_end_loop(mux);
}

/*
void test_eof(iomux_t *mux, int fd, void *priv)
{
    printf("Closing fildescriptor %d \n", fd);
}
*/

void test_connection(iomux_t *mux, int fd, void *priv)
{
    iomux_add(mux, fd, &callbacks);
}

static void loop_end(iomux_t *mux, void *priv)
{
    ut_success();
}

static void loop_hangup(iomux_t *mux, void *priv)
{
    ut_success();
    ut_testing("iomux_loop_end() callback");
    iomux_end_loop(mux);
}

static void loop_next(iomux_t *mux, void *priv)
{
    ut_success();
    ut_testing("iomux_hangup() callback");
    iomux_hangup = 1;
}

void test_mtee_connection(iomux_t *mux, int fd, void *priv)
{
    iomux_callbacks_t *cbs = (iomux_callbacks_t *)priv;
    iomux_add(mux, fd, cbs);
}

int test_mtee_input(iomux_t *iomux, int fd, unsigned char *data, int len, void *priv)
{
    int *count = (int *)priv;
    if (len == 4 && memcmp(data, "CIAO", 4) == 0)
        (*count)++;
    if (*count >= 2)
        iomux_end_loop(iomux);
    return len;
}

int
main(int argc, char **argv)
{
    iomux_t *mux;

    ut_init(basename(argv[0]));
     
    ut_testing("iomux_create(0, 0)");
    mux = iomux_create(0, 0);
    if (mux)
        ut_success();
    else
        ut_failure("returned NULL");

    ut_testing("opening server socket");
    server = open_socket("localhost", TEST_SERVER_PORT);
    if (!server) 
        ut_failure("Error : %s\n", strerror(errno));
    else
        ut_success();
    ut_testing("iomux_add(mux, server=%d)", server);
    ut_validate_int(iomux_add(mux, server, &callbacks), 1);
    if (!iomux_listen(mux, server))
        exit(-1);

    ut_testing("opening client connection");
    client = open_connection("localhost", TEST_SERVER_PORT, 5);
    if (!client) 
        ut_failure("Error : %s\n", strerror(errno));
    else
        ut_success();
    ut_testing("iomux_add(mux, client=%d)", client);
    ut_validate_int(iomux_add(mux, client, &callbacks), 1);

    ut_testing("iomux_write(mux, client, %s, %d)", TEST_STRING, strlen(TEST_STRING));
    ut_validate_int(iomux_write(mux, client, TEST_STRING, strlen(TEST_STRING), IOMUX_OUTPUT_MODE_NONE), strlen(TEST_STRING));

    ut_testing("iomux_input_callback() callback");
    iomux_loop(mux, NULL);
    ut_success();

    struct timeval tv = { 0, 5000 };

    int cnt = 0;
    ut_testing("iomux_schedule()");
    uint64_t timerid = iomux_schedule(mux, &tv, test_timeout_nofd, &cnt, NULL);
    if (timerid > 0)
        ut_success();
    else
        ut_failure("Can't obtain a timer id");

    ut_testing("iomux_unschedule()");
    ut_validate_int(iomux_unschedule(mux, timerid), 1);

    ut_testing("timer runs");
    timerid = iomux_schedule(mux, &tv, test_timeout_nofd, &cnt, NULL);
    iomux_loop(mux, NULL);
    ut_validate_int(cnt, 1);

    iomux_loop_next_cb(mux, loop_next, NULL);
    iomux_loop_end_cb(mux, loop_end, NULL);
    iomux_hangup_cb(mux, loop_hangup, NULL);

    ut_testing("iomux_loop_next() callback");

    iomux_loop(mux, &tv);

    
    iomux_t *mux2 = iomux_create(0, 0);
    iomux_move(mux, mux2);

    ut_testing("iomux_write(mux2, client, %s, %d)", TEST_STRING, strlen(TEST_STRING));
    ut_validate_int(iomux_write(mux2, client, TEST_STRING, strlen(TEST_STRING), IOMUX_OUTPUT_MODE_NONE), strlen(TEST_STRING));

    ut_testing("iomux_input_callback() callback");
    iomux_loop(mux2, NULL);
    ut_success();

    iomux_destroy(mux2);
    iomux_destroy(mux);

    close(server);
    close(client);

#ifndef NO_PTHREAD

    int count = 0;

    iomux_callbacks_t icbs = {
        .mux_input = test_mtee_input,
        .priv = &count
    };

    iomux_callbacks_t cbs = {
        .mux_connection = test_mtee_connection,
        .priv = &icbs
    };

    server = open_socket("localhost", TEST_SERVER_PORT);
    mux = iomux_create(0, 0);
    iomux_add(mux, server, &cbs);
    iomux_listen(mux, server);

    client = open_connection("localhost", TEST_SERVER_PORT, 5);
    int client2 = open_connection("localhost", TEST_SERVER_PORT, 5);

    int tee_fd;
    ut_testing("iomtee_open(&tee_fd, 2, client, client2)");
    iomtee_t *tee = iomtee_open(&tee_fd, 2, client, client2);
    ut_validate_int((tee_fd >= 0), 1);

    int rc = write(tee_fd, "CIAO", 4);
    if (rc != 4) {
        printf("Can't write to tee_fd: %s\n", strerror(errno));
        exit(-1);
    }
    ut_testing("write(tee_fd, \"CIAO\", 4)");

    iomux_loop(mux, &tv);

    ut_validate_int(count, 2);

    ut_testing("close(client); write(tee_fd, \"CIAO\", 4)");
    // closing one of the endpoints, the tee still works 
    // but this time only one receiver will be notified
    close(client);
    rc = write(tee_fd, "CIAO", 4);
    if (rc != 4) {
        printf("Can't write to tee_fd: %s\n", strerror(errno));
        exit(-1);
    }
    iomux_loop(mux, &tv);
    ut_validate_int(count, 3);

    int pfd[2];
    rc = pipe(pfd);
    if (rc != 0) {
        printf("Can't create pipe: %s\n", strerror(errno));
        exit(-1);
    }
    iomtee_add_fd(tee, pfd[1]);
    rc = write(tee_fd, "TEST", 4);
    if (rc != 4) {
        printf("Can't write to tee_fd: %s\n", strerror(errno));
        exit(-1);
    }
    char buf[4];
    int rb = read(pfd[0], buf, 4);
    ut_testing("iomtee: dynamically added fd receives bytes");
    ut_validate_int(rb, 4);
    ut_testing("iomtee: dynamically added fd receives the correct bytes");
    ut_validate_buffer(buf, rb, "TEST", 4);
    iomux_destroy(mux);
    iomtee_close(tee);
    close(pfd[0]);
    close(pfd[1]);
    close(server);
    close(client2);
#endif

    ut_summary();

    exit(ut_failed);
}
