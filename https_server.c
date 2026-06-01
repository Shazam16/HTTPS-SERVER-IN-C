/*
 * https_server.c — Minimal HTTP/1.1 server from scratch in C
 *
 * Features:
 *   - TCP socket setup (IPv4)
 *   - HTTP/1.1 request parsing (method, path, headers, body)
 *   - Static file serving from ./public/
 *   - Built-in routes: GET /, GET /hello, POST /echo
 *   - Proper HTTP response formatting (status line, headers, body)
 *   - Keep-alive support
 *   - MIME type detection
 *   - Graceful 404 / 405 error responses
 *
 * Build:
 *   gcc -Wall -Wextra -o server https_server.c
 *
 * Run:
 *   ./server [port]        (default port: 8080)
 *
 * Test:
 *   curl http://localhost:8080/
 *   curl http://localhost:8080/hello
 *   curl -X POST -d "hello world" http://localhost:8080/echo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ─────────────────────────── constants ─────────────────────────── */

#define DEFAULT_PORT     8080
#define BACKLOG          128
#define RECV_BUF_SIZE    8192
#define SEND_BUF_SIZE    65536
#define MAX_HEADERS      64
#define MAX_PATH         256
#define PUBLIC_DIR       "./public"

/* ─────────────────────────── types ─────────────────────────────── */

typedef struct {
    char name[128];
    char value[512];
} http_header_t;

typedef struct {
    char method[16];
    char path[MAX_PATH];
    char version[16];
    http_header_t headers[MAX_HEADERS];
    int  header_count;
    char *body;          /* points into raw buffer — do NOT free */
    int   body_len;
} http_request_t;

typedef struct {
    int    status;
    char   content_type[64];
    char  *body;
    size_t body_len;
    int    free_body;    /* 1 = caller should free body */
} http_response_t;

/* ─────────────────────────── utilities ─────────────────────────── */

/* Trim leading/trailing whitespace in-place (returns same pointer) */
static char *strtrim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s) - 1;
    while (e >= s && (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n')) *e-- = '\0';
    return s;
}

static const char *status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

static const char *mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcasecmp(ext, ".js")   == 0) return "application/javascript; charset=utf-8";
    if (strcasecmp(ext, ".json") == 0) return "application/json";
    if (strcasecmp(ext, ".txt")  == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(ext, ".png")  == 0) return "image/png";
    if (strcasecmp(ext, ".jpg")  == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".gif")  == 0) return "image/gif";
    if (strcasecmp(ext, ".svg")  == 0) return "image/svg+xml";
    if (strcasecmp(ext, ".ico")  == 0) return "image/x-icon";
    if (strcasecmp(ext, ".pdf")  == 0) return "application/pdf";
    return "application/octet-stream";
}

/* ─────────────────────────── request parser ─────────────────────── */

/*
 * Parse raw HTTP request text into http_request_t.
 * Returns 0 on success, -1 on malformed input.
 *
 * Layout of a raw HTTP request:
 *   METHOD SP path SP HTTP/1.x CRLF
 *   Header: value CRLF
 *   ...
 *   CRLF
 *   [body]
 */
static int parse_request(char *raw, int raw_len, http_request_t *req) {
    memset(req, 0, sizeof(*req));

    /* ── request line ── */
    char *line_end = strstr(raw, "\r\n");
    if (!line_end) return -1;
    *line_end = '\0';

    if (sscanf(raw, "%15s %255s %15s", req->method, req->path, req->version) != 3)
        return -1;

    char *cursor = line_end + 2;   /* skip \r\n */

    /* ── headers ── */
    while (req->header_count < MAX_HEADERS) {
        char *end = strstr(cursor, "\r\n");
        if (!end) break;
        *end = '\0';

        if (cursor == end || strlen(cursor) == 0) {
            /* blank line → end of headers */
            cursor = end + 2;
            break;
        }

        char *colon = strchr(cursor, ':');
        if (colon) {
            *colon = '\0';
            strncpy(req->headers[req->header_count].name,  strtrim(cursor),  127);
            strncpy(req->headers[req->header_count].value, strtrim(colon+1), 511);
            req->header_count++;
        }

        cursor = end + 2;
    }

    /* ── body ── */
    int offset = (int)(cursor - raw);
    req->body_len = raw_len - offset;
    req->body = (req->body_len > 0) ? cursor : NULL;

    return 0;
}

