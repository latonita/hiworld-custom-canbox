/* ============================================================================
 * hal_cpu.h -- the three Cortex-M core instructions this firmware issues
 * directly, as the HAL's business rather than everyone's.
 *
 * WHY THIS EXISTS
 * `app/include/n32l406_hw.h` already carries static-inline irq_disable() /
 * irq_enable() / wait_for_irq(). That header is GENERATED from the vendor SDK
 * (tools/generate/gen_hw.py), so it is not the place to add a host-test escape
 * hatch -- and without one, any service that needs a critical section cannot be
 * unit-tested, because `cpsid i` does not assemble for x86.
 *
 * Before this file existed, app/services/param_store.cpp carried its own copy
 * of the two instructions with its own host guard. One copy, in the layer that
 * owns the hardware, is the right number.
 *
 * THESE ARE NOT NVIC CALLS. They are PRIMASK, i.e. all-or-nothing masking of
 * configurable-priority interrupts; they do not touch the NVIC's enable bits,
 * which is why they live here and not in hal_nvic.h.
 *
 * WHERE THEY ARE USED, and why each one is load-bearing:
 *   - the boot sequence runs with interrupts masked from entry to the first
 *     watchdog kick, so no half-initialised peripheral can fire a handler;
 *   - the parameter-page save brackets its erase+program, because a flash
 *     controller operation that is interrupted mid-sequence can leave the page
 *     in a state the loader will not accept;
 *   - the CAN re-init during autobaud brackets the controller reset;
 *   - wait_for_irq is the deep-sleep entry, after which only the PB8 EXTI line
 *     can wake the part.
 * ==========================================================================*/

#ifndef HAL_CPU_H
#define HAL_CPU_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CANBOX_HOST_TEST)

/* Host build: there is no PRIMASK and nothing is concurrent, so these are
 * no-ops. The sequencing they protect on target is still exercised by the
 * tests -- what is not testable here is the masking itself, which is noted in
 * docs/misra-cpp.md. */
static inline void cpu_irq_disable(void) { }
static inline void cpu_irq_enable(void)  { }
static inline void cpu_wait_for_irq(void) { }

#else

/* The "memory" clobber is required, not decorative: it stops the compiler
 * hoisting a load or sinking a store across the barrier, which would silently
 * move work out of the critical section. */
static inline void cpu_irq_disable(void)
{
    __asm volatile("cpsid i" ::: "memory");
}

static inline void cpu_irq_enable(void)
{
    __asm volatile("cpsie i" ::: "memory");
}

static inline void cpu_wait_for_irq(void)
{
    __asm volatile("wfi");
}

#endif /* CANBOX_HOST_TEST */

#ifdef __cplusplus
}
#endif

#endif /* HAL_CPU_H */
