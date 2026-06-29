/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Author             : WCH
 * Version            : V1.0.1
 * Date               : 2025/01/09
 * Description        : Main program body.
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/
/*
 * CH32V003 UART IAP Bootloader
 *
 * It receives a firmware image over UART at 460800 baud, writes it to
 * flash starting at 0x08002000, and then jumps to the application.
 *
 * Boot modes (selectable at compile time via UPGRADE_MODE):
 *   - COMMAND mode: checks a calibration value in flash. If the
 *     application has written CheckNum (0x5AA55AA5) to CalAddr
 *     (0x08003FFC), the bootloader enters IAP mode to receive a new
 *     firmware. Otherwise it jumps directly to the application.
 *   - IO mode: checks the state of pin PC3 at startup. If PC3 is
 *     high (not pulled low by the application), the bootloader
 *     enters IAP mode. Otherwise it jumps to the application.
 *
 * UART protocol packet format:
 *   [0xAA] [0x55] [Cmd] [Len] [Data...] [ChecksumL] [ChecksumH] [0x55] [0xAA]
 *   Commands: 0x80=PROM, 0x81=ERASE, 0x82=VERIFY, 0x83=END, 0x84=JUMP
 *   Checksum: 16-bit sum of Cmd + Len + Data bytes (low byte first)
 */
/*
 * Note: BRR register values for different system clock frequencies:
 *   baud rate = 460800 ------- USARTx->BRR = 0X34 ------- System clock = 24MHz
 *   baud rate = 460800 ------- USARTx->BRR = 0X68 ------- System clock = 48MHz
 */
#include "debug.h"
#include "string.h"
#include "iap.h"

/* Flag set by UART_Rx_Deal() when a complete firmware image is received */
extern u8 End_Flag;

/* Compile-time selection: COMMAND or IO boot mode */
#define UPGRADE_MODE_COMMAND   0
#define UPGRADE_MODE_IO        1
#define UPGRADE_MODE           UPGRADE_MODE_IO

/*********************************************************************
 * @fn      IAP_2_APP
 *
 * @brief   Transition from bootloader (IAP) to application (APP).
 *          Clears RCC flags, sets boot mode to USER flash, and
 *          triggers a system reset so the MCU starts from 0x08000000.
 *
 * @return  none
 */
void IAP_2_APP(void)
{
    RCC_ClearFlag();
    SystemReset_StartMode(Start_Mode_USER);
    NVIC_SystemReset();
}

/*********************************************************************
 * @fn      main
 *
 * @brief   Bootloader entry point.
 *          1. Enables clocks for GPIOD (USART1 TX/RX), USART1, GPIOC.
 *          2. Configures USART1 for 460800 baud.
 *          3. Checks whether to jump to the application or enter
 *             IAP mode based on the selected upgrade mode.
 *          4. If entering IAP mode: configures status LED pins
 *             (PC4, PC7 as push-pull outputs) and polls USART1
 *             for incoming firmware data.
 *
 * @return  none
 */
int main(void)
{
    /* Enable peripheral clocks: GPIOD (USART1 pins), USART1, GPIOC (status LEDs) */
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD |
                       RCC_APB2Periph_USART1 |
                       RCC_APB2Periph_GPIOC;

    /* Configure USART1: 460800 baud, 8N1, no flow control */
    USART1_CFG();

#if UPGRADE_MODE == UPGRADE_MODE_COMMAND
    /*
     * Command mode: check if flash is programmed (first word != 0xFFFFFFFF)
     * and if the application has requested a firmware update by writing
     * CheckNum to CalAddr. If so, stay in IAP mode. Otherwise jump to APP.
     */
    if (*(uint32_t *)FLASH_Base != 0xFFFFFFFF)
    {
        if (*(uint32_t *)CalAddr == CheckNum)
        {
            IAP_2_APP();
            while (1);
        }
    }
#elif UPGRADE_MODE == UPGRADE_MODE_IO
    /*
     * IO mode: check PC3 pin state. If high, the application has not
     * been started yet (or wants a re-flash), so jump to APP.
     * If low (pin pulled to GND by the application), stay in IAP mode.
     */
    if (PC3_Check() != 0)
    {
        IAP_2_APP();
        while (1);
    }
#endif

    /*
     * Configure GPIOC pins 4 and 7 as push-pull outputs (mode bits = 0x3).
     * These pins are used as status LEDs during the IAP process.
     * Set both pins high (LEDs off) initially.
     */
     
    /* 
    GPIOC->CFGLR = (GPIOC->CFGLR & ~((0xF << 16) | (0xF << 28)))
                  | ((0x3 << 16) | (0x3 << 28));
    GPIOC->BSHR  = (1 << 4) | (1 << 7);
    */

    /*
     * Main IAP loop: continuously poll USART1 for incoming data.
     * When a complete packet is received, UART_Rx_Deal() processes it
     * (erase, program, verify, or end command).
     * When End_Flag is set, the firmware transfer is complete and
     * the bootloader jumps to the application.
     */
    while (1)
    {
        /* Check if a byte has been received in the USART1 data register */
        if ((USART1->STATR & USART_FLAG_RXNE) != (uint16_t)RESET)
        {
            UART_Rx_Deal();
        }

        /* Firmware update complete — jump to the application */
        if (End_Flag)
        {
            IAP_2_APP();
            while (1);
        }
    }
}
