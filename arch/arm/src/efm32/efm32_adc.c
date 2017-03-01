/****************************************************************************
 * arch/arm/src/efm32/efm32_adc.c
 *
 *   Copyright (C) 2014 Bouteville Pierre-Noel. All rights reserved.
 *   Copyright (C) 2016 Gregory Nutt. All rights reserved.
 *   Authors: Bouteville Pierre-Noel <pnb990@gmail.com>
 *            Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <errno.h>
#include <assert.h>
#include <debug.h>
#include <unistd.h>

#include <arch/board/board.h>
#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/analog/adc.h>

#include "up_internal.h"
#include "up_arch.h"

#include "chip.h"
#include "efm32.h"
#include "efm32_adc.h"

/* ADC "lower half" support must be enabled */

#ifdef CONFIG_EFM32_ADC

/* Some ADC peripheral must be enabled */

#if defined(CONFIG_EFM32_ADC1)

/* This implementation is for the EFM32GG Only */

#if defined(CONFIG_EFM32_EFM32GG)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
/* ADC interrupts ***********************************************************/

/* The maximum number of channels that can be sampled.  If dma support is
 * not enabled, then only a single channel can be sampled.  Otherwise,
 * data overruns would occur.
 */

#ifdef CONFIG_ADC_DMA
#  define ADC_MAX_SAMPLES 16
#  warning "not tested !"
#else
#  define ADC_MAX_SAMPLES 1
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure describes the state of one ADC block */

