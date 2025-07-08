/***************************************************************************//**
* \file main.c
* \version 1.0
*
* \details  This is the source code for th FX2G3 device CANFD interface application.
*           for ModusToolbox.
*
*           See \ref README.md ["README.md"]
*
*******************************************************************************
* \copyright
* (c) (2025), Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.
*
* SPDX-License-Identifier: Apache-2.0
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "cy_pdl.h"
#include <string.h>
#include "app_version.h"
#include "cy_debug.h"
#include "cybsp.h"

/* Debug log related initilization */
#define LOGGING_SCB             (SCB4)
#define LOGGING_SCB_IDX         (4)
#define DEBUG_LEVEL             (3u)

/* Debug log related initialization */
#if DEBUG_INFRA_EN
/* RAM buffer used to hold debug log data. */
#define LOGBUF_SIZE (1024u)

uint8_t logBuff[LOGBUF_SIZE];
cy_stc_debug_config_t dbgCfg = {
    .pBuffer         = logBuff,
    .traceLvl        = DEBUG_LEVEL,
    .bufSize         = LOGBUF_SIZE,
#if USBFS_LOGS_ENABLE
    .dbgIntfce       = CY_DEBUG_INTFCE_USBFS_CDC,
#else
    .dbgIntfce       = CY_DEBUG_INTFCE_UART_SCB4,
#endif
    .printNow        = true
};

#endif /* DEBUG_INFRA_EN*/

#define GPIO_TOGGLE_HSIOM P4_4_GPIO
#define GPIO_TOGGLE_PORT P4_4_PORT
#define GPIO_TOGGLE_PIN P4_4_PIN

#define CANFD_NODE_1 0x1D0
#define CANFD_NODE_2 0x244
#define USE_CANFD_NODE CANFD_NODE_1

#define CANFD_HW_CHANNEL 0
#define CANFD_BUFFER_INDEX 0
#define CANFD_DLC 8
#define CANFD_HW CANFD0
#define CANFD_INTERRUPT canfd_0_interrupt0_IRQn

void Cy_GpioToggle(uint16_t delay);

/* This is a shared context structure, unique for each canfd channel */
cy_stc_canfd_context_t canfd_context;

/**
 * \name Cy_Fx2g3_InitPeripheralClocks
 * \brief Function used to enable clocks to different peripherals on the FX2G3 device.
 * \param adcClkEnable Whether to enable clock to the ADC in the USBSS block.
 * \param usbfsClkEnable Whether to enable bus reset detect clock input to the USBFS block.
 * \retval None
 */
void Cy_Fx2g3_InitPeripheralClocks (
        bool adcClkEnable,
        bool usbfsClkEnable)
{
    if (adcClkEnable) {
        /* Divide PERI clock at 75 MHz by 75 to get 1 MHz clock using 16-bit divider #1. */
        Cy_SysClk_PeriphSetDivider(CY_SYSCLK_DIV_16_BIT, 1, 74);
        Cy_SysClk_PeriphEnableDivider(CY_SYSCLK_DIV_16_BIT, 1);
        Cy_SysLib_DelayUs(10U);
        Cy_SysClk_PeriphAssignDivider(PCLK_LVDS2USB32SS_CLOCK_SAR, CY_SYSCLK_DIV_16_BIT, 1);
    }

    if (usbfsClkEnable) {
        /* Divide PERI clock at 75 MHz by 750 to get 100 KHz clock using 16-bit divider #2. */
        Cy_SysClk_PeriphSetDivider(CY_SYSCLK_DIV_16_BIT, 2, 749);
        Cy_SysClk_PeriphEnableDivider(CY_SYSCLK_DIV_16_BIT, 2);
        Cy_SysLib_DelayUs(10U);
        Cy_SysClk_PeriphAssignDivider(PCLK_USB_CLOCK_DEV_BRS, CY_SYSCLK_DIV_16_BIT, 2);
    }
}

/**
 * \name Cy_Fx2g3_OnResetInit
 * \details
 * This function performs initialization that is required to enable scatter
 * loading of data into the High BandWidth RAM during device boot-up. The FX2G3
 * device comes up with the High BandWidth RAM disabled and hence any attempt
 * to read/write the RAM will cause the processor to hang. The RAM needs to
 * be enabled with default clock settings to allow scatter loading to work.
 * This function needs to be called from Cy_OnResetUser.
 * \retval None
 */
