#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

static void **libraries;
static size_t library_count, library_capacity;
static unsigned long expression_id;
static bool cleanup_registered;
static void cleanup(void) {
    while (library_count) dlclose(libraries[--library_count]);
    free(libraries); libraries = NULL; library_capacity = 0;
}
/* Return a loaded object; source and .so names are private to this compilation. */
static void *compile_object(const char *source, bool global) {
    char directory[] = "/tmp/crepl-XXXXXX";
    if (!mkdtemp(directory)) return NULL;
    char input[128], output[128];
    snprintf(input, sizeof input, "%s/input.c", directory);
    snprintf(output, sizeof output, "%s/output.so", directory);
    FILE *file = fopen(input, "w");
    bool written = false;
    if (file) {
        written = fputs(source, file) >= 0;
        if (fclose(file)) written = false;
    }
    void *handle = NULL;
    if (written) {
        pid_t child = fork();
        if (child == 0) {
            execlp("gcc", "gcc", "-std=gnu11", "-shared", "-fPIC", "-O0",
                   "-Wno-implicit-function-declaration", "-o", output, input, (char *)NULL);
            _exit(127);
        }
        if (child > 0) {
            int status; pid_t result;
            do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
            if (result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0)
                handle = dlopen(output, RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL));
        }
    }
    unlink(input); unlink(output); rmdir(directory);
    return handle;
}
bool compile_and_load_function(const char *definition) {
    if (!definition) return false;
    if (!cleanup_registered) {
        if (atexit(cleanup)) return false;
        cleanup_registered = true;
    }
    if (library_count == library_capacity) {
        size_t capacity = library_capacity ? library_capacity * 2 : 16;
        if (capacity < library_capacity || capacity > (size_t)-1 / sizeof *libraries) return false;
        void **grown = realloc(libraries, capacity * sizeof *libraries);
        if (!grown) return false;
        libraries = grown; library_capacity = capacity;
    }
    void *handle = compile_object(definition, true);
    if (!handle) return false;
    libraries[library_count++] = handle;
    return true;
}
bool evaluate_expression(const char *expression, int *result) {
    if (!expression || !result) return false;
    char name[80];
    snprintf(name, sizeof name, "__crepl_expression_%lu", expression_id++);
    size_t length = strlen(expression);
    if (length > (size_t)-1 - sizeof name - 64) return false;
    size_t capacity = length + sizeof name + 64;
    char *source = malloc(capacity);
    if (!source) return false;
    snprintf(source, capacity, "int %s(void) { return (\n%s\n); }\n", name, expression);
    void *handle = compile_object(source, false);
    free(source);
    if (!handle) return false;
    dlerror();
    void *symbol = dlsym(handle, name);
    bool success = dlerror() == NULL;
    if (success) {
        int (*function)(void);
        _Static_assert(sizeof function == sizeof symbol, "POSIX function pointer size");
        memcpy(&function, &symbol, sizeof function);
        *result = function(); /* Evaluation occurs in the REPL process. */
    }
    dlclose(handle);
    return success;
}
int main(void) {
    char *line = NULL; size_t capacity = 0;
    while (getline(&line, &capacity, stdin) >= 0) {
        char *start = line;
        while (isspace((unsigned char)*start)) ++start;
        if (!*start) continue;
        if (!strncmp(start, "int", 3) && !isalnum((unsigned char)start[3]) && start[3] != '_') {
            puts(compile_and_load_function(start) ? "OK." : "ERROR.");
        } else {
            int value;
            if (evaluate_expression(start, &value)) printf("= %d.\n", value);
            else puts("ERROR.");
        }
        fflush(stdout);
    }
    free(line);
    return ferror(stdin) || ferror(stdout) ? 1 : 0;
}
