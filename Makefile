UNAME := $(shell uname)

LDFLAGS += -L.

ifeq ($(UNAME), Linux)
LDFLAGS += -pthread
else
LDFLAGS +=
endif

ifeq ($(UNAME), Darwin)
SHAREDFLAGS = -dynamiclib
SHAREDEXT = dylib
else
SHAREDFLAGS = -shared
SHAREDEXT = so
endif

ifeq ("$(LIBDIR)", "")
LIBDIR=/usr/local/lib
endif

ifeq ("$(INCDIR)", "")
INCDIR=/usr/local/include
endif


#CC = gcc
TARGETS = $(patsubst %.c, %.o, $(wildcard src/*.c))

TESTS = $(patsubst %.c, %, $(wildcard test/*.c))
TEST_EXEC_ORDER =  iomux_test

all: objects static shared

.PHONY: static
static: objects
	ar -r libiomux.a src/*.o

shared: objects
	$(CC) $(LDFLAGS) $(SHAREDFLAGS) src/*.o -o libiomux.$(SHAREDEXT)

.PHONY: objects
objects: CFLAGS += -fPIC -Isrc -Wall -Werror -Wno-parentheses -Wno-pointer-sign -DTHREAD_SAFE -g -O3
objects: $(TARGETS)

clean:
	rm -f src/*.o
	rm -f test/*_test
	rm -f libiomux.a
	rm -f libiomux.$(SHAREDEXT)

.PHONY: libut
libut:
	@cd support/libut; make

.PHONY: tests
tests: CFLAGS += -Isrc -Isupport/libut/src -Wall -Werror -Wno-parentheses -Wno-pointer-sign -DTHREAD_SAFE -g -O3
tests: libut static
	@for i in $(TESTS); do\
	  echo "$(CC) $(CFLAGS) $$i.c -o $$i libiomux.a $(LDFLAGS) -lm";\
	  $(CC) $(CFLAGS) $$i.c -o $$i libiomux.a support/libut/libut.a $(LDFLAGS) -lm;\
	done;\
	for i in $(TEST_EXEC_ORDER); do echo; test/$$i; echo; done

.PHONY: test
test: tests

install:
	 @echo "Installing libraries in $(LIBDIR)"; \
	 cp -v libiomux.a $(LIBDIR)/;\
	 cp -v libiomux.$(SHAREDEXT) $(LIBDIR)/;\
	 echo "Installing headers in $(INCDIR)"; \
	 cp -v src/*.h $(INCDIR)/;
