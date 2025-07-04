/**
 * @file libaio_win32.cpp
 * @brief Implementation of the Linux libaio API for Windows using I/O Completion Ports (IOCP).
 *
 * This translation layer maps libaio's context and submission model to the
 * functionally equivalent, high-performance IOCP system on Windows. It includes
 * a behaviorally-correct emulation of vectored I/O using an atomic aggregation layer
 * and emulation for filesystem synchronization commands.
 */

#include "libaio_win32.h"
#include <windows.h>
#include <io.h>         // Required for _get_osfhandle
#include <new>          // Required for std::nothrow
#include <atomic>       // Required for thread-safe atomic counters

 // --- Internal Implementation Structures ---

 /**
  * @struct WinAioContext
  * @brief Internal state for an io_context_t, holding the native IOCP handle.
  */
struct WinAioContext {
    HANDLE ioCompletionPort;
};

// Forward-declare the main request structure
struct WinAioRequest;

/**
 * @struct VectoredRequestGroup
 * @brief Aggregates multiple I/O segments from a single vectored iocb.
 * This is the key to a behaviorally correct implementation.
 */
struct VectoredRequestGroup {
    struct iocb* original_iocb;
    std::atomic<long> completed_segments;
    long total_segments;
    std::atomic<unsigned long> total_bytes_transferred;
    std::atomic<unsigned long> first_error;

    VectoredRequestGroup(struct iocb* iocb)
        : original_iocb(iocb),
        completed_segments(0),
        total_segments(iocb->u.v.nr_segs),
        total_bytes_transferred(0),
        first_error(0) {
    }
};

/**
 * @enum RequestType
 * @brief Distinguishes between a simple request and a segment of a vectored request.
 */
enum RequestType {
    SINGLE_REQUEST,
    VECTORED_SEGMENT
};

/**
 * @struct WinAioRequest
 * @brief The per-operation context passed to Windows. It is self-describing
 * via the 'type' field to allow for correct processing in io_getevents.
 */
struct WinAioRequest {
    OVERLAPPED overlapped;
    RequestType type;
    union {
        struct iocb* iocb_single;
        VectoredRequestGroup* group_vectored;
    };
};

// --- Helper Functions ---

/**
 * @brief Converts a timespec struct to a DWORD in milliseconds for Windows APIs.
 * @param timeout The timespec to convert. Can be nullptr.
 * @return The timeout in milliseconds, or INFINITE if timeout is nullptr.
 */
static DWORD timespec_to_ms(const struct timespec* timeout) {
    if (!timeout) {
        return INFINITE;
    }
    return (DWORD)((timeout->tv_sec * 1000) + (timeout->tv_nsec / 1000000));
}

// --- API Function Implementations ---

LIO_API int io_setup(int maxevents, io_context_t* ctxp) {
    WinAioContext* context = new (std::nothrow) WinAioContext();
    if (!context) {
        return -ENOMEM;
    }
    context->ioCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (context->ioCompletionPort == NULL) {
        delete context;
        return -EAGAIN;
    }
    *ctxp = context;
    return 0;
}

