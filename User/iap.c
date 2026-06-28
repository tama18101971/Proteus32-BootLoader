/********************************** (C) COPYRIGHT  *******************************
 * File Name          : iap.c
 * Author             : WCH
 * Version            : V1.0.1
 * Date               : 2025/01/09
 * Description        : IAP
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
u8  Verify_Star_flag = 0;
u8  Fast_Program_Buf[128];
u16 CodeLen          = 0;
u8  End_Flag         = 0;
u8  EP2_Rx_Buffer[USBD_DATA_SIZE + 4];
#define isp_cmd_t ((isp_cmd *)EP2_Rx_Buffer)

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
 * @return  ERR_ERROR - ERROR
 *          ERR_SUCCESS - SUCCESS
 *          ERR_End - End
 */
u8 RecData_Deal(void)
{
    u8  i, s, Lenth;

    Lenth = isp_cmd_t->UART.Len;

    switch (isp_cmd_t->UART.Cmd)
    {
        case CMD_IAP_ERASE:
            FLASH_Unlock_Fast();
            FLASH_EraseAllPages();
            s = ERR_SUCCESS;
            break;

        case CMD_IAP_PROM:
            for (i = 0; i < Lenth; i++)
            {
                Fast_Program_Buf[CodeLen + i] = isp_cmd_t->UART.data[i];
            }
            CodeLen += Lenth;
            if (CodeLen >= 64)
            {
                CH32_IAP_Program(Program_addr, (u32 *)Fast_Program_Buf);
                CodeLen -= 64;
                for (i = 0; i < CodeLen; i++)
                {
                    Fast_Program_Buf[i] = Fast_Program_Buf[64 + i];
                }
                Program_addr += 0x40;
            }
            s = ERR_SUCCESS;
            break;

        case CMD_IAP_VERIFY:
            if (Verify_Star_flag == 0)
            {
                Verify_Star_flag = 1;
                for (i = 0; i < (64 - CodeLen); i++)
                {
                    Fast_Program_Buf[CodeLen + i] = 0xFF;
                }
                CH32_IAP_Program(Program_addr, (u32 *)Fast_Program_Buf);
                CodeLen = 0;
            }
            s = ERR_SUCCESS;
            for (i = 0; i < Lenth; i++)
            {
                if (isp_cmd_t->UART.data[i] != *(u8 *)(Verify_addr + i))
                {
                    s = ERR_ERROR;
                    break;
                }
            }
            Verify_addr += Lenth;
            break;

        case CMD_IAP_END:
            Verify_Star_flag = 0;
            End_Flag          = 1;
            Program_addr      = FLASH_Base;
            Verify_addr       = FLASH_Base;
            s = ERR_End;

            /* Erase last page (0x08003FFC & 0xFFFFFFC0 = 0x08003FC0) */
            FLASH_ErasePage_Fast(0x08003FC0);
            FLASH_ProgramWord(CalAddr, CheckNum);
            /* Set bits 0x8000 and 0x80 in control register */
            FLASH->CTLR |= (uint32_t)0x00008080;
            break;

        case CMD_JUMP_IAP:
            s = ERR_SUCCESS;
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

    if ((isp_cmd_t->other.buf[0] == CMD_IAP_PROM) ||
        (isp_cmd_t->other.buf[0] == CMD_IAP_VERIFY))
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

    if (s != ERR_End)
    {
        UART1_SendData(Uart_Sync_Head1);
        UART1_SendData(Uart_Sync_Head2);
        UART1_SendData(0x00);
        UART1_SendData(s == ERR_ERROR ? 0x01 : 0x00);
        UART1_SendData(Uart_Sync_Head2);
        UART1_SendData(Uart_Sync_Head1);
    }
}
