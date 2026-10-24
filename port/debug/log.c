#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "debug.h"
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { LINE_LIMIT = 8192, LINE_SIZE = 256 };

static char lines[LINE_LIMIT][LINE_SIZE];
static uint32_t total;
static char partial[LINE_SIZE];
static size_t partial_length;
static int pipe_read = -1;
static int terminal = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void store_line(void) {
    pthread_mutex_lock(&lock);
    memcpy(lines[total % LINE_LIMIT], partial, partial_length);
    lines[total % LINE_LIMIT][partial_length] = 0;
    total++;
    pthread_mutex_unlock(&lock);
    partial_length = 0;
}

static void *reader(void *unused) {
    (void)unused;
    char buffer[4096];
    for (;;) {
        ssize_t got = read(pipe_read, buffer, sizeof buffer);
        if (got <= 0) return NULL;
        if (terminal >= 0) {
            ssize_t written = write(terminal, buffer, (size_t)got);
            (void)written;
        }
        for (ssize_t i = 0; i < got; ++i) {
            if (buffer[i] == '\n') store_line();
            else if (partial_length < LINE_SIZE - 1) partial[partial_length++] = buffer[i];
        }
    }
}

static void crashed(int signal_number) {
    int fd = terminal >= 0 ? terminal : 2;
    char text[64];
    int length = snprintf(text, sizeof text, "crash: signal %d\n", signal_number);
    ssize_t written = write(fd, text, (size_t)length);
    (void)written;
    void *frames[64];
    backtrace_symbols_fd(frames, backtrace(frames, 64), fd);
    signal(signal_number, SIG_DFL);
    debug_blackbox_crash();
    raise(signal_number);
}

static void catch_crashes(void) {
    static char stack[1 << 16];
    stack_t alternate = {.ss_sp = stack, .ss_size = sizeof stack};
    sigaltstack(&alternate, NULL);
    struct sigaction action = {.sa_handler = crashed, .sa_flags = SA_ONSTACK};
    int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (int i = 0; i < (int)(sizeof signals / sizeof signals[0]); ++i) sigaction(signals[i], &action, NULL);
}

void debug_log_start(void) {
    int fds[2];
    if (pipe(fds)) return;
    terminal = dup(2);
    pipe_read = fds[0];
    fflush(stdout);
    fflush(stderr);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[1]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    pthread_t thread;
    pthread_create(&thread, NULL, reader, NULL);
    pthread_detach(thread);
    catch_crashes();
}

uint32_t debug_log_total(void) {
    pthread_mutex_lock(&lock);
    uint32_t count = total;
    pthread_mutex_unlock(&lock);
    return count;
}

int debug_log_line(uint32_t number, char *line, size_t size) {
    pthread_mutex_lock(&lock);
    int found = number < total && total - number <= LINE_LIMIT;
    if (found) snprintf(line, size, "%s", lines[number % LINE_LIMIT]);
    pthread_mutex_unlock(&lock);
    return found;
}