/* Look up a header value (case-insensitive name) */
static const char *get_header(const http_request_t *req, const char *name) {
    for (int i = 0; i < req->header_count; i++)
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    return NULL;
}

/* ─────────────────────────── response writer ────────────────────── */

/*
 * Build and send the HTTP response.  Extra headers string is optional (NULL ok).
 * extra_headers format: "Name: value\r\nName2: value2\r\n"
 */
static void send_response(int fd, const http_response_t *res, const char *extra_headers) {
    char header_buf[1024];
    int hlen = snprintf(header_buf, sizeof(header_buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n"
        "Server: scratch-httpd/1.0\r\n"
        "%s"
        "\r\n",
        res->status, status_text(res->status),
        res->content_type,
        res->body_len,
        extra_headers ? extra_headers : ""
    );

    /* Send header */
    send(fd, header_buf, hlen, 0);

    /* Send body */
    if (res->body && res->body_len > 0)
        send(fd, res->body, res->body_len, 0);
}

static void send_text(int fd, int status, const char *text) {
    http_response_t res = {
        .status    = status,
        .body      = (char *)text,
        .body_len  = strlen(text),
        .free_body = 0,
    };
    strncpy(res.content_type, "text/plain; charset=utf-8", sizeof(res.content_type)-1);
    send_response(fd, &res, NULL);
}

static void send_html(int fd, int status, const char *html) {
    http_response_t res = {
        .status    = status,
        .body      = (char *)html,
        .body_len  = strlen(html),
        .free_body = 0,
    };
    strncpy(res.content_type, "text/html; charset=utf-8", sizeof(res.content_type)-1);
    send_response(fd, &res, NULL);
}

/* ─────────────────────────── static file handler ───────────────── */

static int serve_file(int fd, const char *url_path) {
    /* Reject path traversal attempts */
    if (strstr(url_path, "..")) {
        send_text(fd, 400, "400 Bad Request: path traversal not allowed\n");
        return -1;
    }

    /* Build filesystem path */
    char fs_path[MAX_PATH + sizeof(PUBLIC_DIR) + 4];
    if (strcmp(url_path, "/") == 0)
        snprintf(fs_path, sizeof(fs_path), "%s/index.html", PUBLIC_DIR);
    else
        snprintf(fs_path, sizeof(fs_path), "%s%s", PUBLIC_DIR, url_path);

    /* Stat the file */
    struct stat st;
    if (stat(fs_path, &st) < 0 || !S_ISREG(st.st_mode)) {
        return 0;   /* not found → let caller issue 404 */
    }

    /* Read file */
    FILE *f = fopen(fs_path, "rb");
    if (!f) {
        send_text(fd, 500, "500 Internal Server Error\n");
        return -1;
    }

    char *data = malloc(st.st_size);
    if (!data) { fclose(f); send_text(fd, 500, "500 Out of Memory\n"); return -1; }
    fread(data, 1, st.st_size, f);
    fclose(f);

    http_response_t res = {
        .status    = 200,
        .body      = data,
        .body_len  = st.st_size,
        .free_body = 1,
    };
    strncpy(res.content_type, mime_type(fs_path), sizeof(res.content_type)-1);
    send_response(fd, &res, NULL);
    free(data);
    return 1;
}

/* ─────────────────────────── built-in routes ───────────────────── */

static void route_get_root(int fd) {
    const char *html =
        "<!DOCTYPE html>\n"
        "<html lang='en'>\n"
        "<head><meta charset='UTF-8'><title>scratch-httpd</title>"
        "<style>body{font-family:monospace;max-width:600px;margin:60px auto;background:#0d1117;color:#e6edf3}"
        "h1{color:#58a6ff}a{color:#79c0ff}</style></head>\n"
        "<body>\n"
        "<h1>&#128640; scratch-httpd</h1>\n"
        "<p>HTTP/1.1 server written from scratch in C — no libraries.</p>\n"
        "<ul>\n"
        "<li><a href='/hello'>GET /hello</a> — greeting endpoint</li>\n"
        "<li><code>POST /echo</code> — echoes your request body</li>\n"
        "<li>Static files served from <code>./public/</code></li>\n"
        "</ul>\n"
        "</body></html>\n";
    send_html(fd, 200, html);
}

static void route_get_hello(int fd, const char *name_param) {
    char body[256];
    if (name_param)
        snprintf(body, sizeof(body), "Hello, %s!\n", name_param);
    else
        snprintf(body, sizeof(body), "Hello, World!\n");
    send_text(fd, 200, body);
}

static void route_post_echo(int fd, const http_request_t *req) {
    if (!req->body || req->body_len == 0) {
        send_text(fd, 204, "");
        return;
    }
    /* Echo the body back verbatim */
    const char *ct = get_header(req, "Content-Type");
    http_response_t res = {
        .status    = 200,
        .body      = req->body,
        .body_len  = req->body_len,
        .free_body = 0,
    };
    strncpy(res.content_type, ct ? ct : "application/octet-stream", sizeof(res.content_type)-1);
    send_response(fd, &res, NULL);
}

/* ─────────────────────────── router ────────────────────────────── */

/*
 * Very simple prefix-based router.
 * Returns a query-string pointer if one is present (after '?'), or NULL.
 */
static char *split_query(char *path) {
    char *q = strchr(path, '?');
    if (q) { *q = '\0'; return q + 1; }
    return NULL;
}

static void dispatch(int fd, http_request_t *req) {
    char *query = split_query(req->path);   /* mutates req->path */
    (void)query;                            /* extend as needed */

    /* GET / */
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/") == 0) {
        /* Try static index.html first, fall back to built-in */
        if (serve_file(fd, "/") <= 0)
            route_get_root(fd);
        return;
    }

    /* GET /hello */
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/hello") == 0) {
        route_get_hello(fd, query ? strstr(query, "name=") : NULL);
        return;
    }

    /* POST /echo */
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/echo") == 0) {
        route_post_echo(fd, req);
        return;
    }

    /* Static files */
    if (strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0) {
        int r = serve_file(fd, req->path);
        if (r == 1)  return;             /* served */
        if (r == 0)  { send_text(fd, 404, "404 Not Found\n"); return; }
        return;                          /* error already sent */
    }

    send_text(fd, 405, "405 Method Not Allowed\n");
}

