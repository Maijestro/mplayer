/*
 * AmigaOS4 Extended Memory support for MPlayer
 */

#ifndef AMIGA_EXTMEM_H
#define AMIGA_EXTMEM_H

#include <stdint.h>

/* Initialize/cleanup ExtMem system */
int  amiga_extmem_init(void);
void amiga_extmem_cleanup(void);

/* Allocate/free large buffers via ExtMem */
void *amiga_extmem_alloc(uint64_t size, void **extmem_obj);
void  amiga_extmem_free(void *ptr, uint64_t size, void *extmem_obj);

/* Check availability */
int amiga_extmem_available(void);

#endif /* AMIGA_EXTMEM_H */
