#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "debug.h"
#include "port_symbols.h"
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <unwind.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#endif

enum { LINE_LIMIT = 8192, LINE_SIZE = 256, TRACE_LIMIT = 64 };

// Windows' C runtime buffers _IOLBF streams fully, so there stdout goes unbuffered to show each line as it is written
#ifdef _WIN32
#define NULL_DEVICE "NUL"
#define LINE_BUFFERED _IONBF
static int make_pipe(int fds[2]) { return _pipe(fds, 1 << 16, _O_BINARY); }

// The processes the game starts (a restart for a recording) get its standard handles, which dup2 moves only in a
// console program: they are pointed at the pipe too, as the descriptors are elsewhere.
static void share_redirect(void) {
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(fileno(stdout)));
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(fileno(stderr)));
}
#else
#define NULL_DEVICE "/dev/null"
#define LINE_BUFFERED _IOLBF
static int make_pipe(int fds[2]) { return pipe(fds); }
static void share_redirect(void) {}
#endif

static char lines[LINE_LIMIT][LINE_SIZE];
static uint32_t total;
static char partial[LINE_SIZE];
static size_t partial_length;
static int pipe_read = -1;
static int terminal = -1;
static SDL_SpinLock lock;

static void store_line(void) {
    SDL_LockSpinlock(&lock);
    memcpy(lines[total % LINE_LIMIT], partial, partial_length);
    lines[total % LINE_LIMIT][partial_length] = 0;
    total++;
    SDL_UnlockSpinlock(&lock);
    partial_length = 0;
}

static int reader(void *unused) {
    (void)unused;
    char buffer[4096];
    for (;;) {
        int got = (int)read(pipe_read, buffer, sizeof buffer);
        if (got <= 0) return 0;
        if (terminal >= 0) {
            int written = (int)write(terminal, buffer, (unsigned)got);
            (void)written;
        }
        for (int i = 0; i < got; ++i) {
            if (buffer[i] == '\n') store_line();
            else if (buffer[i] != '\r' && partial_length < LINE_SIZE - 1) partial[partial_length++] = buffer[i];
        }
    }
}

static void say(int fd, const char *text) {
    int written = (int)write(fd, text, (unsigned)strlen(text));
    (void)written;
}

typedef struct {
    uintptr_t frames[TRACE_LIMIT];
    int count;
} Trace;

static _Unwind_Reason_Code trace_frame(struct _Unwind_Context *context, void *data) {
    Trace *trace = (Trace *)data;
    if (trace->count == TRACE_LIMIT) return _URC_END_OF_STACK;
    trace->frames[trace->count++] = (uintptr_t)_Unwind_GetIP(context);
    return _URC_NO_REASON;
}

// The addresses first, which needs nothing that can fail; then each again with its symbol from the debug build's
// symbol table, which reading may fail in a broken process; then the recording up to the crash.
static void report_crash(const char *what, const Trace *given) {
    int fd = terminal >= 0 ? terminal : 2;
    char line[512];
    snprintf(line, sizeof line, "crash: %s\n", what);
    say(fd, line);
    Trace trace = {{0}, 0};
    if (given) trace = *given;
    else _Unwind_Backtrace(trace_frame, &trace);
    for (int i = 0; i < trace.count; ++i) {
        snprintf(line, sizeof line, "  #%d 0x%llx\n", i, (unsigned long long)trace.frames[i]);
        say(fd, line);
    }
    for (int i = 0; i < trace.count; ++i) {
        char name[400];
        if (!port_symbol_describe(trace.frames[i], name, sizeof name)) continue;
        snprintf(line, sizeof line, "  #%d %s\n", i, name);
        say(fd, line);
    }
    debug_recording_crash();
}

#ifdef _WIN32
// The stack at the exception, walked by Windows' own unwinder from the registers it saved: libunwind stops at a leaf
// function, which has no entry in the unwind tables, where its return address is the top of the stack.
static void trace_exception(CONTEXT context, Trace *trace) {
    while (trace->count < TRACE_LIMIT && context.Rip) {
        trace->frames[trace->count++] = (uintptr_t)context.Rip;
        DWORD64 base = 0, frame = 0;
        void *handler_data = NULL;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &base, NULL);
        if (function) {
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handler_data, &frame, NULL);
        } else {
            context.Rip = *(DWORD64 *)(uintptr_t)context.Rsp;
            context.Rsp += 8;
        }
    }
}

// Windows ends the process itself after the filter, with its own crash report
static LONG WINAPI crashed(EXCEPTION_POINTERS *exception) {
    char what[96];
    snprintf(what, sizeof what, "exception 0x%08lx at 0x%llx", exception->ExceptionRecord->ExceptionCode,
             (unsigned long long)(uintptr_t)exception->ExceptionRecord->ExceptionAddress);
    Trace trace = {{0}, 0};
    trace_exception(*exception->ContextRecord, &trace);
    report_crash(what, &trace);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void aborted(int signal_number) {
    report_crash("abort", NULL);
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

static void catch_crashes(void) {
    SetUnhandledExceptionFilter(crashed);
    signal(SIGABRT, aborted);
}
#else
static void crashed(int signal_number) {
    char what[32];
    snprintf(what, sizeof what, "signal %d", signal_number);
    report_crash(what, NULL);
    signal(signal_number, SIG_DFL);
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
#endif

void debug_log_start(void) {
    int fds[2];
    if (make_pipe(fds)) return;
    // a Windows program started without a console has no stdout or stderr to redirect: they get one first
    if (fileno(stdout) < 0 && !freopen(NULL_DEVICE, "w", stdout)) return;
    if (fileno(stderr) < 0 && !freopen(NULL_DEVICE, "w", stderr)) return;
    terminal = dup(fileno(stderr));
    pipe_read = fds[0];
    fflush(stdout);
    fflush(stderr);
    dup2(fds[1], fileno(stdout));
    dup2(fds[1], fileno(stderr));
    close(fds[1]);
    share_redirect();
    setvbuf(stdout, NULL, LINE_BUFFERED, 0);
    SDL_DetachThread(SDL_CreateThread(reader, "debug log", NULL));
    catch_crashes();
}

uint32_t debug_log_total(void) {
    SDL_LockSpinlock(&lock);
    uint32_t count = total;
    SDL_UnlockSpinlock(&lock);
    return count;
}

int debug_log_line(uint32_t number, char *line, size_t size) {
    SDL_LockSpinlock(&lock);
    int found = number < total && total - number <= LINE_LIMIT;
    if (found) snprintf(line, size, "%s", lines[number % LINE_LIMIT]);
    SDL_UnlockSpinlock(&lock);
    return found;
}
