/**
 * @file logger.c
 *
 * @brief Implements the Logger.
 *
 * This file contains the format buffer, transmit ring, drop counter and
 * the INIT / READY / LOGGING lifecycle, as defined in architecture
 * section 3.9.
 */

#include "logger.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The only hardware header an SEB module includes (constitution IV.C). It
 * provides the HAL of the target family and SEB_BOARD_LOG_UART. */
#include "board_config.h"

#if (SEB_CFG_LOGGER_ENABLED == 1)

/*
 * Logger state machine (architecture section 3.9.6)
 * -------------------------------------------------
 * The Logger has three states and no fourth public state. Init failure
 * stays in INIT with is_ready false; log calls are discarded (FR-003).
 *
 *   INIT    --> READY     Logger_Init() succeeds (UART handle ready)
 *   READY   --> LOGGING   A line is queued, or Process() starts a transfer
 *   LOGGING --> READY     Process() sees an empty ring and !tx_busy
 *
 * Formatting and ring writes run in the main loop. The registered
 * TX-complete callback only clears tx_busy. The next Process() call
 * retires the finished run and may start the next one.
 *
 * The debug UART is SEB_BOARD_LOG_UART from board_config.h. It is owned and
 * initialized by CubeMX; the Logger does not call HAL_UART_Init().
 */

/**
 * @brief Defines the Logger lifecycle states.
 *
 * The states are internal to the Logger. No interface in section 3.9.3
 * exposes them.
 */
typedef enum
{
    LOGGER_STATE_INIT = 0,
    LOGGER_STATE_READY,
    LOGGER_STATE_LOGGING
} LoggerState_t;

/** Printf-style work buffer. A line is built here before the ring copy. */
static char log_format_buf[SEB_CFG_LOGGER_FORMAT_SIZE];

/** Byte ring waiting for HAL_UART_Transmit_IT(). */
static uint8_t log_tx_buf[SEB_CFG_LOGGER_TX_SIZE];

/** Next byte to send. */
static uint16_t tx_head;

/** Next free write index. */
static uint16_t tx_tail;

/** Bytes currently in the ring, including any in-flight transfer. */
static uint16_t tx_count;

/** Bytes started with HAL_UART_Transmit_IT and not yet retired. */
static uint16_t tx_run_len;

/** Set while an interrupt-driven transfer is in flight. Written from the ISR. */
static volatile bool tx_busy;

/** Set once Logger_Init() has succeeded. */
static bool is_ready;

/** Section 3.9.6 state. */
static LoggerState_t state;

/** Incremented once per rejected whole line (overflow only). */
static volatile uint32_t log_drop_count;

/**
 * @brief Clears tx_busy when the debug UART finishes a transfer.
 *
 * Registered on SEB_BOARD_LOG_UART from Logger_Init(). Does no formatting
 * and does not write the ring.
 *
 * @param huart
 *        Handle that completed; unused because the callback is registered
 *        on the debug UART only.
 */
static void Logger_TxCpltCallback(UART_HandleTypeDef *huart)
{
    (void)huart;
    tx_busy = false;
}

/**
 * @brief Returns the wire-format name of a log level.
 *
 * @param level
 *        Severity to name.
 *
 * @return The level token, or NULL when level is not one of the five.
 */
static const char *Logger_LevelName(LogLevel_t level)
{
    const char *name;

    switch (level)
    {
        case LOG_LEVEL_DEBUG:
            name = "DEBUG";
            break;
        case LOG_LEVEL_INFO:
            name = "INFO";
            break;
        case LOG_LEVEL_WARNING:
            name = "WARNING";
            break;
        case LOG_LEVEL_ERROR:
            name = "ERROR";
            break;
        case LOG_LEVEL_CRITICAL:
            name = "CRITICAL";
            break;
        default:
            name = NULL;
            break;
    }

    return name;
}

/**
 * @brief Writes one character into a bounded buffer, counting every attempt.
 *
 * Characters that would land on or past the last byte are counted but not
 * stored, so the return length matches vsnprintf.
 */
static void Logger_OutChar(char *dst, size_t size, size_t *written, char ch)
{
    if ((*written + 1U) < size)
    {
        dst[*written] = ch;
    }
    (*written)++;
}

/**
 * @brief Writes an unsigned value in the given base, with optional zero-pad.
 */
