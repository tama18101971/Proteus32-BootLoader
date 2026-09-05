/********************************** (C) COPYRIGHT  *******************************
 * File Name          : iap.c
 * Author             : WCH / Proteus32
 * Version            : V2.0.0
 * Date               : 2026/09/05
 * Description        : IAP — UART protocol v2, WL_FLASH-safe.
 *
 * Protocol v2 (TZ BOOTLOADER_TZ.md v1.0):
 *   - no FLASH_EraseAllPages() (MER) anywhere; ERASE = 252 x 64-B page erase
 *     strictly inside [0x08000000, 0x08003F00);
 *   - END performs no flash operations at all and replies ACK before reset;
 *   - PROM/VERIFY guarded: any attempt outside the app area -> ERR_RANGE
 *     before a single byte is written;
 *   - READ (0x84, addr, u32 LE) dumps 64 B of the whole user flash (read-only);
 *   - status codes: 0x00 OK, 0x01 ERR, 0x02 ERR_RANGE, 0x03 ERR_VERIFY.
 *******************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/
#include "iap.h"
#include "string.h"
#include "flash.h"
#include "core_riscv.h"

/******************************************************************************/

u32 Program_addr     = FLASH_Base;
u32 Verify_addr      = FLASH_Base;
u8  Fast_Program_Buf[128];
u16 CodeLen          = 0;
u8  End_Flag         = 0;
u8  EP2_Rx_Buffer[USBD_DATA_SIZE + 4];
#define isp_cmd_t ((isp_cmd *)EP2_Rx_Buffer)

/*********************************************************************
 * @fn      Session_Reset
 *
 * @brief   Reset the UART session state (P5 of the TZ): aborted
 *          sessions must not leak into the next one.
 *
 * @return  none
 */
static void Session_Reset(void)
{
    Program_addr     = FLASH_Base;
    Verify_addr      = FLASH_Base;
    CodeLen          = 0;
}

/*********************************************************************
 * @fn      USART1_CFG
 *
 * @brief   GPIOD-USART1 init
 *
 * @return  none
 */
void USART1_CFG(void)
{
    GPIOD->CFGLR = 0X48B44444;
    GPIOD->BCR   = (((uint32_t)0x01) << 6);
    USART1->CTLR2 |= USART_StopBits_1;

    USART1->CTLR1 = 0X200C;
    USART1->CTLR3 |= USART_HardwareFlowControl_None;
    USART1->BRR    = 0X34;
    USART1->CTLR1 |= ((uint16_t)0x2000);
}

/*********************************************************************
 * @fn      RecData_Deal
 *
 * @brief   UART deal data
 *
 * @return  ERR_SUCCESS - SUCCESS
 *          ERR_ERROR - ERROR
 *          ERR_RANGE - out of app-area bounds
 *          ERR_VERIFY - verify mismatch
 *          ERR_Read_Ok - READ completed (data in isp_cmd_t->UART.data)
 */
