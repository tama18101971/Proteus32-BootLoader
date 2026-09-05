/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Author             : WCH / Proteus32
 * Version            : V2.0.0
 * Date               : 2026/09/05
 * Description        : Main program body.
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/
/*
 * CH32V003 UART IAP Bootloader (protocol v2, WL_FLASH-safe)
 *
 * The bootloader runs from the separate boot-area flash (0x00000000, 1920 B),
 * receives a firmware image over UART at 460800 baud and writes it to the
 * user flash starting at 0x08000000. The settings region WL_FLASH
 * (0x08003F00..0x08003FFF) is never written or erased.
 *
 * Boot mode is fixed at compile time to IO: PC3 held low (button pressed to
 * GND) at power-on -> stay in IAP mode; PC3 high -> jump straight to the
 * application. The old COMMAND mode (magic word 0x5AA55AA5 at 0x08003FFC)
 * and CMD_JUMP_IAP are permanently removed (TZ sections 3.3, 3.5.2).
 *
 * UART protocol packet format:
 *   [0xAA] [0x55] [Cmd] [Len] [Data...] [ChecksumL] [ChecksumH] [0x55] [0xAA]
 *   Commands: 0x80=PROM, 0x81=ERASE, 0x82=VERIFY, 0x83=END, 0x84=READ
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

/* Boot mode fixed at compile time: IO only (PC3 button).
 * The COMMAND mode constant is intentionally gone — see the header comment. */
#define UPGRADE_MODE_IO    1
#define UPGRADE_MODE       UPGRADE_MODE_IO

/* ~3 cycles per iteration at 24 MHz => ~125 ns; 1,500,000 iterations give
 * ~150-190 ms — comfortably above the >=100 ms UART drain pause (TZ 3.5.2). */
#define END_DRAIN_DELAY    1500000

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
 *          3. IO mode: PC3 high -> jump to the application; PC3 low
 *             (button pressed) -> stay in IAP mode and poll USART1.
 *          4. When END (0x83) is received: drain the UART for >=100 ms
 *             and reset into the application.
 *
 * @return  none
 */
int main(void)
{
    u32 i;
    u32 led_tick = 0;
    u8  led4_on  = 0;
    u8  led7_on  = 0;

    /* Enable peripheral clocks: GPIOD (USART1 pins), USART1, GPIOC (status LEDs) */
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD |
                       RCC_APB2Periph_USART1 |
                       RCC_APB2Periph_GPIOC;

    /* Configure USART1: 460800 baud, 8N1, no flow control */
    USART1_CFG();

    /* IO mode (fixed): PC3 high (no button) -> jump to the application.
     * PC3 low (button pressed to GND) -> stay in IAP mode. */
    if (PC3_Check() != 0)
    {
        IAP_2_APP();
        while (1);
    }

    /*
     * Status LEDs: PC4 (green) and PC7 (red) both lit (active-high, "1" =
     * LED on) as the "bootloader entered" indicator. Push-pull outputs.
     * During the IAP wait PC7 blinks at ~4 Hz, PC4 stays on; on active
     * UART exchange PC4 blinks at ~4 Hz as well. Both LEDs are off when
     * the transfer completes (before the reset into the app).
     */
    GPIOC->CFGLR = (GPIOC->CFGLR & ~((0xF << 16) | (0xF << 28)))
                  | ((0x3 << 16) | (0x3 << 28));
    GPIOC->BSHR  = (1 << 4) | (1 << 7);
    GPIOC->BCR   = 0;
    led_tick     = 0;

    /*
     * Main IAP loop: continuously poll USART1 for incoming data.
     * When End_Flag is set the transfer is complete: drain the UART
     * (>=100 ms) so the host receives the END ACK, then jump to APP.
     */
    while (1)
    {
        /* Blink source: loop iteration count. ~6.0e6 iterations = 1 s at
         * 24 MHz (measured via the END drain), so 1.5e6 = 250 ms = 4 Hz. */
        led_tick++;

        /* Active UART exchange: blink PC4 (green) at ~4 Hz; PC7 off. */
        if (led4_on)
        {
            if ((led_tick & 0x7FFFF) == 0)
            {
                GPIOC->BSHR = (uint32_t)0x0010;  /* PC4 off */
                led4_on     = 0;
            }
        }
        else if ((USART1->STATR & USART_FLAG_RXNE) != (uint16_t)RESET)
        {
            GPIOC->BCR = (uint32_t)0x0010;       /* PC4 on */
            led4_on    = 1;
            UART_Rx_Deal();
        }

        /* Idle wait: blink PC7 (red) at ~4 Hz. */
        if (led4_on == 0)
        {
            if ((led_tick & 0x7FFFF) == 0)
            {
                if (led7_on)
                {
                    GPIOC->BSHR = (uint32_t)0x0080;  /* PC7 off */
                    led7_on     = 0;
                }
                else
                {
                    GPIOC->BCR  = (uint32_t)0x0080;  /* PC7 on */
                    led7_on     = 1;
                }
            }
        }

        /* Firmware update complete — jump to the application */
        if (End_Flag)
        {
            GPIOC->BSHR = (uint32_t)0x0090;  /* both LEDs off */
            for (i = 0; i < END_DRAIN_DELAY; i++)
            {
                __asm__ volatile("nop");
            }
            IAP_2_APP();
            while (1);
        }
    }
}