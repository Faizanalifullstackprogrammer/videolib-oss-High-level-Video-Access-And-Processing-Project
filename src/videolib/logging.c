/*****************************************************************************
 *
 * logging.c
 *   Logging primitives used in context of SV.
 *
 *****************************************************************************
 *
 * Copyright 2013-2022 Sighthound, Inc.
 *
 * Licensed under the GNU GPLv3 license found at
 * https://www.gnu.org/licenses/gpl-3.0.txt
 *
 * Alternative licensing available from Sighthound, Inc.
 * by emailing opensource@sighthound.com
 *
 * This file is part of the Sighthound Video project which can be found at
 * https://github.url/thing
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; using version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 *****************************************************************************/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

// Logs to the given logger, formatting using printf.  Limited to 2048 char
// outputs (to avoid a slow malloc).  Note that a \n will be auto-added.
//
// If the given logger is NULL, logs to stderr...
//
// NOTE: Since this can call back into python, I don't think it's super-fast.
void log_impl(log_fn_t logFn, int level, const char* format, va_list ap)
{
    if (logFn == NULL) {
        vfprintf(stderr, format, ap);
        putc('\n', stderr);
    } else {
        char buffer[2048];
        vsnprintf(buffer, sizeof(buffer), format, ap);
        logFn(level, buffer);
    }
}
void log_msg(log_fn_t logFn, int level, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    log_impl(logFn, level, format, ap);
}
void log_err(log_fn_t logFn, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    log_impl(logFn, kLogLevelError, format, ap);
}
void log_warn(log_fn_t logFn, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    log_impl(logFn, kLogLevelWarning, format, ap);
}
void log_info(log_fn_t logFn, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    log_impl(logFn, kLogLevelInfo, format, ap);
}
void log_dbg(log_fn_t logFn, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    log_impl(logFn, kLogLevelDebug, format, ap);
}

///////////////////////////////////////////////////////////////////////////////

