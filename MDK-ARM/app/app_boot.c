#include "app_boot.h"
#include "int_bootloader.h"
#include <stdio.h>
#include <string.h>

/*
 * ============================================================================
 *  应用层实现（与用户交互）
 * ----------------------------------------------------------------------------
 *  状态转移图：
 *
 *     上电 ──┬─ A 区无合法 APP ──> ST_WAIT（无限等待指令）
 *            └─ A 区有合法 APP ──> ST_WAIT（等 3 秒；期间有输入则取消倒计时）
 *                                     │
 *                                     ├─ 超时 ────────> 跳转 APP
 *                                     └─ START:<len> ─> ST_ERASE
 *
 *     ST_ERASE ── 擦完 A 区 + 回显 READY ──> ST_RECV
 *     ST_RECV  ── 收够 <len> 字节 ─────────> ST_CHECK
 *              ── 断流超时且没收够 ────────> 报错回 ST_WAIT
 *     ST_CHECK ── 长度/向量校验通过 ───────> 跳转 APP
 *              ── 任一校验失败 ────────────> 报错回 ST_WAIT
 * ============================================================================
 */

/* ============================== 类型 ============================== */

typedef enum
{
    ST_WAIT = 0,    /* 等待指令 / 启动窗口期 */
    ST_ERASE,       /* 擦除 A 区 */
    ST_RECV,        /* 接收固件（接口层在中断里写 Flash） */
    ST_CHECK        /* 接收完毕，校验并决定是否跳转 */
} app_state_t;

typedef enum
{
    CMD_UNKNOWN = 0,
    CMD_START,
    CMD_INFO
} app_cmd_t;

/* ============================== 变量 ============================== */

static volatile app_state_t state = ST_WAIT;

/* 指令行缓冲（可能由多帧拼成一行） */
static char     cmd_line[APP_BOOT_CMD_LINE_MAX];
static uint8_t  cmd_len = 0U;

/* 升级过程 */
static uint32_t target_len  = 0U;   /* 用户声明的固件长度 */
static uint32_t next_dot_at = 0U;   /* 下一个进度点的字节数 */

/* 启动窗口期 */
static uint8_t  wait_window_active = 0U;
static uint32_t wait_deadline      = 0U;

/* ============================== 内部函数声明 ============================== */

static void           cmd_normalize(char *line);
static app_cmd_t      parse_command(char *line, uint32_t *len_out);
static void           handle_command(void);
static void           print_info(void);
static void           reset_to_wait(void);

/* ============================== 指令解析 ============================== */

/**
 * @brief  规整化一行指令：去首尾空白和 \r\n，并全部转成大写
 * @note   串口助手勾"发送新行"会带上 \r\n，大小写也可能随手敲，这里统一容错。
 */
static void cmd_normalize(char *line)
{
    uint16_t len;
    uint16_t i;
    uint16_t head;
    char     c;

    len = (uint16_t)strlen(line);

    /* 去尾部空白 / CR / LF */
    while (len > 0U)
    {
        c = line[len - 1U];
        if ((c == '\r') || (c == '\n') || (c == ' ') || (c == '\t'))
        {
            len--;
        }
        else
        {
            break;
        }
    }
    line[len] = '\0';

    /* 去头部空白 */
    head = 0U;
    while (head < len)
    {
        c = line[head];
        if ((c == ' ') || (c == '\t'))
        {
            head++;
        }
        else
        {
            break;
        }
    }
    if (head > 0U)
    {
        memmove(line, line + head, (size_t)(len - head + 1U));
        len -= head;
    }

    /* 全部转大写，便于比较 */
    for (i = 0U; i < len; i++)
    {
        if ((line[i] >= 'a') && (line[i] <= 'z'))
        {
            line[i] = (char)(line[i] - 'a' + 'A');
        }
    }
}

/**
 * @brief  解析一行指令
 * @param  line    指令字符串（会被就地规整化，调用方请传副本）
 * @param  len_out START 指令解析出的长度
 * @retval 指令类型
 * @note   宽容策略：
 *           - 关键字大小写无关（已转大写）
 *           - START 与数字之间的分隔符 : , = 空格 都接受
 *           - 数字后不允许再有多余内容
 */
static app_cmd_t parse_command(char *line, uint32_t *len_out)
{
    char    *p;
    uint32_t value  = 0U;
    uint8_t  digits = 0U;

    cmd_normalize(line);

    if (line[0] == '\0')
    {
        return CMD_UNKNOWN;
    }

    if (strcmp(line, "INFO") == 0)
    {
        return CMD_INFO;
    }

    if (strncmp(line, "START", 5U) != 0)
    {
        return CMD_UNKNOWN;
    }

    /* 跳过关键字后面任意分隔符 */
    p = line + 5;
    while ((*p == ' ') || (*p == '\t') || (*p == ':') || (*p == ',') || (*p == '='))
    {
        p++;
    }

    /* 十进制数字 */
    while ((*p >= '0') && (*p <= '9'))
    {
        value = value * 10U + (uint32_t)(*p - '0');
        digits++;
        if (digits > 6U)    /* A 区最多 49152，6 位足够；再长必然非法 */
        {
            break;
        }
        p++;
    }

    if (digits == 0U)
    {
        return CMD_UNKNOWN;
    }

    /* 数字后面只允许有空白 */
    while ((*p == ' ') || (*p == '\t'))
    {
        p++;
    }
    if (*p != '\0')
    {
        return CMD_UNKNOWN;
    }

    *len_out = value;
    return CMD_START;
}

