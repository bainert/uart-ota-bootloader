#include "int_bootloader.h"

//接收数据变量定义
uint8_t uart_rx_buf[UART_RX_BUF_SIZE] = {0};
uint16_t uart_rx_buf_size_sum = 0;
uint16_t uart_rx_buf_size = 0;

//A区起始地址
uint32_t flash_addr_A_begin = 0x08004000;
//A区结束地址(flash末尾)
uint32_t flash_addr_A_end = 0x0800FFFF;
#define SRAM_START_ADDR  0x20000000     // SRAM 起始（查你芯片手册）
#define SRAM_END_ADDR    0x20004FFF     // SRAM 末尾：必须按你芯片 SRAM 大小改！

//地址偏移量
uint16_t flash_addr_offset = 0;
//需擦除页首地址
uint32_t page_addr = 0;

//数据遗留标志位
uint8_t data_remain_flag = 0;
//遗留的数据
uint8_t remain_data = 0;


//flash擦除函数
static void flash_erase()
{
	    //flash擦除标志位
        uint8_t flash_erase_flag = 0;
        //待擦除页首地址
        uint32_t page_addr = 0;

        //判断当前地址内容是否为0xff，如果不是，则擦除当前地址页
        for(uint16_t i = 0; i < uart_rx_buf_size; i++)
        {
            //读取当前地址内容
            uint8_t data = *(volatile uint8_t*)(flash_addr_A_begin + flash_addr_offset + i);
            if(data != 0xff)
            {
                //擦除标志位拉高
                flash_erase_flag = 1;
                //计算当前页首地址
                page_addr = (flash_addr_A_begin + flash_addr_offset + i) - (flash_addr_A_begin + flash_addr_offset + i) % 0x0400;
                break;
            }
        }

        //执行擦除
        if(flash_erase_flag)
        {
            //擦除当前页
            FLASH_EraseInitTypeDef EraseInitStruct;
            uint32_t PageError = 0;   // ← 必须传变量地址，不能传 0/NULL
            EraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
            EraseInitStruct.Banks = FLASH_BANK_1;
            EraseInitStruct.PageAddress = page_addr;
            EraseInitStruct.NbPages = 1;

            HAL_FLASHEx_Erase(&EraseInitStruct, &PageError);
        }

}

//flash写入函数
static void flash_write()
{
    uint16_t data = 0;
	if((uart_rx_buf_size % 2) != 0) //接收数据长度为奇数
    {
        if(data_remain_flag == 0) //数据遗留标志位为0
        {
            //写入flash 遗留最后一位
            for(uint16_t i = 0; i < uart_rx_buf_size; i+=2)
            {
                if(i < uart_rx_buf_size - 1)
                {
                     data = uart_rx_buf[i] | (uart_rx_buf[i+1] << 8);
                    HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flash_addr_A_begin + flash_addr_offset + i, data);
                }
                else //最后一位遗留
                {
                    remain_data = uart_rx_buf[i];
                    data_remain_flag = 1;
                }
            }
            //地址偏移量增加
            flash_addr_offset += uart_rx_buf_size - 1;
            //数据遗留标志位拉高
            data_remain_flag = 1;
        }
        else //数据遗留标志位为1
        {
            data_remain_flag = 0; //数据遗留标志位拉低
            for(uint16_t i = 0; i < uart_rx_buf_size; i+=2)
            {
                if(i == 0) //拼接上次遗留的字节
                {
                     data = remain_data | (uart_rx_buf[i] << 8);
                }
                else if(i < uart_rx_buf_size)
                {
                     data = uart_rx_buf[i-1] | (uart_rx_buf[i] << 8);
                }
                HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flash_addr_A_begin + flash_addr_offset + i, data);
            }
            //地址偏移量增加
            flash_addr_offset += uart_rx_buf_size + 1;
        }
    }

    else //接收数据长度为偶数
    {
       if(data_remain_flag == 1) //数据遗留标志位为1
        {
            //写入flash（拼接上次遗留字节 + 当前偶数部分）
            for(uint16_t i = 0; i < uart_rx_buf_size; i+=2)
            {
                if(i == 0) //拼接上次遗留的字节
                {
                     data = remain_data | (uart_rx_buf[i] << 8);
                }
                else
                {
                    data = uart_rx_buf[i-1] | (uart_rx_buf[i] << 8);
                }
                HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flash_addr_A_begin + flash_addr_offset + i, data);
            }
            // 偶数长度 + 历史遗留1字节 = 奇数总字节，把真正的最后一字节留待下次拼接
            remain_data = uart_rx_buf[uart_rx_buf_size - 1];
            data_remain_flag = 1;
            //地址偏移量增加
            flash_addr_offset += uart_rx_buf_size;
        }
        
        else //数据遗留标志位为0
        {
            //直接写入flash
            for (uint16_t i = 0; i < uart_rx_buf_size; i+=2)
            {
                data = uart_rx_buf[i] | (uart_rx_buf[i+1] << 8);
                HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flash_addr_A_begin + flash_addr_offset + i, data);     
            }
            
            //地址偏移量增加
            flash_addr_offset += uart_rx_buf_size;
        }
    }
}

