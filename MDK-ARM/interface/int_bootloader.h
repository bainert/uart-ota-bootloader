#ifndef __INT_BOOTLOADER_H__
#define __INT_BOOTLOADER_H__

/* 
    B区起始地址: 0x08000000 大小16K
    A区起始地址: 0x08004000 大小48K
*/
#include "usart.h"
#include "string.h"
#include "stdio.h"

//接收数据缓冲区大小
#define UART_RX_BUF_SIZE 512

extern uint16_t uart_rx_buf_size_sum;

void int_bootloader_init(void);
void int_bootloader_jump_to_app(void);

#endif