static void Logger_OutUnsigned(char *dst,
                               size_t size,
                               size_t *written,
                               unsigned long value,
                               unsigned int base,
                               bool upper,
                               unsigned int width,
                               bool zero_pad)
{
    char digits[11];
    unsigned int count = 0U;
    const char *lut = (upper == true) ? "0123456789ABCDEF" : "0123456789abcdef";

    do
    {
        digits[count] = lut[value % (unsigned long)base];
        count++;
        value /= (unsigned long)base;
    } while (value != 0UL);

    while (width > count)
    {
        Logger_OutChar(dst, size, written, (zero_pad == true) ? '0' : ' ');
        width--;
    }

    while (count > 0U)
    {
        count--;
        Logger_OutChar(dst, size, written, digits[count]);
    }
}

/**
 * @brief Heap-free vsnprintf for the Logger integer/string conversions.
 *
 * libc vsnprintf on this nano.specs toolchain pulls findfp / malloc / _sbrk,
 * which FR-011 forbids. This helper implements the same bounded contract for
 * %% %c %s %d %i %u %x %X, optional l, width, and 0-pad. An unsupported
 * conversion stops the formatter: '%' and the rest of the format are
 * copied as literal text and no further arguments are consumed.
 */
static int Logger_Vsnprintf(char *dst, size_t size, const char *fmt, va_list args)
{
    size_t written = 0U;

    if (fmt == NULL)
    {
        if (size > 0U)
        {
            dst[0] = '\0';
        }
        return 0;
    }

    while (*fmt != '\0')
    {
        bool zero_pad;
        unsigned int width;
        bool is_long;

        if (*fmt != '%')
        {
            Logger_OutChar(dst, size, &written, *fmt);
            fmt++;
            continue;
        }

        fmt++;
        if (*fmt == '%')
        {
            Logger_OutChar(dst, size, &written, '%');
            fmt++;
            continue;
        }

        zero_pad = false;
        width = 0U;
        if (*fmt == '0')
        {
            zero_pad = true;
            fmt++;
        }

        while ((*fmt >= '0') && (*fmt <= '9'))
        {
            width = (width * 10U) + (unsigned int)(*fmt - '0');
            fmt++;
        }

        is_long = false;
        if (*fmt == 'l')
        {
            is_long = true;
            fmt++;
        }

        switch (*fmt)
        {
            case 's':
            {
                const char *s = va_arg(args, const char *);
                if (s == NULL)
                {
                    s = "";
                }
                while (*s != '\0')
                {
                    Logger_OutChar(dst, size, &written, *s);
                    s++;
                }
                break;
            }
            case 'c':
                Logger_OutChar(dst, size, &written, (char)va_arg(args, int));
                break;
            case 'd':
            case 'i':
            {
                long value = (is_long == true) ? va_arg(args, long) : (long)va_arg(args, int);
                unsigned long magnitude;

                if (value < 0L)
                {
                    Logger_OutChar(dst, size, &written, '-');
                    /* Convert to unsigned before negate so INT32_MIN
                     * does not overflow signed long. */
                    magnitude = 0UL - (unsigned long)value;
                }
                else
                {
                    magnitude = (unsigned long)value;
                }
                Logger_OutUnsigned(dst, size, &written, magnitude,
                                   10U, false, width, zero_pad);
                break;
            }
            case 'u':
            {
                unsigned long value = (is_long == true) ?
                    va_arg(args, unsigned long) :
                    (unsigned long)va_arg(args, unsigned int);
                Logger_OutUnsigned(dst, size, &written, value,
                                   10U, false, width, zero_pad);
                break;
            }
            case 'x':
            case 'X':
            {
                unsigned long value = (is_long == true) ?
                    va_arg(args, unsigned long) :
                    (unsigned long)va_arg(args, unsigned int);
                Logger_OutUnsigned(dst, size, &written, value,
                                   16U, (*fmt == 'X'), width, zero_pad);
                break;
            }
            default:
                /* Stop converting. Emit '%' and the remainder as
                 * literals so a later specifier cannot va_arg-mismatch. */
                Logger_OutChar(dst, size, &written, '%');
                while (*fmt != '\0')
                {
                    Logger_OutChar(dst, size, &written, *fmt);
                    fmt++;
                }
                break;
        }

        if (*fmt != '\0')
        {
            fmt++;
        }
    }

    if (size > 0U)
    {
        size_t term = (written < size) ? written : (size - 1U);
        dst[term] = '\0';
    }

    return (int)written;
}

