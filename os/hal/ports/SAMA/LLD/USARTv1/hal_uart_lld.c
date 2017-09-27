/*
    ChibiOS - Copyright (C) 2006..2016 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/**
 * @file    USARTv1/hal_uart_lld.c
 * @brief   SAMA low level UART driver code.
 *
 * @addtogroup UART
 * @{
 */

#include "hal.h"

#if HAL_USE_UART || defined(__DOXYGEN__)

/*===========================================================================*/
/* Driver local definitions.                                                 */
/*===========================================================================*/

/*===========================================================================*/
/* Driver exported variables.                                                */
/*===========================================================================*/

/** @brief UART0 UART driver identifier.*/
#if SAMA_UART_USE_UART0 || defined(__DOXYGEN__)
UARTDriver UARTD0;
#endif

/** @brief USART1 UART driver identifier.*/
#if SAMA_UART_USE_UART1 || defined(__DOXYGEN__)
UARTDriver UARTD1;
#endif

/** @brief USART2 UART driver identifier.*/
#if SAMA_UART_USE_UART2 || defined(__DOXYGEN__)
UARTDriver UARTD2;
#endif

/** @brief UART3 UART driver identifier.*/
#if SAMA_UART_USE_UART3 || defined(__DOXYGEN__)
UARTDriver UARTD3;
#endif

/** @brief UART4 UART driver identifier.*/
#if SAMA_UART_USE_UART4 || defined(__DOXYGEN__)
UARTDriver UARTD4;
#endif

/*===========================================================================*/
/* Driver local variables and types.                                         */
/*===========================================================================*/
/**
 * @brief Linked List view0 word aligned
 */
  ALIGNED_VAR(4) static lld_view0 descriptor0;

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

/**
 * @brief   Status bits translation.
 *
 * @param[in] isr       UART ISR register value
 *
 * @return  The error flags.
 */
static uartflags_t translate_errors(uint32_t isr) {
  uartflags_t sts = 0;

  if (isr & UART_SR_OVRE)
    sts |= UART_OVERRUN_ERROR;
  if (isr & UART_SR_PARE)
    sts |= UART_PARITY_ERROR;
  if (isr & UART_SR_FRAME)
    sts |= UART_SR_FRAME;
  return sts;
}

/**
 * @brief   Puts the receiver in the UART_RX_IDLE state.
 *
 * @param[in] uartp     pointer to the @p UARTDriver object
 */
static void uart_enter_rx_idle_loop(UARTDriver *uartp) {
  
  /* Disabling BIE interrupt if rx callback is null */
  if (uartp->config->rxchar_cb == NULL)
    uartp->dmarx->xdmac->XDMAC_CHID[uartp->dmarx->chid].XDMAC_CID =  XDMAC_CID_BID;

  descriptor0.mbr_ubc = XDMA_UBC_NVIEW_NDV0 | XDMA_UBC_NDEN_UPDATED |
                        XDMA_UBC_NDE_FETCH_EN | XDMA_UBC_UBLEN(1);
  descriptor0.mbr_nda = &descriptor0;
  descriptor0.mbr_ta = (uint32_t*)&uartp->rxbuf;

  /* Configure First Descriptor Address CNCDAx */
  uartp->dmarx->xdmac->XDMAC_CHID[uartp->dmarx->chid].XDMAC_CNDA =
                                   (((uint32_t)&descriptor0) & 0xFFFFFFFC);
  /* Configure the XDMAC_CNDCx register */
  uartp->dmarx->xdmac->XDMAC_CHID[uartp->dmarx->chid].XDMAC_CNDC =
              XDMAC_CNDC_NDE_DSCR_FETCH_EN | XDMAC_CNDC_NDDUP_DST_PARAMS_UPDATED | XDMAC_CNDC_NDVIEW_NDV0;

  dmaChannelEnable(uartp->dmarx);
}

/**
 * @brief   UART de-initialization.
 * @details This function must be invoked with interrupts disabled.
 *
 * @param[in] uartp     pointer to the @p UARTDriver object
 */
