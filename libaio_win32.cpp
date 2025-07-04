/**
 * @file libaio_win32.cpp
 * @brief Implementation of the Linux libaio API for Windows using I/O Completion Ports (IOCP).
 *
 * This translation layer maps libaio's context and submission model to the
 * functionally equivalent, high-performance IOCP system on Windows.
 */

#include "libaio_win32.h"
#include <windows.h>
#include <io.h>         // Required for _get_osfhandle
#include <new>          // Required for std::nothrow

 // --- Internal Implementation Structures ---

 /**
  * @struct WinAioContext
  * @brief Internal state for an io_context_t.
  */
struct WinAioContext {
    HANDLE ioCompletionPort;
};

/**
 * @struct WinAioRequest
 * @brief Per-operation context structure linking Windows OVERLAPPED and the original iocb.
 */
struct WinAioRequest {
    OVERLAPPED overlapped;
    struct iocb* original_iocb;
};

// --- Helper Functions ---

/**
 * @brief Converts a timespec struct to a DWORD in milliseconds.
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
    if (!context || !context->ioCompletionPort) {
        return -EINVAL;
    }

    long submitted_count = 0;
    for (long i = 0; i < nr; ++i) {
        struct iocb* req = iocbs[i];
        if (!req) continue;

        HANDLE fileHandle = (HANDLE)_get_osfhandle(req->aio_fildes);
        if (fileHandle == INVALID_HANDLE_VALUE) continue;

        if (CreateIoCompletionPort(fileHandle, context->ioCompletionPort, (ULONG_PTR)0, 0) == NULL) {
            continue;
        }

        WinAioRequest* win_req = new (std::nothrow) WinAioRequest();
        if (!win_req) break;
        win_req->original_iocb = req;

        ZeroMemory(&win_req->overlapped, sizeof(OVERLAPPED));
        win_req->overlapped.Offset = (DWORD)(req->u.c.offset & 0xFFFFFFFF);
        win_req->overlapped.OffsetHigh = (DWORD)((req->u.c.offset >> 32) & 0xFFFFFFFF);

        BOOL result = FALSE;
        if (req->aio_lio_opcode == IO_CMD_PREAD) {
            result = ReadFile(fileHandle, req->u.c.buf, (DWORD)req->u.c.nbytes, NULL, &win_req->overlapped);
        }
        else if (req->aio_lio_opcode == IO_CMD_PWRITE) {
            result = WriteFile(fileHandle, req->u.c.buf, (DWORD)req->u.c.nbytes, NULL, &win_req->overlapped);
        }
        else {
            delete win_req;
            continue;
        }

        if (!result) {
            if (GetLastError() != ERROR_IO_PENDING) {
                delete win_req;
                continue;
            }
        }
        submitted_count++;
    }
    return submitted_count;
}

LIO_API int io_getevents(io_context_t ctx, long min_nr, long nr, struct io_event* events, struct timespec* timeout) {
    WinAioContext* context = static_cast<WinAioContext*>(ctx);
    if (!context || !context->ioCompletionPort || min_nr < 0 || min_nr > nr || !events) {
        return -EINVAL;
    }
    if (min_nr == 0 && nr == 0) {
        return 0;
    }

    DWORD timeout_ms = timespec_to_ms(timeout);
    long events_collected = 0;

    // The main loop for retrieving completed events.
    while (events_collected < nr) {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED overlapped_ptr = NULL;

        // Determine the timeout for this specific call.
        // If we haven't met min_nr yet, we must wait.
        // If we have met min_nr, we should not wait any longer (timeout = 0)
        // and just retrieve any events that are already complete.
        DWORD current_timeout = (events_collected < min_nr) ? timeout_ms : 0;

        BOOL result = GetQueuedCompletionStatus(
            context->ioCompletionPort,
            &bytesTransferred,
            &completionKey,
            &overlapped_ptr,
            current_timeout
        );

        if (!result && overlapped_ptr == NULL) {
            // No packet was dequeued. This is either a timeout or a serious error.
            if (GetLastError() == WAIT_TIMEOUT) {
                // This is not an error, we just didn't get an event in time.
                // We break here because we should not wait again.
                break;
            }
            else {
                // A more serious error with the IOCP itself.
                return -EIO;
            }
        }

        // A completion packet was dequeued (even if the I/O operation itself failed).
        // Retrieve our custom request structure from the OVERLAPPED pointer.
        WinAioRequest* win_req = CONTAINING_RECORD(overlapped_ptr, WinAioRequest, overlapped);

        // Populate the user's io_event structure.
        struct io_event* current_event = &events[events_collected];
        current_event->data = win_req->original_iocb->data;
        current_event->obj = win_req->original_iocb;

        if (result) {
            // I/O was successful.
            current_event->res = bytesTransferred;
            current_event->res2 = 0; // No error
        }
        else {
            // I/O failed. GetLastError() holds the error code for the specific I/O.
            current_event->res = 0;
            current_event->res2 = GetLastError();
        }

        // The operation is complete, so we must free the request structure we allocated in io_submit.
        delete win_req;
        events_collected++;

        // If we've met the minimum, and there are no more waiting events, we can stop.
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