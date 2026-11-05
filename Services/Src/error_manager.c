/**
 * @file error_manager.c
 *
 * @brief Implements the Error Manager.
 *
 * This file contains the error table (code and severity), the last error
 * and the per-code report counters, as defined in architecture section 3.8.
 */

#include "error_manager.h"

#include <stddef.h>

#include "logger.h"

/*
 * Error Manager state machine (architecture section 3.8.7)
 * ---------------------------------------------------------
 *   IDLE --> REPORT --> CLASSIFY --> HANDLE_ERROR --> IDLE
 *
 * Reports come from the main loop only, so the whole sequence runs inside
 * one ErrorManager_Report() call and no report can be lost or queued:
 *   REPORT        record the code as the last error
 *   CLASSIFY      look up its severity in error_table
 *   HANDLE_ERROR  count it and log it
 * Module-specific recovery stays in the reporting module (section 3.8.6).
 */

/**
 * @brief Holds one known error code and its severity.
 */
typedef struct
{
    SEB_ErrorCode_t code;
    ErrorSeverity_t severity;
} ErrorEntry_t;

/** Every known code and its severity. Add one line per new code. */
static const ErrorEntry_t error_table[] =
{
    { SEB_ERROR_SYSTEM_LOGGER_INIT,      ERROR_SEVERITY_WARNING  },
    { SEB_ERROR_SYSTEM_TIMER_INIT,       ERROR_SEVERITY_CRITICAL },
    { SEB_ERROR_SYSTEM_TIMER_POOL_EMPTY, ERROR_SEVERITY_ERROR    },
    /* A UART line fault is a warning because the module recovers it;
     * exhausted recovery is critical. */
    { SEB_ERROR_LOCAL_UART_INIT,         ERROR_SEVERITY_ERROR    },
    { SEB_ERROR_LOCAL_UART_TX,           ERROR_SEVERITY_WARNING  },
    { SEB_ERROR_LOCAL_UART_RX,           ERROR_SEVERITY_WARNING  },
    { SEB_ERROR_LOCAL_UART_TIMEOUT,      ERROR_SEVERITY_WARNING  },
    { SEB_ERROR_LOCAL_UART_FATAL,        ERROR_SEVERITY_CRITICAL },
    /* Lost outgoing data is an error; a bad received frame is a warning
     * because the sender times out and retransmits it. */
    { SEB_ERROR_PACKET_QUEUE_FULL,       ERROR_SEVERITY_ERROR    },
    { SEB_ERROR_PACKET_LENGTH,           ERROR_SEVERITY_WARNING  },
    { SEB_ERROR_PACKET_CRC,              ERROR_SEVERITY_WARNING  },
    { SEB_ERROR_PACKET_FUNCTION,         ERROR_SEVERITY_WARNING  },
};

/** Number of entries in error_table. */
#define ERROR_TABLE_COUNT   (sizeof(error_table) / sizeof(error_table[0]))

/** Reports per table entry; the extra last slot counts unknown codes. */
static uint8_t report_count[ERROR_TABLE_COUNT + 1U];

/** Last recorded error. */
static SEB_ErrorCode_t last_error = SEB_ERROR_NONE;

/** Severity of last_error, used to keep a CRITICAL error. */
static ErrorSeverity_t last_severity = ERROR_SEVERITY_INFO;

/**
 * @brief Finds an error code in error_table.
 *
 * @param error
 *        Code to find.
 *
 * @return Its index, or ERROR_TABLE_COUNT when the code is unknown.
 */
static size_t ErrorManager_Find(SEB_ErrorCode_t error)
{
    size_t slot = 0U;

    while ((slot < ERROR_TABLE_COUNT) && (error_table[slot].code != error))
    {
        slot++;
    }

    return slot;
}

Status_t ErrorManager_Init(void)
{
    size_t slot;

    for (slot = 0U; slot <= ERROR_TABLE_COUNT; slot++)
    {
        report_count[slot] = 0U;
    }

    last_error    = SEB_ERROR_NONE;
    last_severity = ERROR_SEVERITY_INFO;

    return STATUS_OK;
}

void ErrorManager_Report(SEB_ErrorCode_t error)
{
    size_t          slot;
    ErrorSeverity_t severity;

    if (error != SEB_ERROR_NONE)
    {
        slot     = ErrorManager_Find(error);
        severity = ErrorManager_GetSeverity(error);

        /* A recorded CRITICAL error is kept so that the Application
         * Manager always sees the original cause (section 3.1 ERROR). */
        if (last_severity != ERROR_SEVERITY_CRITICAL)
        {
            last_error    = error;
            last_severity = severity;
        }

        /* Saturating count. Logging only on powers of two keeps an error
         * that repeats every loop from flooding the Logger. */
        if (report_count[slot] < UINT8_MAX)
        {
            report_count[slot]++;

            if ((report_count[slot] & (report_count[slot] - 1U)) == 0U)
            {
                /* Severities map one to one onto the upper log levels. */
                static const LogLevel_t log_level[] =
                {
                    LOG_LEVEL_INFO, LOG_LEVEL_WARNING,
                    LOG_LEVEL_ERROR, LOG_LEVEL_CRITICAL
                };

#if (SEB_CFG_LOGGER_ENABLED == 1)
                Logger_LogFormatted(log_level[severity],
                                    "ERR:0x%04X x%u",
                                    (unsigned int)error,
                                    (unsigned int)report_count[slot]);
#else
                (void)log_level;
#endif
            }
        }
    }
}

SEB_ErrorCode_t ErrorManager_GetLastError(void)
{
    return last_error;
}

ErrorSeverity_t ErrorManager_GetSeverity(SEB_ErrorCode_t error)
{
    ErrorSeverity_t severity = ERROR_SEVERITY_ERROR;
    const size_t    slot     = ErrorManager_Find(error);

    if (error == SEB_ERROR_NONE)
    {
        severity = ERROR_SEVERITY_INFO;
    }
    else if (slot < ERROR_TABLE_COUNT)
    {
        severity = error_table[slot].severity;
    }
    else
    {
        /* Unknown code: treated as ERROR, never ignored. */
    }

    return severity;
}

void ErrorManager_Process(void)
{
    /* No deferred work yet; see error_manager.h. */
}
