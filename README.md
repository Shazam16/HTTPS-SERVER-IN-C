# scratch-httpd

A minimal HTTP/1.1 server built from scratch in pure C — no frameworks, no external libraries, just raw POSIX sockets and the standard library.

---

## Features

- TCP socket server (IPv4, `SO_REUSEADDR`)
- Hand-rolled HTTP/1.1 request parser (request line, headers, body)
- Built-in routes: `GET /`, `GET /hello`, `POST /echo`
- Static file serving from `./public/`
- Automatic MIME type detection (HTML, CSS, JS, JSON, images, and more)
- Proper HTTP response formatting — status line, headers, body
- `Content-Length` body reading for POST requests
- Path traversal protection (`..` blocked)
- Graceful shutdown on `SIGINT` / `SIGTERM`

---

## Build

Requires a C compiler and a POSIX-compatible system (Linux, macOS).

```bash
gcc -Wall -Wextra -o server https_server.c
```

---

## Run

```bash
./server            # listens on port 8080 (default)
./server 9000       # listens on a custom port
```

---

## Usage

### Built-in routes

```bash
# Homepage
curl http://localhost:8080/

# Greeting
curl http://localhost:8080/hello
curl http://localhost:8080/hello?name=Ada

# Echo — returns your request body verbatim
curl -X POST -d "hello world" http://localhost:8080/echo
curl -X POST -d '{"key":"value"}' -H "Content-Type: application/json" http://localhost:8080/echo
```

### Static files

Place any files inside `./public/` and they are served automatically:

```
./public/
├── index.html      →  GET /
├── style.css       →  GET /style.css
├── app.js          →  GET /app.js
└── logo.png        →  GET /logo.png
```

---

## Project Structure

```
.
├── https_server.c   # entire server — single file
├── README.md
└── public/          # static files (create this directory)
```

---

## How It Works

### 1. Socket setup
A standard TCP socket is created, bound to the given port, and set to listen. `SO_REUSEADDR` is enabled so the server can restart immediately without waiting for the OS to release the port.

### 2. Accept loop
The server loops on `accept()`, blocking until a client connects. Each connection is handled synchronously (one at a time) then closed.

### 3. Request parsing
Raw bytes from the socket are parsed by hand:
- The first line is split into **method**, **path**, and **HTTP version**
- Headers are parsed as `Name: value` pairs, stored in a fixed array
- The body pointer is set based on `Content-Length`

### 4. Routing
The path and method are matched against built-in routes. If none match and the method is `GET`, the server looks for a matching file in `./public/`. Unmatched requests get a `404` or `405` response.

### 5. Response formatting
Responses are written as raw strings:
```
HTTP/1.1 200 OK\r\n
Content-Type: text/html; charset=utf-8\r\n
Content-Length: 42\r\n
\r\n
<body>
```

---

## Supported MIME Types

| Extension | MIME Type |
|---|---|
| `.html`, `.htm` | `text/html; charset=utf-8` |
| `.css` | `text/css; charset=utf-8` |
| `.js` | `application/javascript; charset=utf-8` |
| `.json` | `application/json` |
| `.txt` | `text/plain; charset=utf-8` |
| `.png` | `image/png` |
| `.jpg`, `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |
| `.svg` | `image/svg+xml` |
| `.ico` | `image/x-icon` |
| `.pdf` | `application/pdf` |

---

## Limitations

- **Single-threaded** — one connection handled at a time; not suitable for concurrent load
- **No TLS** — HTTP only (despite the filename); add OpenSSL for HTTPS
- **No chunked transfer encoding** — large streaming responses not supported
- **Fixed buffer size** — requests larger than 8 KB are truncated
- **No keep-alive pipelining** — connection closes after each response

---

## Extending

Add a new route in the `dispatch()` function in `https_server.c`:

```c
if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/time") == 0) {
    char body[64];
    time_t t = time(NULL);
    snprintf(body, sizeof(body), "%s", ctime(&t));
    send_text(fd, 200, body);
    return;
}
```

---

## License

Public domain. Do whatever you want with it.
