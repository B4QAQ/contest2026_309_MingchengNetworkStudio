/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_addrenv.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Identity virtual<->physical address helpers for the OpenAMP/libmetal
 * NuttX shim (nuttx/openamp/libmetal/lib/system/nuttx/io.c).
 *
 * Problem:
 *   libmetal's NuttX io shim translates addresses through
 *   up_addrenv_va_to_pa()/up_addrenv_pa_to_va().  On ARMv7-A those two
 *   symbols are only compiled when CONFIG_MM_PGALLOC is enabled
 *   (nuttx/arch/arm/src/armv7-a/arm_physpgaddr.c and arm_virtpgaddr.c,
 *   gated by `ifeq ($(CONFIG_MM_PGALLOC),y)` in Make.defs/CMakeLists).
 *   The page allocator is designed for CONFIG_ARCH_ADDRENV (kernel/user
 *   address space) builds; the HD-RK3506-EVM NSH build uses the classic
 *   flat-map model (CONFIG_MM_PGALLOC not set), so enabling OPENAMP
 *   previously failed to link with undefined up_addrenv_va_to_pa /
 *   up_addrenv_pa_to_va.
 *
 * Solution:
 *   RK3506 NuttX runs with an identity MMU map (rk3506_start.c fills the
 *   L1 with DRAM 0x00000000-0x07ffffff and peripherals 0xff000000-0xffffffff,
 *   both va==pa).  Virtual and physical addresses are therefore the same
 *   value for every address NuttX can generate, so the correct translation
 *   is the identity function.
 *
 *   These definitions live in the vendor chip tree (not in arch/) so that
 *   no generic ARMv7-A code is touched.  They are only compiled when
 *   CONFIG_OPENAMP is enabled AND CONFIG_MM_PGALLOC is not (the same
 *   condition under which the symbols would otherwise be missing); if a
 *   future configuration enables the page allocator, the arch-provided
 *   real implementations take over and this file drops out.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <nuttx/arch.h>

#if defined(CONFIG_OPENAMP) && !defined(CONFIG_MM_PGALLOC)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_addrenv_va_to_pa
 *
 * Description:
 *   Return the physical address for a virtual address.  The RK3506 NuttX
 *   flat map is 1:1 (DRAM and peripheral sections are identity-mapped in
 *   rk3506_start.c), so the physical address equals the virtual address.
 *
 * Input Parameters:
 *   va - The virtual address to translate.
 *
 * Returned Value:
 *   The corresponding physical address (identity).
 *
 ****************************************************************************/

uintptr_t up_addrenv_va_to_pa(void *va)
{
  return (uintptr_t)va;
}

/****************************************************************************
 * Name: up_addrenv_pa_to_va
 *
 * Description:
 *   Return the virtual address for a physical address.  Identity, see
 *   up_addrenv_va_to_pa() above.
 *
 * Input Parameters:
 *   pa - The physical address to translate.
 *
 * Returned Value:
 *   The corresponding virtual address (identity).
 *
 ****************************************************************************/

FAR void *up_addrenv_pa_to_va(uintptr_t pa)
{
  return (FAR void *)pa;
}

#endif /* CONFIG_OPENAMP && !CONFIG_MM_PGALLOC */
