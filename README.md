# Multithreaded HTTP/1.0 Web Server in C

This project implements a concurrent HTTP/1.0 web server written from scratch in C, relying only on the POSIX Socket API and pthreads, with no external HTTP library involved. It was built as coursework for the Computer Networks course and aims to reconstruct, end to end, the chain that connects raw sockets to the HTTP/1.0 application protocol, the layer that is normally hidden behind software such as Apache or nginx.

## Overview

The server listens for TCP connections on a local port using the standard socket primitives (`socket`, `bind`, `listen`, `accept`, `send`, `recv`, `close`), and implements a functional subset of HTTP/1.0 limited to the GET method and the delivery of static HTML pages. Concurrency is handled through POSIX threads, with every connection served by its own dedicated thread. On top of this, the server includes a set of robustness controls, such as proper handling of partial sends and centralized resource cleanup, along with a basic security control against directory traversal attempts.

## Architecture

The server keeps a clear separation between accepting connections and handling them. The main thread is responsible for the entire setup phase: it creates the passive socket, enables address reuse through `setsockopt` with `SO_REUSEADDR`, binds it to the listening port, and puts it into listening mode. Once this is done, it enters a loop around a blocking call to `accept`. Every time a new connection arrives, the file descriptor is copied onto the heap with `malloc`, a new worker thread is created with `pthread_create`, and `pthread_detach` is called immediately afterward so the thread's resources are reclaimed automatically once it finishes, without the main thread ever calling `pthread_join`. This keeps the main thread free to return to `accept` as quickly as possible, and it never touches application level logic itself.

All protocol handling happens inside the worker thread, in the function `gestisci_client`. Each worker owns the full lifecycle of a single HTTP connection: it receives the request through `recv`, parses the request line with `strtok_r`, chosen specifically because it is reentrant and therefore safe to use across threads running concurrently, validates the requested resource, opens it if it exists, and streams the response back to the client. Because every worker allocates its buffers on its own stack, there is no shared mutable state between connections and therefore no need for locks or synchronization on the hot path.

A support function, `invia_completo`, wraps `send` in a loop that tracks how many bytes have actually been transmitted against the total expected, since a single call to `send` is not guaranteed to write an entire buffer at once, particularly when the kernel's transport buffers are close to full. The same idea applies when a file is streamed back to the client: it is read from disk in fixed blocks of 1024 bytes rather than being loaded into memory all at once, which keeps the process's memory footprint constant no matter how large the file being served is, and each block is fully sent before the next one is read.

## Request Handling

Once a request reaches a worker thread, the server responds according to what it finds. A request line that cannot be parsed, for instance because the method or the URL is missing, results in a 400 Bad Request response. If the method extracted from the request is anything other than GET, the server replies with 501 Not Implemented, since no other method is supported.

Before any filesystem access takes place, the URL is checked for directory traversal attempts, as described in the next section, and requests that fail this check are rejected immediately with 403 Forbidden. Otherwise, the URL is normalized: a request for the root path is mapped to `index.html`, and the leading slash is stripped before the file is looked up relative to the server's working directory. If the requested file exists, it is opened and streamed back with a 200 OK status. If it does not exist, the server attempts to serve a custom `404.html` page with a 404 Not Found status, and if even that file is missing, it falls back to a minimal hardcoded HTML response so the connection never hangs or closes inconsistently.

## Security and Robustness

Because the server accepts arbitrary input from the network, request handling includes several defensive checks. The most important one targets directory traversal: before opening any file, the parser scans the requested URL for the sequence `..` using `strstr`, and rejects the request with 403 Forbidden the instant it finds one, before any call to `fopen` takes place. Absolute paths are handled the same way: after the leading slash is stripped from the URL, if another slash remains at the start, as would happen with a request for `//etc/passwd`, the request is treated as an attempt to escape the server's root directory and is rejected with the same status code.

File paths are also assembled exclusively through `snprintf` rather than unsafe string concatenation, which enforces the fixed size of the destination buffer and prevents buffer overflows even when a malformed or intentionally oversized path is supplied.

On the concurrency side, the main risk in a multithreaded server like this one is a race condition on the client file descriptor. If the main thread passed the address of its own local variable to each new worker, that variable would be overwritten on the next iteration of the accept loop before the worker had a chance to read it. Allocating the descriptor on the heap for every connection avoids this entirely, at the cost of an equally important obligation: releasing that memory once it is no longer needed. This is handled by copying the descriptor's value at the very start of the worker function and freeing the pointer immediately afterward, so the heap allocation never outlives its single use. On the same front, calling `pthread_detach` right after every `pthread_create` prevents finished threads from lingering as zombies and slowly exhausting the operating system's process table.