void
Cy_Fx2g3_OnResetInit (
        void)
{
    /* Enable clk_hf4 with IMO as input. */
    SRSS->CLK_ROOT_SELECT[4] = SRSS_CLK_ROOT_SELECT_ENABLE_Msk;

    /* Enable LVDS2USB32SS IP and select clk_hf[4] as clock input. */
    MAIN_REG->CTRL = (
            MAIN_REG_CTRL_IP_ENABLED_Msk |
            (1UL << MAIN_REG_CTRL_NUM_FAST_AHB_STALL_CYCLES_Pos) |
            (1UL << MAIN_REG_CTRL_NUM_SLOW_AHB_STALL_CYCLES_Pos) |
            (3UL << MAIN_REG_CTRL_DMA_SRC_SEL_Pos));
}

/**
 * \name Cy_PrintVersionInfo
 * \brief Function to print version information to UART console.
 * \param type Type of version string.
 * \param version Version number including major, minor, patch and build number.
 * \retval None
 */
void Cy_PrintVersionInfo (const char *type, uint32_t version)
{
    char tString[32];
    uint16_t vBuild;
    uint8_t vMajor, vMinor, vPatch;
    uint8_t typeLen = strlen(type);

    vMajor = (version >> 28U);
    vMinor = ((version >> 24U) & 0x0FU);
    vPatch = ((version >> 16U) & 0xFFU);
    vBuild = (uint16_t)(version & 0xFFFFUL);

    memcpy(tString, type, typeLen);
    tString[typeLen++] = '0' + (vMajor / 10);
    tString[typeLen++] = '0' + (vMajor % 10);
    tString[typeLen++] = '.';
    tString[typeLen++] = '0' + (vMinor / 10);
    tString[typeLen++] = '0' + (vMinor % 10);
    tString[typeLen++] = '.';
    tString[typeLen++] = '0' + (vPatch / 10);
    tString[typeLen++] = '0' + (vPatch % 10);
    tString[typeLen++] = '.';
    tString[typeLen++] = '0' + (vBuild / 1000);
    tString[typeLen++] = '0' + ((vBuild % 1000) / 100);
    tString[typeLen++] = '0' + ((vBuild % 100) / 10);
    tString[typeLen++] = '0' + (vBuild % 10);
    tString[typeLen++] = '\r';
    tString[typeLen++] = '\n';
    tString[typeLen] = 0;

    DBG_APP_INFO("%s", tString);
}

/**
 * \name Cy_CANFD_ISR
 * \brief This is the interrupt handler function for the canfd interrupt.
 * \retval None
 */
void Cy_CANFD_ISR(void)
{

    /* Just call the IRQ handler with the current channel number and context */
    Cy_CANFD_IrqHandler(CANFD_HW, CANFD_HW_CHANNEL, &canfd_context);
}

/**
 * \name Cy_CAN_SendData
 * \brief This is the function to Send data on the CAN bus.
 * \retval CANFD message transmission status
 */
cy_en_canfd_status_t Cy_CAN_SendData(void)
{
    cy_en_canfd_status_t status;

    status = Cy_CANFD_UpdateAndTransmitMsgBuffer(CANFD_HW,
                                                 CANFD_HW_CHANNEL,
                                                 &CANFD_txBuffer_0,
                                                 CANFD_BUFFER_INDEX,
                                                 &canfd_context);
    return status;
}

/**
 * \name Cy_CAN_ReceiveCb
 * \brief This is the callback function for canfd reception
 * \param msg_valid Message received properly or not
 * \param msg_buf_fifo_num RxFIFO number of the received message
 * \param canfd_rx_buf Message buffer
 * \retval None
 */
void Cy_CAN_ReceiveCb(bool msg_valid,
                       uint8_t msg_buf_fifo_num,
                       cy_stc_canfd_rx_buffer_t *canfd_rx_buf)
{

    /* Array to hold the data bytes of the CANFD frame */
    uint8_t canfd_data_buffer[CANFD_DLC];
    /* Variable to hold the data length code of the CANFD frame */
    uint32_t canfd_dlc;
    /* Variable to hold the Identifier of the CANFD frame */
    uint32_t canfd_id;

    uint8_t index = 0;

    if (true == msg_valid)
    {
        /* Checking whether the frame received is a data frame */
        if (CY_CANFD_RTR_DATA_FRAME == canfd_rx_buf->r0_f->rtr)
        {
        	Cy_GpioToggle(100);
            canfd_dlc = canfd_rx_buf->r1_f->dlc;
            canfd_id = canfd_rx_buf->r0_f->id;

            DBG_APP_INFO(" %d Bytes received from Node - with 0x%x identifier \n\r",canfd_dlc,canfd_id);

            memcpy((uint8_t *)canfd_data_buffer, (uint8_t *)canfd_rx_buf->data_area_f, canfd_dlc);

            for(index = 0; index < canfd_dlc; index++)
            {
            	DBG_APP_INFO("RX Buf[%d]: 0x%x \n\r", index,canfd_data_buffer[index]);
            }
        }
    }
}

