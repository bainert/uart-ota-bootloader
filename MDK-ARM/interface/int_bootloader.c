#include "int_bootloader.h"
#include <stdio.h>

/*
 * ============================================================================
 *  接口层（底层能力层）实现
 * ----------------------------------------------------------------------------
 *  本文件只提供"原语"，不含任何业务判断（不知道 START 指令，也不知道窗口期）：
 *
 *      [串口中断] --收到一帧--> 烧录会话已开启 ? --是--> 直接写 Flash
 *                                              --否--> 置标志等应用层取走
 *
 *  ⚠️ 该设计成立的前提（应用层需保证）：串口助手的"包间隔"远大于写一帧的耗时。
 *     写 256 字节 = 128 个半字 × 52.5µs ≈ 6.7ms，所以包间隔 500ms 有约 74 倍余量。
 *     若改成不带延时地连续发送，USART 只有 2 字节硬件缓冲，会丢数据。
 * ============================================================================
 */

/* ============================== 串口接收 ============================== */

uint8_t         uart_rx_buf[UART_RX_BUF_SIZE];  /* 接收缓冲 */

static volatile uint16_t rx_frame_size  = 0U;   /* 当前帧长度 */
static volatile uint8_t  rx_frame_ready = 0U;   /* 有一帧数据等待应用层取走 */
static volatile uint32_t rx_total       = 0U;   /* 累计接收字节数 */
static volatile uint32_t last_rx_tick   = 0U;   /* 最后一次收到数据的时刻 */

/* ============================== 烧录会话 ============================== */

static volatile uint8_t  session_active  = 0U;  /* 1 = 边收边写模式 */
static uint32_t          session_total   = 0U;  /* 本次声明的固件总长度 */
static uint32_t          session_received = 0U; /* 已写入 Flash 的字节数 */
static uint32_t          write_offset    = 0U;  /* 相对 A 区起始的写指针 */
static uint8_t           half_carry      = 0U;  /* 半字低位暂存字节 */
static uint8_t           half_has_carry  = 0U;  /* 是否暂存了一个字节 */
static volatile uint8_t  overflow_flag   = 0U;  /* 收到的数据多于声明长度 */
static volatile uint8_t  prog_err_flag   = 0U;  /* Flash 编程出错 */

/* ============================== 内部函数声明 ============================== */

static void flash_write_stream(const uint8_t *data, uint16_t len);
static void flash_flush_carry(void);

/* ============================== Flash 写入 ============================== */

/**
 * @brief  把一段字节流按半字(16bit)写入 A 区
 * @param  data 数据首地址
 * @param  len  字节数
 * @note   STM32 的 Flash 最小编程单位是 16 位，所以按"两字节拼一个半字"来写。
 *         若这一段长度是奇数，最后一个字节会暂存到 half_carry，等下一段的
 *         首字节到达后拼成完整半字（这就是"跨帧奇数字节"的正确解法）。
 *         本函数在串口中断中被调用，耗时 ≈ len/2 × 52.5µs。
 */
static void flash_write_stream(const uint8_t *data, uint16_t len)
{
    uint16_t i;
    uint16_t halfword;

    for (i = 0U; i < len; i++)
    {
        if (half_has_carry == 0U)
        {
            half_carry     = data[i];
            half_has_carry = 1U;
        }
        else
        {
            halfword = (uint16_t)half_carry | ((uint16_t)data[i] << 8);

            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                  APP_BASE_ADDR + write_offset,
                                  halfword) != HAL_OK)
            {
                prog_err_flag = 1U;
                return;
            }

            write_offset  += 2U;
            half_has_carry = 0U;
        }
    }
}

/**
 * @brief  收尾：若还暂存着一个字节（固件总长为奇数），补 0xFF 写成最后一个半字
 */
static void flash_flush_carry(void)
{
    uint16_t halfword;

    if (half_has_carry != 0U)
    {
        halfword = (uint16_t)half_carry | 0xFF00U;

        (void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                APP_BASE_ADDR + write_offset,
                                halfword);

        write_offset  += 2U;
        half_has_carry = 0U;
    }
}

/* ============================== 烧录会话 ============================== */

/**
 * @brief  开启烧录会话：擦除 A 区并进入"边收边写"模式
 * @param  total_len 本次计划写入的固件总字节数（应用层从 START 指令解析得到）
 * @note   本函数会阻塞 1~2 秒（48 页 × 20~40ms）。这段期间 CPU 从 Flash 取指
 *         会被 Flash 控制器拖住，串口数据会丢——所以应用层必须在它返回之后，
 *         回显 "READY" 再让用户发送固件。
 */
