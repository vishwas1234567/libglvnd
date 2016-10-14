/*
 * Copyright (c) 2015, NVIDIA CORPORATION.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and/or associated documentation files (the
 * "Materials"), to deal in the Materials without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Materials, and to
 * permit persons to whom the Materials are furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * unaltered in all copies or substantial portions of the Materials.
 * Any additions, deletions, or changes to the original source files
 * must be clearly indicated in accompanying documentation.
 *
 * If only executable code is distributed, then the accompanying
 * documentation must state that "this software is based in part on the
 * work of the Khronos Group."
 *
 * THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * MATERIALS OR THE USE OR OTHER DEALINGS IN THE MATERIALS.
 */

#include "glvnd_genentry.h"
#include "utils_misc.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>

#define USE_ASM (defined(USE_X86_ASM) ||    \
                 defined(USE_X86_64_ASM) || \
                 defined(USE_ARMV7_ASM))

#if defined(__GNUC__) && USE_ASM

/// The maximum number of entrypoints that we can generate.
#define GENERATED_ENTRYPOINT_MAX 4096

/// The size of each generated entrypoint.
static const int STUB_ENTRY_SIZE = 16;

#if defined(USE_X86_ASM)
/// A template used to generate an entrypoint.
static unsigned char STUB_TEMPLATE[] =
{
    0xe9, 0x78, 0x56, 0x34, 0x12, // jmp 0x12345678
};

static const int DISPATCH_FUNC_OFFSET = 1;
static const int DISPATCH_FUNC_OFFSET_REL = 5;

#elif defined(USE_X86_64_ASM)
// For x86_64, the offset from the entrypoint to the dispatch function might be
// more than 2^31, and there's no JMP instruction that takes a 64-bit offset.
static unsigned char STUB_TEMPLATE[] =
{
    0x48, 0xb8, 0xbd, 0xac, 0xcd, 0xab, 0x78, 0x56, 0x34, 0x12, // movabs 0x12345678abcdacbd,%rax
    0xff, 0xe0, // jmp *%rax
};

static const int DISPATCH_FUNC_OFFSET = 2;

#elif defined(USE_ARMV7_ASM)
// Thumb bytecode
static const uint16_t STUB_TEMPLATE[] =
{
    // ldr ip, 1f
    0xf8df, 0xc004,
    // bx ip
    0x4760,
    // nop
    0xbf00,
    // Offset that needs to be patched
    // 1:
    0x0000, 0x0000,
};

static const int DISPATCH_FUNC_OFFSET = 8;

#else
#error "Can't happen -- not implemented"
#endif

typedef struct GLVNDGenEntrypointRec
{
    /// The name of the function.
    char *procName;

    /// The generated entrypoint function, mapped as read/write.
    uint8_t *entrypointWrite;

    /// The generated entrypoint function, mapped as read/exec.
    GLVNDentrypointStub entrypointExec;

    /// Set to 1 if we've assigned a dispatch function to this entrypoint.
    int assigned;
} GLVNDGenEntrypoint;

/**
 * Allocates memory for all of the entrypoint functions.
 *
 * \return Zero on success, non-zero on failure.
 */
static int InitEntrypoints(void);

/**
 * Generates a new entrypoint.
 *
 * \param entry The entrypoint structure to fill in.
 * \param index The index of the dispatch function.
 */
static void GenerateEntrypointFunc(GLVNDGenEntrypoint *entry, int index);

/**
 * A default function plugged into the entrypoints. This is called if no vendor
 * library has supplied a dispatch function.
 */
static void *DefaultDispatchFunc(void);

/**
 * Patches an entrypoint to assign a dispatch function to it.
 */
static void SetDispatchFuncPointer(GLVNDGenEntrypoint *entry,
        GLVNDentrypointStub dispatch);

static GLVNDGenEntrypoint entrypoints[GENERATED_ENTRYPOINT_MAX] = {};
static uint8_t *entrypointBufferWrite = NULL;
static uint8_t *entrypointBufferExec = NULL;
static int entrypointCount = 0;

GLVNDentrypointStub glvndGenerateEntrypoint(const char *procName)
{
    int i;

    if (InitEntrypoints() != 0) {
        return NULL;
    }

    for (i=0; i<entrypointCount; i++) {
        if (strcmp(procName, entrypoints[i].procName) == 0) {
            // We already generated this function, so return it.
            return entrypoints[i].entrypointExec;
        }
    }

    if (entrypointCount < GENERATED_ENTRYPOINT_MAX) {
        GLVNDGenEntrypoint *entry = &entrypoints[entrypointCount];
        entry->procName = strdup(procName);
        if (entry->procName == NULL) {
            return NULL;
        }
        entry->assigned = 0;
        GenerateEntrypointFunc(entry, entrypointCount);

        entrypointCount++;
        return entry->entrypointExec;
    }

    return NULL;
}