static void uart_stop(UARTDriver *uartp) {

  /* Stops RX and TX DMA channels.*/
  dmaChannelDisable(uartp->dmarx);
  dmaChannelDisable(uartp->dmatx);
  
  /* Stops UART operations.*/
  uartp->uart->UART_CR = UART_CR_RSTRX | UART_CR_RSTTX;

  /* Resets UART's register */
  uartp->uart->UART_MR = 0;
}

/**
 * @brief   UART initialization.
 * @details This function must be invoked with interrupts disabled.
 *
 * @param[in] uartp     pointer to the @p UARTDriver object
 */
static void uart_start(UARTDriver *uartp) {
  uint32_t cr;
  const uint32_t tmo = uartp->config->timeout;
  Uart *u = uartp->uart;

  /* Defensive programming, starting from a clean state.*/
  uart_stop(uartp);

  /* Baud rate setting.*/
  u->UART_BRGR = UART_BRGR_CD(uartp->clock / (16 * uartp->config->speed));

  /* Clearing pending flags */
  u->UART_CR = UART_CR_RSTSTA;

  /* Enabling interrupts */
  u->UART_IER = UART_IER_OVRE | UART_IER_FRAME | UART_IER_PARE;

  cr = UART_CR_RXEN | UART_CR_TXEN;
  u->UART_CR = uartp->config->cr | cr;
  u->UART_MR = uartp->config->mr;

  /* Set receive timeout and checks if it is really applied.*/
  if (tmo > 0) {
    /*
     * TODO: insert Function parameters check
     */
    u->UART_RTOR = tmo;
  }

  /* Starting the receiver idle loop.*/
  uart_enter_rx_idle_loop(uartp);
}

/**
 * @brief   RX DMA common service routine.
 *
 * @param[in] uartp     pointer to the @p UARTDriver object
 * @param[in] flags     pre-shifted content of the ISR register
 */
static void uart_lld_serve_rx_end_irq(UARTDriver *uartp, uint32_t flags) {

  /* DMA errors handling.*/
#if defined(SAMA_UART_DMA_ERROR_HOOK)
  if ((flags & (XDMAC_CIS_RBEIS | XDMAC_CIS_ROIS)) != 0) {
    SAMA_UART_DMA_ERROR_HOOK(uartp);
  }
#else
  (void)flags;
#endif

  if (uartp->rxstate == UART_RX_IDLE) {
    /* Receiver in idle state, a callback is generated, if enabled, for each
       received character and then the driver stays in the same state.*/
    _uart_rx_idle_code(uartp);
  }
  else {
    /* Receiver in active state, a callback is generated, if enabled, after
       a completed transfer.*/
    dmaChannelDisable(uartp->dmarx);
    _uart_rx_complete_isr_code(uartp);
  }
}

/**
 * @brief   TX DMA common service routine.
 *
 * @param[in] uartp     pointer to the @p UARTDriver object
 * @param[in] flags     pre-shifted content of the ISR register
 */
static void uart_lld_serve_tx_end_irq(UARTDriver *uartp, uint32_t flags) {

  /* DMA errors handling.*/
#if defined(STM32_UART_DMA_ERROR_HOOK)
  if ((flags & (XDMAC_CIS_WBEIS | XDMAC_CIS_ROIS)) != 0) {
    SAMA_UART_DMA_ERROR_HOOK(uartp);
  }
#else
  (void)flags;
#endif

  dmaChannelDisable(uartp->dmatx);

  /* A callback is generated, if enabled, after a completed transfer.*/
  _uart_tx1_isr_code(uartp);
}

/**
 * @brief   UART common service routine.
 *
 * @param[in] uartp     pointer to the @p UARTDriver object
 */
