libiomux
========

iomux - High-performance aynchronous I/O multiplexing and timers

C library initially inspired to the perl module IO::Multiplex
( https://metacpan.org/pod/IO::Multiplex )
This library allows to efficiently handle i/o from/to multiple
filedescriptors and timers.
It supports different backends which can be chosen at compile time.
The default is select() but support for epoll() and kqueue() is available
(by defining HAVE_EPOLL or HAVE_QUEUE respectively at compile time).

A single mux is able to handle efficiently tens of thousands of active
filedescriptors, and multiple muxes can be used seemlessy
by different threads.

Timers are implemented using a priority queue to ensure O(1) extraction of the 
earliest timer. Insertion and deletion are still O(logN) operations and should
be performed wisely when using a huge amount of timers.

Timeout on filedescriptors are not going into the priority queue but are handled
together with the i/o so (re/un)setting a timeout on a managed filedescriptor
is a very cheap operation.


