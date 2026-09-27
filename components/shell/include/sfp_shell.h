// Line-based command shell over the SDK. One command per line in, one JSON
// object per line out, so the pytest suite can parse every reply directly.
#ifndef SFP_SHELL_H
#define SFP_SHELL_H

#include <stddef.h>

// Runs one command line and writes the JSON reply (no trailing newline) into
// out. Returns the reply length.
size_t sfp_shell_handle(const char *line, char *out, size_t out_len);

#endif