Regardless of whether a connection completes normally or fails partway through, execution converges on a single cleanup label at the end of the worker function, which closes the file if one was opened, closes the client socket, and terminates the thread through `pthread_exit`. Using `goto` for this purpose is a deliberate and common pattern in C, since it achieves the same result that a finally block achieves in languages that provide one: resources are released exactly once, no matter which path execution took to get there.

Finally, `SO_REUSEADDR` is enabled on the listening socket so the server can be restarted immediately during development without running into the `Address already in use` error caused by a previous connection still sitting in the `TIME_WAIT` state.

## Tech Stack

The server is written in C and relies exclusively on POSIX APIs, the Socket API for networking and pthreads for concurrency. Development took place on Windows, while compilation and execution happen in a native Linux environment through WSL running Ubuntu, since neither the Socket API nor pthread are directly compatible with Winsock2 or Win32.

## Project Structure

The project is organized into four files. `server.c` contains the implementation of the concurrent server, `index.html` is the page served on `GET /`, `404.html` is the custom error page, and `avvia.sh` is a small script that compiles and starts the server automatically.

```
.
├── server.c
├── index.html
├── 404.html
└── avvia.sh
```

## Building and Running

From the project directory, running the build script compiles and starts the server.

```bash
./avvia.sh
```

This is equivalent to compiling and launching it manually:

```bash
gcc -Wall -Wextra -pthread -o server server.c
./server
```

Either way, once the server is running it prints `Server HTTP in ascolto su http://localhost:8080` and blocks on `accept`, waiting for incoming connections on port 8080.

## Testing

The behavior described above can be verified directly against the running server using `curl`.

### Home page

A request to the root path returns the home page with a 200 status.

```bash
curl http://localhost:8080/
```

### Missing resource

Requesting a path that does not exist on the filesystem triggers the 404 handling.

```bash
curl http://localhost:8080/does_not_exist.html
```

### Directory traversal attempt

```bash
curl -v --path-as-is "http://localhost:8080/../../etc/passwd"
```

The `--path-as-is` flag is required here because `curl` normally resolves `../` segments on the client side according to RFC 3986 before the request is even sent. Without it, the server would simply receive a request for `/etc/passwd` with no trace of the traversal attempt, the check would never trigger, and the response would be a plain 404 instead of a 403. With the flag in place, the raw path reaches the server exactly as typed, the traversal check catches it, and the response is 403 Forbidden. The same result is obtained by requesting a path with a leading double slash, which the server also treats as an attempt to reach outside its root directory.

### Unsupported method

```bash
curl -v -X POST http://localhost:8080/
```

Since the extracted method is not GET, the server responds with 501 Not Implemented.

### Concurrency

To confirm that connections are actually handled in parallel rather than one at a time, three requests can be launched simultaneously and their status codes compared.

```bash
curl -s -o /dev/null -w "req1: %{http_code}\n" http://localhost:8080/ & \
curl -s -o /dev/null -w "req2: %{http_code}\n" http://localhost:8080/404.html & \
curl -s -o /dev/null -w "req3: %{http_code}\n" http://localhost:8080/ & wait
```

All three requests are accepted in quick succession and completed by three independent threads. The order in which the responses print can vary between runs, which is itself confirmation that they are being served concurrently rather than sequentially.

## Limitations and Future Work

The server currently focuses on serving text content. Because files are opened with `fopen` in text mode and every response is sent with a `Content-Type` header hardcoded to `text/html`, it does not correctly handle binary resources such as images, CSS files, or JavaScript. Extending it into a proper static file server would start with opening files in binary mode and introducing a mapping from file extension to MIME type, generated dynamically instead of hardcoded.

On the protocol side, the server currently understands only GET. Adding support for HEAD and POST, along with a subset of HTTP/1.1 that allows a single connection to be reused for multiple requests instead of closing after each one, would bring the server closer to what a real HTTP client expects and would reduce the latency caused by opening a new TCP connection every time.

The biggest scalability limitation comes from the threading model itself. Creating and tearing down a thread for every connection has real overhead under heavy load. Replacing this with a pool of worker threads allocated in advance, so that a connection is handed to an already running thread instead of spawning a new one, would remove that overhead and keep CPU usage more stable under a high volume of concurrent requests.

## About

Author: Mateusz Barabanow

