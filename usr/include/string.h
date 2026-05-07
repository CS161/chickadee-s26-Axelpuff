/* usr/include/string.h
 * POSIX string and memory functions for Chickadee userspace.
 *
 * All implementations already live in lib.cc; this header makes them visible
 * to C code cross-compiled for Chickadee.  strdup() and strerror() are
 * implemented in u-lib.cc since they depend on malloc() and the errno table.
 *
 * This header is pure C so that ncurses can include it during cross-compile.
 */
#ifndef CHICKADEE_STRING_H
#define CHICKADEE_STRING_H

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

/* When included from a Chickadee C++ translation unit, lib.hh already
 * declares memcpy, strlen, strcmp, etc. inside extern "C".  Only emit
 * the declarations that lib.hh does NOT provide (strrchr, strdup,
 * strerror) and skip the rest to avoid duplicate-declaration errors. */
#ifndef CHICKADEE_LIB_HH

#ifdef __cplusplus
extern "C" {
#endif

/* Memory functions */
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
void* memchr(const void* s, int c, size_t n);

/* String length */
size_t strlen(const char* s);
size_t strnlen(const char* s, size_t maxlen);

/* String copy */
char* strcpy(char* dst, const char* src);
char* strncpy(char* dst, const char* src, size_t n);
size_t strlcpy(char* dst, const char* src, size_t maxlen);

/* String compare */
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);

/* String search */
char* strchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);

/* Numeric conversion */
long strtol(const char* s, char** endptr, int base);
unsigned long strtoul(const char* s, char** endptr, int base);
int atoi(const char* s);

#ifdef __cplusplus
}
#endif

#endif /* !CHICKADEE_LIB_HH */

/* These are not in lib.hh and must always be declared. */
#ifdef __cplusplus
extern "C" {
#endif

char* strrchr(const char* s, int c);
char* strdup(const char* s);
const char* strerror(int errnum);

#ifdef __cplusplus
}
#endif

#endif /* CHICKADEE_STRING_H */
