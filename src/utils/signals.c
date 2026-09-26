#include "utils/signals.h"

#include <signal.h>
#include <stdlib.h>

// SIGWINCH is BSD/Linux and is 28 on both; strict C17 hides it on macOS.
#ifndef SIGWINCH
#define SIGWINCH 28
#endif

static volatile sig_atomic_t g_interrupted = 0;
static volatile sig_atomic_t g_terminated = 0;
static volatile sig_atomic_t g_resized = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static void handle_sigterm(int sig) {
    (void)sig;
    g_terminated = 1;
}

static void handle_sigwinch(int sig) {
    (void)sig;
    g_resized = 1;
}

void install_sigint_handler(void) {
    signal(SIGINT, handle_sigint);
}

void install_sigterm_handler(void) {
    signal(SIGTERM, handle_sigterm);
}

void install_sigwinch_handler(void) {
    signal(SIGWINCH, handle_sigwinch);
}

void restore_sigpipe(void) {
    signal(SIGPIPE, SIG_DFL);
}

bool interrupted(void) {
    return g_interrupted != 0;
}

bool terminated(void) {
    return g_terminated != 0;
}

bool take_terminal_resize(void) {
    sig_atomic_t pending = g_resized;
    g_resized = 0;
    return pending != 0;
}

void die_from_sigterm(void) {
    signal(SIGTERM, SIG_DFL);
    raise(SIGTERM);
    _Exit(1);
}