/**
 * @brief Heap-free snprintf used to build wire lines and the error stub.
 */
static int Logger_Snprintf(char *dst, size_t size, const char *fmt, ...)
{
    va_list args;
    int written;

    /* MISRA deviation: Rule 17.1 - variable arguments for printf-style
     * logging. Diagnostic only. */
    va_start(args, fmt);
    written = Logger_Vsnprintf(dst, size, fmt, args);
    va_end(args);
    return written;
}

/**
 * @brief Copies a whole line into the transmit ring, or drops it.
 *
 * The queued bytes are left untouched when the line does not fit. NULL
 * is ignored. A successful enqueue moves READY to LOGGING.
 *
 * @param line
 *        NUL-terminated wire line, including CRLF.
 */
static void Logger_Enqueue(const char *line)
{
    uint16_t line_len = 0U;
    uint16_t i;
    bool accepted = false;

    if (line != NULL)
    {
        line_len = (uint16_t)strlen(line);
        if (line_len > 0U)
        {
            if (((uint32_t)tx_count + (uint32_t)line_len) <= SEB_CFG_LOGGER_TX_SIZE)
            {
                accepted = true;
            }
            else
            {
                log_drop_count++;
            }
        }
    }

    if (accepted == true)
    {
        for (i = 0U; i < line_len; i++)
        {
            log_tx_buf[tx_tail] = (uint8_t)line[i];
            tx_tail++;
            /* Unsigned wrap at the ring size (MISRA 12.4 / defined unsigned
             * overflow): tail returns to 0, never indexes past TX_SIZE. */
            if (tx_tail >= SEB_CFG_LOGGER_TX_SIZE)
            {
                tx_tail = 0U;
            }
        }
        tx_count = (uint16_t)(tx_count + line_len);
        state = LOGGER_STATE_LOGGING;
    }
}

Status_t Logger_Init(void)
{
    Status_t status = STATUS_ERROR;
    const HAL_UART_StateTypeDef uart_state = HAL_UART_GetState(SEB_BOARD_LOG_UART);

    if ((uart_state != HAL_UART_STATE_RESET) &&
        (uart_state != HAL_UART_STATE_ERROR))
    {
        (void)memset(log_format_buf, 0, sizeof(log_format_buf));
        (void)memset(log_tx_buf, 0, sizeof(log_tx_buf));
        tx_head = 0U;
        tx_tail = 0U;
        tx_count = 0U;
        tx_run_len = 0U;
        tx_busy = false;
        log_drop_count = 0U;

        if (HAL_UART_RegisterCallback(SEB_BOARD_LOG_UART,
                                      HAL_UART_TX_COMPLETE_CB_ID,
                                      Logger_TxCpltCallback) == HAL_OK)
        {
            is_ready = true;
            state = LOGGER_STATE_READY;
            status = STATUS_OK;
        }
        else
        {
            is_ready = false;
            state = LOGGER_STATE_INIT;
        }
    }
    else
    {
        is_ready = false;
        state = LOGGER_STATE_INIT;
    }

    return status;
}

void Logger_Log(LogLevel_t level, const char *message)
{
    const char *level_name;
    int written;

    if ((is_ready == true) &&
        (message != NULL) &&
        (level >= SEB_CFG_LOGGER_MIN_LEVEL))
    {
        level_name = Logger_LevelName(level);
        if (level_name != NULL)
        {
            written = Logger_Snprintf(log_format_buf,
                                      SEB_CFG_LOGGER_FORMAT_SIZE,
                                      "[%lu] [%s] %s\r\n",
                                      (unsigned long)HAL_GetTick(),
                                      level_name,
                                      message);

            if ((written < 0) ||
                ((uint32_t)written >= SEB_CFG_LOGGER_FORMAT_SIZE))
            {
                log_format_buf[SEB_CFG_LOGGER_FORMAT_SIZE - 3U] = '\r';
                log_format_buf[SEB_CFG_LOGGER_FORMAT_SIZE - 2U] = '\n';
                log_format_buf[SEB_CFG_LOGGER_FORMAT_SIZE - 1U] = '\0';
            }

            Logger_Enqueue(log_format_buf);
        }
    }
}

void Logger_LogError(SEB_ErrorCode_t error)
{
    char payload[12];

    (void)Logger_Snprintf(payload, sizeof(payload), "ERR:0x%04X", (unsigned int)error);
    Logger_Log(LOG_LEVEL_ERROR, payload);
}