/**
 * \name Cy_PeripheralInit
 * \brief Initialize peripherals used by the application.
 * \retval None
 */
void Cy_PeripheralInit(void)
{
    cy_stc_gpio_pin_config_t pinCfg;
    cy_stc_sysint_t intrCfg;
    cy_en_canfd_status_t status;

    /* Do all the relevant clock configuration */
    Cy_Fx2g3_InitPeripheralClocks(false, true);

    /* Unlock and then disable the watchdog. */
    Cy_WDT_Unlock();
    Cy_WDT_Disable();

    /*
     * If logging is done through the USBFS port, ISR execution is required in this application.
     * Set BASEPRI value to 0 to ensure all exceptions can run and then enable interrupts.
     */
#if (CY_CPU_CORTEX_M4)
    __set_BASEPRI(0);
#endif /* CY_CPU_CORTEX_M4 */

    /* Enable all interrupts. */
    __enable_irq();

    /* Configure P4.4 as strong drive GPIO. */
    memset((void *)&pinCfg, 0, sizeof(pinCfg));
    pinCfg.driveMode = CY_GPIO_DM_STRONG_IN_OFF;
    pinCfg.hsiom = GPIO_TOGGLE_HSIOM;
    Cy_GPIO_Pin_Init(GPIO_TOGGLE_PORT, GPIO_TOGGLE_PIN, &pinCfg);

    /* Register ISR for and enable CAN Interrupt. */
#if CY_CPU_CORTEX_M4
    intrCfg.intrSrc      = CANFD_INTERRUPT;
    intrCfg.intrPriority = 1U;
#else
    intrCfg.cm0pSrc      = CANFD_INTERRUPT;
    intrCfg.intrSrc      = NvicMux5_IRQn;
    intrCfg.intrPriority = 1;
#endif /* CY_CPU_CORTEX_M4 */
    Cy_SysInt_Init(&intrCfg, &Cy_CANFD_ISR);
    NVIC_EnableIRQ(intrCfg.intrSrc);


    status = Cy_CANFD_Init(CANFD_HW, CANFD_HW_CHANNEL, &CANFD_config,
                           &canfd_context);

    if (CY_CANFD_SUCCESS != status)
    {
        while (1);
    }
}

/**
 * \name main(void)
 * \brief Entry to the application.
 * \retval Does not return
 */
int main(void)
{
	/* Initialize the PDL driver library and set the clock variables. */
    /* Note: All FX devices,  share a common configuration structure. */
	Cy_PDL_Init(&cy_deviceIpBlockCfgFX3G2);

	/* Initialize the device and board peripherals */
	cybsp_init();

	/* Initialize the device and board peripherals */
	Cy_PeripheralInit();

#if DEBUG_INFRA_EN
#if !USBFS_LOGS_ENABLE
    /* Initialize the UART for logging. */
    InitUart(LOGGING_SCB_IDX);
#endif /* !USBFS_LOGS_ENABLE */

    /*
     * Initialize the logger module. We are using a blocking print option which will
     * output the messages immediately without buffering.
     */
    Cy_Debug_LogInit(&dbgCfg);
#endif /* DEBUG_INFRA_EN */

    Cy_SysLib_Delay(500);
    CANFD_T0RegisterBuffer_0.id = USE_CANFD_NODE;
    Cy_Debug_AddToLog(1, "********** FX2G3:CANFD Interface application ********** \r\n");

    /* Print application version information. */
    Cy_PrintVersionInfo("APP_VERSION: ", APP_VERSION_NUM);
    Cy_SysLib_Delay(500);

    for (;;)
    {
    	Cy_CAN_SendData();
    	DBG_APP_INFO("CAN Node - 0x%x \n\r", USE_CANFD_NODE);
    	Cy_SysLib_Delay(1000);
    }

    /* Return statement will not be hit. */
    return 0;
}

/**
 * \name Cy_GpioToggle
 * \brief set a GPIO pin, wait, clear the GPIO pin
 * \param delay specify time to wait before clearing pin in ms
 * \retval None
 */
void Cy_GpioToggle(uint16_t delay)
{
    Cy_GPIO_Set(GPIO_TOGGLE_PORT, GPIO_TOGGLE_PIN);
    Cy_SysLib_Delay(delay);
    Cy_GPIO_Clr(GPIO_TOGGLE_PORT, GPIO_TOGGLE_PIN);
}
/* [] END OF FILE */
