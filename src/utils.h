#pragma once

#ifndef _UTILS_H_
#define _UTILS_H_

#include "st.h"
#include "parse.h"

// Required for error and warning macros.
#include <stdio.h>

#define VME_VERSION "0.0.1"

// Marcros to print warings, errors, etc.
#define warnf(fmt, ...) \
  fprintf(stderr, "\033[33mWarn:\033[0m " fmt "\n", __VA_ARGS__)

#define warn(str) \
  fputs("\033[33mWarn:\033[0m " str "\n", stderr)

#define errf(fmt, ...) \
  fprintf(stderr, "\033[31mError:\033[0m " fmt "\n", __VA_ARGS__)

#define err(str) \
  fputs("\033[31mError:\033[0m " str "\n", stderr)

// Print any text enclosed in a Select Graphic Rendition control sequence
#define sgr(sq, fmt, ...) \
  printf("\033[" sq "m" fmt "\033[m\n", __VA_ARGS__)

// Print a control sequence introducer followed by the given control sequence
#define csi(sq) \
  printf("\033[" sq)


// Returns the file name of the file to execute
// or `NULL` if the arguments were invalid.
// Exit in case this function returns `NULL`.
const char* parse_args(int argc, const char* argv[]);

// Print a warning if the file extension of the
// given file is not `.vm`.
void warn_file_ext(const char* filename);

// Print a warning if the file doens't end with
// a newline character.
void warn_eof_nl(void);

// Print a warning about lit exceeding the maximum
// allowed 16-bit number range.
void warn_sat_uilit(int lit);

// Print a warning that the identifer of
// length `len` exceeds the maximum identifer
// length `max`. It will therefore be truncated to
// the first `MAX_IDENT_LEN` characters.
void warn_trunc_ident(const char* blk, size_t len, size_t max);

// Warn the user that `parse` didn't receive a
// symbol table which means that any label-related
// instruction doesn't work.
void warn_no_st(const struct SymKey* key, const struct SymVal* val);

// Generate startup code
Insts* gen_startup(const SymbolTable* st);

#endif  // _UTILS_H_