void Logger_LogFormatted(LogLevel_t level, const char *format, ...)
{
    const char *level_name;
    int prefix_len;
    int payload_len;
    uint16_t payload_room;
    uint16_t end;
    va_list args;

    if ((is_ready == true) &&
        (format != NULL) &&
        (level >= SEB_CFG_LOGGER_MIN_LEVEL))
    {
        level_name = Logger_LevelName(level);
        if (level_name != NULL)
        {
            prefix_len = Logger_Snprintf(log_format_buf,
                                         SEB_CFG_LOGGER_FORMAT_SIZE,
                                         "[%lu] [%s] ",
                                         (unsigned long)HAL_GetTick(),
                                         level_name);

            if ((prefix_len < 0) ||
                ((uint32_t)prefix_len >= (SEB_CFG_LOGGER_FORMAT_SIZE - 3U)))
            {
                log_format_buf[SEB_CFG_LOGGER_FORMAT_SIZE - 3U] = '\r';
                log_format_buf[SEB_CFG_LOGGER_FORMAT_SIZE - 2U] = '\n';
                log_format_buf[SEB_CFG_LOGGER_FORMAT_SIZE - 1U] = '\0';
                Logger_Enqueue(log_format_buf);
            }
            else
            {
                /* Reserve two bytes after the payload so CRLF always fits. */
                payload_room = (uint16_t)(SEB_CFG_LOGGER_FORMAT_SIZE -
                                          (uint16_t)prefix_len - 2U);

                /* MISRA deviation: Rule 17.1 - variable arguments for
                 * printf-style logging. Diagnostic only. libc
                 * vsnprintf is not used: this nano.specs toolchain
                 * pulls malloc / _sbrk through findfp (FR-011). */
                va_start(args, format);
                payload_len = Logger_Vsnprintf(&log_format_buf[prefix_len],
                                               (size_t)payload_room,
                                               format,
                                               args);
                va_end(args);

                if ((payload_len < 0) ||
                    ((uint32_t)payload_len >= (uint32_t)payload_room))
                {
                    end = (uint16_t)((uint16_t)prefix_len + payload_room - 1U);
                }
                else
                {
                    end = (uint16_t)((uint16_t)prefix_len + (uint16_t)payload_len);
                }

                log_format_buf[end] = '\r';
                log_format_buf[end + 1U] = '\n';
                log_format_buf[end + 2U] = '\0';

                Logger_Enqueue(log_format_buf);
            }
        }
    }
}

void Logger_Process(void)
{
    const bool busy = tx_busy;
    uint16_t run;

    if (is_ready == true)
    {
        /* Sample tx_busy once. If TC arrives between checks, a second read
         * would start another Transmit_IT of the same un-retired bytes. */
        if ((busy == false) && (tx_run_len > 0U))
        {
            tx_head = (uint16_t)(tx_head + tx_run_len);
            /* Unsigned wrap at the ring size (MISRA 12.4 / defined unsigned
             * overflow): head returns to 0, never indexes past TX_SIZE. */
            if (tx_head >= SEB_CFG_LOGGER_TX_SIZE)
            {
                tx_head = (uint16_t)(tx_head - SEB_CFG_LOGGER_TX_SIZE);
            }
            tx_count = (uint16_t)(tx_count - tx_run_len);
            tx_run_len = 0U;
        }

        if ((tx_count == 0U) && (tx_run_len == 0U) && (busy == false))
        {
            tx_head = 0U;
            tx_tail = 0U;
            state = LOGGER_STATE_READY;
        }
        else if ((tx_count > 0U) && (tx_run_len == 0U) && (busy == false))
        {
            run = (uint16_t)(SEB_CFG_LOGGER_TX_SIZE - tx_head);
            if (run > tx_count)
            {
                run = tx_count;
            }

            if (HAL_UART_Transmit_IT(SEB_BOARD_LOG_UART, &log_tx_buf[tx_head], run) == HAL_OK)
            {
                tx_busy = true;
                tx_run_len = run;
                state = LOGGER_STATE_LOGGING;
            }
        }
        else
        {
            /* Transfer still in flight, or a run is awaiting the next
             * Process() call after a wrap. */
        }
    }
}

#endif /* SEB_CFG_LOGGER_ENABLED == 1 */