static void serve_uart_irq(UARTDriver *uartp) {
  Uart *u = uartp->uart;
  uint32_t imr = u->UART_IMR;
  uint32_t sr;

  /* Reading and clearing status.*/
  sr = u->UART_SR;
  u->UART_CR |= UART_CR_RSTSTA;

  if (sr & (UART_SR_OVRE | UART_SR_FRAME  | UART_SR_PARE)) {
    _uart_rx_error_isr_code(uartp, translate_errors(sr));
  }

  if ((imr & UART_IMR_TXEMPTY) && (sr & (UART_SR_TXRDY | UART_SR_TXEMPTY))) {
    /* TC interrupt disabled.*/
    u->UART_IDR |= UART_IDR_TXEMPTY;

    /* End of transmission, a callback is generated.*/
    _uart_tx2_isr_code(uartp);
  }
}

/*===========================================================================*/
/* Driver interrupt handlers.                                                */
/*===========================================================================*/

#if SAMA_UART_USE_UART0 || defined(__DOXYGEN__)
/**
 * @brief   UART0 IRQ handler.
 *
 * @isr
 */
OSAL_IRQ_HANDLER(SAMA_UART0_HANDLER) {

  OSAL_IRQ_PROLOGUE();

  serve_uart_irq(&UARTD0);
  aicAckInt();
  OSAL_IRQ_EPILOGUE();
}
#endif /* SAMA_UART_USE_UART0 */

#if SAMA_UART_USE_UART1 || defined(__DOXYGEN__)
/**
 * @brief   UART1 IRQ handler.
 *
 * @isr
 */
OSAL_IRQ_HANDLER(SAMA_UART1_HANDLER) {

  OSAL_IRQ_PROLOGUE();

  serve_uart_irq(&UARTD1);
  aicAckInt();
  OSAL_IRQ_EPILOGUE();
}
#endif /* SAMA_UART_USE_UART1 */

#if SAMA_UART_USE_UART2 || defined(__DOXYGEN__)
/**
 * @brief   UART2 IRQ handler.
 *
 * @isr
 */
OSAL_IRQ_HANDLER(SAMA_UART2_HANDLER) {

  OSAL_IRQ_PROLOGUE();

  serve_uart_irq(&UARTD2);
  aicAckInt();
  OSAL_IRQ_EPILOGUE();
}
#endif /* SAMA_UART_USE_UART2 */

#if SAMA_UART_USE_UART3 || defined(__DOXYGEN__)
/**
 * @brief   UART3 IRQ handler.
 *
 * @isr
 */
OSAL_IRQ_HANDLER(SAMA_UART3_HANDLER) {

  OSAL_IRQ_PROLOGUE();

  serve_uart_irq(&UARTD3);
  aicAckInt();
  OSAL_IRQ_EPILOGUE();
}
#endif /* SAMA_UART_USE_UART3 */

#if SAMA_UART_USE_UART4 || defined(__DOXYGEN__)
/**
 * @brief   UART4 IRQ handler.
 *
 * @isr
 */
OSAL_IRQ_HANDLER(SAMA_UART4_HANDLER) {

  OSAL_IRQ_PROLOGUE();

  serve_uart_irq(&UARTD4);
  aicAckInt();
  OSAL_IRQ_EPILOGUE();
}
#endif /* SAMA_UART_USE_UART4 */

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   Low level UART driver initialization.
 *
 * @notapi
 */
