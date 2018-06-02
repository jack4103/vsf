#include "vsf.h"
#include "core.h"

static struct vsfhal_info_t vsfhal_info =
{
	0, CORE_VECTOR_TABLE,
	CORE_CLKEN,
	CORE_HCLKSRC, CORE_PLLSRC,
	HSI_FREQ_HZ, HSE_FREQ_HZ,
	CORE_PLL_FREQ_HZ, CORE_HCLK_FREQ_HZ, CORE_PCLK1_FREQ_HZ, CORE_PCLK2_FREQ_HZ,
};

vsf_err_t vsfhal_core_get_info(struct vsfhal_info_t **info)
{
	*info = &vsfhal_info;
	return VSFERR_NONE;
}

// Pendsv
struct vsfhal_pendsv_t
{
	void (*on_pendsv)(void *);
	void *param;
} static vsfhal_pendsv;

ROOTFUNC void PendSV_Handler(void)
{
	if (vsfhal_pendsv.on_pendsv != NULL)
	{
		vsfhal_pendsv.on_pendsv(vsfhal_pendsv.param);
	}
}

vsf_err_t vsfhal_core_pendsv_config(void (*on_pendsv)(void *), void *param)
{
	vsfhal_pendsv.on_pendsv = on_pendsv;
	vsfhal_pendsv.param = param;

	if (vsfhal_pendsv.on_pendsv != NULL)
	{
		SCB->SHP[10] = 0xFF;
	}
	return VSFERR_NONE;
}

vsf_err_t vsfhal_core_pendsv_trigger(void)
{
	SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
	return VSFERR_NONE;
}

WEAKFUNC void HardFault_Handler(void)
{
	while (1);
}

uint32_t vsfhal_core_get_stack(void)
{
	return __get_MSP();
}

vsf_err_t vsfhal_core_set_stack(uint32_t sp)
{
	__set_MSP(sp);
	return VSFERR_NONE;
}

vsf_err_t vsfhal_core_init(void *p)
{ 
	if (p != NULL)
	{
		stm32f7_info = *(struct stm32f7_info_t *)p;
	}

	// enable cache
	SCB_EnableICache();
	SCB_EnableDCache();
	
	// enable art
	FLASH->ACR |= FLASH_ACR_ARTEN | 6;
	
	// enable hsi and select hclksrc to hsi
	RCC->CR |= RCC_CR_HSION;
	while (!(RCC->CR & RCC_CR_HSIRDY));
	RCC->CFGR &= ~RCC_CFGR_SW;
	while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI);

    // enable clk
    if (stm32f7_info.clk_enable & STM32F7_CLK_HSE)
    {
		RCC->CR |= RCC_CR_HSEON;
		while (!(RCC->CR & RCC_CR_HSERDY));
    }
    else
	{
		RCC->CR &= ~RCC_CR_HSEON;
	}

	RCC->CR &= ~RCC_CR_PLLON;
    if (stm32f7_info.clk_enable & STM32F7_HCLKSRC_PLL)
	{
		uint32_t n, m, p, input, output, pllcfgr;
		pllcfgr = RCC->PLLCFGR & 0xf0000000;

		pllcfgr |= stm32f7_info.pllsrc == STM32F7_PLLSRC_HSI ? 0 :
				RCC_PLLCFGR_PLLSRC_HSE;
		input = stm32f7_info.pllsrc == STM32F7_PLLSRC_HSI ?
				stm32f7_info.hsi_freq_hz : stm32f7_info.hse_freq_hz;

		if (input % 2000000)
		{
			m = input / 1000000;
			input = 1000000;
		}
		else
		{
			m = input / 2000000;
			input = 2000000;
		}
		pllcfgr |= m;

		for (p = 2; p <= 8; p += 2)
		{
			if ((stm32f7_info.pll_freq_hz * p >= 192000000) &&
				(stm32f7_info.pll_freq_hz * p <= 432000000))
			{
				break;
			}
		}
		if (p > 8)
			return  VSFERR_FAIL;
		pllcfgr |= (p / 2 - 1) << 16;

		output = stm32f7_info.pll_freq_hz * p;
		n = output / input;
		pllcfgr |= n << 6;

		pllcfgr |= (output / 48000000) << 24;
		
		RCC->PLLCFGR = pllcfgr;
		RCC->CR |= RCC_CR_PLLON;
		while (!(RCC->CR & RCC_CR_PLLRDY));
	} 

	// set pclk and hclk
	RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
	// TODO
	RCC->CFGR |= (0x6ul << 10) | (0x6ul << 13) | (0 << 4);

	// select
	RCC->CFGR |= stm32f7_info.hclksrc;
	while (((RCC->CFGR & RCC_CFGR_SWS) >> 2) != stm32f7_info.hclksrc);

	SCB->VTOR = stm32f7_info.vector_table;
	SCB->AIRCR = 0x05FA0000 | stm32f7_info.priority_group;
	return VSFERR_NONE;
}

