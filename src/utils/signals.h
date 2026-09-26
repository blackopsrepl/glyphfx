// Signal handling. Handlers only set flags; the run loop consumes them so
// teardown (cursor restore) happens through normal control flow.
#ifndef GLYPHFX_SIGNALS_H
#define GLYPHFX_SIGNALS_H

#include <stdbool.h>

void install_sigint_handler(void);
void install_sigterm_handler(void);
void install_sigwinch_handler(void);
void restore_sigpipe(void);

bool interrupted(void);
bool terminated(void);
// Consumes a pending SIGWINCH notification.
bool take_terminal_resize(void);

// Finishes a deferred SIGTERM with the default action; does not return.
void die_from_sigterm(void);

#endif
