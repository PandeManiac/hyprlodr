/* Minimal freestanding stdint.h for the HYPRLODR Windows x64 target. */
#ifndef HYPRLODR_CRT_STDINT_H
#define HYPRLODR_CRT_STDINT_H

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
typedef unsigned long long uintptr_t;
typedef long long intptr_t;

#define INT32_MAX 2147483647
#define UINT32_MAX 4294967295u
#define UINT64_MAX 18446744073709551615ull
#define SIZE_MAX 18446744073709551615ull

#endif