// 串口中断接收回调函数
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if(huart->Instance == USART1)
    {
        //保存实际接收数据的总长度
        uart_rx_buf_size = Size;
        uart_rx_buf_size_sum += Size;

        //todo: 写入程序到flash
        //解锁flash
        HAL_FLASH_Unlock();

        //擦除flash
        //flash_erase();

        //写入flash
        flash_write();

        //锁定flash
        HAL_FLASH_Lock();
        //清空接收缓冲区
        memset(uart_rx_buf, 0, UART_RX_BUF_SIZE);
        HAL_UARTEx_ReceiveToIdle_IT(&huart1, (uint8_t *)uart_rx_buf, UART_RX_BUF_SIZE);
    }
}

// 初始化中断引导程序
void int_bootloader_init(void)
{
	//清除串口标志
	__HAL_UART_CLEAR_OREFLAG(&huart1);
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);

    //擦除a区flash
    HAL_FLASH_Unlock();
    //擦除48页
    FLASH_EraseInitTypeDef EraseInitStruct;
    uint32_t PageError = 0;   // ← 必须传变量地址，不能传 0/NULL
    EraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
    EraseInitStruct.Banks = FLASH_BANK_1;
    EraseInitStruct.PageAddress = flash_addr_A_begin;
    EraseInitStruct.NbPages = 48;

    HAL_FLASHEx_Erase(&EraseInitStruct, &PageError);

    HAL_FLASH_Lock();

    //开启串口中断接收
    HAL_UARTEx_ReceiveToIdle_IT(&huart1, (uint8_t *)uart_rx_buf, UART_RX_BUF_SIZE);
}

//A区跳转函数
void int_bootloader_jump_to_app(void)
{
    //定义函数指针类型
    typedef void (*pFunction)(void);

    //获取应用程序的复位向量地址
    uint32_t app_reset_vector = *(volatile uint32_t *)(flash_addr_A_begin + 4);
    //获取应用程序的栈顶指针地址
    uint32_t app_stack_pointer = *(volatile uint32_t *)(flash_addr_A_begin);

    //校验应用程序的复位向量和栈顶指针是否有效
    if (app_reset_vector < flash_addr_A_begin || app_reset_vector > flash_addr_A_end)
    {
        //应用程序复位向量无效，返回
        printf("Invalid app reset vector: 0x%08X\n", app_reset_vector);
        return;
    }
    if (app_stack_pointer < SRAM_START_ADDR || app_stack_pointer > SRAM_END_ADDR)
    {
        // 栈顶指针不在 RAM 内，无效
        printf("Invalid app stack pointer: 0x%08X\n", app_stack_pointer);
        return;  
    }
    printf("校验成功");

    //关闭中断
    __disable_irq();
    //注销hal库
    HAL_DeInit();
    //设置主堆栈指针
    __set_MSP(app_stack_pointer);
    //重定向中断向量表到应用程序的起始地址
    SCB->VTOR = flash_addr_A_begin;

    //跳转到应用程序的复位向量地址
    pFunction app_dump = (pFunction)app_reset_vector;
    app_dump();
}
