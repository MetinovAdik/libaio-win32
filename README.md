# libaio-win32

A high-fidelity, header-compatible Windows implementation of the Linux `libaio` library, built on top of the native Windows I/O Completion Port (IOCP) API.

This project allows software originally developed for Linux that relies on `libaio` for high-performance asynchronous I/O to be compiled and run on Windows with minimal source code changes.

## Overview

Linux's Asynchronous I/O (AIO) facility, `libaio`, is a cornerstone for high-performance applications like databases, web servers, and scientific computing tools. However, this API is specific to the Linux kernel. Windows provides its own powerful, but different, asynchronous I/O model: I/O Completion Ports (IOCP).

`libaio-win32` acts as a translation layer or "shim" that exposes the `libaio` C-style API but internally drives it with a robust IOCP-based engine. This enables developers to maintain a single, clean, `libaio`-based codebase for asynchronous file I/O that can be deployed on both Linux and Windows.

## Features

This library aims for behavioral parity with the core `libaio` API, not just signature compatibility.

*   **Core API Implemented**: `io_setup`, `io_submit`, `io_getevents`, and `io_destroy`.
*   **Positional I/O**: Full support for `IO_CMD_PREAD` and `IO_CMD_PWRITE`.
*   **Vectored I/O (Scatter/Gather)**: Behaviorally-correct implementation of `IO_CMD_PREADV` and `IO_CMD_PWRITEV`. A single vectored submission correctly generates a single completion event.
*   **Filesystem Synchronization**: Support for `IO_CMD_FSYNC` and `IO_CMD_FDSYNC` to ensure data integrity.
*   **Thread-Safe**: Designed with `std::atomic` to be safe for use in multi-threaded IOCP environments.
*   **Professional Error Reporting**: Maps Windows error codes to their closest POSIX `errno` equivalents for consistent error handling.

### Current Project Status

The library is considered **feature-complete for its primary goal**. It covers the vast majority of `libaio`'s functional surface area.

Specialized features such as `io_cancel`, `IO_CMD_POLL`, and eventfd notification (`IOCB_FLAG_RESFD`) are **not implemented** due to the high complexity and low direct mappability between the Linux and Windows I/O models. See the source code for a detailed audit of implemented features.

## Getting Started

### Prerequisites

*   **Visual Studio 2019 or newer**, with the "Desktop development with C++" workload installed.
*   The project is configured to build a 64-bit (x64) DLL.

### Building the Library

1.  Clone the repository:
    ```bash
    git clone https://github.com/MetinovAdik/libaio-win32.git
    ```
2.  Open the `libaio-win32.sln` solution file in Visual Studio.
3.  Select the `Release` and `x64` configuration from the dropdown menus.
4.  Build the solution (Build > Build Solution or `Ctrl+Shift+B`).

The build artifacts will be generated in the `x64/Release/` directory:
*   `aio.dll`: The dynamic-link library.
*   `aio.lib`: The import library required by the linker.

## How to Use

To compile a Windows application against `libaio-win32`:

1.  **Include the Header**: In your C/C++ source code, include the library's header. It is recommended to add the project's root directory to your compiler's include paths.
    ```cpp
    #include "libaio_win32.h"
    ```

2.  **Link the Library**: In your project's linker settings, add `aio.lib` as an additional dependency and ensure the linker's library path points to the directory containing the `.lib` file.

3.  **Ensure DLL is Available**: The compiled `aio.dll` must be present in the same directory as your final executable or in a directory listed in the system's `PATH` environment variable for the program to run.

### Example: Porting a Linux Application

Imagine your Linux build command is:
```bash
g++ my_app.cpp -o my_app -laio
```

On Windows, using the Visual Studio `cl.exe` compiler, the equivalent would be:
```bash
cl.exe my_app.cpp /I"C:\path\to\libaio-win32" /link /LIBPATH:"C:\path\to\libaio-win32\x64\Release" aio.lib
```

## License

This project is licensed under the **MIT License**. See the `LICENSE` file for details.