u8 RecData_Deal(void)
{
    u8  i, s;
    u8  Lenth;
    u32 a;

    Lenth = isp_cmd_t->UART.Len;
    s     = ERR_SUCCESS;

    switch (isp_cmd_t->UART.Cmd)
    {
        case CMD_IAP_ERASE:
            /* Page erase the app area [FLASH_Base, FLASH_APP_END) only.
             * 252 x 64 B; WL_FLASH (0x08003F00..0x08003FFF) is never touched. */
            FLASH_Unlock_Fast();
            for (a = FLASH_Base; a < FLASH_APP_END; a += FLASH_PAGE_SIZE)
            {
                FLASH_ErasePage_Fast(a);
            }
            FLASH_Lock_Fast();
            /* Reset session state (P5 of the TZ) */
            Session_Reset();
            break;

        case CMD_IAP_PROM:
            /* Guard: refuse the whole block BEFORE any write. The 252nd page
             * ends exactly at FLASH_APP_END; nothing beyond may be touched. */
            if (Program_addr + FLASH_PAGE_SIZE > FLASH_APP_END)
            {
                s = ERR_RANGE;
                break;
            }
            for (i = 0; i < Lenth; i++)
            {
                Fast_Program_Buf[CodeLen + i] = isp_cmd_t->UART.data[i];
            }
            CodeLen += Lenth;
            if (CodeLen >= 64)
            {
                /* Re-enter fast program mode for this block: ERASE locks it
                 * afterwards (TZ 3.5.2), and a locked flash silently skips
                 * programming (VERIFY then fails at block 0). Unlock is
                 * idempotent, so this is safe across the whole session. */
                FLASH_Unlock_Fast();
                CH32_IAP_Program(Program_addr, (u32 *)Fast_Program_Buf);
                CodeLen -= 64;
                for (i = 0; i < CodeLen; i++)
                {
                    Fast_Program_Buf[i] = Fast_Program_Buf[64 + i];
                }
                Program_addr += 0x40;
            }
            break;

        case CMD_IAP_VERIFY:
            /* Guard: never verify outside the app area. */
            if (Verify_addr + FLASH_PAGE_SIZE > FLASH_APP_END)
            {
                s = ERR_RANGE;
                break;
            }
            for (i = 0; i < Lenth; i++)
            {
                if (isp_cmd_t->UART.data[i] != *(u8 *)(Verify_addr + i))
                {
                    s = ERR_VERIFY;
                    break;
                }
            }
            /* On mismatch the address is NOT advanced: a resend retries
             * the same block. */
            if (s == ERR_SUCCESS)
            {
                Verify_addr += Lenth;
            }
            break;

        case CMD_IAP_END:
            /* No flash operations at all: no magic word, no page erase.
             * Reply ACK (status = ERR_SUCCESS), then main() waits >=100 ms
             * and resets into the application. */
            Session_Reset();
            End_Flag = 1;
            break;

        case CMD_IAP_READ:
            /* Payload = target address, u32 little-endian. */
            a = (u32)isp_cmd_t->UART.data[0]
              | ((u32)isp_cmd_t->UART.data[1] << 8)
              | ((u32)isp_cmd_t->UART.data[2] << 16)
              | ((u32)isp_cmd_t->UART.data[3] << 24);
            /* Read-only window: the whole user flash (incl. WL_FLASH),
             * 64-B aligned window — data replaces the request payload. */
            if ((a < FLASH_Base) || (a + FLASH_PAGE_SIZE > FLASH_USER_END))
            {
                s = ERR_RANGE;
                break;
            }
            for (i = 0; i < FLASH_PAGE_SIZE; i++)
            {
                isp_cmd_t->UART.data[i] = *(u8 *)(a + i);
            }
            s = ERR_Read_Ok;
            break;

        default:
            s = ERR_ERROR;
            break;
    }

    return s;
}

/*********************************************************************
 * @fn      PC3_Check
 *
 * @brief   Check PC3 state
 *
 * @return  1 - APP
 *          0 - IAP
 */
u8 PC3_Check(void)
{
    GPIOC->CFGLR = (GPIOC->CFGLR & 0xFFFF0FFF) | 0x00008000;
    GPIOC->BSHR  = 0x00000008;

    /* Delay 10 cycles and check pin state */
    if (GPIOC->INDR & GPIO_Pin_3) return 1;
    __asm__ volatile("nop; nop; nop; nop;");
    return (GPIOC->INDR & GPIO_Pin_3) ? 1 : 0;
}

/*********************************************************************
 * @fn      UART1_SendMultiyData
 *
 * @brief   Send multiple bytes via USART1
 *
 * @param   pbuf - data buffer
 *          num  - number of bytes
 *
 * @return  none
 */
void UART1_SendMultiyData(u8 *pbuf, u8 num)
{
    u8 i = 0;

    while (i < num)
    {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET)
            ;
        USART_SendData(USART1, pbuf[i]);
        i++;
    }
}

/*********************************************************************
 * @fn      UART1_SendData
 *
 * @brief   USART1 send one byte
 *
 * @param   data - byte to send
 *
 * @return  none
 */
