#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define MAX_SYSCALLS 1024
#define TOP_N 5
typedef struct { char name[64]; double time; } syscall_stat;
typedef struct { syscall_stat stats[MAX_SYSCALLS]; int count; double total_time; } syscall_stats;
extern char **environ;

int parse_strace_line(char *line, char *name, double *duration) {
    const char *start = line;
    if (!strncmp(start, "<... ", 5)) start += 5;
    const char *end = start;
    while (islower((unsigned char)*end) || isdigit((unsigned char)*end) || *end == '_') ++end;
    size_t n = (size_t)(end - start);
    if (!n || n >= 64 || (!strncmp(line, "<... ", 5) ? strncmp(end, " resumed>", 9) : *end != '(')) return 0;
    char *left = strrchr(line, '<'), *tail;
    if (!left) return 0;
    errno = 0;
    double value = strtod(left + 1, &tail);
    if (tail == left + 1 || *tail++ != '>' || errno || !isfinite(value) || value < 0) return 0;
    while (isspace((unsigned char)*tail)) ++tail;
    if (*tail) return 0;
    memcpy(name, start, n); name[n] = 0; *duration = value;
    return 1;
}
void add_syscall(syscall_stats *s, const char *name, double time) {
    if (!isfinite(time) || time < 0 || strlen(name) >= sizeof s->stats[0].name) return;
    int i;
    for (i = 0; i < s->count; ++i) if (!strcmp(s->stats[i].name, name)) break;
    if (i == s->count) {
        if (s->count == MAX_SYSCALLS) return;
        strcpy(s->stats[i].name, name); s->stats[i].time = 0; ++s->count;
    }
    s->stats[i].time += time; s->total_time += time;
}
static int by_time(const void *a, const void *b) {
    const syscall_stat *x = a, *y = b;
    if (x->time != y->time) return x->time > y->time ? -1 : 1;
    return strcmp(x->name, y->name);
}
void print_top_syscalls(syscall_stats *s, int n) {
    qsort(s->stats, (size_t)s->count, sizeof s->stats[0], by_time);
    if (n > s->count) n = s->count;
    for (int i = 0; i < n; ++i) {
        int ratio = s->total_time > 0 ? (int)(s->stats[i].time / s->total_time * 100) : 0;
        printf("%s (%d%%)\n", s->stats[i].name, ratio);
    }
    static const char delimiter[80];
    fwrite(delimiter, 1, sizeof delimiter, stdout); fflush(stdout);
}
static char *resolve(const char *command) {
    if (strchr(command, '/')) return access(command, X_OK) == 0 ? strdup(command) : NULL;
    const char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin";
    for (const char *p = path;;) {
        const char *end = strchr(p, ':');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        size_t size = length + strlen(command) + 3;
        char *candidate = malloc(size);
        if (!candidate) return NULL;
        if (length) snprintf(candidate, size, "%.*s/%s", (int)length, p, command);
        else snprintf(candidate, size, "./%s", command);
        if (!access(candidate, X_OK)) return candidate;
        free(candidate);
        if (!end) break;
        p = end + 1;
    }
    return NULL;
}
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: sperf COMMAND [ARG]...\n"); return 1; }
    char *target = resolve(argv[1]);
    char *tracer = resolve("strace");
    if (!tracer && access("/usr/bin/strace", X_OK) == 0) tracer = strdup("/usr/bin/strace");
    if (!target || !tracer) { fprintf(stderr, "sperf: command or strace not executable\n"); free(target); free(tracer); return 127; }
    char **args = calloc((size_t)argc + 8, sizeof *args);
    if (!args) { free(target); free(tracer); return 1; }
    args[0] = "strace"; args[1] = "-T"; args[2] = "-s"; args[3] = "80";
    args[4] = "-o"; args[5] = "/proc/self/fd/3"; args[6] = "--"; args[7] = target;
    for (int i = 2; i < argc; ++i) args[i + 6] = argv[i];
    int channel[2];
    if (pipe(channel)) { perror("pipe"); free(args); free(target); free(tracer); return 1; }
    pid_t child = fork();
    if (child == 0) {
        close(channel[0]);
        if (channel[1] != 3) { if (dup2(channel[1], 3) < 0) _exit(126); close(channel[1]); }
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd < 0 || dup2(nullfd, STDOUT_FILENO) < 0 || dup2(nullfd, STDERR_FILENO) < 0) _exit(126);
        if (nullfd > 3) close(nullfd);
        execve(tracer, args, environ); _exit(127);
    }
    close(channel[1]); free(args); free(target); free(tracer);
    if (child < 0) { perror("fork"); close(channel[0]); return 1; }
    syscall_stats *stats = calloc(1, sizeof *stats);
    size_t used = 0, capacity = 4096;
    char *line = malloc(capacity);
    if (!stats || !line) { kill(child, SIGTERM); close(channel[0]); waitpid(child, NULL, 0); free(stats); free(line); return 1; }
    double last = now(); bool dirty = false, failed = false;
    for (;;) {
        struct pollfd fd = {.fd = channel[0], .events = POLLIN};
        int ready = poll(&fd, 1, 100);
        if (ready < 0) { if (errno == EINTR) continue; failed = true; break; }
        if (ready) {
            char bytes[4096]; ssize_t got = read(channel[0], bytes, sizeof bytes);
            if (got < 0) { if (errno == EINTR) continue; failed = true; break; }
            if (!got) break;
            for (ssize_t i = 0; i < got; ++i) {
                if (used + 1 >= capacity) {
                    char *grown = realloc(line, capacity * 2);
                    if (!grown) { failed = true; break; }
                    line = grown; capacity *= 2;
                }
                line[used++] = bytes[i];
                if (bytes[i] == '\n') {
                    line[used] = 0; char name[64]; double duration;
                    if (parse_strace_line(line, name, &duration)) { add_syscall(stats, name, duration); dirty = true; }
                    used = 0;
                }
            }
            if (failed) break;
        }
        if (dirty && now() - last >= .1) { print_top_syscalls(stats, TOP_N); last = now(); dirty = false; }
    }
    close(channel[0]);
    if (failed) kill(child, SIGTERM);
    int status = 0; pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (dirty || !stats->count) print_top_syscalls(stats, TOP_N);
    free(line); free(stats);
    if (failed || waited < 0) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
