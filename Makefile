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
SOURCES = $(wildcard src/*.c)

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
objects: 
	@if [ "X$$USE_SELECT" = "X" ]; then \
	    UNAME=`uname`; \
	    if [ "$$UNAME" = "Darwin" ]; then \
		PLATFORM_CFLAGS="-DHAVE_KQUEUE"; \
	    elif [ "$$UNAME" = "Linux" ]; then \
		KERNEL_VERSION=`uname -r | cut -d- -f1`; \
		MIN_VERSION=2.6.27; \
		SMALLER_VERSION=`echo "$$KERNEL_VERSION\n$$MIN_VERSION" | sort -V | head -1`; \
		if [ "$$SMALLER_VERSION" = "$$MIN_VERSION" ]; then \
		    PLATFORM_CFLAGS="-DHAVE_EPOLL"; \
		fi; \
	    fi; \
	fi; \
	for i in $(SOURCES); do \
	    echo "$(CC) $(CFLAGS) $$PLATFORM_CFLAGS -c $$i -o $${i%.*}.o"; \
	    $(CC) $(CFLAGS) $$PLATFORM_CFLAGS -c $$i -o $${i%.*}.o; \
	done

clean:
	rm -f src/*.o
	rm -f test/*_test
	rm -f libiomux.a
	rm -f libiomux.$(SHAREDEXT)

.PHONY: libut
libut:
	@if [ ! -f support/libut/Makefile ]; then git submodule init; git submodule update; fi; make -C support/libut

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