void uart_lld_init(void) {

#if SAMA_UART_USE_UART0
  uartObjectInit(&UARTD0);
  UARTD0.uart      = UART0;
  UARTD0.clock     = SAMA_UART0CLK;
  UARTD0.rxdmamode = XDMAC_CC_TYPE_PER_TRAN |
                     XDMAC_CC_MBSIZE_SINGLE |
                     XDMAC_CC_DSYNC_PER2MEM |
                     XDMAC_CC_PROT_SEC |
                     XDMAC_CC_CSIZE_CHK_1 |
                     XDMAC_CC_DWIDTH_BYTE |
                     XDMAC_CC_SIF_AHB_IF1 |
                     XDMAC_CC_DIF_AHB_IF0 |
                     XDMAC_CC_SAM_FIXED_AM |
                     XDMAC_CC_DAM_INCREMENTED_AM |
                     XDMAC_CC_PERID(PERID_UART0_RX);
  UARTD0.txdmamode = XDMAC_CC_TYPE_PER_TRAN |
                     XDMAC_CC_MBSIZE_SINGLE |
                     XDMAC_CC_DSYNC_MEM2PER |
                     XDMAC_CC_PROT_SEC |
                     XDMAC_CC_CSIZE_CHK_1 |
                     XDMAC_CC_DWIDTH_BYTE |
                     XDMAC_CC_SIF_AHB_IF0 |
                     XDMAC_CC_DIF_AHB_IF1 |
                     XDMAC_CC_SAM_INCREMENTED_AM |
                     XDMAC_CC_DAM_FIXED_AM |
                     XDMAC_CC_PERID(PERID_UART0_TX);
  UARTD0.dmarx     = 0;
  UARTD0.dmatx     = 0;
#endif

#if SAMA_UART_USE_UART1
  uartObjectInit(&UARTD1);
  UARTD1.uart      = UART1;
  UARTD1.clock     = SAMA_UART1CLK;
  UARTD1.rxdmamode = XDMAC_CC_TYPE_PER_TRAN |
                     XDMAC_CC_MBSIZE_SINGLE |
                     XDMAC_CC_DSYNC_PER2MEM |
                     XDMAC_CC_PROT_SEC |
                     XDMAC_CC_CSIZE_CHK_1 |
                     XDMAC_CC_DWIDTH_BYTE |
                     XDMAC_CC_SIF_AHB_IF1 |
                     XDMAC_CC_DIF_AHB_IF0 |
                     XDMAC_CC_SAM_FIXED_AM |
                     XDMAC_CC_DAM_INCREMENTED_AM |
                     XDMAC_CC_PERID(PERID_UART1_RX);
  UARTD1.txdmamode = XDMAC_CC_TYPE_PER_TRAN |
                     XDMAC_CC_MBSIZE_SINGLE |
                     XDMAC_CC_DSYNC_MEM2PER |
                     XDMAC_CC_PROT_SEC |
                     XDMAC_CC_CSIZE_CHK_1 |
                     XDMAC_CC_DWIDTH_BYTE |
                     XDMAC_CC_SIF_AHB_IF0 |
                     XDMAC_CC_DIF_AHB_IF1 |
                     XDMAC_CC_SAM_INCREMENTED_AM |
                     XDMAC_CC_DAM_FIXED_AM |
                     XDMAC_CC_PERID(PERID_UART1_TX);
  UARTD1.dmarx     = 0;
  UARTD1.dmatx     = 0;
#endif

#if SAMA_UART_USE_UART2
  uartObjectInit(&UARTD2);
  UARTD2.uart      = UART2;
  UARTD2.clock     = SAMA_UART2CLK;
  UARTD2.rxdmamode = XDMAC_CC_TYPE_PER_TRAN |
                     XDMAC_CC_MBSIZE_SINGLE |
                     XDMAC_CC_DSYNC_PER2MEM |
                     XDMAC_CC_PROT_SEC |
                     XDMAC_CC_CSIZE_CHK_1 |
                     XDMAC_CC_DWIDTH_BYTE |
                     XDMAC_CC_SIF_AHB_IF1 |
                     XDMAC_CC_DIF_AHB_IF0 |
                     XDMAC_CC_SAM_FIXED_AM |
                     XDMAC_CC_DAM_INCREMENTED_AM |
                     XDMAC_CC_PERID(PERID_UART2_RX);
  UARTD2.txdmamode = XDMAC_CC_TYPE_PER_TRAN |
                     XDMAC_CC_MBSIZE_SINGLE |
                     XDMAC_CC_DSYNC_MEM2PER |
                     XDMAC_CC_PROT_SEC |
                     XDMAC_CC_CSIZE_CHK_1 |
                     XDMAC_CC_DWIDTH_BYTE |
                     XDMAC_CC_SIF_AHB_IF0 |
                     XDMAC_CC_DIF_AHB_IF1 |
                     XDMAC_CC_SAM_INCREMENTED_AM |
                     XDMAC_CC_DAM_FIXED_AM |
                     XDMAC_CC_PERID(PERID_UART2_TX);
  UARTD2.dmarx     = 0;
  UARTD2.dmatx     = 0;
#endif

#if SAMA_UART_USE_UART3
  uartObjectInit(&UARTD3);
  UARTD3.uart      = UART3;
  UARTD3.clock     = SAMA_UART3CLK;
  UARTD3.rxdmamode = XDMAC_CC_TYPE_PER_TRAN |
                     XDMAC_CC_MBSIZE_SINGLE |
                     XDMAC_CC_DSYNC_PER2MEM |
                     XDMAC_CC_PROT_SEC |
                     XDMAC_CC_CSIZE_CHK_1 |
                     XDMAC_CC_DWIDTH_BYTE |
                     XDMAC_CC_SIF_AHB_IF1 |
                     XDMAC_CC_DIF_AHB_IF0 |
                     XDMAC_CC_SAM_FIXED_AM |
                     XDMAC_CC_DAM_INCREMENTED_AM |
                     XDMAC_CC_PERID(PERID_UART3_RX);
  UARTD3.txdmamode = XDMAC_CC_TYPE_PER_TRAN |
                     XDMAC_CC_MBSIZE_SINGLE |
                     XDMAC_CC_DSYNC_MEM2PER |
                     XDMAC_CC_PROT_SEC |
                     XDMAC_CC_CSIZE_CHK_1 |
                     XDMAC_CC_DWIDTH_BYTE |
                     XDMAC_CC_SIF_AHB_IF0 |
                     XDMAC_CC_DIF_AHB_IF1 |
                     XDMAC_CC_SAM_INCREMENTED_AM |
                     XDMAC_CC_DAM_FIXED_AM |
                     XDMAC_CC_PERID(PERID_UART3_TX);
  UARTD3.dmarx     = 0;
  UARTD3.dmatx     = 0;
#endif

#if SAMA_UART_USE_UART4
  uartObjectInit(&UARTD4);
  UARTD4.uart      = UART4;
  UARTD4.clock     = SAMA_UART4CLK;
  UARTD4.rxdmamode = XDMAC_CC_TYPE_PER_TRAN |
                     XDMAC_CC_MBSIZE_SINGLE |
                     XDMAC_CC_DSYNC_PER2MEM |
                     XDMAC_CC_PROT_SEC |
                     XDMAC_CC_CSIZE_CHK_1 |
                     XDMAC_CC_DWIDTH_BYTE |
                     XDMAC_CC_SIF_AHB_IF1 |
                     XDMAC_CC_DIF_AHB_IF0 |
                     XDMAC_CC_SAM_FIXED_AM |
                     XDMAC_CC_DAM_INCREMENTED_AM |
                     XDMAC_CC_PERID(PERID_UART4_RX);
  UARTD4.txdmamode = XDMAC_CC_TYPE_PER_TRAN |
                     XDMAC_CC_MBSIZE_SINGLE |
                     XDMAC_CC_DSYNC_MEM2PER |
                     XDMAC_CC_PROT_SEC |
                     XDMAC_CC_CSIZE_CHK_1 |
                     XDMAC_CC_DWIDTH_BYTE |
                     XDMAC_CC_SIF_AHB_IF0 |
                     XDMAC_CC_DIF_AHB_IF1 |
                     XDMAC_CC_SAM_INCREMENTED_AM |
                     XDMAC_CC_DAM_FIXED_AM |
                     XDMAC_CC_PERID(PERID_UART4_TX);
  UARTD4.dmarx     = 0;
  UARTD4.dmatx     = 0;
#endif

}

