/********************************** (C) COPYRIGHT  *******************************
 * File Name          : iap.h
 * Author             : WCH / Proteus32
 * Version            : V2.0.0
 * Date               : 2026/09/05
 * Description        : IAP — UART protocol v2, WL_FLASH-safe.
 *
 * Protocol v2 changes (TZ BOOTLOADER_TZ.md v1.0):
 *   - ERASE erases 252 pages of the app area [0x08000000, 0x08003F00) only;
 *   - END performs no flash operations (no 0x5AA55AA5 magic anymore);
 *   - PROM/VERIFY are guarded against any access beyond the app area;
 *   - READ (0x84) replaces the old JUMP command and returns 64 data bytes.
 *******************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/
#ifndef __IAP_H
#define __IAP_H

#include "ch32v00x.h"
#include "stdio.h"

#define USBD_DATA_SIZE    64

/* Memory layout (CH32V003F4U6, fast-flash 64-B granule):
 *   app area    [FLASH_Base,     FLASH_APP_END)  — only writable range
 *   WL_FLASH    [FLASH_APP_END,  FLASH_USER_END) — read-only for the bootloader
 */
#define FLASH_Base        0x08000000
#define FLASH_APP_END     0x08003F00
#define FLASH_USER_END    0x08004000
#define FLASH_PAGE_SIZE   0x40

#define Uart_Sync_Head1   0xaa
#define Uart_Sync_Head2   0x55

#define CMD_IAP_PROM      0x80
#define CMD_IAP_ERASE     0x81
#define CMD_IAP_VERIFY    0x82
#define CMD_IAP_END       0x83
#define CMD_IAP_READ      0x84

/* Response status codes (ST byte of the AA 55 00 ST 55 AA frame) */
#define ERR_SUCCESS       0x00
#define ERR_ERROR         0x01
#define ERR_RANGE         0x02
#define ERR_VERIFY        0x03
/* Internal marker: READ completed OK — the caller replies with a data frame */
#define ERR_Read_Ok       0x10

typedef union __attribute__((aligned(4))) _ISP_CMD
{
    struct
    {
        u8 Cmd;
        u8 Len;
        u8 data[64];
    } UART;

    struct
    {
        u8 buf[64 + 2];
    } other;
} isp_cmd;

typedef void (*iapfun)(void);

extern u8 EP2_Rx_Buffer[USBD_DATA_SIZE + 4];

u8   RecData_Deal(void);
u8   PC3_Check(void);
void USART1_CFG(void);
void UART_Rx_Deal(void);

#endif