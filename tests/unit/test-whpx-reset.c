/* SPDX-License-Identifier: GPL-2.0-or-later */
/* check-whpx-reset.py compiles the actual WHPX and xstate helpers into this test. */
#include "qemu/osdep.h"
#include "cpu.h"
#include "qemu/memalign.h"
#include "qemu/error-report.h"
#include "host-cpu.h"
#include <winhvplatform.h>
#include "system/whpx.h"
#include "system/whpx-internal.h"
#include "system/whpx-common.h"
#include "system/whpx-accel-ops.h"
#include "whpx/whpx-i386.h"

#undef X86_CPU
#define X86_CPU(cs) container_of(cs, X86CPU, parent_obj)
#ifndef WHV_E_INSUFFICIENT_BUFFER
#define WHV_E_INSUFFICIENT_BUFFER ((HRESULT)0x80370301)
#endif

struct whpx_state whpx_global;
struct WHPDispatch whp_dispatch;
bool whpx_irqchip_in_kernel = true;
static bool legacy, fail_read, fail_registers, short_read;
static unsigned read_calls, sets, registers, errors;
static UINT32 required_size = 4096;
static uint64_t mtrr_cap;
static bool fail_mtrr_query, fail_mtrr_write;
static unsigned mtrr_writes;

bool target_big_endian(void)
{
    return false;
}

void error_report(const char *fmt, ...)
{
    errors++;
}

void *qemu_memalign(size_t alignment, size_t size)
{
    void *p = _aligned_malloc(size, alignment);
    assert(p);
    return p;
}

void qemu_vfree(void *p)
{
    _aligned_free(p);
}

int getpagesize(void)
{
    return 4096;
}

bool whpx_is_legacy_os(void)
{
    return legacy;
}

bool whpx_has_xsave(void)
{
    return true;
}

static size_t whpx_get_xsave_max_len(void)
{
    return 832;
}

void cpu_x86_cpuid(CPUX86State *env, uint32_t leaf, uint32_t subleaf,
                   uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    assert(leaf == 0xd && subleaf == XSTATE_YMM_BIT);
    *eax = x86_ext_save_areas[subleaf].size;
    *ebx = x86_ext_save_areas[subleaf].offset;
    *ecx = *edx = 0;
}

static HRESULT WINAPI get_legacy(WHV_PARTITION_HANDLE partition, UINT32 vp,
                                 void *buffer, UINT32 size, UINT32 *written)
{
    X86XSaveHeader *header;
    uint64_t value = UINT64_C(0x1122334455667788);

    read_calls++;
    if (fail_read) {
        return E_FAIL;
    }
    *written = required_size;
    if (size < required_size) {
        return WHV_E_INSUFFICIENT_BUFFER;
    }
    memset(buffer, 0, size);
    header = (void *)((uint8_t *)buffer + sizeof(X86LegacyXSaveArea));
    header->xstate_bv = 7;
    header->xcomp_bv = (UINT64_C(1) << 63) | 7;
    memcpy((uint8_t *)buffer + x86_ext_save_areas[XSTATE_YMM_BIT].offset,
           &value, sizeof(value));
    if (short_read) {
        *written = 8;
    }
    return S_OK;
}

static HRESULT WINAPI get_modern(WHV_PARTITION_HANDLE partition, UINT32 vp,
                                 WHV_VIRTUAL_PROCESSOR_STATE_TYPE type,
                                 void *buffer, UINT32 size, UINT32 *written)
{
    assert(type == WHvVirtualProcessorStateTypeXsaveState);
    return get_legacy(partition, vp, buffer, size, written);
}

static HRESULT WINAPI set_legacy(WHV_PARTITION_HANDLE partition, UINT32 vp,
                                 void *buffer, UINT32 size)
{
    sets++;
    assert(size == required_size);
    assert(((X86XSaveHeader *)((uint8_t *)buffer +
                              sizeof(X86LegacyXSaveArea)))->xstate_bv == 0);
    for (unsigned i = 832; i < size; i++) {
        assert(((uint8_t *)buffer)[i] == 0);
    }
    return S_OK;
}