/**
 * @brief   Configures and activates the UART peripheral.
 *
 * @param[in] uartp     pointer to the @p UARTDriver object
 *
 * @notapi
 */
void uart_lld_start(UARTDriver *uartp) {

  if (uartp->state == UART_STOP) {
#if SAMA_UART_USE_UART0
    if (&UARTD0 == uartp) {
      uartp->dmarx = dmaChannelAllocate(SAMA_UART_UART0_IRQ_PRIORITY,
                                       (sama_dmaisr_t)uart_lld_serve_rx_end_irq,
                                       (void *)uartp);

      uartp->dmatx = dmaChannelAllocate(SAMA_UART_UART0_IRQ_PRIORITY,
                                       (sama_dmaisr_t)uart_lld_serve_tx_end_irq,
                                       (void *)uartp);
      pmcEnableUART0();
      aicSetSourcePriority(ID_UART0, SAMA_UART_UART0_IRQ_PRIORITY);
      aicSetSourceHandler(ID_UART0, SAMA_UART0_HANDLER);
      aicEnableInt(ID_UART0);

      /* Configuring destination and mode of txdma channel*/
      dmaChannelSetDestination(uartp->dmatx, &uartp->uart->UART_THR);
      dmaChannelSetMode(uartp->dmatx, uartp->txdmamode);

      /* Configuring source and mode of rxdma channel*/
      dmaChannelSetSource(uartp->dmarx, &uartp->uart->UART_RHR);
      dmaChannelSetMode(uartp->dmarx, uartp->rxdmamode);
    }
#endif

#if SAMA_UART_USE_UART1
    if (&UARTD1 == uartp) {
      uartp->dmarx = dmaChannelAllocate(SAMA_UART_UART1_IRQ_PRIORITY,
                                      (sama_dmaisr_t)uart_lld_serve_rx_end_irq,
                                      (void *)uartp);

      uartp->dmatx = dmaChannelAllocate(SAMA_UART_UART1_IRQ_PRIORITY,
                                      (sama_dmaisr_t)uart_lld_serve_tx_end_irq,
                                      (void *)uartp);
      pmcEnableUART1();
      aicSetSourcePriority(ID_UART1, SAMA_UART_UART1_IRQ_PRIORITY);
      aicSetSourceHandler(ID_UART1, SAMA_UART1_HANDLER);
      aicEnableInt(ID_UART1);

      /* Configuring destination and mode of txdma channel*/
      dmaChannelSetDestination(uartp->dmatx, &uartp->uart->UART_THR);
      dmaChannelSetMode(uartp->dmatx, uartp->txdmamode);

      /* Configuring source and mode of rxdma channel*/
      dmaChannelSetSource(uartp->dmarx, &uartp->uart->UART_RHR);
      dmaChannelSetMode(uartp->dmarx, uartp->rxdmamode);
    }
#endif

#if SAMA_UART_USE_UART2
    if (&UARTD2 == uartp) {
      uartp->dmarx = dmaChannelAllocate(SAMA_UART_UART2_IRQ_PRIORITY,
                                       (sama_dmaisr_t)uart_lld_serve_rx_end_irq,
                                       (void *)uartp);

      uartp->dmatx = dmaChannelAllocate(SAMA_UART_UART2_IRQ_PRIORITY,
                                       (sama_dmaisr_t)uart_lld_serve_tx_end_irq,
                                       (void *)uartp);
      pmcEnableUART2();
      aicSetSourcePriority(ID_UART2, SAMA_UART_UART2_IRQ_PRIORITY);
      aicSetSourceHandler(ID_UART2, SAMA_UART2_HANDLER);
      aicEnableInt(ID_UART2);

      /* Configuring destination and mode of txdma channel*/
      dmaChannelSetDestination(uartp->dmatx, &uartp->uart->UART_THR);
      dmaChannelSetMode(uartp->dmatx, uartp->txdmamode);

      /* Configuring source and mode of rxdma channel*/
      dmaChannelSetSource(uartp->dmarx, &uartp->uart->UART_RHR);
      dmaChannelSetMode(uartp->dmarx, uartp->rxdmamode);
    }
#endif

#if SAMA_UART_USE_UART3
    if (&UARTD3 == uartp) {
      uartp->dmarx = dmaChannelAllocate(SAMA_UART_UART3_IRQ_PRIORITY,
                                       (sama_dmaisr_t)uart_lld_serve_rx_end_irq,
                                       (void *)uartp);

      uartp->dmatx = dmaChannelAllocate(SAMA_UART_UART3_IRQ_PRIORITY,
                                       (sama_dmaisr_t)uart_lld_serve_tx_end_irq,
                                       (void *)uartp);
      pmcEnableUART3();
      aicSetSourcePriority(ID_UART3, SAMA_UART_UART3_IRQ_PRIORITY);
      aicSetSourceHandler(ID_UART3, SAMA_UART3_HANDLER);
      aicEnableInt(ID_UART3);

      /* Configuring destination and mode of txdma channel*/
      dmaChannelSetDestination(uartp->dmatx, &uartp->uart->UART_THR);
      dmaChannelSetMode(uartp->dmatx, uartp->txdmamode);

      /* Configuring source and mode of rxdma channel*/
      dmaChannelSetSource(uartp->dmarx, &uartp->uart->UART_RHR);
      dmaChannelSetMode(uartp->dmarx, uartp->rxdmamode);
    }
#endif

#if SAMA_UART_USE_UART4
    if (&UARTD4 == uartp) {
      uartp->dmarx = dmaChannelAllocate(SAMA_UART_UART4_IRQ_PRIORITY,
                                       (sama_dmaisr_t)uart_lld_serve_rx_end_irq,
                                       (void *)uartp);

      uartp->dmatx = dmaChannelAllocate(SAMA_UART_UART4_IRQ_PRIORITY,
                                       (sama_dmaisr_t)uart_lld_serve_tx_end_irq,
                                       (void *)uartp);
      pmcEnableUART4();
      aicSetSourcePriority(ID_UART4, SAMA_UART_UART4_IRQ_PRIORITY);
      aicSetSourceHandler(ID_UART4, SAMA_UART4_HANDLER);
      aicEnableInt(ID_UART4);

      /* Configuring destination and mode of txdma channel*/
      dmaChannelSetDestination(uartp->dmatx, &uartp->uart->UART_THR);
      dmaChannelSetMode(uartp->dmatx, uartp->txdmamode);

      /* Configuring source and mode of rxdma channel*/
      dmaChannelSetSource(uartp->dmarx, &uartp->uart->UART_RHR);
      dmaChannelSetMode(uartp->dmarx, uartp->rxdmamode);
    }
#endif

    /* Static DMA setup, the transfer size depends on the USART settings,
       it is 16 bits if M=1 and PCE=0 else it is 8 bits.*/
   // if ((uartp->config->cr1 & (USART_CR1_M | USART_CR1_PCE)) == USART_CR1_M0)
   //   uartp->dmamode |= STM32_DMA_CR_PSIZE_HWORD | STM32_DMA_CR_MSIZE_HWORD;

    uartp->rxbuf = 0;
  }

  uartp->rxstate = UART_RX_IDLE;
  uartp->txstate = UART_TX_IDLE;
  uart_start(uartp);
}

