/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2020-2022 Rockchip Electronics Co., Ltd.
 */

#include "hal_base.h"

#ifdef HAL_AP_CORE
uint32_t SystemCoreClock = 816000000;

/*----------------------------------------------------------------------------
  System Core Clock update function
 *----------------------------------------------------------------------------*/
void SystemCoreClockUpdate(void)
{
}

void SystemInit(void)
{
    /* do not use global variables because this function is called before
     * reaching pre-main. RW section may be overwritten afterwards.
     */
    // Invalidate entire Unified TLB
  __set_TLBIALL(0);

  // Invalidate entire branch predictor array
  __set_BPIALL(0);
  __DSB();
  __ISB();

  //  Invalidate instruction cache and flush branch target cache
  __set_ICIALLU(0);
  __DSB();
  __ISB();

  // Invalidate data cache
#ifdef BETAFLIGHT_BAREMETAL
  /* L1C_InvalidateDCacheAll() walks every data/unified cache level reported
   * by CLIDR.  On RK3506 that includes the cluster-wide unified L2, so an AMP
   * secondary calling it can discard dirty U-Boot/Linux lines.  Maintain only
   * CPU2's private L1 data cache here. */
  __L1C_MaintainDCacheSetWay(0U, 0U);
#else
  L1C_InvalidateDCacheAll();
#endif

#if ((__FPU_PRESENT == 1) && (__FPU_USED == 1))
  // Enable FPU
  __FPU_Enable();
#endif

  // Create Translation Table
  MMU_CreateTranslationTable();

  // Enable MMU
  MMU_Enable();

  // Enable Caches
  L1C_EnableCaches();
  L1C_EnableBTAC();

#if (__L2C_PRESENT == 1) && !defined(BETAFLIGHT_BAREMETAL)
  // Enable GIC
  L2C_Enable();
#endif

  // IRQ Initialize
}

void DataInit(void)
{
    typedef struct {
        uint32_t* dest;
        uint32_t  wlen;
    } __zero_table_t;

    extern const __zero_table_t __zero_table_start__;
    extern const __zero_table_t __zero_table_end__;

    for (__zero_table_t const *pTable = &__zero_table_start__; pTable < &__zero_table_end__; ++pTable) {
        for (unsigned long i = 0u; i < pTable->wlen; ++i) {
            pTable->dest[i] = 0u;
        }
    }


}

#endif  /* End of HAL_AP_CORE */