/* ============================== 回显 ============================== */

/**
 * @brief  回显 A 区状态（INFO 指令）
 */
static void print_info(void)
{
    uint8_t  valid;
    uint32_t size;

    valid = int_bootloader_app_is_valid();
    size  = int_bootloader_app_scan_size();

    printf("\r\n--- APP INFO ---\r\n");
    printf("APP valid  : %s\r\n", (valid != 0U) ? "YES" : "NO");
    printf("APP size   : %u bytes\r\n", (unsigned int)size);
    printf("A  region  : 0x%08X ~ 0x%08X (%u KB)\r\n",
           (unsigned int)APP_BASE_ADDR,
           (unsigned int)APP_END_ADDR,
           (unsigned int)(APP_MAX_SIZE / 1024U));
    printf("boot wait  : %u ms\r\n", (unsigned int)APP_BOOT_WAIT_MS);
    printf("recv total : %u bytes\r\n", (unsigned int)int_bootloader_rx_total());
    printf("---\r\n");
}

/* ============================== 指令处理 ============================== */

/**
 * @brief  处理 cmd_line 里攒到的指令
 * @note   解析成功 -> 立即执行；解析失败且已收到换行 -> 报错清缓冲；
 *         解析失败但还没收到换行 -> 认为指令还没发完，继续等下一帧。
 */
static void handle_command(void)
{
    char      tmp[APP_BOOT_CMD_LINE_MAX];
    app_cmd_t cmd;
    uint32_t  len = 0U;

    /* 用副本解析，保留 cmd_line 原样以便继续拼接 */
    memcpy(tmp, cmd_line, (size_t)(cmd_len + 1U));
    cmd = parse_command(tmp, &len);

    if (cmd == CMD_INFO)
    {
        print_info();
        cmd_len     = 0U;
        cmd_line[0] = '\0';
    }
    else if (cmd == CMD_START)
    {
        if ((len == 0U) || (len > APP_MAX_SIZE))
        {
            printf("[ERR] bad length %u, must be 1~%u\r\n",
                   (unsigned int)len, (unsigned int)APP_MAX_SIZE);
            cmd_len     = 0U;
            cmd_line[0] = '\0';
        }
        else
        {
            target_len  = len;
            cmd_len     = 0U;
            cmd_line[0] = '\0';
            printf("[OTA] target size = %u bytes\r\n", (unsigned int)target_len);
            state = ST_ERASE;
        }
    }
    else
    {
        /* 已经收到换行、或缓冲被塞满：这一行确实非法 */
        if ((cmd_line[cmd_len - 1U] == '\n') || (cmd_len >= (APP_BOOT_CMD_LINE_MAX - 1U)))
        {
            printf("[ERR] unknown command: \"%s\"\r\n", cmd_line);
            printf("[ERR] usage: START:<len_dec> | INFO\r\n");
            cmd_len     = 0U;
            cmd_line[0] = '\0';
        }
    }
}

/* ============================== 状态复位 ============================== */

/**
 * @brief  退回"等待指令"状态
 * @note   不会去修 A 区：升级失败后 A 区本来就是残缺的，
 *         下次上电 int_bootloader_app_is_valid() 会把它判为非法，不会跳进去。
 */
static void reset_to_wait(void)
{
    state              = ST_WAIT;
    cmd_len            = 0U;
    cmd_line[0]        = '\0';
    target_len         = 0U;
    next_dot_at        = 0U;
    wait_window_active = 0U;
}

/* ============================== 初始化 ============================== */

/**
 * @brief  应用层初始化：上电调用一次
 */
void app_boot_init(void)
{
    /* 底层：清标志 + 开启中断接收（不再无条件擦除 A 区） */
    int_bootloader_init();

    printf("\r\n[BOOT] STM32 UART Bootloader (A:0x%08X, %uKB)\r\n",
           (unsigned int)APP_BASE_ADDR,
           (unsigned int)(APP_MAX_SIZE / 1024U));

    if (int_bootloader_app_is_valid() != 0U)
    {
        /* A 区有固件：给它一个"想升级就赶紧说话"的窗口期 */
        wait_window_active = 1U;
        wait_deadline      = HAL_GetTick() + APP_BOOT_WAIT_MS;

        printf("[BOOT] APP found, wait %u ms (any input cancels the timer)\r\n",
               (unsigned int)APP_BOOT_WAIT_MS);
        printf("[BOOT] no input -> jump to APP\r\n");
    }
    else
    {
        /* A 区没固件：只能停在 Bootloader 等指令 */
        wait_window_active = 0U;
        printf("[BOOT] no valid APP, stay in bootloader\r\n");
    }

    printf("[BOOT] cmd: START:<len_dec>  |  INFO   (e.g. START:4164)\r\n");
    printf("[BOOT] after READY, send the .bin (keep 256B / 500ms packet delay)\r\n");
}

