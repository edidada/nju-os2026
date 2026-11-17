#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <spawn.h>
#include <ctype.h>

#define HEADER_LIMIT 16384
#define PATH_LIMIT 4096
#define WORKERS 4
#define DEFAULT_PORT 8080

extern char **environ;
static int cgi_root = -1, null_input = -1;

typedef struct Request {
    int socket, status, done;
    char method[16], path[PATH_LIMIT];
    struct Request *work_next, *log_next;
} Request;

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t log_ready = PTHREAD_COND_INITIALIZER;
static Request *work_head, *work_tail, *log_head, *log_tail;

void log_request(const char *method, const char *path, int status_code) {
    time_t now = time(NULL);
    struct tm tm_info;
    char timestamp[26];
    localtime_r(&now, &tm_info);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);
    printf("[%s] [%s] [%s] [%d]\n", timestamp, method, path, status_code);
    fflush(stdout);
}

static int send_all(int socket, const void *buffer, size_t length) {
    const char *p = buffer;
    while (length) {
        ssize_t n = send(socket, p, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

static int error_response(int socket, int status) {
    const char *reason = status == 400 ? "Bad Request" :
                         status == 404 ? "Not Found" :
                         status == 405 ? "Method Not Allowed" :
                         status == 431 ? "Request Header Fields Too Large" :
                         "Internal Server Error";
    char response[512];
    int n = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
        status, reason, strlen(reason), reason);
    if (n > 0 && (size_t)n < sizeof(response)) send_all(socket, response, (size_t)n);
    return status;
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (unsigned char)tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// Open beneath the original cgi-bin descriptor. No component can be a symlink
// or '..'; validating strings alone would leave a pathname replacement race.
static int open_script(const char *path) {
    if (strncmp(path, "/cgi-bin/", 9) || cgi_root < 0) return -1;
    char decoded[PATH_LIMIT];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)path + 9; *p; p++) {
        unsigned char c = *p;
        if (c == '%') {
            if (!p[1] || !p[2]) return -1;
            int hi = hex_digit(p[1]), lo = hex_digit(p[2]);
            if (hi < 0 || lo < 0) return -1;
            c = (unsigned char)(hi * 16 + lo);
            p += 2;
        }
        if (c < 32 || c == 127 || c == '\\' || n + 1 >= sizeof(decoded)) return -1;
        decoded[n++] = (char)c;
    }
    decoded[n] = 0;
    if (!n) return -1;
    int dir = fcntl(cgi_root, F_DUPFD_CLOEXEC, 3);
    if (dir < 0) return -1;
    char *part = decoded;
    for (;;) {
        char *slash = strchr(part, '/');
        if (slash) *slash = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) {
            close(dir); return -1;
        }
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        if (slash) flags |= O_DIRECTORY;
        int next = openat(dir, part, flags);
        close(dir);
        if (next < 0) return -1;
        if (!slash) {
            struct stat st;
            if (fstat(next, &st) || !S_ISREG(st.st_mode)) {
                close(next); return -1;
            }
            return next;
        }
        dir = next;
        part = slash + 1;
    }
}

static int run_cgi(int script, int output, const char *method, const char *query) {
    size_t count = 0;
    while (environ[count]) count++;
    char **env = calloc(count + 3, sizeof(*env));
    char *method_env = NULL, *query_env = NULL;
    if (!env || asprintf(&method_env, "REQUEST_METHOD=%s", method) < 0 ||
        asprintf(&query_env, "QUERY_STRING=%s", query) < 0) {
        free(env); free(method_env); free(query_env); return -1;
    }
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(environ[i], "REQUEST_METHOD=", 15) &&
            strncmp(environ[i], "QUERY_STRING=", 13)) env[used++] = environ[i];
    }
    env[used++] = method_env;
    env[used++] = query_env;
    env[used] = NULL;

    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (!rc) {
        // All originals have CLOEXEC. fd 3 is retained for script interpreters;
        // execute the validated open file rather than reopening its pathname.
        rc = posix_spawn_file_actions_adddup2(&actions, null_input, STDIN_FILENO);
        if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
        if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, null_input, STDERR_FILENO);
        if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, script, 3);
        pid_t child = -1;
        char *argv[] = {"cgi-script", NULL};
        if (!rc) rc = posix_spawn(&child, "/proc/self/fd/3", &actions, NULL, argv, env);
        posix_spawn_file_actions_destroy(&actions);
        if (!rc) {
            int status;
            pid_t result;
            do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
            if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) rc = -1;
        }
    }
    free(env); free(method_env); free(query_env);
    return rc ? -1 : 0;
}

