#pragma once

#include <errno.h>      // For standard error codes like ENOMEM
#include <time.h>       // For the timespec struct used in io_getevents

/**
 * @file libaio_win32.h
 * @brief Public API header for a Windows compatibility layer for Linux's libaio.
 *
 * This header replicates the essential types and function signatures from the
 * Linux libaio.h to allow for source-level compatibility for applications
 * being ported from Linux to Windows.
 */

 /// Opaque handle representing an asynchronous I/O context.
typedef void* io_context_t;

/**
 * @struct iocb
 * @brief I/O Control Block. Describes a single asynchronous I/O operation.
 *
 * This structure is the primary means of submitting a request. The user populates
 * it with details of the operation (e.g., read/write, buffer, size, offset).
 */
struct iocb {
    void* data;           ///< User-defined data. Returned verbatim in the corresponding io_event.
    unsigned        key;            ///< (Unused in this implementation)
    short           aio_lio_opcode; ///< The I/O command (e.g., IO_CMD_PREAD, IO_CMD_PWRITE).
    short           aio_reqprio;    ///< I/O request priority. (Unused in this implementation)
    int             aio_fildes;     ///< The file descriptor for the I/O operation.

    union {
        struct {
            void* buf;            ///< The buffer for the I/O operation.
            unsigned long nbytes;   ///< The number of bytes to transfer.
            long long offset;       ///< The absolute offset in the file to start the I/O.
        } c; // "c" for common control block operations
    } u;
};

/**
 * @struct io_event
 * @brief Represents a completed I/O operation.
 *
 * This structure is populated by io_getevents with the results of a completed request.
 */
struct io_event {
    void* data;   ///< The user-defined data from the source iocb.
    struct iocb* obj;    ///< A pointer to the source iocb.
    unsigned long   res;    ///< The result of the operation (e.g., bytes transferred).
    unsigned long   res2;   ///< The error code of the operation (0 on success).
};

/// Defines the supported libaio command opcodes.
enum {
    IO_CMD_PREAD = 0,       ///< Positional read operation.
    IO_CMD_PWRITE = 1,      ///< Positional write operation.
};


// C-style linkage is required for the DLL to be compatible with C and other languages.
#ifdef __cplusplus
extern "C" {
#endif

    // LIO_API is used to mark functions for export from the DLL.
#define LIO_API __declspec(dllexport)

/**
 * @brief Creates an asynchronous I/O context.
 * @param maxevents The maximum number of concurrent events the context can handle.
 * @param ctxp A pointer that will receive the new io_context_t handle.
 * @return 0 on success, or a negative errno value on failure.
 */
    LIO_API int io_setup(int maxevents, io_context_t* ctxp);

    /**
     * @brief Submits one or more asynchronous I/O operations.
     * @param ctx The I/O context to which to submit the requests.
     * @param nr The number of requests to submit.
     * @param iocbs An array of pointers to iocb structures.
     * @return The number of requests successfully submitted, or a negative errno value on failure.
     */
    LIO_API int io_submit(io_context_t ctx, long nr, struct iocb** iocbs);

    /**
     * @brief Attempts to read completed I/O events from the completion queue.
     * @param ctx The I/O context to query.
     * @param min_nr The minimum number of events to retrieve.
     * @param nr The maximum number of events to retrieve.
     * @param events A user-provided array to be filled with completed io_event structures.
     * @param timeout The maximum time to wait for events. A null pointer means wait indefinitely.
     * @return The number of events read (>= min_nr), or a negative errno value on failure.
     */
    LIO_API int io_getevents(io_context_t ctx, long min_nr, long nr, struct io_event* events, struct timespec* timeout);

    /**
     * @brief Destroys an asynchronous I/O context and releases its resources.
     * @param ctx The I/O context to destroy.
     * @return 0 on success, or a negative errno value on failure.
     */
    LIO_API int io_destroy(io_context_t ctx);

#ifdef __cplusplus
}
#endif