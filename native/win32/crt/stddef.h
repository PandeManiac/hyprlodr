/* Minimal freestanding stddef.h for the HYPRLODR Windows x64 target. */
#ifndef HYPRLODR_CRT_STDDEF_H
#define HYPRLODR_CRT_STDDEF_H

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifndef NULL
#  define NULL ((void *)0)
#endif

#define offsetof(type, member) ((size_t)&(((type *)0)->member))

#endif