void int_bootloader_flash_begin(uint32_t total_len)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error = 0U;

    /* 先复位会话状态并保持"未开流"，避免擦除期间到达的数据被误写 */
    session_active   = 0U;
    session_total    = total_len;
    session_received = 0U;
    write_offset     = 0U;
    half_carry       = 0U;
    half_has_carry   = 0U;
    overflow_flag    = 0U;
    prog_err_flag    = 0U;

    /* 擦除整个 A 区（48 页） */
    erase_init.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase_init.Banks       = FLASH_BANK_1;
    erase_init.PageAddress = APP_BASE_ADDR;
    erase_init.NbPages     = APP_PAGE_NUM;

    HAL_FLASH_Unlock();
    (void)HAL_FLASHEx_Erase(&erase_init, &page_error);
    /* 擦除后保持解锁：接收阶段要在中断里直接写 Flash */

    /* 擦除完成后再开流，并从此刻开始计时 */
    last_rx_tick   = HAL_GetTick();
    session_active = 1U;
}

/**
 * @brief  结束烧录会话：补最后一个半字 + 锁定 Flash
 */
void int_bootloader_flash_end(void)
{
    session_active = 0U;

    flash_flush_carry();
    HAL_FLASH_Lock();
}

/**
 * @brief  已写入 Flash 的字节数
 */
uint32_t int_bootloader_flash_received(void)
{
    return session_received;
}

/**
 * @brief  是否出现了"收到的数据多于声明长度"
 */
uint8_t int_bootloader_flash_overflow(void)
{
    return overflow_flag;
}

/**
 * @brief  是否出现 Flash 编程错误
 */
uint8_t int_bootloader_flash_error(void)
{
    return prog_err_flag;
}

/* ============================== A 区状态 ============================== */

/**
 * @brief  判断 A 区是否存有"看起来可以运行"的固件
 * @note   只做最小校验：中断向量表前两个字 = 栈顶指针(MSP) + 复位向量。
 *         这不是内容校验，但足够拦住"空片（全 0xFF）"和"写了一半"的情况。
 */
uint8_t int_bootloader_app_is_valid(void)
{
    uint32_t stack_pointer = *(volatile uint32_t *)(APP_BASE_ADDR);
    uint32_t reset_vector  = *(volatile uint32_t *)(APP_BASE_ADDR + 4U);

    /* 复位向量必须落在 A 区范围内 */
    if ((reset_vector < APP_BASE_ADDR) || (reset_vector > APP_END_ADDR))
    {
        return 0U;
    }
    /* Cortex-M 的复位向量是 Thumb 地址，最低位必须为 1 */
    if ((reset_vector & 1U) == 0U)
    {
        return 0U;
    }
    /* 栈顶指针必须落在 SRAM 范围内，且 4 字节对齐 */
    if ((stack_pointer < SRAM_START_ADDR) || (stack_pointer > SRAM_END_ADDR))
    {
        return 0U;
    }
    if ((stack_pointer & 0x3U) != 0U)
    {
        return 0U;
    }

    return 1U;
}

/**
 * @brief  估算 A 区里固件占用的字节数
 * @note   本方案不在 Flash 里存元数据，所以靠"从后往前找第一个非 0xFF 字节"
 *         来推断。因为每次升级都全擦 A 区，所以这个值是准确的。
 * @retval 固件字节数（向上对齐到偶数），0 表示 A 区是空的
 */
uint32_t int_bootloader_app_scan_size(void)
{
    uint32_t i;

    for (i = APP_MAX_SIZE; i > 0U; i--)
    {
        if (*(volatile uint8_t *)(APP_BASE_ADDR + i - 1U) != 0xFFU)
        {
            return (i + 1U) & ~1U;  /* 半字写入，向上对齐到偶数 */
        }
    }

    return 0U;
}

/* ============================== 串口接收 ============================== */