void glvndUpdateEntrypoints(GLVNDentrypointUpdateCallback callback, void *param)
{
    int i;

    for (i=0; i<entrypointCount; i++) {
        if (!entrypoints[i].assigned) {
            GLVNDentrypointStub addr = callback(entrypoints[i].procName, param);
            if (addr != NULL) {
                SetDispatchFuncPointer(&entrypoints[i], addr);
                entrypoints[i].assigned = 1;
            }
        }
    }
}

void glvndFreeEntrypoints(void)
{
    int i;
    for (i=0; i<entrypointCount; i++) {
        free(entrypoints[i].procName);
        entrypoints[i].procName = NULL;
        entrypoints[i].entrypointWrite = NULL;
        entrypoints[i].entrypointExec = NULL;
        entrypoints[i].assigned = 0;
    }
    entrypointCount = 0;

    if (entrypointBufferExec != NULL) {
        FreeExecPages(STUB_ENTRY_SIZE * GENERATED_ENTRYPOINT_MAX,
                entrypointBufferWrite, entrypointBufferExec);
        entrypointBufferWrite = NULL;
        entrypointBufferExec = NULL;
    }
}

int InitEntrypoints(void)
{
    if (entrypointBufferExec == NULL) {
        void *writeBuf, *execBuf;
        if (AllocExecPages(STUB_ENTRY_SIZE * GENERATED_ENTRYPOINT_MAX,
                &writeBuf, &execBuf) != 0) {
            return -1;
        }
        entrypointBufferWrite = (uint8_t *) writeBuf;
        entrypointBufferExec = (uint8_t *) execBuf;
    }
    return 0;
}

void GenerateEntrypointFunc(GLVNDGenEntrypoint *entry, int index)
{
    entry->entrypointWrite = entrypointBufferWrite + (index * STUB_ENTRY_SIZE);
    entry->entrypointExec = (GLVNDentrypointStub)
        (entrypointBufferExec + (index * STUB_ENTRY_SIZE));

    assert(STUB_ENTRY_SIZE >= sizeof(STUB_TEMPLATE));

    // Copy the template into our buffer.
    memcpy(entry->entrypointWrite, STUB_TEMPLATE, sizeof(STUB_TEMPLATE));

#if defined(USE_ARMV7_ASM)
    // Add 1 to the base address to force Thumb mode when jumping to the stub
    entry->entrypointExec = (GLVNDentrypointStub)((char *)entry->entrypointExec + 1);
#endif

    // Assign DefaultDispatchFunc as the dispatch function.
    SetDispatchFuncPointer(entry, (GLVNDentrypointStub) DefaultDispatchFunc);
}

void SetDispatchFuncPointer(GLVNDGenEntrypoint *entry,
        GLVNDentrypointStub dispatch)
{
    uint8_t *code = entry->entrypointWrite;

#if defined(USE_X86_ASM)
    // For x86, we use a JMP instruction with a PC-relative offset. Figure out
    // the offset from the generated entrypoint to the dispatch function.
    intptr_t offset = ((intptr_t) dispatch) - ((intptr_t) entry->entrypointExec) - DISPATCH_FUNC_OFFSET_REL;
    *((intptr_t *) (code + DISPATCH_FUNC_OFFSET)) = offset;

#elif defined(USE_X86_64_ASM)
    // For x86_64, we have to use a movabs instruction, which needs the
    // absolute address of the dispatch function.
    *((GLVNDentrypointStub *) (code + DISPATCH_FUNC_OFFSET)) = dispatch;

#elif defined(USE_ARMV7_ASM)
    *((uint32_t *)(code + DISPATCH_FUNC_OFFSET)) = (uint32_t)dispatch;

    // Make sure the base address has the Thumb mode bit
    assert((uintptr_t)entry->entrypointExec & (uintptr_t)0x1);

    // See http://community.arm.com/groups/processors/blog/2010/02/17/caches-and-self-modifying-code
    __builtin___clear_cache((char *)entry->entrypointExec - 1,
                            (char *)entry->entrypointExec - 1 + sizeof(STUB_TEMPLATE));
#else
#error "Can't happen -- not implemented"
#endif
}

void *DefaultDispatchFunc(void)
{
    // Print a warning message?
    return NULL;
}

#else // defined(__GNUC__) && USE_ASM

GLVNDentrypointStub glvndGenerateEntrypoint(const char *procName)
{
    return NULL;
}

void glvndFreeEntrypoints(void)
{
}

void glvndUpdateEntrypoints(GLVNDentrypointUpdateCallback callback, void *param)
{
}

#endif // defined(__GNUC__) && USE_ASM
