#pragma once

#include <errno.h> 

// Define Linux types. These are opaque handles
typedef void* io_context_t;

// The I/O Control Block structure
struct iocb {
    void* data;
    unsigned        key;
    short           aio_lio_opcode;
    short           aio_reqprio;
    int             aio_fildes;

    union {
        struct {
            void* buf;
            unsigned long nbytes;
            long long offset;
        } c;
    } u;
};

// The event structure for completed operations
struct io_event {
    void* data;
    struct iocb* obj;
    unsigned long   res;
    unsigned long   res2;
};

// The core API functions, exported with C linkage
#ifdef __cplusplus
extern "C" {
#endif

#define LIO_API __declspec(dllexport)

    LIO_API int io_setup(int maxevents, io_context_t* ctxp);
    LIO_API int io_submit(io_context_t ctx, long nr, struct iocb** iocbs);
    LIO_API int io_getevents(io_context_t ctx, long min_nr, long nr, struct io_event* events, struct timespec* timeout);
    LIO_API int io_destroy(io_context_t ctx);

#ifdef __cplusplus
}
#endif