static HRESULT WINAPI set_modern(WHV_PARTITION_HANDLE partition, UINT32 vp,
                                 WHV_VIRTUAL_PROCESSOR_STATE_TYPE type,
                                 void *buffer, UINT32 size)
{
    assert(type == WHvVirtualProcessorStateTypeXsaveState);
    return set_legacy(partition, vp, buffer, size);
}

static HRESULT WINAPI set_registers(WHV_PARTITION_HANDLE partition, UINT32 vp,
                                    const WHV_REGISTER_NAME *names, UINT32 count,
                                    const WHV_REGISTER_VALUE *values)
{
    static const WHV_REGISTER_NAME expected[] = {
        WHvRegisterPendingInterruption, WHvRegisterPendingEvent,
        WHvRegisterInterruptState, WHvX64RegisterDeliverabilityNotifications,
        WHvRegisterInternalActivityState,
    };
    registers++;
    if (fail_registers) {
        return E_FAIL;
    }
    if (names[0] == WHvX64RegisterMsrMtrrDefType) {
        unsigned idx = 0;
        assert(names[idx++] == WHvX64RegisterMsrMtrrDefType);
        if (mtrr_cap & MSR_MTRRcap_FIXRANGE_SUPPORT) {
            for (unsigned i = 0; i < 11; i++) {
                assert(names[idx++] == WHvX64RegisterMsrMtrrFix64k00000 + i);
            }
        }
        for (unsigned i = 0; i < (mtrr_cap & 0xff); i++) {
            assert(names[idx++] == WHvX64RegisterMsrMtrrPhysBase0 + i);
            assert(names[idx++] == WHvX64RegisterMsrMtrrPhysMask0 + i);
        }
        assert(count == idx);
        for (unsigned i = 0; i < count; i++) {
            assert(values[i].Reg128.Low64 == 0 && values[i].Reg128.High64 == 0);
        }
        if (fail_mtrr_write) {
            return E_FAIL;
        }
        mtrr_writes++;
        return S_OK;
    }
    assert(count == ARRAY_SIZE(expected) - !whpx_irqchip_in_kernel);
    for (unsigned i = 0; i < count; i++) {
        assert(names[i] == expected[i]);
        assert(values[i].Reg128.Low64 ==
               (i == count - 1 && whpx_irqchip_in_kernel && vp != 0));
        assert(values[i].Reg128.High64 == 0);
    }
    return S_OK;
}

static HRESULT WINAPI get_registers(WHV_PARTITION_HANDLE partition, UINT32 vp,
                                    const WHV_REGISTER_NAME *names, UINT32 count,
                                    WHV_REGISTER_VALUE *values)
{
    assert(count == 1 && names[0] == WHvX64RegisterMsrMtrrCap);
    if (fail_mtrr_query) {
        return E_FAIL;
    }
    values[0].Reg64 = mtrr_cap;
    return S_OK;
}

#include "whpx-reset-under-test.h"

