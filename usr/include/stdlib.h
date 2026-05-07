/* usr/include/stdlib.h
 * Standard utility functions for Chickadee userspace.
 *
 * Numeric conversions (atoi, strtol, strtoul) and random-number functions
 * are implemented in lib.cc.  Heap allocation (malloc, free, calloc,
 * realloc) is implemented in u-malloc.cc.  getenv() and
 * abort() are in u-lib.cc.
 *
 * This header is pure C so that ncurses can include it during cross-compile.
 */
#ifndef CHICKADEE_STDLIB_H
#define CHICKADEE_STDLIB_H

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#define RAND_MAX 0x7FFFFFFF

/* Heap allocation, termination, and environment are not in lib.hh
 * and must always be declared.  Numeric conversions and rand are already
 * in lib.hh when compiling Chickadee C++ sources, so skip them there
 * to avoid duplicate declarations. */
#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size);
void  free(void* ptr);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);

void exit(int status) __attribute__((noreturn));
void abort(void)      __attribute__((noreturn));

char* getenv(const char* name);

#ifndef CHICKADEE_LIB_HH
int           atoi(const char* s);
long          strtol(const char* s, char** endptr, int base);
unsigned long strtoul(const char* s, char** endptr, int base);
int  rand(void);
void srand(unsigned int seed);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CHICKADEE_STDLIB_H */