void UART1_SendData(u8 data)
{
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET)
        ;
    USART_SendData(USART1, data);
}

/*********************************************************************
 * @fn      Uart1_Rx
 *
 * @brief   Uart1 receive one byte
 *
 * @return  received byte
 */
u8 Uart1_Rx(void)
{
    while ((USART1->STATR & USART_FLAG_RXNE) == RESET)
        ;
    return (uint16_t)(USART1->DATAR & (uint16_t)0x01FF);
}

/*********************************************************************
 * @fn      UART_Rx_Deal
 *
 * @brief   UART Rx data deal
 *
 * @return  none
 */
void UART_Rx_Deal(void)
{
    u8  i, s;
    u16 Data_add = 0;

    if (Uart1_Rx() != Uart_Sync_Head1) return;
    if (Uart1_Rx() != Uart_Sync_Head2) return;

    isp_cmd_t->UART.Cmd = Uart1_Rx();
    Data_add += isp_cmd_t->UART.Cmd;
    isp_cmd_t->UART.Len = Uart1_Rx();
    Data_add += isp_cmd_t->UART.Len;

    /* ERASE and VERIFY carry a 4-byte address placeholder after Len. */
    if (isp_cmd_t->UART.Cmd == CMD_IAP_ERASE ||
        isp_cmd_t->UART.Cmd == CMD_IAP_VERIFY)
    {
        isp_cmd_t->other.buf[2] = Uart1_Rx();
        Data_add += isp_cmd_t->other.buf[2];
        isp_cmd_t->other.buf[3] = Uart1_Rx();
        Data_add += isp_cmd_t->other.buf[3];
        isp_cmd_t->other.buf[4] = Uart1_Rx();
        Data_add += isp_cmd_t->other.buf[4];
        isp_cmd_t->other.buf[5] = Uart1_Rx();
        Data_add += isp_cmd_t->other.buf[5];
    }

    /* Payload: PROM/VERIFY = 64 B, READ = 4 B (addr, u32 LE). */
    if (isp_cmd_t->UART.Len > 0)
    {
        for (i = 0; i < isp_cmd_t->UART.Len; i++)
        {
            isp_cmd_t->UART.data[i] = Uart1_Rx();
            Data_add += isp_cmd_t->UART.data[i];
        }
    }

    if (Uart1_Rx() != (uint8_t)(Data_add & 0xFF)) return;
    if (Uart1_Rx() != (uint8_t)(Data_add >> 8))   return;
    if (Uart1_Rx() != Uart_Sync_Head2)             return;
    if (Uart1_Rx() != Uart_Sync_Head1)             return;

    s = RecData_Deal();

    if (s == ERR_Read_Ok)
    {
        /* READ response: AA 55 84 00 <Data[64]> CKL CKH 55 AA
         * CK = (0x84 + 0x00 + sum(Data)) mod 0x10000, little-endian. */
        u16 ck = CMD_IAP_READ;
        for (i = 0; i < FLASH_PAGE_SIZE; i++)
        {
            ck += isp_cmd_t->UART.data[i];
        }
        UART1_SendData(Uart_Sync_Head1);
        UART1_SendData(Uart_Sync_Head2);
        UART1_SendData(CMD_IAP_READ);
        UART1_SendData(0x00);
        UART1_SendMultiyData((u8 *)&isp_cmd_t->UART.data[0], FLASH_PAGE_SIZE);
        UART1_SendData((u8)(ck & 0xFF));
        UART1_SendData((u8)(ck >> 8));
        UART1_SendData(Uart_Sync_Head2);
        UART1_SendData(Uart_Sync_Head1);
        return;
    }

    /* Base response: AA 55 00 ST 55 AA (byte[2] is a constant 0x00, ST is
     * the status code 0x00..0x03). END replies ACK too, then resets. */
    UART1_SendData(Uart_Sync_Head1);
    UART1_SendData(Uart_Sync_Head2);
    UART1_SendData(0x00);
    UART1_SendData(s);
    UART1_SendData(Uart_Sync_Head2);
    UART1_SendData(Uart_Sync_Head1);
}