struct efm32_dev_s
{
  FAR const struct adc_callback_s *cb;
  uint8_t  irq;       /* Interrupt generated by this ADC block */
  uint8_t  nchannels; /* Number of channels */
  uint8_t  current;   /* Current ADC channel being converted */
  xcpt_t   isr;       /* Interrupt handler for this ADC block */
  uint32_t base;      /* Base address of registers unique to this ADC block */
  uint8_t  chanlist[ADC_MAX_SAMPLES];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* ADC Register access */

static uint32_t adc_getreg(struct efm32_dev_s *priv, int offset);
static void     adc_putreg(struct efm32_dev_s *priv, int offset, uint32_t value);
static void     adc_hw_reset(struct efm32_dev_s *priv, bool reset);

/* ADC Interrupt Handler */

static int adc_interrupt(int irq, FAR void *context, FAR struct adc_dev_s *dev);

/* ADC Driver Methods */

static int  adc_bind(FAR struct adc_dev_s *dev,
                     FAR const struct adc_callback_s *callback);
static void adc_reset(FAR struct adc_dev_s *dev);
static int  adc_setup(FAR struct adc_dev_s *dev);
static void adc_shutdown(FAR struct adc_dev_s *dev);
static void adc_rxint(FAR struct adc_dev_s *dev, bool enable);
static int  adc_ioctl(FAR struct adc_dev_s *dev, int cmd, unsigned long arg);
static void adc_enable(FAR struct efm32_dev_s *priv, bool enable);

#ifdef ADC_HAVE_TIMER
static void adc_timstart(FAR struct efm32_dev_s *priv, bool enable);
static int  adc_timinit(FAR struct efm32_dev_s *priv);
#endif

#if defined(CONFIG_EFM32_EFM32GG)
static void adc_startconv(FAR struct efm32_dev_s *priv, bool enable);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* ADC interface operations */

static const struct adc_ops_s g_adcops =
{
  .ao_bind     = adc_bind,
  .ao_reset    = adc_reset,
  .ao_setup    = adc_setup,
  .ao_shutdown = adc_shutdown,
  .ao_rxint    = adc_rxint,
  .ao_ioctl    = adc_ioctl,
};

/* ADC1 state */

#ifdef CONFIG_EFM32_ADC1
static struct efm32_dev_s g_adcpriv1 =
{
  .irq         = EFM32_IRQ_ADC0,
  .isr         = adc_interrupt,
  .base        = EFM32_ADC1_BASE,
};

static struct adc_dev_s g_adcdev1 =
{
  .ad_ops      = &g_adcops,
  .ad_priv     = &g_adcpriv1,
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: adc_getreg
 *
 * Description:
 *   Read the value of an ADC register.
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   offset - The offset to the register to read
 *
 * Returned Value:
 *
 ****************************************************************************/

static uint32_t adc_getreg(struct efm32_dev_s *priv, int offset)
{
  return getreg32(priv->base + offset);
}

/****************************************************************************
 * Name: adc_getreg
 *
 * Description:
 *   Read the value of an ADC register.
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   offset - The offset to the register to read
 *
 * Returned Value:
 *
 ****************************************************************************/

static void adc_putreg(struct efm32_dev_s *priv, int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: ADC_CalibrateLoadScan
 *
 * Description:
 *   Load SCAN calibrate register with predefined values for a certain
 *   reference.
 *
 *   During production, calibration values are made and stored in the device
 *   information page for known references. Notice that for external references,
 *   calibration values must be determined explicitly, and this function
 *   will not modify the calibration register.
 *
 * Input Parameters:
 *   adc - Pointer to ADC peripheral register block.
 *   ref - Reference to load calibrated values for. No values are loaded for
 *   external references.
 *
 ****************************************************************************/

static void ADC_CalibrateLoadScan(ADC_TypeDef *adc, ADC_Ref_TypeDef ref)
{
  uint32_t cal;

  /* Load proper calibration data depending on selected reference
   * NOTE: We use ...SCAN... defines below, they are the same as
   * similar ...SINGLE... defines.
   */

  switch (ref)
  {
  case adcRef1V25:
    cal  = adc->CAL & ~(_ADC_CAL_SCANOFFSET_MASK | _ADC_CAL_SCANGAIN_MASK);
    cal |= ((DEVINFO->ADC0CAL0 & _DEVINFO_ADC0CAL0_1V25_GAIN_MASK) >>
            _DEVINFO_ADC0CAL0_1V25_GAIN_SHIFT) << _ADC_CAL_SCANGAIN_SHIFT;
    cal |= ((DEVINFO->ADC0CAL0 & _DEVINFO_ADC0CAL0_1V25_OFFSET_MASK) >>
            _DEVINFO_ADC0CAL0_1V25_OFFSET_SHIFT) << _ADC_CAL_SCANOFFSET_SHIFT;
    adc->CAL = cal;
    break;

  case adcRef2V5:
    cal  = adc->CAL & ~(_ADC_CAL_SCANOFFSET_MASK | _ADC_CAL_SCANGAIN_MASK);
    cal |= ((DEVINFO->ADC0CAL0 & _DEVINFO_ADC0CAL0_2V5_GAIN_MASK) >>
            _DEVINFO_ADC0CAL0_2V5_GAIN_SHIFT) << _ADC_CAL_SCANGAIN_SHIFT;
    cal |= ((DEVINFO->ADC0CAL0 & _DEVINFO_ADC0CAL0_2V5_OFFSET_MASK) >>
            _DEVINFO_ADC0CAL0_2V5_OFFSET_SHIFT) << _ADC_CAL_SCANOFFSET_SHIFT;
    adc->CAL = cal;
    break;

  case adcRefVDD:
    cal  = adc->CAL & ~(_ADC_CAL_SCANOFFSET_MASK | _ADC_CAL_SCANGAIN_MASK);
    cal |= ((DEVINFO->ADC0CAL1 & _DEVINFO_ADC0CAL1_VDD_GAIN_MASK) >>
            _DEVINFO_ADC0CAL1_VDD_GAIN_SHIFT) << _ADC_CAL_SCANGAIN_SHIFT;
    cal |= ((DEVINFO->ADC0CAL1 & _DEVINFO_ADC0CAL1_VDD_OFFSET_MASK) >>
            _DEVINFO_ADC0CAL1_VDD_OFFSET_SHIFT) << _ADC_CAL_SCANOFFSET_SHIFT;
    adc->CAL = cal;
    break;

  case adcRef5VDIFF:
    cal  = adc->CAL & ~(_ADC_CAL_SCANOFFSET_MASK | _ADC_CAL_SCANGAIN_MASK);
    cal |= ((DEVINFO->ADC0CAL1 & _DEVINFO_ADC0CAL1_5VDIFF_GAIN_MASK) >>
            _DEVINFO_ADC0CAL1_5VDIFF_GAIN_SHIFT) << _ADC_CAL_SCANGAIN_SHIFT;
    cal |= ((DEVINFO->ADC0CAL1 & _DEVINFO_ADC0CAL1_5VDIFF_OFFSET_MASK) >>
            _DEVINFO_ADC0CAL1_5VDIFF_OFFSET_SHIFT) << _ADC_CAL_SCANOFFSET_SHIFT;
    adc->CAL = cal;
    break;

  case adcRef2xVDD:
    /* Gain value not of relevance for this reference, leave as is */

    cal  = adc->CAL & ~_ADC_CAL_SCANOFFSET_MASK;
    cal |= ((DEVINFO->ADC0CAL2 & _DEVINFO_ADC0CAL2_2XVDDVSS_OFFSET_MASK) >>
            _DEVINFO_ADC0CAL2_2XVDDVSS_OFFSET_SHIFT) << _ADC_CAL_SCANOFFSET_SHIFT;
    adc->CAL = cal;
    break;

  /* For external references, the calibration must be determined for the
   * specific application and set explicitly.
   */

  default:
    break;
  }
}

/****************************************************************************
 * Name: ADC_CalibrateLoadSingle
 *
 * Description:
 *   Load SINGLE calibrate register with predefined values for a certain
 *   reference.
 *
 *   During production, calibration values are made and stored in the device
 *   information page for known references. Notice that for external references,
 *   calibration values must be determined explicitly, and this function
 *   will not modify the calibration register.
 *
 * Input Parameters:
 *   adc - Pointer to ADC peripheral register block.
 *   ref - Reference to load calibrated values for. No values are loaded for
 *         external references.
 *
 ****************************************************************************/

static void ADC_CalibrateLoadSingle(ADC_TypeDef *adc, ADC_Ref_TypeDef ref)
{
  uint32_t cal;

  /* Load proper calibration data depending on selected reference
   * NOTE: We use ...SCAN... defines below, they are the same as
   * similar ...SINGLE... defines.
   */

  switch (ref)
  {
  case adcRef1V25:
    cal  = adc->CAL & ~(_ADC_CAL_SINGLEOFFSET_MASK | _ADC_CAL_SINGLEGAIN_MASK);
    cal |= ((DEVINFO->ADC0CAL0 & _DEVINFO_ADC0CAL0_1V25_GAIN_MASK) >>
            _DEVINFO_ADC0CAL0_1V25_GAIN_SHIFT) << _ADC_CAL_SINGLEGAIN_SHIFT;
    cal |= ((DEVINFO->ADC0CAL0 & _DEVINFO_ADC0CAL0_1V25_OFFSET_MASK) >>
            _DEVINFO_ADC0CAL0_1V25_OFFSET_SHIFT) << _ADC_CAL_SINGLEOFFSET_SHIFT;
    adc->CAL = cal;
    break;

  case adcRef2V5:
    cal  = adc->CAL & ~(_ADC_CAL_SINGLEOFFSET_MASK | _ADC_CAL_SINGLEGAIN_MASK);
    cal |= ((DEVINFO->ADC0CAL0 & _DEVINFO_ADC0CAL0_2V5_GAIN_MASK) >>
            _DEVINFO_ADC0CAL0_2V5_GAIN_SHIFT) << _ADC_CAL_SINGLEGAIN_SHIFT;
    cal |= ((DEVINFO->ADC0CAL0 & _DEVINFO_ADC0CAL0_2V5_OFFSET_MASK) >>
            _DEVINFO_ADC0CAL0_2V5_OFFSET_SHIFT) << _ADC_CAL_SINGLEOFFSET_SHIFT;
    adc->CAL = cal;
    break;

  case adcRefVDD:
    cal  = adc->CAL & ~(_ADC_CAL_SINGLEOFFSET_MASK | _ADC_CAL_SINGLEGAIN_MASK);
    cal |= ((DEVINFO->ADC0CAL1 & _DEVINFO_ADC0CAL1_VDD_GAIN_MASK) >>
            _DEVINFO_ADC0CAL1_VDD_GAIN_SHIFT) << _ADC_CAL_SINGLEGAIN_SHIFT;
    cal |= ((DEVINFO->ADC0CAL1 & _DEVINFO_ADC0CAL1_VDD_OFFSET_MASK) >>
            _DEVINFO_ADC0CAL1_VDD_OFFSET_SHIFT) << _ADC_CAL_SINGLEOFFSET_SHIFT;
    adc->CAL = cal;
    break;

  case adcRef5VDIFF:
    cal  = adc->CAL & ~(_ADC_CAL_SINGLEOFFSET_MASK | _ADC_CAL_SINGLEGAIN_MASK);
    cal |= ((DEVINFO->ADC0CAL1 & _DEVINFO_ADC0CAL1_5VDIFF_GAIN_MASK) >>
            _DEVINFO_ADC0CAL1_5VDIFF_GAIN_SHIFT) << _ADC_CAL_SINGLEGAIN_SHIFT;
    cal |= ((DEVINFO->ADC0CAL1 & _DEVINFO_ADC0CAL1_5VDIFF_OFFSET_MASK) >>
            _DEVINFO_ADC0CAL1_5VDIFF_OFFSET_SHIFT) << _ADC_CAL_SINGLEOFFSET_SHIFT;
    adc->CAL = cal;
    break;

  case adcRef2xVDD:
    /* Gain value not of relevance for this reference, leave as is */

    cal  = adc->CAL & ~_ADC_CAL_SINGLEOFFSET_MASK;
    cal |= ((DEVINFO->ADC0CAL2 & _DEVINFO_ADC0CAL2_2XVDDVSS_OFFSET_MASK) >>
            _DEVINFO_ADC0CAL2_2XVDDVSS_OFFSET_SHIFT) << _ADC_CAL_SINGLEOFFSET_SHIFT;
    adc->CAL = cal;
    break;

  /* For external references, the calibration must be determined for the
   * specific application and set explicitly.
   */

  default:
    break;
  }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ADC_Init
 *   Initialize ADC.
 *
 * Description:
 *   Initializes common parts for both single conversion and scan sequence.
 *   In addition, single and/or scan control configuration must be done, please
 *   refer to ADC_InitSingle() and ADC_InitScan() respectively.
 *
 *   NOTE: This function will stop any ongoing conversion.
 *
 * Input Parameters:
 *   adc - Pointer to ADC peripheral register block.
 *   int - Pointer to ADC initialization structure.
 *
 ****************************************************************************/

void ADC_Init(ADC_TypeDef *adc, const ADC_Init_TypeDef *init)
{
  uint32_t tmp;

  EFM_ASSERT(ADC_REF_VALID(adc));

  /* Make sure conversion is not in progress */

  adc->CMD = ADC_CMD_SINGLESTOP | ADC_CMD_SCANSTOP;

  tmp = ((uint32_t)(init->ovsRateSel) << _ADC_CTRL_OVSRSEL_SHIFT) |
        (((uint32_t)(init->timebase) << _ADC_CTRL_TIMEBASE_SHIFT) & _ADC_CTRL_TIMEBASE_MASK) |
        (((uint32_t)(init->prescale) << _ADC_CTRL_PRESC_SHIFT) & _ADC_CTRL_PRESC_MASK) |
        ((uint32_t)(init->lpfMode) << _ADC_CTRL_LPFMODE_SHIFT) |
        ((uint32_t)(init->warmUpMode) << _ADC_CTRL_WARMUPMODE_SHIFT);

  if (init->tailgate)
    {
      tmp |= ADC_CTRL_TAILGATE;
    }

  adc->CTRL = tmp;
}

/****************************************************************************
 * Name: ADC_InitScan
 *
 * Description:
 *   Initialize ADC scan sequence.
 *
 *   Please refer to ADC_Start() for starting scan sequence.
 *
 *   When selecting an external reference, the gain and offset calibration
 *   must be set explicitly (CAL register). For other references, the
 *   calibration is updated with values defined during manufacturing.
 *
 *   NOTE: This function will stop any ongoing scan sequence.
 *
 * Input Parameters:
 *   adc - Pointer to ADC peripheral register block.
 *   init - Pointer to ADC initialization structure.
 *
 ****************************************************************************/

void ADC_InitScan(ADC_TypeDef *adc, const ADC_InitScan_TypeDef *init)
{
  uint32_t tmp;

  EFM_ASSERT(ADC_REF_VALID(adc));

  /* Make sure scan sequence is not in progress */

  adc->CMD = ADC_CMD_SCANSTOP;

  /* Load proper calibration data depending on selected reference */

  ADC_CalibrateLoadScan(adc, init->reference);

  tmp = ((uint32_t)(init->prsSel) << _ADC_SCANCTRL_PRSSEL_SHIFT) |
        ((uint32_t)(init->acqTime) << _ADC_SCANCTRL_AT_SHIFT) |
        ((uint32_t)(init->reference) << _ADC_SCANCTRL_REF_SHIFT) |
        init->input |
        ((uint32_t)(init->resolution) << _ADC_SCANCTRL_RES_SHIFT);

  if (init->prsEnable)
    {
      tmp |= ADC_SCANCTRL_PRSEN;
    }

  if (init->leftAdjust)
    {
      tmp |= ADC_SCANCTRL_ADJ_LEFT;
    }

  if (init->diff)
    {
      tmp |= ADC_SCANCTRL_DIFF;
    }

  if (init->rep)
    {
      tmp |= ADC_SCANCTRL_REP;
    }

  adc->SCANCTRL = tmp;
}

/****************************************************************************
 * Name: ADC_InitSingle
 *
 * Description:
 *   Initialize single ADC sample conversion.
 *
 *   Please refer to ADC_Start() for starting single conversion.
 *
 *   When selecting an external reference, the gain and offset calibration
 *   must be set explicitly (CAL register). For other references, the
 *   calibration is updated with values defined during manufacturing.
 *
 *   NOTE: This function will stop any ongoing single conversion.
 *
 * Input Parameters:
 *   adc - Pointer to ADC peripheral register block.
 *   init - Pointer to ADC initialization structure.
 *
 ****************************************************************************/

void ADC_InitSingle(ADC_TypeDef *adc, const ADC_InitSingle_TypeDef *init)
{
  uint32_t tmp;

  EFM_ASSERT(ADC_REF_VALID(adc));

  /* Make sure single conversion is not in progress */

  adc->CMD = ADC_CMD_SINGLESTOP;

  /* Load proper calibration data depending on selected reference */

  ADC_CalibrateLoadSingle(adc, init->reference);

  tmp = ((uint32_t)(init->prsSel) << _ADC_SINGLECTRL_PRSSEL_SHIFT) |
        ((uint32_t)(init->acqTime) << _ADC_SINGLECTRL_AT_SHIFT) |
        ((uint32_t)(init->reference) << _ADC_SINGLECTRL_REF_SHIFT) |
        ((uint32_t)(init->input) << _ADC_SINGLECTRL_INPUTSEL_SHIFT) |
        ((uint32_t)(init->resolution) << _ADC_SINGLECTRL_RES_SHIFT);

  if (init->prsEnable)
    {
      tmp |= ADC_SINGLECTRL_PRSEN;
    }

  if (init->leftAdjust)
    {
      tmp |= ADC_SINGLECTRL_ADJ_LEFT;
    }

  if (init->diff)
    {
      tmp |= ADC_SINGLECTRL_DIFF;
    }

  if (init->rep)
    {
      tmp |= ADC_SINGLECTRL_REP;
    }

  adc->SINGLECTRL = tmp;
}

/****************************************************************************
 * Name: ADC_PrescaleCalc
 *
 * Description:
 *   Calculate prescaler value used to determine ADC clock.
 *
 *   The ADC clock is given by: HFPERCLK / (prescale + 1).
 *
 * Input Parameters:
 *   adcFreq  ADC frequency wanted. The frequency will automatically
 *            be adjusted to be within valid range according to reference manual.
 *  hfperFreq Frequency in Hz of reference HFPER clock. Set to 0 to
 *            use currently defined HFPER clock setting.
 *
 * Returned Value:
 *   Prescaler value to use for ADC in order to achieve a clock value
 *   <= @p adcFreq.
 *
 ****************************************************************************/

uint8_t ADC_PrescaleCalc(uint32_t adcFreq, uint32_t hfperFreq)
{
  uint32_t ret;

  /* Make sure selected ADC clock is within valid range */

  if (adcFreq > ADC_MAX_CLOCK)
    {
      adcFreq = ADC_MAX_CLOCK;
    }
  else if (adcFreq < ADC_MIN_CLOCK)
    {
      adcFreq = ADC_MIN_CLOCK;
    }

  /* Use current HFPER frequency? */

  if (!hfperFreq)
    {
      hfperFreq = CMU_ClockFreqGet(cmuClock_HFPER);
    }

  ret = (hfperFreq + adcFreq - 1) / adcFreq;
  if (ret)
    {
      ret--;
    }

  return (uint8_t)ret;
}

/****************************************************************************
 * Name: ADC_Reset
 *
 * Description:
 *   Reset ADC to same state as after a HW reset.
 *
 * @note
 *   The ROUTE register is NOT reset by this function, in order to allow for
 *   centralized setup of this feature.
 *
 * Input Parameters:
 *   adc - Pointer to ADC peripheral register block.
 *
 ****************************************************************************/

void ADC_Reset(ADC_TypeDef *adc)
{
  /* Stop conversions, before resetting other registers. */

  adc->CMD        = ADC_CMD_SINGLESTOP | ADC_CMD_SCANSTOP;
  adc->SINGLECTRL = _ADC_SINGLECTRL_RESETVALUE;
  adc->SCANCTRL   = _ADC_SCANCTRL_RESETVALUE;
  adc->CTRL       = _ADC_CTRL_RESETVALUE;
  adc->IEN        = _ADC_IEN_RESETVALUE;
  adc->IFC        = _ADC_IFC_MASK;
  adc->BIASPROG   = _ADC_BIASPROG_RESETVALUE;

  /* Load calibration values for the 1V25 internal reference. */

  ADC_CalibrateLoadSingle(adc, adcRef1V25);
  ADC_CalibrateLoadScan(adc, adcRef1V25);

  /* Do not reset route register, setting should be done independently */
}

/****************************************************************************
 * Name: ADC_TimebaseCalc
 *
 * Description:
 *   Calculate timebase value in order to get a timebase providing at least 1us.
 *
 * Input Parameters:
 *   hfperFreq Frequency in Hz of reference HFPER clock. Set to 0 to
 *              use currently defined HFPER clock setting.
 *
 * Returned Value:
 *   Timebase value to use for ADC in order to achieve at least 1 us.
 *
 ****************************************************************************/

uint8_t ADC_TimebaseCalc(uint32_t hfperFreq)
{
  if (!hfperFreq)
    {
      hfperFreq = CMU_ClockFreqGet(cmuClock_HFPER);

      /* Just in case, make sure we get non-zero freq for below calculation */

      if (!hfperFreq)
        {
          hfperFreq = 1;
        }
    }

#if defined(_EFM32_GIANT_FAMILY) || defined(_EFM32_WONDER_FAMILY)
  /* Handle errata on Giant Gecko, max TIMEBASE is 5 bits wide or max 0x1F
   * cycles. This will give a warmp up time of e.g. 0.645us, not the
   * required 1us when operating at 48MHz. One must also increase acqTime
   * to compensate for the missing clock cycles, adding up to 1us in total.
   * See reference manual for details.
   */

  if (hfperFreq > 32000000)
    {
      hfperFreq = 32000000;
    }
#endif

  /* Determine number of HFPERCLK cycle >= 1us */

  hfperFreq += 999999;
  hfperFreq /= 1000000;

  /* Return timebase value (N+1 format) */

  return (uint8_t)(hfperFreq - 1);
}
endif /* defined(ADC_COUNT) && (ADC_COUNT > 0) */

/****************************************************************************
 * Name: adc_tim_dumpregs
 *
 * Description:
 *   Dump all timer registers.
 *
 * Input parameters:
 *   priv - A reference to the ADC block status
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef ADC_HAVE_TIMER
static void adc_tim_dumpregs(struct efm32_dev_s *priv, FAR const char *msg)
{
#if defined(CONFIG_DEBUG_ANALOG) && defined(CONFIG_DEBUG_INFO)
  ainfo("%s:\n", msg);
  ainfo("  CR1: %04x CR2:  %04x SMCR:  %04x DIER:  %04x\n",
        tim_getreg(priv, EFM32_GTIM_CR1_OFFSET),
        tim_getreg(priv, EFM32_GTIM_CR2_OFFSET),
        tim_getreg(priv, EFM32_GTIM_SMCR_OFFSET),
        tim_getreg(priv, EFM32_GTIM_DIER_OFFSET));
  ainfo("   SR: %04x EGR:  0000 CCMR1: %04x CCMR2: %04x\n",
        tim_getreg(priv, EFM32_GTIM_SR_OFFSET),
        tim_getreg(priv, EFM32_GTIM_CCMR1_OFFSET),
        tim_getreg(priv, EFM32_GTIM_CCMR2_OFFSET));
  ainfo(" CCER: %04x CNT:  %04x PSC:   %04x ARR:   %04x\n",
        tim_getreg(priv, EFM32_GTIM_CCER_OFFSET),
        tim_getreg(priv, EFM32_GTIM_CNT_OFFSET),
        tim_getreg(priv, EFM32_GTIM_PSC_OFFSET),
        tim_getreg(priv, EFM32_GTIM_ARR_OFFSET));
  ainfo(" CCR1: %04x CCR2: %04x CCR3:  %04x CCR4:  %04x\n",
        tim_getreg(priv, EFM32_GTIM_CCR1_OFFSET),
        tim_getreg(priv, EFM32_GTIM_CCR2_OFFSET),
        tim_getreg(priv, EFM32_GTIM_CCR3_OFFSET),
        tim_getreg(priv, EFM32_GTIM_CCR4_OFFSET));

  if (priv->tbase == EFM32_TIM1_BASE || priv->tbase == EFM32_TIM8_BASE)
    {
      ainfo("  RCR: %04x BDTR: %04x DCR:   %04x DMAR:  %04x\n",
            tim_getreg(priv, EFM32_ATIM_RCR_OFFSET),
            tim_getreg(priv, EFM32_ATIM_BDTR_OFFSET),
            tim_getreg(priv, EFM32_ATIM_DCR_OFFSET),
            tim_getreg(priv, EFM32_ATIM_DMAR_OFFSET));
    }
  else
    {
      ainfo("  DCR: %04x DMAR: %04x\n",
            tim_getreg(priv, EFM32_GTIM_DCR_OFFSET),
            tim_getreg(priv, EFM32_GTIM_DMAR_OFFSET));
    }
#endif
}
#endif

/****************************************************************************
 * Name: adc_startconv
 *
 * Description:
 *   Start (or stop) the ADC conversion process in DMA mode
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   enable - True: Start conversion
 *
 * Returned Value:
 *
 ****************************************************************************/

#if defined(CONFIG_EFM32_EFM32GG)
static void adc_startconv(struct efm32_dev_s *priv, bool enable)
{
  uint32_t regval;

  ainfo("enable: %d\n", enable);

  regval = adc_getreg(priv, EFM32_ADC_CR2_OFFSET);
  if (enable)
    {
      /* Start conversion of regular channles */

      regval |= ADC_CR2_SWSTART;
    }
  else
    {
      /* Disable the conversion of regular channels */

      regval &= ~ADC_CR2_SWSTART;
    }

  adc_putreg(priv, EFM32_ADC_CR2_OFFSET, regval);
}
#endif

/****************************************************************************
 * Name: adc_hw_reset
 *
 * Description:
 *   Deinitializes the ADCx peripheral registers to their default
 *   reset values. It could set all the ADCs configured.
 *
 * Input Parameters:
 *   regaddr - The register to read
 *   reset - Condition, set or reset
 *
 * Returned Value:
 *
 ****************************************************************************/

static void adc_hw_reset(struct efm32_dev_s *priv, bool reset)
{
  irqstate_t flags;
  uint32_t regval;
  uint32_t adcbit;

  /* Pick the appropriate bit in the APB2 reset register */

  /* Disable interrupts.  This is necessary because the APB2RTSR register
   * is used by several different drivers.
   */

  flags = enter_critical_section();

  /* Set or clear the selected bit in the APB2 reset register */

  regval = getreg32(EFM32_RCC_APB2RSTR);
  if (reset)
    {
      /* Enable  ADC reset state */

      regval |= adcbit;
    }
  else
    {
      /* Release ADC from reset state */

      regval &= ~adcbit;
    }

  putreg32(regval, EFM32_RCC_APB2RSTR);
  leave_critical_section(flags);
}

/****************************************************************************
 * Name: adc_enable
 *
 * Description    : Enables or disables the specified ADC peripheral.
 *                  Also, starts a conversion when the ADC is not
 *                  triggered by timers
 *
 * Input Parameters:
 *
 *   enable - true:  enable ADC conversion
 *            false: disable ADC conversion
 *
 * Returned Value:
 *
 ****************************************************************************/

static void adc_enable(FAR struct efm32_dev_s *priv, bool enable)
{
  uint32_t regval;

  ainfo("enable: %d\n", enable);

  regval  = adc_getreg(priv, EFM32_ADC_CR2_OFFSET);
  if (enable)
    {
      regval |= ADC_CR2_ADON;
    }
  else
    {
      regval &= ~ADC_CR2_ADON;
    }

  adc_putreg(priv, EFM32_ADC_CR2_OFFSET, regval);
}

/****************************************************************************
 * Name: adc_bind
 *
 * Description:
 *   Bind the upper-half driver callbacks to the lower-half implementation.  This
 *   must be called early in order to receive ADC event notifications.
 *
 ****************************************************************************/

static int adc_bind(FAR struct adc_dev_s *dev,
                    FAR const struct adc_callback_s *callback)
{
  FAR struct efm32_dev_s *priv = (FAR struct efm32_dev_s *)dev->ad_priv;

  DEBUGASSERT(priv != NULL);
  priv->cb = callback;
  return OK;
}

/****************************************************************************
 * Name: adc_reset
 *
 * Description:
 *   Reset the ADC device.  Called early to initialize the hardware. This
 *   is called, before adc_setup() and on error conditions.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

static void adc_reset(FAR struct adc_dev_s *dev)
{
  FAR struct efm32_dev_s *priv = (FAR struct efm32_dev_s *)dev->ad_priv;
  irqstate_t flags;
  uint32_t regval;
  int offset;
  int i;
#ifdef ADC_HAVE_TIMER
  int ret;
#endif

  ainfo("intf: ADC%d\n", priv->intf);
  flags = enter_critical_section();

  /* Enable ADC reset state */

  adc_hw_reset(priv, true);

  /* Release ADC from reset state */

  adc_hw_reset(priv, false);

  /* Initialize the ADC data structures */

  /* Initialize the watchdog high threshold register */

  adc_putreg(priv, EFM32_ADC_HTR_OFFSET, 0x00000fff);

  /* Initialize the watchdog low threshold register */

  adc_putreg(priv, EFM32_ADC_LTR_OFFSET, 0x00000000);

  /* Initialize the same sample time for each ADC 55.5 cycles
   *
   * During sample cycles channel selection bits must remain unchanged.
   *
   *   000:   1.5 cycles
   *   001:   7.5 cycles
   *   010:  13.5 cycles
   *   011:  28.5 cycles
   *   100:  41.5 cycles
   *   101:  55.5 cycles
   *   110:  71.5 cycles
   *   111: 239.5 cycles
   */

  adc_putreg(priv, EFM32_ADC_SMPR1_OFFSET, 0x00b6db6d);
  adc_putreg(priv, EFM32_ADC_SMPR2_OFFSET, 0x00b6db6d);

  /* ADC CR1 Configuration */

  regval  = adc_getreg(priv, EFM32_ADC_CR1_OFFSET);

  /* Initialize the Analog watchdog enable */

  regval |= ADC_CR1_AWDEN;
  regval |= (priv->chanlist[0] << ADC_CR1_AWDCH_SHIFT);

  /* Enable interrupt flags */

  regval |= ADC_CR1_ALLINTS;

  adc_putreg(priv, EFM32_ADC_CR1_OFFSET, regval);

  /* ADC CR2 Configuration */

  regval  = adc_getreg(priv, EFM32_ADC_CR2_OFFSET);

  /* Clear CONT, continuous mode disable */

  regval &= ~ADC_CR2_CONT;

  /* Set ALIGN (Right = 0) */

  regval &= ~ADC_CR2_ALIGN;

  adc_putreg(priv, EFM32_ADC_CR2_OFFSET, regval);

  /* Configuration of the channel conversions */

  regval = adc_getreg(priv, EFM32_ADC_SQR3_OFFSET) & ADC_SQR3_RESERVED;
  for (i = 0, offset = 0; i < priv->nchannels && i < 6; i++, offset += 5)
    {
      regval |= (uint32_t)priv->chanlist[i] << offset;
    }

  adc_putreg(priv, EFM32_ADC_SQR3_OFFSET, regval);

  regval = adc_getreg(priv, EFM32_ADC_SQR2_OFFSET) & ADC_SQR2_RESERVED;
  for (i = 6, offset = 0; i < priv->nchannels && i < 12; i++, offset += 5)
    {
      regval |= (uint32_t)priv->chanlist[i] << offset;
    }

  adc_putreg(priv, EFM32_ADC_SQR2_OFFSET, regval);

  regval = adc_getreg(priv, EFM32_ADC_SQR1_OFFSET) & ADC_SQR1_RESERVED;
  for (i = 12, offset = 0; i < priv->nchannels && i < 16; i++, offset += 5)
    {
      regval |= (uint32_t)priv->chanlist[i] << offset;
    }

  /* ADC CCR configuration */

  regval  = getreg32(EFM32_ADC_CCR);
  regval &= ~(ADC_CCR_MULTI_MASK | ADC_CCR_DELAY_MASK | ADC_CCR_DDS | ADC_CCR_DMA_MASK |
              ADC_CCR_ADCPRE_MASK | ADC_CCR_VBATE | ADC_CCR_TSVREFE);
  regval |=  (ADC_CCR_MULTI_NONE | ADC_CCR_DMA_DISABLED | ADC_CCR_ADCPRE_DIV2);
  putreg32(regval, EFM32_ADC_CCR);

  /* Set the number of conversions */

  DEBUGASSERT(priv->nchannels <= ADC_MAX_SAMPLES);

  regval |= (((uint32_t)priv->nchannels-1) << ADC_SQR1_L_SHIFT);
  adc_putreg(priv, EFM32_ADC_SQR1_OFFSET, regval);

  /* Set the channel index of the first conversion */

  priv->current = 0;

  /* Set ADON to wake up the ADC from Power Down state. */

  adc_enable(priv, true);

  adc_startconv(priv, true);

  leave_critical_section(flags);

  ainfo("SR:   0x%08x CR1:  0x%08x CR2:  0x%08x\n",
        adc_getreg(priv, EFM32_ADC_SR_OFFSET),
        adc_getreg(priv, EFM32_ADC_CR1_OFFSET),
        adc_getreg(priv, EFM32_ADC_CR2_OFFSET));
  ainfo("SQR1: 0x%08x SQR2: 0x%08x SQR3: 0x%08x\n",
        adc_getreg(priv, EFM32_ADC_SQR1_OFFSET),
        adc_getreg(priv, EFM32_ADC_SQR2_OFFSET),
        adc_getreg(priv, EFM32_ADC_SQR3_OFFSET));
}

/****************************************************************************
 * Name: adc_setup
 *
 * Description:
 *   Configure the ADC. This method is called the first time that the ADC
 *   device is opened.  This will occur when the port is first opened.
 *   This setup includes configuring and attaching ADC interrupts.  Interrupts
 *   are all disabled upon return.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

static int adc_setup(FAR struct adc_dev_s *dev)
{
  FAR struct efm32_dev_s *priv = (FAR struct efm32_dev_s *)dev->ad_priv;
  int ret;

  /* Attach the ADC interrupt */

  ret = irq_attach(priv->irq, priv->isr, dev);
  if (ret == OK)
    {
      /* Make sure that the ADC device is in the powered up, reset state */

      adc_reset(dev);

      /* Enable the ADC interrupt */

      ainfo("Enable the ADC interrupt: irq=%d\n", priv->irq);
      up_enable_irq(priv->irq);
    }

  return ret;
}

/****************************************************************************
 * Name: adc_shutdown
 *
 * Description:
 *   Disable the ADC.  This method is called when the ADC device is closed.
 *   This method reverses the operation the setup method.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

static void adc_shutdown(FAR struct adc_dev_s *dev)
{
  FAR struct efm32_dev_s *priv = (FAR struct efm32_dev_s *)dev->ad_priv;

  /* Disable ADC interrupts and detach the ADC interrupt handler */

