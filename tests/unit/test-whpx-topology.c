/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Run with check-whpx-topology.py: the generated header contains real source. */
#include "qemu/osdep.h"
#include "cpu.h"
#include "whpx-topology-under-test.h"

static bool rejects_without_changes(X86CPU *cpu, uint32_t msr, bool write)
{
    X86CPU before;
    uint64_t value = UINT64_C(0xfedcba9876543210);

    memcpy(&before, cpu, sizeof(before));
    return !whpx_access_msr(&cpu->env, msr, write, &value) &&
           value == UINT64_C(0xfedcba9876543210) &&
           memcmp(&before, cpu, sizeof(before)) == 0;
}

int main(void)
{
    const struct {
        const char *name;
        X86CPUTopoInfo topology; /* dies, modules, cores, threads */
        unsigned packages;
        uint64_t expected;
    } cases[] = {
        { "8 cores, no SMT", {1, 1, 8, 1}, 1, UINT64_C(0x00080008) },
        { "8 cores, SMT", {1, 1, 8, 2}, 1, UINT64_C(0x00080010) },
        { "2 packages, 4 cores, SMT", {1, 1, 4, 2}, 2,
          UINT64_C(0x00040008) },
        { "2 packages, multiple dies/modules, SMT", {2, 2, 2, 2}, 2,
          UINT64_C(0x00080010) },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        for (unsigned package = 0; package < cases[i].packages; package++) {
            X86CPU cpu = {0};
            uint64_t value = UINT64_MAX, reference;
            bool accepted;

            cpu.env.topo_info = cases[i].topology;
            cpu.socket_id = package;
            reference = cpu_x86_get_msr_core_thread_count(&cpu);
            accepted = whpx_access_msr(&cpu.env, MSR_CORE_THREAD_COUNT,
                                       false, &value);
            if (!accepted || value != cases[i].expected || value != reference) {
                fprintf(stderr, "FAIL %s package %u: accepted=%d value=%" PRIx64
                        " helper=%" PRIx64 " expected=%" PRIx64 "\n",
                        cases[i].name, package, accepted, value, reference,
                        cases[i].expected);
                return EXIT_FAILURE;
            }
            if (!rejects_without_changes(&cpu, MSR_CORE_THREAD_COUNT, true) ||
                !rejects_without_changes(&cpu, UINT32_MAX, false) ||
                !rejects_without_changes(&cpu, UINT32_MAX, true)) {
                fprintf(stderr, "FAIL %s package %u: refusal changed state\n",
                        cases[i].name, package);
                return EXIT_FAILURE;
            }
            printf("PASS %s package %u: MSR 0x35=%" PRIx64
                   "; read-only and unknown-MSR refusals preserve state\n",
                   cases[i].name, package, value);
        }
    }
    return EXIT_SUCCESS;
}