int main(void)
{
    uint32_t eax, ebx, ecx, edx;

    host_cpuid(0xd, XSTATE_YMM_BIT, &eax, &ebx, &ecx, &edx);
    x86_ext_save_areas[XSTATE_YMM_BIT].offset = ebx;
    whp_dispatch.WHvGetVirtualProcessorState = get_modern;
    whp_dispatch.WHvGetVirtualProcessorXsaveState = get_legacy;
    whp_dispatch.WHvSetVirtualProcessorState = set_modern;
    whp_dispatch.WHvSetVirtualProcessorXsaveState = set_legacy;
    whp_dispatch.WHvSetVirtualProcessorRegisters = set_registers;
    whp_dispatch.WHvGetVirtualProcessorRegisters = get_registers;

    for (unsigned api = 0; api < 2; api++) {
        legacy = api;
        for (unsigned vp = 0; vp < 8; vp++) {
            X86CPU cpu = {0};
            AccelCPUState accel = {0};
            CPUState *cs = &cpu.parent_obj;
            unsigned before;

            cs->accel = &accel;
            cs->cpu_index = vp;
            cs->halted = vp != 0;
            cpu.env.xsave_buf_len = 832;
            cpu.env.xsave_buf = qemu_memalign(4096, 832);
            cpu.env.features[FEAT_XSAVE_XCR0_LO] = 7;
            before = read_calls;
            if (whpx_get_xsave_state(cs) < 0) {
                fputs("FAIL XSAVE read did not negotiate required buffer size\n",
                      stderr);
                return 1;
            }
            assert(read_calls == before + 2 && accel.xsave_state_size == required_size);
            assert(cpu.env.xmm_regs[0].ZMM_Q(2) == UINT64_C(0x1122334455667788));
            before = read_calls;
            assert(whpx_get_xsave_state(cs) == 0 && read_calls == before + 1);

            cpu.env.xstate_bv = 0;
            memset(cpu.env.xmm_regs, 0, sizeof(cpu.env.xmm_regs));
            cpu.env.cr[4] = 0;
            before = sets;
            test_xstate_write(cs, WHPX_LEVEL_RESET_STATE);
            assert(sets == before + 1);
            test_xstate_write(cs, WHPX_LEVEL_RUNTIME_STATE);
            assert(sets == before + 1);
            cpu.env.cr[4] = CR4_OSXSAVE_MASK;
            accel.xsave_state_stale = true;
            test_xstate_write(cs, WHPX_LEVEL_RUNTIME_STATE);
            assert(sets == before + 1);

            accel.window_registered = accel.ready_for_pic_interrupt = true;
            accel.interruptable = accel.interruption_pending = true;
            accel.window_priority = 0x70;
            accel.xsave_state_stale = true;
            memset(&accel.exit_ctx, 0xff, sizeof(accel.exit_ctx));
            assert(whpx_reset_vcpu_state(cs));
            assert(!accel.window_registered && !accel.ready_for_pic_interrupt);
            assert(!accel.interruptable && !accel.interruption_pending);
            assert(!accel.xsave_state_stale && accel.window_priority == 0);
            assert(accel.exit_ctx.ExitReason == 0);
            assert(accel.xsave_state_size == required_size);

            whpx_irqchip_in_kernel = false;
            assert(whpx_reset_vcpu_state(cs));
            whpx_irqchip_in_kernel = true;

            fail_registers = true;
            accel.interruption_pending = true;
            assert(!whpx_reset_vcpu_state(cs) && accel.interruption_pending);
            fail_registers = false;
            fail_read = true;
            before = read_calls;
            assert(whpx_get_xsave_state(cs) < 0 && read_calls == before + 1);
            assert(accel.xsave_state_stale);
            fail_read = false;
            short_read = true;
            assert(whpx_get_xsave_state(cs) < 0 && accel.xsave_state_stale);
            short_read = false;
            qemu_vfree(cpu.env.xsave_buf);
        }
        printf("PASS %s API: 8 VPs, size retry/cache/writeback, pending reset, "
               "and explicit failure paths\n", legacy ? "legacy" : "modern");
    }
    assert(sets == 16 && registers == 48 && errors == 48);
    {
        X86CPU cpu = {0};
        AccelCPUState accel = {0};
        CPUState *cs = &cpu.parent_obj;
        const unsigned counts[] = {0, 8, 16};
        cs->accel = &accel;
        cpu.env.features[FEAT_1_EDX] = CPUID_MTRR;
        for (unsigned fixed = 0; fixed < 2; fixed++) {
            for (unsigned i = 0; i < ARRAY_SIZE(counts); i++) {
                mtrr_cap = counts[i] | (fixed ? MSR_MTRRcap_FIXRANGE_SUPPORT : 0);
                assert(whpx_reset_vcpu_state(cs));
            }
        }
        assert(mtrr_writes == 6);
        accel.interruption_pending = true;
        fail_mtrr_query = true;
        assert(!whpx_reset_vcpu_state(cs) && accel.interruption_pending);
        fail_mtrr_query = false;
        mtrr_cap = 17;
        assert(!whpx_reset_vcpu_state(cs) && accel.interruption_pending);
        mtrr_cap = 8;
        fail_mtrr_write = true;
        assert(!whpx_reset_vcpu_state(cs) && accel.interruption_pending);
        assert(mtrr_writes == 6);
        puts("PASS MTRR reset: fixed/variable ranges, 0/8/16 banks and failures");
    }
    return 0;
}