static int status_number(const char *text) {
    if (!text[0] || !text[1] || !text[2] || text[0] < '1' || text[0] > '5' ||
        text[1] < '0' || text[1] > '9' || text[2] < '0' || text[2] > '9' ||
        (text[3] && text[3] != ' ' && text[3] != '\t')) return -1;
    return (text[0] - '0') * 100 + (text[1] - '0') * 10 + text[2] - '0';
}

static int relay_cgi(int socket, int output) {
    char header[HEADER_LIMIT + 1];
    size_t used = 0;
    int complete = 0;
    if (lseek(output, 0, SEEK_SET) < 0) return error_response(socket, 500);
    // Inspect only headers; the complete response remains in the temporary
    // file so a script that exits unsuccessfully cannot send a partial 200.
    while (used < HEADER_LIMIT && !complete) {
        ssize_t n = read(output, header + used, HEADER_LIMIT - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
        header[used] = 0;
        char *end = strstr(header, "\r\n\r\n");
        size_t skip = 4;
        if (!end) { end = strstr(header, "\n\n"); skip = 2; }
        if (end) {
            size_t length = (size_t)(end - header) + skip;
            if (memchr(header, 0, length)) return error_response(socket, 500);
            header[length - 1] = 0;
            complete = 1;
        }
    }
    if (!complete) return error_response(socket, 500);
    int raw = !strncmp(header, "HTTP/", 5), status = 200;
    char *save = NULL;
    char *line = strtok_r(header, "\n", &save);
    if (!line) return error_response(socket, 500);
    if (raw) {
        size_t length = strlen(line);
        if (length && line[length - 1] == '\r') line[length - 1] = 0;
        if (strncmp(line, "HTTP/1.0 ", 9) && strncmp(line, "HTTP/1.1 ", 9)) return error_response(socket, 500);
        status = status_number(line + 9);
        if (status < 0) return error_response(socket, 500);
        line = strtok_r(NULL, "\n", &save);
    }
    int headers = 0;
    for (; line; line = strtok_r(NULL, "\n", &save)) {
        size_t length = strlen(line);
        if (length && line[length - 1] == '\r') line[--length] = 0;
        if (!length) break;
        char *colon = strchr(line, ':');
        if (!colon || colon == line) return error_response(socket, 500);
        for (char *p = line; p < colon; p++) {
            if (!isalnum((unsigned char)*p) && !strchr("!#$%&'*+-.^_`|~", *p)) return error_response(socket, 500);
        }
        headers++;
        if (!raw && (size_t)(colon - line) == 6 && !strncasecmp(line, "Status", 6)) {
            char *value = colon + 1;
            while (*value == ' ' || *value == '\t') value++;
            status = status_number(value);
            if (status < 0) return error_response(socket, 500);
        }
    }
    if (!raw && !headers) return error_response(socket, 500);
    if (lseek(output, 0, SEEK_SET) < 0) return error_response(socket, 500);
    if (!raw) {
        char prefix[96];
        int n = snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d CGI Response\r\nConnection: close\r\n", status);
        if (send_all(socket, prefix, (size_t)n)) return status;
    }
    char buffer[16384];
    for (;;) {
        ssize_t n = read(output, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (send_all(socket, buffer, (size_t)n)) break;
    }
    return status;
}

static int handle_request(Request *request) {
    char buffer[HEADER_LIMIT + 1];
    size_t used = 0;
    while (used < HEADER_LIMIT) {
        ssize_t n = recv(request->socket, buffer + used, HEADER_LIMIT - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return error_response(request->socket, 400);
        if (memchr(buffer + used, 0, (size_t)n)) return error_response(request->socket, 400);
        used += (size_t)n;
        buffer[used] = 0;
        if (strstr(buffer, "\r\n\r\n") || strstr(buffer, "\n\n")) break;
    }
    if (!strstr(buffer, "\r\n\r\n") && !strstr(buffer, "\n\n")) return error_response(request->socket, 431);
    char *end = strchr(buffer, '\n');
    if (!end) return error_response(request->socket, 400);
    *end = 0;
    char target[PATH_LIMIT], protocol[16], extra;
    if (sscanf(buffer, "%15s %4095s %15s %c", request->method, target, protocol, &extra) != 3 ||
        (strcmp(protocol, "HTTP/1.0") && strcmp(protocol, "HTTP/1.1"))) return error_response(request->socket, 400);
    char *query = strchr(target, '?');
    if (query) *query++ = 0;
    else query = target + strlen(target);
    for (const unsigned char *p = (const unsigned char *)target; *p; p++) {
        if (*p < 33 || *p == 127 || *p == '#') return error_response(request->socket, 400);
    }
    snprintf(request->path, sizeof(request->path), "%s", target);
    if (strcmp(request->method, "GET")) return error_response(request->socket, 405);
    int script = open_script(target);
    if (script < 0) return error_response(request->socket, 404);
    char name[] = "/tmp/nju-httpd-XXXXXX";
    int output = mkostemp(name, O_CLOEXEC);
    if (output < 0) { close(script); return error_response(request->socket, 500); }
    unlink(name);
    int result = run_cgi(script, output, request->method, query);
    close(script);
    int status = result ? error_response(request->socket, 500) : relay_cgi(request->socket, output);
    close(output);
    return status;
}

static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&queue_lock);
        while (!work_head) pthread_cond_wait(&work_ready, &queue_lock);
        Request *request = work_head;
        work_head = request->work_next;
        if (!work_head) work_tail = NULL;
        pthread_mutex_unlock(&queue_lock);
        int status = handle_request(request);
        close(request->socket);
        pthread_mutex_lock(&queue_lock);
        request->status = status;
        request->done = 1;
        pthread_cond_signal(&log_ready);
        pthread_mutex_unlock(&queue_lock);
    }
    return NULL;
}