/**
 * @brief   Deactivates the UART peripheral.
 *
 * @param[in] uartp     pointer to the @p UARTDriver object
 *
 * @notapi
 */
void uart_lld_stop(UARTDriver *uartp) {

  if (uartp->state == UART_READY) {
    uart_stop(uartp);
    dmaChannelRelease(uartp->dmarx);
    dmaChannelRelease(uartp->dmatx);

#if SAMA_UART_USE_UART0
    if (&UARTD0 == uartp) {
      pmcDisableUART0();
      return;
    }
#endif

#if SAMA_UART_USE_UART1
    if (&UARTD1 == uartp) {
      pmcDisableUART1();
      return;
    }
#endif

#if SAMA_UART_USE_UART2
    if (&UARTD2 == uartp) {
      pmcDisableUART2();
      return;
    }
#endif

#if SAMA_UART_USE_UART3
    if (&UARTD3 == uartp) {
      pmcDisableUART3();
      return;
    }
#endif

#if SAMA_UART_USE_UART4
    if (&UARTD4 == uartp) {
      pmcDisableUART4();
      return;
    }
#endif

  }
}

/**
 * @brief   Starts a transmission on the UART peripheral.
 * @note    The buffers are organized as uint8_t arrays for data sizes below
 *          or equal to 8 bits else it is organized as uint16_t arrays.
 *
 * @param[in] uartp     pointer to the @p UARTDriver object
 * @param[in] n         number of data frames to send
 * @param[in] txbuf     the pointer to the transmit buffer
 *
 * @notapi
 */