  up_disable_irq(priv->irq);
  irq_detach(priv->irq);

  /* Disable and reset the ADC module */

  adc_hw_reset(priv, true);
}

/****************************************************************************
 * Name: adc_rxint
 *
 * Description:
 *   Call to enable or disable RX interrupts.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

static void adc_rxint(FAR struct adc_dev_s *dev, bool enable)
{
  FAR struct efm32_dev_s *priv = (FAR struct efm32_dev_s *)dev->ad_priv;
  uint32_t regval;

  ainfo("intf: %d enable: %d\n", priv->intf, enable);

  regval = adc_getreg(priv, EFM32_ADC_CR1_OFFSET);
  if (enable)
    {
      /* Enable the end-of-conversion ADC and analog watchdog interrupts */

      regval |= ADC_CR1_ALLINTS;
    }
  else
    {
      /* Disable all ADC interrupts */

      regval &= ~ADC_CR1_ALLINTS;
    }

  adc_putreg(priv, EFM32_ADC_CR1_OFFSET, regval);
}

/****************************************************************************
 * Name: adc_ioctl
 *
 * Description:
 *   All ioctl calls will be routed through this method.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

static int adc_ioctl(FAR struct adc_dev_s *dev, int cmd, unsigned long arg)
{
  return -ENOTTY;
}

/****************************************************************************
 * Name: adc_interrupt
 *
 * Description:
 *   Common ADC interrupt handler.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

static int adc_interrupt(int irq, FAR void *context, FAR struct adc_dev_s *dev)
{
  FAR struct efm32_dev_s *priv = (FAR struct efm32_dev_s *)dev->ad_priv;
  uint32_t adcsr;
  int32_t  value;

  /* Identifies the interruption AWD, OVR or EOC */

  adcsr = adc_getreg(priv, EFM32_ADC_SR_OFFSET);
  if ((adcsr & ADC_SR_AWD) != 0)
    {
      awarn("WARNING: Analog Watchdog, Value converted out of range!\n");
    }

  /* EOC: End of conversion */

  if ((adcsr & ADC_SR_EOC) != 0)
    {
      /* Read the converted value and clear EOC bit
       * (It is cleared by reading the ADC_DR)
       */

      value  = adc_getreg(priv, EFM32_ADC_DR_OFFSET);
      value &= ADC_DR_DATA_MASK;

      /* Verify that the upper-half driver has bound its callback functions */

      if (priv->cb != NULL)
        {
          /* Give the ADC data to the ADC driver.  The ADC receive method
           * accepts 3 parameters:
           *
           * 1) The first is the ADC device instance for this ADC block.
           * 2) The second is the channel number for the data, and
           * 3) The third is the converted data for the channel.
           */

          DEBUGASSERT(priv->cb->au_receive != NULL);
          priv->cb->au_receive(dev, priv->chanlist[priv->current], value);
        }

      /* Set the channel number of the next channel that will complete conversion */

      priv->current++;

      if (priv->current >= priv->nchannels)
        {
          /* Restart the conversion sequence from the beginning */

          priv->current = 0;
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: efm32_adcinitialize
 *
 * Description:
 *   Initialize the ADC.
 *
 *   The logic is, save nchannels : # of channels (conversions) in ADC_SQR1_L
 *   Then, take the chanlist array and store it in the SQR Regs,
 *     chanlist[0] -> ADC_SQR3_SQ1
 *     chanlist[1] -> ADC_SQR3_SQ2
 *     ...
 *     chanlist[15]-> ADC_SQR1_SQ16
 *
 *   up to
 *     chanlist[nchannels]
 *
 * Input Parameters:
 *   intf      - Could be {1,2,3} for ADC1, ADC2, or ADC3
 *   chanlist  - The list of channels
 *   nchannels - Number of channels
 *
 * Returned Value:
 *   Valid ADC device structure reference on succcess; a NULL on failure
 *
 ****************************************************************************/

struct adc_dev_s *efm32_adcinitialize(int intf, const uint8_t *chanlist, int nchannels)
{
  FAR struct adc_dev_s   *dev;
  FAR struct efm32_dev_s *priv;

  ainfo("intf: %d nchannels: %d\n", intf, nchannels);

#ifdef CONFIG_EFM32_ADC1
  if (intf == 1)
    {
      ainfo("ADC1 Selected\n");
      dev = &g_adcdev1;
    }
  else
#endif
#ifdef CONFIG_EFM32_ADC2
  if (intf == 2)
    {
      ainfo("ADC2 Selected\n");
      dev = &g_adcdev2;
    }
  else
#endif
#ifdef CONFIG_EFM32_ADC3
  if (intf == 3)
    {
      ainfo("ADC3 Selected\n");
      dev = &g_adcdev3;
    }
  else
#endif
    {
      aerr("ERROR: No ADC interface defined\n");
      return NULL;
    }

  /* Configure the selected ADC */

  priv     = dev->ad_priv;
  priv->cb = NULL;

  DEBUGASSERT(nchannels <= ADC_MAX_SAMPLES);
  priv->nchannels = nchannels;

  memcpy(priv->chanlist, chanlist, nchannels);
  return dev;
}

#endif /* CONFIG_EFM32_EFM32GG */
#endif /* CONFIG_EFM32_ADC1 */
#endif /* CONFIG_EFM32_ADC */
