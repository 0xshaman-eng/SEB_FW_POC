/**
 * @file status_codes.h
 *
 * @brief Provides the shared operation status type for the SEB modules.
 *
 * This header defines the generic pass/fail result returned by module
 * interfaces, and the error codes and severities shared by every module
 * (architecture section 3.8.3).
 */

#ifndef STATUS_CODES_H
#define STATUS_CODES_H

#include <stdint.h>

/**
 * @brief 16-bit error code, 0xCCNN: CC is the category and NN the error
 *        within it (architecture section 3.8.5).
 */
typedef uint16_t SEB_ErrorCode_t;

/**
 * @brief Defines how serious an error is (architecture section 3.8.5).
 */
typedef enum
{
    ERROR_SEVERITY_INFO = 0,    /**< Informational; no action required.   */
    ERROR_SEVERITY_WARNING,     /**< Abnormal; operation continues.        */
    ERROR_SEVERITY_ERROR,       /**< Operation failed; module recovers.    */
    ERROR_SEVERITY_CRITICAL     /**< System failure; system recovery.      */
} ErrorSeverity_t;

/*
 * Error codes. Each module adds the codes it raises here, and their
 * severity to the table in error_manager.c.
 *
 * Categories: 0x01xx System (software and startup faults), 0x02xx UART,
 * 0x03xx Packet, 0x04xx Link, 0x05xx GPIO, 0x06xx Configuration,
 * 0x07xx Watchdog/Reset (reset-related events).
 */

/** No error. Never reported; returned by ErrorManager_GetLastError(). */
#define SEB_ERROR_NONE                      ((SEB_ErrorCode_t)0x0000U)

/** Logger_Init() failed. Logging is not critical (section 3.9.4). */
#define SEB_ERROR_SYSTEM_LOGGER_INIT        ((SEB_ErrorCode_t)0x0101U)

/** TimerManager_Init() failed. Nothing can time out without it. */
#define SEB_ERROR_SYSTEM_TIMER_INIT         ((SEB_ErrorCode_t)0x0102U)

/** TimerManager_Start() found no free timer. */
#define SEB_ERROR_SYSTEM_TIMER_POOL_EMPTY   ((SEB_ErrorCode_t)0x0103U)

/** PacketManager_Packetize() found the TX queue full; the data is lost. */
#define SEB_ERROR_PACKET_QUEUE_FULL         ((SEB_ErrorCode_t)0x0301U)

/** Received frame too short, too long, or its size does not match LENGTH. */
#define SEB_ERROR_PACKET_LENGTH             ((SEB_ErrorCode_t)0x0302U)

/** Received frame CRC does not match. */
#define SEB_ERROR_PACKET_CRC                ((SEB_ErrorCode_t)0x0303U)

/** Received frame carries an unknown function code. */
#define SEB_ERROR_PACKET_FUNCTION           ((SEB_ErrorCode_t)0x0304U)

/**
 * @brief Defines the generic result of a module operation.
 */
typedef enum
{
    STATUS_OK = 0,
    STATUS_ERROR
} Status_t;

#endif /* STATUS_CODES_H */
