#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "connections.h"

#include "iomux.h"

#include "testing.h"

#define TEST_STRING "CIAO"
#define TEST_SERVER_PORT   6543
#define TEST_CLIENT_PORT   6544

void test_input(IOMux *mux, int fd, void *data, int len, void *priv);
void test_timeout(IOMux *mux, int fd, void *priv);
//void test_eof(IOMux *mux, int fd, void *priv);
void test_connection(IOMux *mux, int fd, void *priv);

struct {
    int number;
    char string[256];
} test_context;

int fd1, fd2;
int client, server;

IOMuxCallbacks callbacks = 
{ 
    test_input, test_timeout, NULL, test_connection, (void *)&test_context
};

void test_input(IOMux *mux, int fd, void *data, int len, void *priv)
{
    struct timeval tv = { 1, 0 };
    fd = fd;
    priv = priv;

    if (len != strlen(TEST_STRING)) {
        t_failure("len %d should %d", len, strlen(TEST_STRING));
    } else {
        t_validate_buffer(data, len, TEST_STRING, len);
        t_testing("IOMuxSetTimeout(mux, server=%d, tv={ 1, 0 })", server);
        t_validate_int(IOMuxSetTimeout(mux, server, &tv), 1);
    }
}

void test_timeout(IOMux *mux, int fd, void *priv)
{
    //struct timeval tv = { 1, 0 };
    fd = fd;
    priv = priv;
    t_testing("IOMuxEndLoop(mux)");
    IOMuxEndLoop(mux);
}

/*
void test_eof(iomux_t *mux, int fd, void *priv)
{
    printf("Closing fildescriptor %d \n", fd);
}
*/

void test_connection(IOMux *mux, int fd, void *priv)
{
    priv = priv;
    IOMuxAdd(mux, fd, &callbacks);
}

int
main(int argc, char **argv)
{
    IOMux *mux;

    argc = argc;
    argv = argv;
    
    t_init();
     
    t_testing("IOMuxCreate()");
    mux = IOMuxCreate();
    if (mux)
        t_success();
    else
        t_failure("returned NULL");

    t_testing("opening server socket");
    server = OpenSocket("localhost", TEST_SERVER_PORT);
    if (!server) 
        t_failure("Error : %s\n", strerror(errno));
    else
        t_success();
    t_testing("IOMuxAdd(mux, server=%d)", server);
    t_validate_int(IOMuxAdd(mux, server, &callbacks), 1);
    if (!IOMuxListen(mux, server))
        exit(-1);
    
    t_testing("opening client connection");
    client = OpenConnection("localhost", TEST_SERVER_PORT, 5);
    if (!client) 
        t_failure("Error : %s\n", strerror(errno));
    else
        t_success();
    t_testing("IOMuxAdd(mux, client=%d)", client);
    t_validate_int(IOMuxAdd(mux, client, &callbacks), 1);

    t_testing("IOMuxWrite(mux, client, %s, %d)", TEST_STRING, strlen(TEST_STRING));
    t_validate_int(IOMuxWrite(mux, client, TEST_STRING, strlen(TEST_STRING)), strlen(TEST_STRING));

    t_testing("MuxInput() callback");
    IOMuxLoop(mux, 0);
    t_success();

    IOMuxDestroy(mux);

    t_summary();

    close(fd1);
    close(fd2);

    exit(0);
}
