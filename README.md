# cserve

A hand-written HTTP/1.1 server in C, built from raw sockets - no framework, no HTTP library. Serves static files and a small JSON API over an `epoll`-based event loop.

> **Status:** work in progress. See [Roadmap](#roadmap) for what's done vs. planned.

---

## Features

- [x] TCP server on raw BSD sockets (`socket` -> `bind` -> `listen` -> `accept`)
- [ ] Hand-written HTTP/1.1 request parser (request line + headers)
- [ ] Spec-valid response builder (`Content-Type`, `Content-Length`, `Date`, `Connection`)
- [ ] Static file serving with an extension -> MIME map
- [ ] Path-traversal protection (`..` rejected -> `403`)
- [ ] Single-threaded `epoll` event loop for concurrent connections
- [ ] Non-blocking I/O with a per-connection state machine (handles partial reads)
- [ ] JSON API layer (`/api/health`, `/api/echo`, `/api/kv`)
- [ ] Test suite + benchmarks

---

## Build & Run

```sh
make            # builds ./cserver with -Wall -Wextra
./cserver       # listens on port 8080 by default
```

Then:

```sh
curl -v localhost:8080/
```

**Requirements:** Linux (uses `epoll`, `gcc`, `make`).

---

## Design decisions & tradeoffs

### Why `epoll` over threads?

With threads, each connection gets its own OS thread, but each thread carries a stack (default ~8MB of address space on Linux) and the kernel pays scheduling/conext-siwtch cost as the count grows. Since HTTP connections spend most of their life idle waiting on the network, you'd be funding thousands of mostly-sleeping threads.

The solution is IO Multiplexing. There are 3 options: `select`, `poll`, and `epoll`. I chose to use `epoll` because the other two hand the kernel the entire fd set on every call and scan it in O(n). `epoll` maintains the interest set in the kernel and returns just the ready ones, so it stays cheap as connections grow.

The tradeoff to using just `epoll` on a single threaded loop, is that there is no CPU parallelism. Any handler that blocks stalls *every* connection.

### How are partial reads handled?

TCP is a byte stream, meaning that `read()` will gives you whatever bytes have arrived. Non blocking sockets sharpen this, when nothing more has arrived yet you get `EAGAIN`/`EWOULDBLOCK`.

This is why we need a per-connection state machine. Each connection owns a buffer, we append whatever we read and scan for the end-of-headers marker `\r\n\r\n`. Until we see it we stay in `READING_HEADERS`. Once headers are read, we parse `Content-Length` and if there is a body stay in `READING_BODY` until we have accummulated that many bytes, then `WRITING`, then `DONE`.

### Security notes

- **Path traversal** - resolve the path and confirm it's still under root, then return `403` if not.
- **Request-size cap** - unbounded buffering lets a client stream endless headers and exhaust memory. Cap it and return `413`
- **Malformed input** - malformed input returns `400` rather than crashing.

---

## Roadmap

**Week 1 - correctness:** sockets -> request parsing -> response builder -> static files -> full header parsing/refactor -> robustness -> tests + README.

**Week 2 - depth:** `epoll` concurrency -> per-connection state machine -> JSON API -> observability -> test suite -> benchmarks -> docs.

**What I'd do next:** HTTP keep-alive, thread pool behind `epoll`

---

## License

[MIT](LICENSE)
