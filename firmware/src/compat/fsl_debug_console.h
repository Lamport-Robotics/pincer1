/* Compatibility shim: NXP's Plug & Trust middleware includes this MCUXpresso-SDK header when
 * AX_EMBEDDED=1. We are on the pico-sdk, so route its console macros to plain stdio. */
#ifndef PINCER_FSL_DEBUG_CONSOLE_H
#define PINCER_FSL_DEBUG_CONSOLE_H
#include <stdio.h>
#define PRINTF printf
#define SCANF scanf
#define PUTCHAR putchar
#define GETCHAR getchar
#endif