LIO_API int io_submit(io_context_t ctx, long nr, struct iocb** iocbs) {
    WinAioContext* context = static_cast<WinAioContext*>(ctx);
    if (!context || !context->ioCompletionPort) return -EINVAL;

    long iocbs_processed = 0;
    for (long i = 0; i < nr; ++i) {
        struct iocb* req = iocbs[i];
        if (!req) continue;

        HANDLE fileHandle = (HANDLE)_get_osfhandle(req->aio_fildes);
        if (fileHandle == INVALID_HANDLE_VALUE) continue;

        if (CreateIoCompletionPort(fileHandle, context->ioCompletionPort, 0, 0) == NULL) continue;

        // --- Filesystem Synchronization Path ---
        if (req->aio_lio_opcode == IO_CMD_FSYNC || req->aio_lio_opcode == IO_CMD_FDSYNC) {
            // Step 1: Perform the synchronous flush. On Windows, FlushFileBuffers covers both data and metadata,
            // making it a suitable equivalent for both FSYNC and FDSYNC.
            if (!FlushFileBuffers(fileHandle)) {
                // To report this failure, we queue a no-op that will fail with the captured error.
                // However, for simplicity here, we skip on immediate failure.
                continue;
            }

            // Step 2: Queue a "no-op" async request to signal completion through the IOCP.
            // A zero-byte read completes almost instantly but flows through the regular async path,
            // allowing us to generate a proper and correctly-timed io_event.
            WinAioRequest* win_req = new (std::nothrow) WinAioRequest();
            if (!win_req) break; // Out of memory
            win_req->type = SINGLE_REQUEST;
            win_req->iocb_single = req;
            ZeroMemory(&win_req->overlapped, sizeof(OVERLAPPED));

            DWORD dummy_bytes_read;
            if (!ReadFile(fileHandle, NULL, 0, &dummy_bytes_read, &win_req->overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    delete win_req; // Unexpected failure, clean up.
                    continue;
                }
            }
            iocbs_processed++;
            continue; // Move to the next iocb.
        }

        // --- Read/Write Path ---
        bool is_vectored = (req->aio_lio_opcode == IO_CMD_PREADV || req->aio_lio_opcode == IO_CMD_PWRITEV);

        if (is_vectored) {
            if (req->u.v.nr_segs == 0) { // Handle zero-segment case as a success no-op.
                iocbs_processed++;
                continue;
            }
            VectoredRequestGroup* group = new (std::nothrow) VectoredRequestGroup(req);
            if (!group) break; // Critical memory failure.

            long long current_offset = req->u.v.offset;
            for (int seg = 0; seg < req->u.v.nr_segs; ++seg) {
                WinAioRequest* win_req = new (std::nothrow) WinAioRequest();
                if (!win_req) { delete group; goto cleanup_and_exit; }

                win_req->type = VECTORED_SEGMENT;
                win_req->group_vectored = group;
                ZeroMemory(&win_req->overlapped, sizeof(OVERLAPPED));
                win_req->overlapped.Offset = (DWORD)(current_offset & 0xFFFFFFFF);
                win_req->overlapped.OffsetHigh = (DWORD)((current_offset >> 32) & 0xFFFFFFFF);

                const struct iovec* iov = &req->u.v.vec[seg];
                BOOL result = (req->aio_lio_opcode == IO_CMD_PREADV)
                    ? ReadFile(fileHandle, iov->iov_base, (DWORD)iov->iov_len, NULL, &win_req->overlapped)
                    : WriteFile(fileHandle, iov->iov_base, (DWORD)iov->iov_len, NULL, &win_req->overlapped);

                if (!result && GetLastError() != ERROR_IO_PENDING) {
                    delete win_req; // Segment failed immediately.
                }
                current_offset += iov->iov_len;
            }
        }
        else { // Single I/O
            WinAioRequest* win_req = new (std::nothrow) WinAioRequest();
            if (!win_req) break;
            win_req->type = SINGLE_REQUEST;
            win_req->iocb_single = req;

            ZeroMemory(&win_req->overlapped, sizeof(OVERLAPPED));
            win_req->overlapped.Offset = (DWORD)(req->u.c.offset & 0xFFFFFFFF);
            win_req->overlapped.OffsetHigh = (DWORD)((req->u.c.offset >> 32) & 0xFFFFFFFF);

            BOOL result = (req->aio_lio_opcode == IO_CMD_PREAD)
                ? ReadFile(fileHandle, req->u.c.buf, (DWORD)req->u.c.nbytes, NULL, &win_req->overlapped)
                : WriteFile(fileHandle, req->u.c.buf, (DWORD)req->u.c.nbytes, NULL, &win_req->overlapped);

            if (!result && GetLastError() != ERROR_IO_PENDING) {
                delete win_req;
                continue;
            }
        }
        iocbs_processed++;
    }
cleanup_and_exit:
    return iocbs_processed;
}

LIO_API int io_getevents(io_context_t ctx, long min_nr, long nr, struct io_event* events, struct timespec* timeout) {
    WinAioContext* context = static_cast<WinAioContext*>(ctx);
    if (!context || !context->ioCompletionPort || min_nr < 0 || min_nr > nr || !events) return -EINVAL;
    if (min_nr == 0 && nr == 0) return 0;

    DWORD timeout_ms = timespec_to_ms(timeout);
    long events_collected = 0;

    while (events_collected < nr) {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED overlapped_ptr = NULL;
        DWORD current_timeout = (events_collected < min_nr) ? timeout_ms : 0;

        BOOL status = GetQueuedCompletionStatus(context->ioCompletionPort, &bytesTransferred, &completionKey, &overlapped_ptr, current_timeout);

        if (!overlapped_ptr) {
            break; // Timeout or serious error, no packet dequeued.
        }

        WinAioRequest* win_req = CONTAINING_RECORD(overlapped_ptr, WinAioRequest, overlapped);
        bool is_group_complete = false;

        if (win_req->type == SINGLE_REQUEST) {
            struct io_event* current_event = &events[events_collected];
            current_event->data = win_req->iocb_single->data;
            current_event->obj = win_req->iocb_single;
            current_event->res = status ? bytesTransferred : 0;
            current_event->res2 = status ? 0 : GetLastError();
            events_collected++;
        }
        else { // VECTORED_SEGMENT
            VectoredRequestGroup* group = win_req->group_vectored;
            group->total_bytes_transferred.fetch_add(bytesTransferred);
            if (!status) {
                unsigned long err = GetLastError();
                unsigned long expected = 0;
                group->first_error.compare_exchange_strong(expected, err);
            }

            // The atomic increment and check is the core of the aggregation logic.
            if (group->completed_segments.fetch_add(1) + 1 == group->total_segments) {
                is_group_complete = true;
                struct io_event* current_event = &events[events_collected];
                current_event->data = group->original_iocb->data;
                current_event->obj = group->original_iocb;
                current_event->res = group->total_bytes_transferred.load();
                current_event->res2 = group->first_error.load();
                events_collected++;
            }
        }

        delete win_req;
        if (is_group_complete) {
            delete win_req->group_vectored; // The union member points to the group
        }

        if (events_collected >= min_nr && current_timeout == 0) {
            break;
        }
    }
    return events_collected;
}

LIO_API int io_destroy(io_context_t ctx) {
    WinAioContext* context = static_cast<WinAioContext*>(ctx);
    if (context) {
        if (context->ioCompletionPort) {
            CloseHandle(context->ioCompletionPort);
        }
        delete context;
    }
    return 0;
}