static void *logger(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&queue_lock);
        while (!log_head || !log_head->done) pthread_cond_wait(&log_ready, &queue_lock);
        Request *request = log_head;
        log_head = request->log_next;
        if (!log_head) log_tail = NULL;
        pthread_mutex_unlock(&queue_lock);
        log_request(request->method, request->path, request->status);
        free(request);
    }
    return NULL;
}

int main(int argc, char **argv) {
    char *end = NULL;
    long port = DEFAULT_PORT;
    if (argc == 2) { errno = 0; port = strtol(argv[1], &end, 10); }
    if (argc > 2 || (argc == 2 && (errno || !*argv[1] || *end)) || port < 1 || port > 65535) {
        fprintf(stderr, "Usage: httpd [port:1..65535]\n"); return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    cgi_root = open("cgi-bin", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    null_input = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int reuse = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY), .sin_port = htons((unsigned short)port)};
    if (null_input < 0 || server < 0 || setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) ||
        bind(server, (struct sockaddr *)&address, sizeof(address)) || listen(server, SOMAXCONN)) {
        perror("httpd setup"); return 1;
    }
    pthread_t workers[WORKERS], log_thread;
    if (pthread_create(&log_thread, NULL, logger, NULL)) return 1;
    for (int i = 0; i < WORKERS; i++) if (pthread_create(&workers[i], NULL, worker, NULL)) return 1;
    printf("Server listening on port %ld...\n", port);
    fflush(stdout);
    for (;;) {
        int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) { if (errno != EINTR) perror("accept"); continue; }
        struct timeval timeout = {.tv_sec = 30, .tv_usec = 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        Request *request = calloc(1, sizeof(*request));
        if (!request) { close(client); continue; }
        request->socket = client;
        strcpy(request->method, "-"); strcpy(request->path, "-");
        pthread_mutex_lock(&queue_lock);
        if (work_tail) work_tail->work_next = request;
        else work_head = request;
        work_tail = request;
        if (log_tail) log_tail->log_next = request;
        else log_head = request;
        log_tail = request;
        pthread_cond_signal(&work_ready);
        pthread_mutex_unlock(&queue_lock);
    }
}
