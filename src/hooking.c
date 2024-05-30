/*
Cuckoo Sandbox - Automated Malware Analysis.
Copyright (C) 2014-2018 Cuckoo Foundation.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>
#include "assembly.h"
#include "capstone/include/capstone.h"
#include "capstone/include/x86.h"
#include "hooking.h"
#include "memory.h"
#include "misc.h"
#include "native.h"
#include "ntapi.h"
#include "log.h"
#include "pipe.h"
#include "symbol.h"
#include "unhook.h"

#define MISSING_HANDLE_COUNT 128
#define FUNCTIONSTUBSIZE 256

static SYSTEM_INFO g_si;
static csh g_capstone;
SYSTEM_INFO system_info;

static slab_t g_function_stubs;

uintptr_t g_monitor_start;
uintptr_t g_monitor_end;

static uintptr_t g_ntdll_start;
static uintptr_t g_ntdll_end;

static uint32_t g_missing_handle_count;
static HMODULE g_missing_handles[MISSING_HANDLE_COUNT];

static const char *g_missing_blacklist[] = {
    NULL,
};

// Return address for Old_LdrLoadDll. Will be used later on to decide whether
// we are "inside" the monitor.
static uintptr_t g_Old_LdrLoadDll_address;

static void *_cs_malloc(size_t size)
{
    return mem_alloc(size);
}

static void *_cs_calloc(size_t nmemb, size_t size)
{
    return mem_alloc(nmemb * size);
}

static void *_cs_realloc(void *ptr, size_t size)
{
    return mem_realloc(ptr, size);
}

static void _cs_free(void *ptr)
{
    mem_free(ptr);
}

static void _capstone_init()
{
#if __x86_64__
    cs_open(CS_ARCH_X86, CS_MODE_64, &g_capstone);
#else
    cs_open(CS_ARCH_X86, CS_MODE_32, &g_capstone);
#endif
}

static void CALLBACK _ldr_dll_notification(ULONG reason,
    const LDR_DLL_NOTIFICATION_DATA *notification, void *param)
{
    (void) param;

    char library[MAX_PATH];

    // DLL loaded notification.
    if(reason == LDR_DLL_NOTIFICATION_REASON_LOADED && notification != NULL) {
        library_from_unicode_string(notification->Loaded.BaseDllName,
            library, sizeof(library));

        hook_library(library, notification->Loaded.DllBase);
    }
}

int hook_init(HMODULE module_handle)
{
    g_monitor_start = (uintptr_t) module_handle;
    g_monitor_end = g_monitor_start +
        module_image_size((const uint8_t *) module_handle);

    g_ntdll_start = (uintptr_t) GetModuleHandle("ntdll");
    g_ntdll_end = g_ntdll_start +
        module_image_size((const uint8_t *) g_ntdll_start);

    GetSystemInfo(&g_si);
    _capstone_init();
    return 0;
}

int hook_init2()
{
    if(g_capstone != 0) {
        cs_close(&g_capstone);
    }

    cs_opt_mem cs_mem;
    cs_mem.malloc = &_cs_malloc;
    cs_mem.calloc = &_cs_calloc;
    cs_mem.realloc = &_cs_realloc;
    cs_mem.free = &_cs_free;

    // TODO Is there an alternative besides doing your own implementation?
    cs_mem.vsnprintf = &vsnprintf;

    cs_option(0, CS_OPT_MEM, (size_t) (uintptr_t) &cs_mem);
    _capstone_init();

    // Memory for function stubs of all the hooks.
    slab_init(
        &g_function_stubs, FUNCTIONSTUBSIZE, 128, PAGE_EXECUTE_READWRITE
    );

    // TODO At the moment this only works on Vista+, not on Windows XP. As
    // shown by Brad Spengler it's fairly trivial to achieve the same on
    // Windows XP but for now.. it's fine.
    register_dll_notification(&_ldr_dll_notification, NULL);
    return 0;
}

static uintptr_t WINAPI _hook_retaddr4(void *a, void *b, void *c, void *d)
{
    (void) a; (void) b; (void) c; (void) d;

    // Probably gcc specific.
    return (uintptr_t) __builtin_return_address(0);
}

void hook_initcb_LdrLoadDll(hook_t *h)
{
    FARPROC fn = *h->orig;

    *h->orig = (FARPROC) _hook_retaddr4;

    g_Old_LdrLoadDll_address = (uintptr_t) h->handler(NULL, 0, NULL, NULL);

    *h->orig = fn;
}

uint8_t *hook_addrcb_RtlDispatchException(hook_t *h,
    uint8_t *module_address, uint32_t module_size)
{
    (void) h; (void) module_size;

    uint8_t *ki_user_exception_dispatcher = (uint8_t *)
        GetProcAddress((HMODULE) module_address, "KiUserExceptionDispatcher");
    if(ki_user_exception_dispatcher == NULL) {
        pipe("WARNING:ntdll!RtlDispatchException unable to find "
            "KiUserExceptionDispatcher [aborting hook]");
        return NULL;
    }

    // We are looking for the first relative call instruction.
    for (uint32_t idx = 0; idx < 32; idx++) {
        if(*ki_user_exception_dispatcher == 0xe8) {
            return ki_user_exception_dispatcher +
                *(int32_t *)(ki_user_exception_dispatcher + 1) + 5;
        }

        ki_user_exception_dispatcher += lde(ki_user_exception_dispatcher);
    }
    return NULL;
}

int hook_in_monitor()
{
    uintptr_t addrs[RETADDRCNT]; uint32_t count;
    int inside_LdrLoadDll = 0, outside_ntdll = 0, inside_monitor = 0;

    count = stacktrace(NULL, addrs, RETADDRCNT);
    if(count == 0) {
        return 0;
    }

    // If an address that lies within the monitor DLL is found in the
    // stacktrace then we consider this call not interesting. Except for some
    // edge cases, please keep reading.
    for (uint32_t idx = count - 1; idx >= 2 && idx < RETADDRCNT; idx--) {
        if(addrs[idx] >= g_monitor_start && addrs[idx] < g_monitor_end) {
            // If this address belongs to New_LdrLoadDll, our hook handler,
            // then we increase the following flag and continue. This helps us
            // with getting API logs for stuff happening in DllMain.
            if(addrs[idx] == g_Old_LdrLoadDll_address) {
                inside_LdrLoadDll++;
                continue;
            }

            // Inside monitor counts the amount of addresses inside the
            // monitor but without the LdrLoadDll entries.
            inside_monitor++;
            continue;
        }

        if(inside_LdrLoadDll != 0 && (
                addrs[idx] < g_ntdll_start || addrs[idx] > g_ntdll_end)) {
            outside_ntdll++;
        }
    }

    // Most common case. We are not inside LdrLoadDll and this is the first
    // occurrence of our monitor in the stacktrace.
    if(inside_LdrLoadDll == 0 && inside_monitor == 1) {
        return 0;
    }

    // Edge case. We are in LdrLoadDll and find ourselves to the first
    // non-LdrLoadDll occurrence of our monitor in the stacktrace. Or the
    // second of both, or third of both, etc. Also, at least one entry is
    // outside of ntdll, to filter LdrLoadDll's own calls.
    if(inside_LdrLoadDll != 0 && outside_ntdll != 0 &&
            inside_LdrLoadDll == inside_monitor) {
        return 0;
    }

    return 1;
}

int lde(const void *addr)
{
    if(g_capstone == 0) {
        message_box(
            NULL, "Capstone has not been initialized yet!", "Error", 0
        );
        return 0;
    }

    cs_insn *insn;

    size_t count =
        cs_disasm_ex(g_capstone, addr, 16, (uintptr_t) addr, 1, &insn);
    if(count == 0) return 0;

    int size = insn->size;

    cs_free(insn, count);
    return size;
}

int disasm(const void *addr, char *str)
{
    if(g_capstone == 0) {
        pipe("CRITICAL:Capstone has not been initialized yet!");
        return *str = 0, 0;
    }

    cs_insn *insn;

    size_t count =
        cs_disasm_ex(g_capstone, addr, 16, (uintptr_t) addr, 1, &insn);
    if(count == 0) return -1;

    int len = our_snprintf(str, DISASM_BUFSIZ, "%s", insn->mnemonic);
    if(insn->op_str[0] != 0) {
        our_snprintf(str + len, DISASM_BUFSIZ - len, " %s", insn->op_str);
    }

    cs_free(insn, count);
    return 0;
}

int hook_create_stub(uint8_t *tramp, const uint8_t *addr, int len)
{
    const uint8_t *base_addr = addr;

    while (len > 0) {
        int length = lde(addr);
        if(length == 0) return -1;

        // How many bytes left?
        len -= length;

        // Unconditional jump with 32-bit relative offset.
        if(*addr == 0xe9) {
            const uint8_t *target = addr + *(int32_t *)(addr + 1) + 5;
            tramp += asm_jump(tramp, target);
            addr += 5;
        }
        // Call with 32-bit relative offset.
        else if(*addr == 0xe8) {
            const uint8_t *target = addr + *(int32_t *)(addr + 1) + 5;
            tramp += asm_call(tramp, target);
            addr += 5;
        }
        // Conditional jump with 32bit relative offset.
        else if(*addr == 0x0f && addr[1] >= 0x80 && addr[1] < 0x90) {

#if __x86_64__
            pipe("CRITICAL:Conditional jump and calls in 64-bit are "
                 "considered unstable!");
#endif

            // TODO This can be stabilized by creating a 8-bit conditional
            // jump with 32/64-bit jumps at each target. However, this is
            // only required for 64-bit support and then only when this
            // instruction occurs at all in the original function - which is
            // currently not the case.

            // Conditional jumps consist of two bytes.
            *tramp++ = addr[0];
            *tramp++ = addr[1];

            // When a jmp/call is performed, then the relative offset +
            // the instruction pointer + the size of the instruction is the
            // resulting address, so that's our target address.
            // As we have already written the first one or two bytes of the
            // instruction we only have the relative address left - four bytes
            // in total.
            const uint8_t *target = addr + *(int32_t *)(addr + 2) + 6;

            // We have already copied the instruction opcode(s) itself so we
            // just have to calculate the relative address now.
            *(uint32_t *) tramp = target - tramp - 4;
            tramp += 4;

            addr += 6;
        }
        // Unconditional jump with 8bit relative offset.
        else if(*addr == 0xeb) {
            const uint8_t *target = addr + *(int8_t *)(addr + 1) + 2;
            tramp += asm_jump(tramp, target);
            addr += 2;

            // TODO Check the remaining length. Also keep in mind that any
            // following nop's behind this short jump can be included in the
            // remaining available space.
        }
        // Conditional jump with 8bit relative offset.
        else if(*addr >= 0x70 && *addr < 0x80) {

#if __x86_64__
            pipe("CRITICAL:Conditional jumps in 64-bit are "
                 "considered unstable!");
#endif

            // TODO The same as for the 32-bit conditional jumps.

            // Same rules apply as with the 32bit relative offsets, except
            // for the fact that both conditional and unconditional 8bit
            // relative jumps take only one byte for the opcode.

            // Hex representation of the two types of 32bit jumps;
            // 8bit relative conditional jumps:     70..80
            // 32bit relative conditional jumps: 0f 80..90
            // Thus we have to add 0x10 to the opcode of 8bit relative
            // offset jump to obtain the 32bit relative offset jump
            // opcode.
            *tramp++ = 0x0f;
            *tramp++ = addr[0] + 0x10;

            // 8bit relative offset - we have to sign-extend it, by casting it
            // as signed char, in order to calculate the correct address.
            const uint8_t *target = addr + *(int8_t *)(addr + 1) + 2;

            // Calculate the relative address.
            *(uint32_t *) tramp = (uint32_t)(target - tramp - 4);
            tramp += 4;

            addr += 2;
        }
#if __x86_64__
        // In 64-bit mode we have RIP-relative mov and lea instructions. These
        // have to be relocated properly. Handles "mov reg64, qword [offset]"
        // and "lea reg64, qword [offset]".
        else if((*addr == 0x48 || *addr == 0x4c) &&
                (addr[1] == 0x8b || addr[1] == 0x8d) &&
                (addr[2] & 0xc7) == 0x05) {
            // Register index and full address.
            uint32_t reg = ((addr[2] >> 3) & 7) + (*addr == 0x4c ? 8 : 0);
            const uint8_t *target = addr + *(int32_t *)(addr + 3) + 7;

            // mov reg64, address
            tramp[0] = 0x48 + (reg >= 8);
            tramp[1] = 0xb8 + (reg & 7);
            *(const uint8_t **)(tramp + 2) = target;
            tramp += 10;

            // If it was a mov instruction then also emit the pointer
            // dereference part.
            if(addr[1] == 0x8b) {
                // mov reg64, qword [reg64]
                tramp[0] = reg < 8 ? 0x48 : 0x4d;
                tramp[1] = 0x8b;
                tramp[2] = (reg & 7) | ((reg & 7) << 3);
                tramp += 3;
            }
            addr += 7;
        }
#endif
        // Return instruction indicates the end of basic block as well so we
        // have to check if we already have enough space for our hook..
        else if((*addr == 0xc3 || *addr == 0xc2) && len > 0) {
            return -1;
        }
        // This is a regular instruction - copy it right away.
        else {
            while (length-- != 0) {
                *tramp++ = *addr++;
            }
        }
    }

    // Jump to the original function at the point where our stub ends.
    tramp += asm_jump(tramp, addr);
    return addr - base_addr;
}

#if __x86_64__

// We scan 500mb below and above the address - in general this should be
// more than enough to find a hole in which we place our intermediate jumps.
#define CLOSEBY_RANGE 0x20000000

static uint8_t *_hook_alloc_closeby_ptr(uint8_t **last_ptr, uint32_t size)
{
    uint8_t *ret = *last_ptr;
    *last_ptr += size + (8 - (size & 7));

    // We reached the next page - reset the pointer.
    if(((uintptr_t) ret & ~0xfff) !=
            ((uintptr_t)(*last_ptr + size + (8 - (size & 7))) & ~0xfff)) {
        *last_ptr = NULL;
    }
    return ret;
}

static uint8_t *_hook_alloc_closeby(uint8_t *target, uint32_t size)
{
    static uint8_t *last_ptr = NULL; MEMORY_BASIC_INFORMATION_CROSS mbi;

    if(last_ptr != NULL && last_ptr >= target - CLOSEBY_RANGE &&
            last_ptr < target + CLOSEBY_RANGE) {
        return _hook_alloc_closeby_ptr(&last_ptr, size);
    }

    for (uint8_t *addr = target - CLOSEBY_RANGE;
            addr < target + CLOSEBY_RANGE;
            addr += g_si.dwAllocationGranularity) {

        if(virtual_query(addr, &mbi) == FALSE || mbi.State != MEM_FREE) {
            continue;
        }

        if(virtual_alloc((void *) mbi.BaseAddress, g_si.dwPageSize,
                MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE) == NULL) {
            continue;
        }

        memset((void *) mbi.BaseAddress, 0xcc, g_si.dwPageSize);
        last_ptr = (uint8_t *) mbi.BaseAddress;
        return _hook_alloc_closeby_ptr(&last_ptr, size);
    }
    return NULL;
}

int hook_create_jump(hook_t *h)
{
    uint8_t *addr = h->addr + h->skip;
    const uint8_t *target = (const uint8_t *) h->handler;
    int stub_used = h->stub_used - h->skip;

    NTSTATUS status =
        virtual_protect(addr, stub_used, PAGE_EXECUTE_READWRITE);
    if(NT_SUCCESS(status) == FALSE) {
        pipe("CRITICAL:Unable to change memory protection of %z!%z at "
            "0x%X %d to RWX (error code 0x%x)!",
            h->library, h->funcname, addr, stub_used, status);
        return -1;
    }

    // As the target is probably not close enough addr for a 32-bit relative
    // jump we allocate a separate page for an intermediate jump.
    uint8_t *closeby = _hook_alloc_closeby(addr, ASM_JUMP_SIZE);
    if(closeby == NULL) {
        pipe("CRITICAL:Unable to find closeby page for hooking!");
        return -1;
    }

    // Nop all used bytes out with int3's.
    memset(addr, 0xcc, stub_used);

    // Jump from the hooked address to our intermediate jump. The intermediate
    // jump address is within the 32-bit range a 32-bit jump can handle.
    asm_jump_32bit(addr, closeby);

    // Jump from the intermediate jump to the target address. This is a full
    // 64-bit jump.
    asm_jump(closeby, target);

    virtual_protect(addr, stub_used, PAGE_EXECUTE_READ);
    return 0;
}

#else

int hook_create_jump(hook_t *h)
{
    uint8_t *addr = h->addr + h->skip;
    const uint8_t *target = (const uint8_t *) h->handler;
    int stub_used = h->stub_used - h->skip;

    NTSTATUS status =
        virtual_protect(addr, stub_used, PAGE_EXECUTE_READWRITE);
    if(NT_SUCCESS(status) == FALSE) {
        pipe("CRITICAL:Unable to change memory protection of %z!%z at "
            "0x%X %d to RWX (error code 0x%x)!",
            h->library, h->funcname, addr, stub_used, status);
        return -1;
    }

    // Pad all used bytes out with int3's.
    memset(addr, 0xcc, stub_used);

    // Jump from the hooked address to the target address.
    asm_jump_32bit(addr, target);

    virtual_protect(addr, stub_used, PAGE_EXECUTE_READ);
    return 0;
}

#endif

#define MAXRESOLVECNT 50

static int _hook_determine_start(hook_t *h)
{
    // Under Windows 7 some functions have been replaced by a function stub
    // which in turn calls the original function. E.g., a lot of functions
    // which originally went through kernel32.dll now make a pass through
    // kernelbase.dll before reaching kernel32.dll.
    // We follow these jumps and add the regions to the list for unhook
    // detection.
    uint32_t count; uint8_t *addr = h->addr;

    for (count = 0; count < MAXRESOLVECNT; count++) {
        // jmp short imm8
        if(*addr == 0xeb) {
            unhook_detect_add_region(h->funcname, addr, addr, addr, 2);
            addr = addr + 2 + *(int8_t *)(addr + 1);
            continue;
        }

        // jmp dword [addr]
        if(*addr == 0xff && addr[1] == 0x25) {
            unhook_detect_add_region(h->funcname, addr, addr, addr, 6);

#if __x86_64__
            addr += *(int32_t *)(addr + 2) + 6;
#else
            addr = *(uint8_t **)(addr + 2);
#endif

            // In some cases we can't follow the address yet as it is not yet
            // in-memory. If so we just leave it be for the moment until it is
            // available for us.
            uint8_t *addr_deref = *(uint8_t **) addr;
            if(range_is_readable(addr_deref, sizeof(uint8_t *)) == 0) {
                return 0;
            }

            unhook_detect_add_region(h->funcname,
                addr, addr, addr, sizeof(uintptr_t));
            addr = addr_deref;
            continue;
        }

#if !__x86_64__
        // mov edi, edi ; push ebp ; mov ebp, esp ; pop ebp ; jmp short imm8
        if(memcmp(addr, "\x8b\xff\x55\x8b\xec\x5d\xeb", 7) == 0) {
            unhook_detect_add_region(h->funcname, addr, addr, addr, 8);
            addr = addr + 8 + *(int8_t *)(addr + 7);
            continue;
        }

        // mov edi, edi ; push ebp ; mov ebp, esp ; pop ebp ; jmp imm32
        if(memcmp(addr, "\x8b\xff\x55\x8b\xec\x5d\xe9", 7) == 0) {
            unhook_detect_add_region(h->funcname, addr, addr, addr, 11);
            addr = addr + 11 + *(int32_t *)(addr + 7);
            continue;
        }
#endif

        break;
    }

    // To make sure we don't enter an infinite loop.
    if(count == MAXRESOLVECNT) {
        return -1;
    }

    h->addr = addr;

    // If this function is a system call wrapper (and thus its first
    // instruction resembles "mov eax, imm32"), then skip the first
    // instruction.
    // if(memcmp(h->funcname, "Nt", 2) == 0 && *addr == 0xb8) {
        // h->skip += 5;
    // }

    return 0;
}

static int _hook_call_method_arguments(uint8_t *ptr, uint32_t signature)
{
    uint8_t *base = ptr;

#if __x86_64__
// = 16*reg + eflags + 4*scratch_space_reg
#define REG_CONTEXT_SIZE (16*8 + 8 + 4*8)
#else
// = 8*reg + eflags
#define REG_CONTEXT_SIZE (8*4 + 4)
#endif

    for (uint32_t idx = 0; idx < 4; idx++) {
        uint8_t arg = signature & 0xff; signature >>= 8;
        if(arg >= HOOK_INSN_STK(0)) {
            // push d/qword [e/rsp+X]
            ptr += asm_push_stack_offset(
                ptr, 0x1000 + REG_CONTEXT_SIZE +
                sizeof(void *) * idx + (arg - HOOK_INSN_STK(0))
            );
        }
        else if(arg >= HOOK_INSN_EAX) {
            // push register
            ptr += asm_push_register(ptr, arg - HOOK_INSN_EAX + R_R0);
        }
        else {
            // Push null "push 0".
            *ptr++ = 0x6a;
            *ptr++ = 0x00;
        }
    }

#if __x86_64__
    // On 64-bit we have the fastcall calling convention, so we pop the
    // arguments into the appropriate registers.
    *ptr++ = 0x59;                // pop rcx
    *ptr++ = 0x5a;                // pop rdx
    *ptr++ = 0x41; *ptr++ = 0x58; // pop r8
    *ptr++ = 0x41; *ptr++ = 0x59; // pop r9
#endif

    return ptr - base;
}

static int _hook_copy_insns(
    hook_t *h, uint8_t **ptr, uintptr_t *jmpaddr, int *relative,
    uintptr_t *spoff)
{
    uint8_t *addr = h->addr; *jmpaddr = *relative = *spoff = 0;
    while (addr - h->addr < 5) {
        if(*addr == 0xe8) {
            pipe("ERROR:call not yet supported");
            // *relative = 0;
            // *jmpaddr = (uintptr_t) addr + *(uint32_t *)(addr + 1) + 5;
            return -1;
        }
        // TODO Implement support for 64-bit jumps & RIP-relative addressing.
        if(*addr == 0xe9) {
            *relative = 0;
            *jmpaddr = (uintptr_t) addr + *(int32_t *)(addr + 1) + 5;
            addr += 5;
            continue;
        }
        if(*addr == 0xeb) {
            *relative = 0;
            *jmpaddr = (uintptr_t) addr + *(int8_t *)(addr + 1) + 2;
            addr += 2;
            continue;
        }
        if(*addr >= 0x70 && *addr < 0x80) {
            *relative = 1 + *addr - 0x70;
            *jmpaddr = (uintptr_t) addr + *(int8_t *)(addr + 1) + 2;
            addr += 2;
            continue;
        }
        if(*addr == 0x0f && addr[1] >= 0x80 && addr[1] < 0x90) {
            *relative = 1 + addr[1] - 0x80;
            *jmpaddr = (uintptr_t) addr + *(int32_t *)(addr + 2) + 6;
            addr += 6;
            continue;
        }
        if(*addr >= 0x50 && *addr < 0x58) {
            *spoff += sizeof(void *);
        }
#if __x86_64__
        if(*addr == 0x41 && addr[1] >= 0x50 && addr[1] < 0x58) {
            *spoff += sizeof(void *);
        }
#endif

        if(*relative == 0 && *jmpaddr != 0) {
            char hex[40]; hexdump(hex, h->addr, 16);
            pipe("CRITICAL:Unable to create Page Guard hotpatch for 0x%x due "
                "to a limited memory availability (%z).", h->addr, hex);
            return -1;
        }

        uint32_t len = lde(addr);

        memcpy(*ptr, addr, len);
        addr += len;
        *ptr += len;
    }
    return addr - h->addr;
}

static int _hook_emit_jump(uint8_t *ptr, uintptr_t jmpaddr, int relative)
{
    if(jmpaddr == 0) {
        return 0;
    }

    if(relative == 0) {
        return asm_jump_32bit(ptr, (void *) jmpaddr);
    }
    else {
        return asm_jump_32bit_rel(ptr, (void *) jmpaddr, relative - 1);
    }
}

int hook_insn(hook_t *h, uint32_t signature)
{
    uint8_t *ptr = h->func_stub; int r, relative; uintptr_t jmpaddr, spoff;

    ptr += asm_sub_esp_imm(ptr, 0x1000);
    ptr += asm_push_context(ptr);

#if __x86_64__
    // Allocate 32 (= 8*4) bytes for the x86_64 scratch space.
    ptr += asm_sub_regimm(ptr, R_RSP, 4 * sizeof(void *));
#endif

    r = _hook_call_method_arguments(ptr, signature);
    if(r < 0) {
        return r;
    }

    ptr += r;

    // We're cheating a little bit here. The hook() function will create a
    // jump from the targeted instruction(s) to h->handler. Since normally
    // this is a simple jump we will for now fake the handler thing and point
    // it to the function stub which in turn points to the original handler.
    ptr += asm_call(ptr, h->handler);
    h->handler = (FARPROC) h->func_stub;

#if __x86_64__
    // Deallocate 32 (= 8*4) bytes for the x86_64 scratch space.
    ptr += asm_add_regimm(ptr, R_RSP, 4 * sizeof(void *));
#endif

    ptr += asm_pop_context(ptr);
    ptr += asm_add_esp_imm(ptr, 0x1000);

    r = _hook_copy_insns(h, &ptr, &jmpaddr, &relative, &spoff);
    if(r < 0) {
        return r;
    }

    if(jmpaddr != 0) {
        pipe("ERROR:Instruction-level hooking does not yet support jumps");
        return -1;
    }

    ptr += asm_jump(ptr, h->addr + r);

    if((uintptr_t)(ptr - h->func_stub) >= slab_size(&g_function_stubs)) {
        pipe(
            "ERROR:The stub created for hook %z used too much space, space "
            "should be enlarged to accommodate such usage.", h->funcname
        );
        return -1;
    }
    return r;
}

int hook_hotpatch_guardpage(hook_t *h)
{
    uint8_t *ptr = h->func_stub; int r, relative; uintptr_t jmpaddr, spoff;

    ptr += asm_sub_esp_imm(ptr, 0x1000);
    ptr += asm_push_context(ptr);

    r = exploit_insn_rewrite_to_lea(ptr, h->addr);
    if(r < 0) {
        return r;
    }

    ptr += r;
    ptr += asm_push_register(ptr, R_R0);
    ptr += asm_push_register(ptr, R_R0);
    ptr += asm_call(ptr, &exploit_unset_guard_page);
    ptr += asm_call(ptr, &log_guardrw);

    ptr += asm_pop_context(ptr);
    ptr += asm_add_esp_imm(ptr, 0x1000);

    h->stub_used = r = _hook_copy_insns(h, &ptr, &jmpaddr, &relative, &spoff);
    if(r < 0) {
        return r;
    }

    ptr += asm_sub_esp_imm(ptr, 0x1000 - spoff);
    ptr += asm_push_context(ptr);

    // Black magic incoming. Because the register containing the address of
    // the guard page may be changed during the emitted instructions just
    // above here, we are required to use the address set earlier. We'll be
    // using the address that was set earlier through a kind of "unitialized
    // stack variable" method. Seems to work just fine though.
    ptr += asm_sub_esp_imm(ptr, 4);
    ptr += asm_call(ptr, &exploit_set_guard_page);

    ptr += asm_pop_context(ptr);
    ptr += asm_add_esp_imm(ptr, 0x1000 - spoff);

    ptr += _hook_emit_jump(ptr, jmpaddr, relative);

    ptr += asm_jump_32bit(ptr, h->addr + h->stub_used);

    h->handler = (FARPROC) h->func_stub;

    if((uintptr_t)(ptr - h->func_stub) >= slab_size(&g_function_stubs)) {
        pipe(
            "ERROR:The stub created for hook %z used too much space, space "
            "should be enlarged to accommodate such usage.", h->funcname
        );
        return -1;
    }
    return 0;
}

typedef int(WINAPI *pfFunc)(HWND, LPCSTR, LPCSTR, UINT);
pfFunc OldFunctionAddress = NULL;

PIMAGE_NT_HEADERS GetLocalNtHead()
{
	DWORD dwTemp = NULL;
	PIMAGE_DOS_HEADER pDosHead = NULL;
	PIMAGE_NT_HEADERS pNtHead = NULL;
	HMODULE ImageBase = GetModuleHandle(NULL);                              
	pDosHead = (PIMAGE_DOS_HEADER)(DWORD)ImageBase;                         
	dwTemp = (DWORD)pDosHead + (DWORD)pDosHead->e_lfanew;
	pNtHead = (PIMAGE_NT_HEADERS)dwTemp;                                   
	return pNtHead;
}

int IATHook(hook_t *h)
{
	PVOID pFuncAddress = NULL;
	pFuncAddress = GetProcAddress(GetModuleHandleA(h->library), h->funcname);  
  printf("function name = %s\n", h->funcname);
  printf("old = %p\n", pFuncAddress);
  sleep(100);
	OldFunctionAddress = (pfFunc)pFuncAddress;                                  
	PIMAGE_NT_HEADERS pNtHead = GetLocalNtHead();                                  
	PIMAGE_FILE_HEADER pFileHead = (PIMAGE_FILE_HEADER)&pNtHead->FileHeader;
	PIMAGE_OPTIONAL_HEADER pOpHead = (PIMAGE_OPTIONAL_HEADER)&pNtHead->OptionalHeader;

	DWORD dwInputTable = pOpHead->DataDirectory[1].VirtualAddress;    
	DWORD dwTemp = (DWORD)GetModuleHandle(NULL) + dwInputTable;
	PIMAGE_IMPORT_DESCRIPTOR   pImport = (PIMAGE_IMPORT_DESCRIPTOR)dwTemp;
	PIMAGE_IMPORT_DESCRIPTOR   pCurrent = pImport;
	DWORD *pFirstThunk; 

  int flag = 0;
	while (pCurrent->Characteristics && pCurrent->FirstThunk != NULL)
	{
		dwTemp = pCurrent->FirstThunk + (DWORD)GetModuleHandle(NULL);
		pFirstThunk = (DWORD *)dwTemp;                               
		while (*(DWORD*)pFirstThunk != NULL)                      
		{
			if (*(DWORD*)pFirstThunk == (DWORD)OldFunctionAddress)       
			{
				DWORD oldProtected;
				VirtualProtect(pFirstThunk, 0x1000, PAGE_EXECUTE_READWRITE, &oldProtected);  
				dwTemp = (DWORD)h->handler;
        sleep(100);
				memcpy(pFirstThunk, (DWORD *)&dwTemp, 4);
				VirtualProtect(pFirstThunk, 0x1000, oldProtected, &oldProtected);            
        pipe("success to hook function %s\n", h->funcname);
        return 1;
			}
			pFirstThunk++; 
		}
		pCurrent++;        
    
	}
}


HMODULE handle;
struct hookInfo {
    void* source;
    void* destination;
    struct hookInfo* next;
};

struct hookInfo* first = NULL;

LONG exceptionHandler(EXCEPTION_POINTERS* exceptionInfo) {
    log_debug("entering handler\n");
    struct hookInfo* now = first;
    if (exceptionInfo -> ExceptionRecord -> ExceptionCode == EXCEPTION_GUARD_PAGE) {
        log_debug("in first if\n");
        do {
            if (exceptionInfo -> ExceptionRecord -> ExceptionAddress == now -> source) {
#if __x86_64__
                exceptionInfo -> ContextRecord -> Rip = (void*) now -> destination;
#else
                exceptionInfo -> ContextRecord -> Eip = (void*) now -> destination;
                exceptionInfo -> ContextRecord -> EFlags |= PAGE_GUARD;
                log_debug("finish change EIP\n");
                return EXCEPTION_CONTINUE_EXECUTION;
#endif
            }
            now = now -> next;
        } while (now != NULL);

        exceptionInfo -> ContextRecord -> EFlags |= PAGE_GUARD;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    else if (exceptionInfo -> ExceptionRecord -> ExceptionCode == EXCEPTION_SINGLE_STEP) {
        log_debug("in second if\n");
        do {
            DWORD tmp;
            VirtualProtect(now -> source, system_info.dwPageSize, PAGE_EXECUTE_READ | PAGE_GUARD, &tmp);
            now = now -> next;
        } while (now != NULL);
        log_debug("finish add PAGE_GUARD again\n");
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}


struct hookInfo* createNode(void* source, void* destination) {
    struct hookInfo* newNode = (struct hookInfo*)malloc(sizeof(struct hookInfo));
    if (newNode == NULL) {
        log_debug("Memory allocation failed.\n");
        exit(1);
    }
    newNode-> source = source;
    newNode->destination = destination;
    newNode->next = NULL;
    return newNode;
}

void appendNode(struct hookInfo** headRef, void* source, void* destination) {
    struct hookInfo* newNode = createNode(source, destination);
    if (*headRef == NULL) {
        *headRef = newNode;
        return;
    }
    struct hookInfo* current = *headRef;
    while (current->next != NULL) {
        current = current->next;
    }
    current->next = newNode;
    log_debug("finish appendNode\n");
}

void setupHandler() {
    GetSystemInfo(&system_info);
    handle = AddVectoredExceptionHandler(1, exceptionHandler);
    log_debug("success to setup handler\n");
}

BOOL Hook(void* source, void* destination) {
    if (!handle) return FALSE;

    MEMORY_BASIC_INFORMATION source_info;
    if (!VirtualQuery(source, &source_info, sizeof(MEMORY_BASIC_INFORMATION))) {
        log_debug("fail at one\n");
        return FALSE;
    }

    MEMORY_BASIC_INFORMATION destination_info;
    if (!VirtualQuery(destination, &destination_info, sizeof(MEMORY_BASIC_INFORMATION))) {
        log_debug("fail at two\n");
        return FALSE;
    }
    if (source_info.AllocationBase == destination_info.AllocationBase) {
        log_debug("fail at three\n");
        return FALSE;
    }
    appendNode(&first, source, destination);
    DWORD tmp;
    VirtualProtect(source, system_info.dwPageSize, PAGE_EXECUTE_READ | PAGE_GUARD, &tmp);
    log_debug("finish hook\n");
    return TRUE;
}

int check = 0;

int hook(hook_t *h, void *module_handle)
{
    if (!check) {
        setupHandler();
        check++;
    }
    if(h->is_hooked != 0) {
        return 0;
    }
    
    if (Hook(GetProcAddress(GetModuleHandleA(h->library), h->funcname), h->handler)) {
        h -> is_hooked = 1;
        return 1;
  }
}

uint8_t *hook_get_mem()
{
    return slab_getmem(&g_function_stubs);
}

static void _hook_missing_hooks_worker(
    const char *funcname, uintptr_t address, void *module_handle)
{
    // This is not a missing hook.
    for (hook_t *h = sig_hooks(); h->funcname != NULL; h++) {
        if(strcmp(h->funcname, funcname) == 0) {
            return;
        }
    }

    // Check our function name blacklist.
    for (const char **ptr = g_missing_blacklist; *ptr != NULL; ptr++) {
        if(strcmp(*ptr, funcname) == 0) {
            return;
        }
    }

    uint8_t *handler = slab_getmem(&g_function_stubs);
    uint8_t *ptr = handler;

    hook_t h;

    memset(&h, 0, sizeof(h));
    h.addr = (uint8_t *) address;
    h.handler = (FARPROC) handler;
    h.funcname = funcname;

    if(hook(&h, module_handle) == 0) {
        ptr += asm_pushv(ptr, funcname);
        ptr += asm_call(ptr, &log_missing_hook);
        ptr += asm_jump(ptr, h.func_stub);
        log_debug("Welcome missing hook: %s\n", funcname);
    }
    else {
        log_debug("Error hooking missing hook: %s\n", funcname);
    }
}

int hook_missing_hooks(HMODULE module_handle)
{
    for (uint32_t idx = 0; idx < g_missing_handle_count; idx++) {
        if(g_missing_handles[idx] == module_handle) {
            return 0;
        }
    }

    if(g_missing_handle_count == MISSING_HANDLE_COUNT) {
        pipe("CRITICAL:Reached missing handle count!");
        return -1;
    }

    g_missing_handles[g_missing_handle_count++] = module_handle;

    log_debug("Applying missing hooks @ %p\n", module_handle);
    symbol_enumerate_module(module_handle,
        &_hook_missing_hooks_worker, module_handle);
    log_debug("Finished missing hooks @ %p\n", module_handle);
    return 0;
}
