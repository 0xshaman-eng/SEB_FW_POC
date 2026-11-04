/**
 * @file logger.h
 *
 * @brief Provides the interface for the Logger.
 *
 * The Logger records firmware events, warnings, errors and diagnostic
 * information and sends them through the debug UART, as defined in
 * architecture section 3.9. The minimum level is the compile-time
 * constant SEB_CFG_LOGGER_MIN_LEVEL. There is no runtime setter.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>

#include "status_codes.h"

/**
 * Compiles the Logger in (1) or out (0).
 *
 * When this is 0, the public functions are not declared, the LOG_* macros
 * expand to ((void)0), and logger.c contributes no code or RAM.
 */
#define SEB_CFG_LOGGER_ENABLED          1U

/**
 * @brief Defines the five logging severities, lowest to highest.
 *
 * Callers pass one of these values to Logger_Log() and
 * Logger_LogFormatted(). A line is queued only when its level is at least
 * SEB_CFG_LOGGER_MIN_LEVEL.
 */
typedef enum
{
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_CRITICAL
} LogLevel_t;

/**
 * Compile-time minimum level. Lines below this value are discarded.
 *
 * There is no Logger_SetLevel(). Filter tests rebuild with a different
 * value here.
 */
#define SEB_CFG_LOGGER_MIN_LEVEL        LOG_LEVEL_DEBUG

/** Bytes in the printf-style format buffer, including the terminating NUL. */
#define SEB_CFG_LOGGER_FORMAT_SIZE      128U

/** Bytes in the interrupt-driven transmit ring. */
#define SEB_CFG_LOGGER_TX_SIZE          256U

#if (SEB_CFG_LOGGER_ENABLED == 1)

/**
 * @brief Initializes the Logger.
 *
 * Takes the CubeMX-initialized USART3 handle, clears the format buffer
 * and transmit ring, registers a private transmit-complete callback and
 * makes logging available. Does not call HAL_UART_Init() and does not
 * change SEB_CFG_LOGGER_MIN_LEVEL. Calling this function again re-clears
 * the buffers and re-registers the callback.
 *
 * A failure leaves the Logger unused so that the rest of the firmware
 * can continue (architecture section 3.9.4).
 *
 * @return STATUS_OK when the UART handle is ready and logging is enabled;
 *         STATUS_ERROR when the UART handle is not ready.
 */
Status_t Logger_Init(void);

/**
 * @brief Queues a literal log line at the given level.
 *
 * The line is discarded when the Logger is not ready, message is NULL, or
 * level is below SEB_CFG_LOGGER_MIN_LEVEL. A line that does not fit in
 * the ring is dropped as a whole. This function does not start UART I/O.
 *
 * @param level
 *        Severity of the line.
 * @param message
 *        NUL-terminated payload; must not be NULL.
 */
void     Logger_Log(LogLevel_t level, const char *message);

/**
 * @brief Logs a raw 16-bit error code at ERROR.
 *
 * The payload is formatted as ERR:0xNNNN. This is a stub until the Error
 * Manager defines SEB_ErrorCode_t (architecture section 3.9.3).
 *
 * @param error
 *        Raw 16-bit code.
 */
void     Logger_LogError(SEB_ErrorCode_t error);

/**
 * @brief Queues a printf-style log line at the given level.
 *
 * Formats into the 128-byte buffer with the heap-free Logger_Vsnprintf
 * helper (not libc vsnprintf). A NULL format is ignored. An over-long
 * result is truncated and still ends in CRLF. Standard printf is not
 * retargeted.
 *
 * Supported conversions: %% %c %s %d %i %u %x %X. Optional flag `0`
 * (zero-pad), optional decimal width (space-pad when `0` is omitted,
 * e.g. %8u; zero-pad e.g. %04X), optional length modifier `l`
 * (e.g. %lu). An unsupported specifier stops conversion: `%` and the
 * rest of the format are copied as literal text and no further
 * arguments are consumed. Float, %p, ll, and h are not supported.
 *
 * @param level
 *        Severity of the line.
 * @param format
 *        Format string as above; must not be NULL.
 */
void     Logger_LogFormatted(LogLevel_t level, const char *format, ...);

/**
 * @brief Transmits queued bytes on USART3 without blocking.
 *
 * Starts at most one interrupt-driven transfer per call. Returns
 * immediately when the Logger is not ready. Never uses DMA.
 */
void     Logger_Process(void);

#endif /* SEB_CFG_LOGGER_ENABLED == 1 */

/* MISRA deviation: Directive 4.9 - function-like macros so logging
 * compiles out when SEB_CFG_LOGGER_ENABLED is 0. Diagnostic only. */
#if (SEB_CFG_LOGGER_ENABLED == 1)
#define LOG_DEBUG(...)    Logger_LogFormatted(LOG_LEVEL_DEBUG,    __VA_ARGS__)
#define LOG_INFO(...)     Logger_LogFormatted(LOG_LEVEL_INFO,     __VA_ARGS__)
#define LOG_WARNING(...)  Logger_LogFormatted(LOG_LEVEL_WARNING,  __VA_ARGS__)
#define LOG_ERROR(...)    Logger_LogFormatted(LOG_LEVEL_ERROR,    __VA_ARGS__)
#define LOG_CRITICAL(...) Logger_LogFormatted(LOG_LEVEL_CRITICAL, __VA_ARGS__)
#else
#define LOG_DEBUG(...)    ((void)0U)
#define LOG_INFO(...)     ((void)0U)
#define LOG_WARNING(...)  ((void)0U)
#define LOG_ERROR(...)    ((void)0U)
#define LOG_CRITICAL(...) ((void)0U)
#endif

#endif /* LOGGER_H */
