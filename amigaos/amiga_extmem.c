#include <exec/exec.h>
#include <exec/extmem.h>
#include <exec/exectags.h>
#include <proto/exec.h>
#include <stdio.h>
#include <stdint.h>
#include "mp_msg.h"
#include "amiga_extmem.h"

static int extmem_ok = 0;

int amiga_extmem_init(void)
{
    uint64_t test_size = 65536;
    struct ExtMemIFace *test = (struct ExtMemIFace *)AllocSysObjectTags(ASOT_EXTMEM,
        ASOEXTMEM_Size,             &test_size,
        ASOEXTMEM_AllocationPolicy, EXTMEMPOLICY_IMMEDIATE,
        TAG_DONE);
    if (test) {
        FreeSysObject(ASOT_EXTMEM, test);
        extmem_ok = 1;

        return 1;
    }
    extmem_ok = 0;

    return 0;
}

void amiga_extmem_cleanup(void) { extmem_ok = 0; }
int  amiga_extmem_available(void) { return extmem_ok; }

void *amiga_extmem_alloc(uint64_t size, void **extmem_obj)
{
    if (!extmem_ok || !extmem_obj) return NULL;
    uint64_t size64 = size;
    struct ExtMemIFace *iextmem = (struct ExtMemIFace *)AllocSysObjectTags(ASOT_EXTMEM,
        ASOEXTMEM_Size,             &size64,
        ASOEXTMEM_AllocationPolicy, EXTMEMPOLICY_IMMEDIATE,
        TAG_DONE);
    if (!iextmem) return NULL;
    void *ptr = iextmem->Map(0, (uint32_t)size, 0LL, 0);
    if (!ptr) { FreeSysObject(ASOT_EXTMEM, iextmem); return NULL; }
    *extmem_obj = iextmem;
    return ptr;
}

void amiga_extmem_free(void *ptr, uint64_t size, void *extmem_obj)
{
    if (!ptr || !extmem_obj) return;
    struct ExtMemIFace *iextmem = (struct ExtMemIFace *)extmem_obj;
    iextmem->Unmap(ptr, (uint32_t)size);
    FreeSysObject(ASOT_EXTMEM, iextmem);
}