/* ============================== 主状态机 ============================== */

/**
 * @brief  应用层状态机，需被主循环反复调用
 */
void app_boot_poll(void)
{
    uint32_t now;
    uint16_t n;

    now = HAL_GetTick();

    switch (state)
    {
    /* ---------------------------- 等待指令 ---------------------------- */
    case ST_WAIT:

        /* 启动窗口期：收到过任何数据都说明用户在电脑前，取消自动跳转 */
        if (wait_window_active != 0U)
        {
            if (int_bootloader_rx_total() > 0U)
            {
                wait_window_active = 0U;
                printf("[BOOT] user present, auto-jump cancelled\r\n");
            }
            else if (now >= wait_deadline)
            {
                wait_window_active = 0U;
                printf("[BOOT] window timeout -> jump\r\n");
                int_bootloader_jump_to_app();
                /* 只有校验失败才会返回 */
                printf("[BOOT] jump refused (APP invalid)\r\n");
            }
        }

        /* 缓冲被塞满还没成形，丢掉重来，避免永远卡住 */
        if (cmd_len >= (APP_BOOT_CMD_LINE_MAX - 1U))
        {
            cmd_len     = 0U;
            cmd_line[0] = '\0';
        }

        /* 攒一行指令 */
        n = int_bootloader_rx_take((uint8_t *)(cmd_line + cmd_len),
                                   (uint16_t)(APP_BOOT_CMD_LINE_MAX - 1U - cmd_len));
        if (n > 0U)
        {
            cmd_len += (uint8_t)n;
            cmd_line[cmd_len] = '\0';
            handle_command();
        }
        break;

    /* ---------------------------- 擦除 A 区 ---------------------------- */
    case ST_ERASE:

        printf("[OTA] erasing %u pages @0x%08X ...\r\n",
               (unsigned int)APP_PAGE_NUM, (unsigned int)APP_BASE_ADDR);

        /* 内含：擦除 48 页（阻塞 1~2 秒）+ 开启"边收边写"模式。
           这 1~2 秒 CPU 取指被 Flash 控制器拖住、串口数据会丢，
           所以必须先擦完，再回显 READY 让用户发文件 */
        int_bootloader_flash_begin(target_len);

        next_dot_at = APP_BOOT_DOT_INTERVAL;
        state       = ST_RECV;

        printf("[OTA] READY - send the .bin now\r\n");
        break;

    /* ---------------------------- 接收固件 ---------------------------- */
    case ST_RECV:

        /* 进度点：每收满 4KB 回显一个 '.'（在主循环里发，不阻塞中断） */
        while (int_bootloader_flash_received() >= next_dot_at)
        {
            printf(".");
            next_dot_at += APP_BOOT_DOT_INTERVAL;
        }

        /* Flash 写失败：立即停手 */
        if (int_bootloader_flash_error() != 0U)
        {
            printf("\r\n[ERR] flash program failed\r\n");
            int_bootloader_flash_end();
            reset_to_wait();
            printf("[BOOT] back to wait, send START:<len> to retry\r\n");
            break;
        }

        /* 收够了 -> 进入校验 */
        if (int_bootloader_flash_received() >= target_len)
        {
            state = ST_CHECK;
            break;
        }

        /* 断流超时（只在已经收到数据之后才计时，给用户留点选文件的时间） */
        if ((int_bootloader_flash_received() > 0U) &&
            ((now - int_bootloader_last_rx_tick()) > APP_BOOT_RECV_TIMEOUT_MS))
        {
            printf("\r\n[ERR] timeout: got %u / %u bytes\r\n",
                   (unsigned int)int_bootloader_flash_received(),
                   (unsigned int)target_len);
            int_bootloader_flash_end();
            reset_to_wait();
            printf("[BOOT] back to wait, send START:<len> to retry\r\n");
        }
        break;

    /* ---------------------------- 校验与跳转 ---------------------------- */
    case ST_CHECK:

        int_bootloader_flash_end();     /* 补最后一个半字 + 锁定 Flash */

        printf("\r\n[OTA] recv %u / %u bytes\r\n",
               (unsigned int)int_bootloader_flash_received(),
               (unsigned int)target_len);

        if (int_bootloader_flash_overflow() != 0U)
        {
            printf("[ERR] more bytes than declared -> abort\r\n");
            reset_to_wait();
            printf("[BOOT] back to wait, send START:<len> to retry\r\n");
            break;
        }

        if (int_bootloader_flash_received() != target_len)
        {
            printf("[ERR] length mismatch -> abort\r\n");
            reset_to_wait();
            printf("[BOOT] back to wait, send START:<len> to retry\r\n");
            break;
        }

        printf("[OTA] length check OK\r\n");
        int_bootloader_jump_to_app();

        /* 走到这里说明跳转前校验没过 */
        printf("[ERR] APP vector check failed, stay in bootloader\r\n");
        reset_to_wait();
        printf("[BOOT] back to wait, send START:<len> to retry\r\n");
        break;

    default:
        state = ST_WAIT;
        break;
    }
}