void uart_lld_start_send(UARTDriver *uartp, size_t n, const void *txbuf) {

  /* TX DMA channel preparation.*/
  dmaChannelSetSource(uartp->dmatx, txbuf);
  dmaChannelSetTransactionSize(uartp->dmatx, n);

  /* Only enable TC interrupt if there's a callback attached to it.
     Also we need to clear TC flag which could be set before. */
  if (uartp->config->txend2_cb != NULL) {
    uartp->uart->UART_IER = UART_IER_TXEMPTY;
  }

  /* Starting transfer.*/
  dmaChannelEnable(uartp->dmatx);
}

/**
 * @brief   Stops any ongoing transmission.
 * @note    Stopping a transmission also suppresses the transmission callbacks.
 *
 * @param[in] uartp     pointer to the @p UARTDriver object
 *
 * @return              The number of data frames not transmitted by the
 *                      stopped transmit operation.
 *
 * @notapi
 */
size_t uart_lld_stop_send(UARTDriver *uartp) {

  dmaChannelDisable(uartp->dmatx);
  /* number of data frames not transmitted is always zero */
  return 0;
}

/**
 * @brief   Starts a receive operation on the UART peripheral.
 * @note    The buffers are organized as uint8_t arrays for data sizes below
 *          or equal to 8 bits else it is organized as uint16_t arrays.
 *
 * @param[in] uartp     pointer to the @p UARTDriver object
 * @param[in] n         number of data frames to send
 * @param[out] rxbuf    the pointer to the receive buffer
 *
 * @notapi
 */