/**
 * @brief  串口接收事件回调（空闲线 IDLE / 缓冲区满 TC 都会进来）
 * @param  huart 串口句柄
 * @param  Size  本次事件收到的字节数
 * @note   中断里只做两件事：搬（写 Flash）或 放旗子（交应用层）。
 *         每次都重新武装接收，所以 Size 就是"这一帧"的长度。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    uint16_t n;
    uint32_t remaining;

    if (huart->Instance != USART1)
    {
        return;
    }

    rx_frame_size = Size;
    rx_total     += Size;
    last_rx_tick  = HAL_GetTick();

    if (session_active != 0U)
    {
        /* ---- 烧录会话中：这一帧就是固件数据，直接写 Flash ---- */
        if (session_received < session_total)
        {
            remaining = session_total - session_received;
            n = Size;

            /* 只写声明长度以内的部分；越过声明长度说明用户把长度敲小了 */
            if ((uint32_t)n > remaining)
            {
                overflow_flag = 1U;
                n = (uint16_t)remaining;
            }

            flash_write_stream(uart_rx_buf, n);
            session_received += n;
        }
    }
    else
    {
        /* ---- 非烧录期：放旗子，等应用层把这一帧当"命令"取走 ---- */
        rx_frame_ready = 1U;
    }

    /* 重新武装接收，准备下一帧。
       ⚠️ 这里绝不能清零 uart_rx_buf：数据是直接收在该缓冲区里的，
       应用层稍后才通过 int_bootloader_rx_take() 把它取走；
       若在中断里先清掉，应用层只会拿到一串 0x00（指令解析直接失效）。 */
    (void)HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rx_buf, UART_RX_BUF_SIZE);
}

/**
 * @brief  取走一帧收到的数据（应用层用它来读"命令帧"）
 * @param  dst 目标缓冲
 * @param  max 目标缓冲最大可写字节数
 * @retval 实际取走的字节数，0 表示当前没有新帧
 */
uint16_t int_bootloader_rx_take(uint8_t *dst, uint16_t max)
{
    uint16_t len;

    if ((rx_frame_ready == 0U) || (dst == NULL) || (max == 0U))
    {
        return 0U;
    }

    len = rx_frame_size;
    if (len > max)
    {
        len = max;
    }

    /* 极短临界区：防止 memcpy 期间中断把新帧写进同一个缓冲 */
    __disable_irq();
    memcpy(dst, uart_rx_buf, len);
    rx_frame_ready = 0U;
    __enable_irq();

    return len;
}

/**
 * @brief  最后一次收到数据的时刻（ms），应用层用它做"断流超时"判断
 */
uint32_t int_bootloader_last_rx_tick(void)
{
    return last_rx_tick;
}

/**
 * @brief  累计接收字节数，应用层用于 INFO 回显
 */
uint32_t int_bootloader_rx_total(void)
{
    return rx_total;
}

/**
 * @brief  初始化：清标志 + 开启中断接收
 * @note   这里**不再擦除 A 区**——擦除时机由应用层在收到 START 指令后决定
 *         （否则每次上电 A 区的固件都会消失）。
 */
void int_bootloader_init(void)
{
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);

    rx_frame_ready = 0U;
    rx_frame_size  = 0U;
    last_rx_tick   = HAL_GetTick();

    (void)HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rx_buf, UART_RX_BUF_SIZE);
}

/* ============================== 跳转 ============================== */

/**
 * @brief  校验并跳转到 A 区应用程序
 * @note   校验不通过时直接返回（程序停留在 Bootloader），不会"跳飞"。
 *         校验通过则不会返回：关闭中断 → 注销 HAL → 重设 MSP → 重定向 VTOR
 *         → 调用 APP 复位向量，完成"无痕迹"交接。
 */
void int_bootloader_jump_to_app(void)
{
    typedef void (*pFunction)(void);

    uint32_t  stack_pointer = *(volatile uint32_t *)(APP_BASE_ADDR);
    uint32_t  reset_vector  = *(volatile uint32_t *)(APP_BASE_ADDR + 4U);
    pFunction app_entry;

    if (int_bootloader_app_is_valid() == 0U)
    {
        printf("[ERR] APP invalid: SP=0x%08X RV=0x%08X\r\n",
               (unsigned int)stack_pointer, (unsigned int)reset_vector);
        return;
    }

    printf("[OTA] JUMP to 0x%08X (SP=0x%08X)\r\n",
           (unsigned int)reset_vector, (unsigned int)stack_pointer);

    __disable_irq();                    /* 1. 关中断，交接期间不响应任何中断 */
    HAL_DeInit();                       /* 2. 注销 HAL，把外设恢复成复位状态 */
    __set_MSP(stack_pointer);           /* 3. 重设主堆栈指针为 APP 的栈顶 */
    SCB->VTOR = APP_BASE_ADDR;          /* 4. 重定向中断向量表到 A 区 */

    app_entry = (pFunction)reset_vector;
    app_entry();                        /* 5. 跳转，永不返回 */
}
