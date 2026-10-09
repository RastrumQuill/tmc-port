/* Minimal libc stand-in used only when extracting enums for the assembler. */
#ifndef FAKE_STDDEF_H
#define FAKE_STDDEF_H
typedef unsigned int size_t;
#define NULL ((void*)0)
#define offsetof(t, m) ((size_t) & ((t*)0)->m)
#endif