vsf_err_t vsfhal_core_fini(void *p)
{
	return VSFERR_NONE;
}

vsf_err_t vsfhal_core_reset(void *p)
{
	// TODO
	return VSFERR_NONE;
}

uint8_t vsfhal_core_set_intlevel(uint8_t level)
{
	uint8_t origlevel = __get_BASEPRI();
	__set_BASEPRI(level);
	return origlevel;
}

// sleep will enable interrupt
// for cortex processor, if an interrupt occur between enable the interrupt
// 		and __WFI, wfi will not make the core sleep
void vsfhal_core_sleep(uint32_t mode)
{
	vsf_leave_critical();
	// TODO
	__WFI();
}

static void (*tickclk_callback)(void *param) = NULL;
static void *tickclk_param = NULL;
static volatile uint32_t tickcnt = 0;

static uint32_t tickclk_get_ms_local(void)
{
	return tickcnt;
}

uint32_t vsfhal_tickclk_get_ms(void)
{
	uint32_t count1, count2;

	do {
		count1 = tickclk_get_ms_local();
		count2 = tickclk_get_ms_local();
	} while (count1 != count2);
	return count1;
}

uint32_t vsfhal_tickclk_get_us(void)
{
	uint32_t val, load;
	load = SysTick->LOAD;
	val = (load - SysTick->VAL) * 1000;
	return tickcnt * 1000 + val / load;
}

ROOTFUNC void SysTick_Handler(void)
{
	tickcnt++;
	if (tickclk_callback != NULL)
	{
		tickclk_callback(tickclk_param);
	}
}

vsf_err_t vsfhal_tickclk_config_cb(void (*callback)(void*), void *param)
{
	tickclk_callback = callback;
	tickclk_param = param;
	return VSFERR_NONE;
}

vsf_err_t vsfhal_tickclk_start(void)
{
	SysTick->VAL = 0;
	SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
	return VSFERR_NONE;
}

vsf_err_t vsfhal_tickclk_stop(void)
{
	SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
	return VSFERR_NONE;
}

vsf_err_t vsfhal_tickclk_init(int32_t int_priority)
{
	SysTick->LOAD = vsfhal_info.hclk_freq_hz / 1000;
	if (int_priority >= 0)
	{
		SysTick->CTRL = SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_CLKSOURCE_Msk;
		NVIC_SetPriority(SysTick_IRQn, (uint32_t)int_priority);
	}
	else
	{
		SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk;
	}
	return VSFERR_NONE;
}

vsf_err_t vsfhal_tickclk_fini(void)
{
	return vsfhal_tickclk_stop();
}

// special
uint32_t vsfhal_get_pc(void)
{
	uint32_t pc;
	asm("MOV	%0,	pc" : "=r" (pc));
	return pc;
}

void SystemInit(void)
{

}


