void uart_lld_start_receive(UARTDriver *uartp, size_t n, void *rxbuf) {

  /* Stopping previous activity (idle state).*/
  dmaChannelDisable(uartp->dmarx);

  /* Enabling BIE interrupt if disabled */
  if ((uartp->dmarx->xdmac->XDMAC_CHID[uartp->dmarx->chid].XDMAC_CIM & XDMAC_CIM_BIM) == 0) {
  uartp->dmarx->xdmac->XDMAC_CHID[uartp->dmarx->chid].XDMAC_CIE =  XDMAC_CIE_BIE;
  }

  /* Resetting the XDMAC_CNCDAx */
  uartp->dmarx->xdmac->XDMAC_CHID[uartp->dmarx->chid].XDMAC_CNDA = 0;
  /* resetting the XDMAC_CNDCx register */
  uartp->dmarx->xdmac->XDMAC_CHID[uartp->dmarx->chid].XDMAC_CNDC = 0;

  /* RX DMA channel preparation.*/
  dmaChannelSetSource(uartp->dmarx, &uartp->uart->UART_RHR);
  dmaChannelSetDestination(uartp->dmarx, rxbuf);
  dmaChannelSetTransactionSize(uartp->dmarx, n);
  dmaChannelSetMode(uartp->dmarx, uartp->rxdmamode);

  /* Starting transfer.*/
  dmaChannelEnable(uartp->dmarx);
}

/**
 * @brief   Stops any ongoing receive operation.
 * @note    Stopping a receive operation also suppresses the receive callbacks.
 *
 * @param[in] uartp     pointer to the @p UARTDriver object
 *
 * @return              The number of data frames not received by the
 *                      stopped receive operation.
 *
 * @notapi
 */
size_t uart_lld_stop_receive(UARTDriver *uartp) {
  size_t n;

  dmaChannelDisable(uartp->dmarx);
  n = 0;
  uart_enter_rx_idle_loop(uartp);

  return n;
}

#endif /* HAL_USE_UART */

/** @} */
