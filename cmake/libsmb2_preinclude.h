#pragma once

/*
 * ESP-IDF's sys/param.h defines MIN before libsmb2-private.h defines its own
 * copy. Include the system header first, then clear the macro so the vendored
 * libsmb2 sources build without a noisy redefinition warning.
 */
#include <sys/param.h>

#ifdef MIN
#undef MIN
#endif
