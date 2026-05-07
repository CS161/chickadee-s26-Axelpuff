/* usr/include/stdio.h
 * Minimal stdio stub for Chickadee userspace.
 *
 * ncurses references fprintf/stderr in its error paths even when built
 * without utility programs.  We map these to our write()-based printf
 * so they produce visible output without requiring a full FILE* layer.
 *
 * This header is pure C so that ncurses can include it during cross-compile.
 */
#ifndef CHICKADEE_STDIO_H
#define CHICKADEE_STDIO_H

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#define EOF (-1)

/* Opaque FILE type
 * We represent each standard stream as a small struct that records
 * only the underlying file descriptor. */
typedef struct _CHICKADEE_FILE {
    int fd;
} FILE;

#ifdef __cplusplus
extern "C" {
#endif

/* Standard streams:  defined in u-lib.cc */
extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

/* Formatted output */
int fprintf(FILE* stream, const char* format, ...);
int printf(const char* format, ...);
int sprintf(char* buf, const char* format, ...);
int snprintf(char* buf, size_t n, const char* format, ...);
int vfprintf(FILE* stream, const char* format, __builtin_va_list ap);
int vsprintf(char* buf, const char* format, __builtin_va_list ap);
int vsnprintf(char* buf, size_t n, const char* format, __builtin_va_list ap);

/* Character output */
int fputc(int c, FILE* stream);
int fputs(const char* s, FILE* stream);
int putchar(int c);
int puts(const char* s);

/* Flush:  no-op since we use unbuffered write() */
int fflush(FILE* stream);

/* Error reporting */
void perror(const char* s);

/* sscanf:  used by ncurses terminfo parser */
int sscanf(const char* str, const char* format, ...);

#ifdef __cplusplus
}
#endif

#endif /* CHICKADEE_STDIO_H */
