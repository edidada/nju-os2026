#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

struct process {
    int pid, ppid;
    char name[256];
    size_t child, next;
    bool seen;
};
struct frame { size_t index, depth; };
#define NONE ((size_t)-1)

static int by_pid(const void *a, const void *b) {
    const struct process *x = a, *y = b;
    return (x->pid > y->pid) - (x->pid < y->pid);
}
static int by_name(const void *a, const void *b) {
    const struct process *x = *(const struct process *const *)a;
    const struct process *y = *(const struct process *const *)b;
    int result = strcmp(x->name, y->name);
    return result ? result : by_pid(x, y);
}
static size_t find_pid(struct process *nodes, size_t count, int pid) {
    struct process key = {.pid = pid};
    struct process *p = bsearch(&key, nodes, count, sizeof *nodes, by_pid);
    return p ? (size_t)(p - nodes) : NONE;
}
/* comm may contain spaces and closing parentheses: stat's final ')' ends it. */
static bool read_process(int pid, struct process *p) {
    char path[64], line[8192];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return false; /* Processes may exit during enumeration. */
    bool ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!ok) return false;
    char *left = strchr(line, '('), *right = strrchr(line, ')');
    char state;
    int actual, parent;
    if (!left || !right || right <= left || sscanf(line, "%d", &actual) != 1 ||
        actual != pid || sscanf(right + 1, " %c %d", &state, &parent) != 2 || parent < 0)
        return false;
    size_t len = (size_t)(right - left - 1);
    if (len >= sizeof p->name) len = sizeof p->name - 1;
    memcpy(p->name, left + 1, len);
    p->name[len] = '\0';
    /* Keep each process on one output line, including unusual process names. */
    for (size_t i = 0; i < len; ++i)
        if (iscntrl((unsigned char)p->name[i])) p->name[i] = '?';
    p->pid = pid;
    p->ppid = parent;
    p->child = p->next = NONE;
    p->seen = false;
    return true;
}
static void print_tree(struct process *nodes, size_t root, struct frame *stack,
                       bool show_pids) {
    size_t used = 0;
    stack[used++] = (struct frame){root, 0};
    while (used) {
        struct frame f = stack[--used];
        struct process *p = &nodes[f.index];
        if (p->seen) continue;
        p->seen = true;
        for (size_t i = 0; i < f.depth; ++i) fputs("  ", stdout);
        fputs(p->name, stdout);
        if (show_pids) printf("(%d)", p->pid);
        putchar('\n');
        if (p->next != NONE) stack[used++] = (struct frame){p->next, f.depth};
        if (p->child != NONE) stack[used++] = (struct frame){p->child, f.depth + 1};
    }
}
int main(int argc, char **argv) {
    bool show_pids = false, numeric = false, version = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--show-pids")) show_pids = true;
        else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--numeric-sort")) numeric = true;
        else if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) version = true;
        else {
            fprintf(stderr, "Usage: pstree [-p|--show-pids] [-n|--numeric-sort] [-V|--version]\n");
            return 1;
        }
    }
    if (version) { puts("pstree 1.0 (NJU OS M2)"); return 0; }
    DIR *dir = opendir("/proc");
    if (!dir) { perror("/proc"); return 1; }
    size_t count = 0, capacity = 0;
    struct process *nodes = NULL;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!isdigit((unsigned char)entry->d_name[0])) continue;
        char *end;
        errno = 0;
        long pid = strtol(entry->d_name, &end, 10);
        if (errno || *end || pid <= 0 || pid > INT_MAX) continue;
        struct process p;
        if (!read_process((int)pid, &p)) continue;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 128;
            if (next < capacity || next > (size_t)-1 / sizeof *nodes) {
                closedir(dir); free(nodes); return 1;
            }
            struct process *grown = realloc(nodes, next * sizeof *nodes);
            if (!grown) { closedir(dir); free(nodes); return 1; }
            nodes = grown; capacity = next;
        }
        nodes[count++] = p;
    }
    closedir(dir);
    if (!count) { free(nodes); return 0; }
    qsort(nodes, count, sizeof *nodes, by_pid);
    struct process **order = malloc(count * sizeof *order);
    struct frame *stack = malloc(count * sizeof *stack);
    if (!order || !stack) { free(order); free(stack); free(nodes); return 1; }
    for (size_t i = 0; i < count; ++i) order[i] = &nodes[i];
    if (!numeric) qsort(order, count, sizeof *order, by_name);
    size_t roots = NONE;
    for (size_t i = count; i > 0; --i) {
        struct process *p = order[i - 1];
        size_t parent = find_pid(nodes, count, p->ppid);
        size_t *head = parent == NONE || nodes[parent].pid == p->pid ? &roots : &nodes[parent].child;
        p->next = *head;
        *head = (size_t)(p - nodes);
    }
    if (roots != NONE) print_tree(nodes, roots, stack, show_pids);
    /* A changing /proc snapshot can contain disconnected/cyclic relationships. */
    for (size_t i = 0; i < count; ++i)
        if (!order[i]->seen) print_tree(nodes, (size_t)(order[i] - nodes), stack, show_pids);
    free(stack); free(order); free(nodes);
    return ferror(stdout) ? 1 : 0;
}
