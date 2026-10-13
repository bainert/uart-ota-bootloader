#ifndef __INT_BOOTLOADER_H__
#define __INT_BOOTLOADER_H__

/*
 * ============================================================================
 *  接口层（底层能力层）
 * ----------------------------------------------------------------------------
 *  这一层只回答"**怎么干**"，不决定"**什么时候干**"：
 *      - 串口接收缓冲 + 中断搬运          -> int_bootloader_rx_take()
 *      - A 区 Flash 擦除 / 半字写入        -> int_bootloader_flash_begin() 系列
 *      - A 区固件合法性校验 + 干净跳转      -> int_bootloader_app_is_valid() / jump
 *
 *  "什么时候擦、什么时候跳、用户发来的指令是什么意思" 属于应用层，
 *  由 MDK-ARM/app/app_boot.c 负责编排。
 *
 *  Flash 分区（STM32F103C8T6，64KB）：
 *      B 区 Bootloader : 0x08000000 ~ 0x08003FFF   16KB
 *      A 区 Application: 0x08004000 ~ 0x0800FFFF   48KB
 * ============================================================================
 */

#include "usart.h"
#include <stdint.h>
#include <string.h>

/* ------------------------------ 内存布局 ------------------------------ */
#define FLASH_BASE_ADDR     0x08000000U     /* B 区起始（Bootloader） */
#define APP_BASE_ADDR       0x08004000U     /* A 区起始（Application） */
#define APP_END_ADDR        0x0800FFFFU     /* A 区结束 */
#define APP_MAX_SIZE        (48U * 1024U)   /* A 区容量 = 49152 字节 */

#define FLASH_PAGE_SIZE     0x400U          /* 1KB 一页 */
#define APP_PAGE_NUM        48U             /* A 区共 48 页 */

#define SRAM_START_ADDR     0x20000000U     /* SRAM 起始 */
#define SRAM_END_ADDR       0x20004FFFU     /* SRAM 结束（20KB） */

/* ---------------------------- 接收缓冲区 ---------------------------- */
#define UART_RX_BUF_SIZE    512U

/* 串口接收缓冲（中断写入、应用层通过 int_bootloader_rx_take() 取走） */
extern uint8_t uart_rx_buf[UART_RX_BUF_SIZE];

/* --------------------------- 串口接收相关 --------------------------- */
void     int_bootloader_init(void);         /* 清标志 + 开启中断接收（不擦 A 区） */
uint16_t int_bootloader_rx_take(uint8_t *dst, uint16_t max);
                                            /* 取走一帧数据，返回帧长（0 = 无新帧） */
uint32_t int_bootloader_last_rx_tick(void); /* 最后一次收到数据的时刻（ms） */
uint32_t int_bootloader_rx_total(void);     /* 累计接收字节数 */

/* --------------------------- 烧录会话相关 --------------------------- */
/*
 *  用法（由应用层编排）：
 *      int_bootloader_flash_begin(len);   // 擦除 A 区 + 进入"边收边写"模式
 *      ... 等串口把数据发完，期间中断自动写入 Flash ...
 *      int_bootloader_flash_end();        // 收尾（补半字）+ 锁定 Flash
 *      int_bootloader_flash_received();   // 实际写入字节数，用于和 len 比对
 */
void     int_bootloader_flash_begin(uint32_t total_len);
void     int_bootloader_flash_end(void);
uint32_t int_bootloader_flash_received(void);
uint8_t  int_bootloader_flash_overflow(void);   /* 收到数据多于声明长度 */
uint8_t  int_bootloader_flash_error(void);      /* Flash 编程出错 */

/* ------------------------------ A 区状态 ------------------------------ */
uint8_t  int_bootloader_app_is_valid(void);     /* A 区是否有可运行的固件 */
uint32_t int_bootloader_app_scan_size(void);    /* A 区固件占用字节数（估算） */

/* ------------------------------- 跳转 ------------------------------- */
void     int_bootloader_jump_to_app(void);      /* 校验并跳转（校验失败则返回） */

#endif /* __INT_BOOTLOADER_H__ */