/* ─────────────────────────── connection handler ────────────────── */

static void handle_connection(int client_fd, const char *client_ip) {
    char buf[RECV_BUF_SIZE];
    int  total = 0;

    /*
     * Keep reading until we see the end of the HTTP header block (\r\n\r\n).
     * A production server would also handle chunked bodies, etc.
     */
    while (total < RECV_BUF_SIZE - 1) {
        int n = recv(client_fd, buf + total, RECV_BUF_SIZE - 1 - total, 0);
        if (n <= 0) goto done;
        total += n;
        buf[total] = '\0';

        /* Wait until we have the full headers */
        if (strstr(buf, "\r\n\r\n")) break;
    }

    printf("[%s] %.*s\n", client_ip, (int)(strcspn(buf, "\r\n")), buf);

    http_request_t req;
    if (parse_request(buf, total, &req) < 0) {
        send_text(client_fd, 400, "400 Bad Request\n");
        goto done;
    }

    /* If Content-Length says there's more body, read it */
    const char *cl_str = get_header(&req, "Content-Length");
    if (cl_str) {
        int content_length = atoi(cl_str);
        /* Find where the body starts in buf */
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            int have = total - (int)(body_start - buf);
            while (have < content_length && total < RECV_BUF_SIZE - 1) {
                int n = recv(client_fd, buf + total, RECV_BUF_SIZE - 1 - total, 0);
                if (n <= 0) break;
                total += n;
                have  += n;
            }
            buf[total] = '\0';
            /* Re-parse so body pointer is correct */
            parse_request(buf, total, &req);
        }
    }

    dispatch(client_fd, &req);

done:
    close(client_fd);
}

/* ─────────────────────────── main / server loop ────────────────── */

static volatile int running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;

    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGPIPE, SIG_IGN);   /* don't crash on broken pipe */

    /* ── create TCP socket ── */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    /* Allow quick restart without "Address already in use" */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* ── bind ── */
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(port),
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); return 1;
    }

    /* ── listen ── */
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen"); close(server_fd); return 1;
    }

    printf("scratch-httpd listening on http://0.0.0.0:%d\n", port);
    printf("Serving static files from " PUBLIC_DIR "/\n");
    printf("Press Ctrl-C to stop.\n\n");

    /* ── accept loop ── */
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) break;   /* signal interrupted */
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        handle_connection(client_fd, client_ip);
    }

    printf("\nShutting down.\n");
    close(server_fd);
    return 0;
}
