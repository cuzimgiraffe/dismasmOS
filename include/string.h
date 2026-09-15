#ifndef STRING_H
#define STRING_H

#include "types.h"

size_t strlen(const char *str);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dest, const char *src);
char *strcat(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strstr(const char *haystack, const char *needle);
char *strchr(const char *s, int c);
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void itoa(int n, char *str);
void utoa_hex(uint64_t n, char *str);
int isspace(char c);
int isalpha(char c);
int tolower(int c);

#endif
