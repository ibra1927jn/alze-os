/*
 * Anykernel OS v2.1 — Freestanding String/Memory Functions
 *
 * Required because clang/GCC may emit implicit calls to memset/memcpy
 * even in freestanding mode (e.g. struct initialization, aggregate copies).
 */

#ifndef STRING_H
#define STRING_H

#include <stddef.h>

/* Fill n bytes of memory at dest with byte val */
void *memset(void *dest, int val, size_t n);

/* Copy n bytes from src to dest (must not overlap) */
void *memcpy(void *dest, const void *src, size_t n);

/* Copy n bytes from src to dest (may overlap safely) */
void *memmove(void *dest, const void *src, size_t n);

/* Compare n bytes of a and b. Returns 0 if equal. */
int memcmp(const void *a, const void *b, size_t n);

/* Return length of null-terminated string */
size_t strlen(const char *s);

/* Compare two null-terminated strings. Returns 0 if equal. */
int strcmp(const char *a, const char *b);

/* Compare at most n chars of a and b. Returns 0 if equal. */
int strncmp(const char *a, const char *b, size_t n);

/* Copy at most n chars from src to dest, null-padding. */
char *strncpy(char *dest, const char *src, size_t n);

/* Concatenate src to end of dest. dest must have enough space. */
char *strcat(char *dest, const char *src);

/* Find first occurrence of c in s, or NULL. */
char *strchr(const char *s, int c);

/* Find first occurrence of needle in haystack, or NULL. */
char *strstr(const char *haystack, const char *needle);

/* Find first occurrence of c in n bytes of s, or NULL. */
void *memchr(const void *s, int c, size_t n);

#endif /* STRING_H */
