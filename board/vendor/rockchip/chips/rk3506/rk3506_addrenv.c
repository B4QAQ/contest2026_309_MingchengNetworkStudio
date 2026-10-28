/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_addrenv.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Identity virtual<->physical address translation for the flat RK3506
 * kernel build.
 *
 * The libmetal NuttX I/O shim
 * (openamp/libmetal/lib/system/nuttx/io.c) translates shared-memory
 * addresses with up_addrenv_va_to_pa()/up_addrenv_pa_to_va().  The armv7-a
 * port only defines those when CONFIG_MM_PGALLOC is enabled; the RK3506
 * NuttX image runs from a static identity L1 mapping (see the
 * g_rk3506_mappings table in rk3506_start.c), so virtual and physical
 * addresses are identical and the translation is a no-op.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_OPENAMP) && !defined(CONFIG_MM_PGALLOC)

#include <stdint.h>

#include <nuttx/arch.h>

uintptr_t up_addrenv_va_to_pa(FAR void *va)
{
  return (uintptr_t)va;
}

FAR void *up_addrenv_pa_to_va(uintptr_t pa)
{
  return (FAR void *)pa;
}

#endif /* CONFIG_OPENAMP && !CONFIG_MM_PGALLOC */
