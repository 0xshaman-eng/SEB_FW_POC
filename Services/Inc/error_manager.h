/**
 * @file error_manager.h
 *
 * @brief Provides the interface for the Error Manager.
 *
 * The Error Manager records errors reported by the firmware modules,
 * classifies them by severity and logs them, as defined in architecture
 * section 3.8. Error codes and severities are in status_codes.h.
 *
 * ErrorManager_Report() must be called from the main loop only. An
 * interrupt sets a flag and its module reports in its own process function.
 */

#ifndef ERROR_MANAGER_H
#define ERROR_MANAGER_H

#include "status_codes.h"

/**
 * @brief Initializes the Error Manager.
 *
 * Clears the last error and the report counters, so that no error is
 * recorded (section 3.8.4). Must be called before the other services.
 *
 * @return STATUS_OK always.
 */
Status_t        ErrorManager_Init(void);

/**
 * @brief Records, classifies and logs an error.
 *
 * The error becomes the last error unless a CRITICAL error is already
 * recorded: a CRITICAL error is kept until ErrorManager_Init() so that it
 * is not hidden by later, lesser errors. To avoid flooding the log, a code
 * is logged on its 1st, 2nd, 4th, 8th ... report. SEB_ERROR_NONE is
 * ignored.
 *
 * @param error
 *        Code from status_codes.h.
 */
void            ErrorManager_Report(SEB_ErrorCode_t error);

/**
 * @brief Returns the last recorded error.
 *
 * @return The last error, or SEB_ERROR_NONE when none has been reported.
 */
SEB_ErrorCode_t ErrorManager_GetLastError(void);

/**
 * @brief Returns the severity of an error code.
 *
 * @param error
 *        Code from status_codes.h.
 *
 * @return The severity in the error table; ERROR_SEVERITY_ERROR for a code
 *         that is not in the table; ERROR_SEVERITY_INFO for SEB_ERROR_NONE.
 */
ErrorSeverity_t ErrorManager_GetSeverity(SEB_ErrorCode_t error);

/**
 * @brief Performs the periodic processing of the Error Manager.
 *
 * Reports are fully handled inside ErrorManager_Report(), so there is no
 * deferred work yet. The call is kept in the main loop (section 2.5) for
 * the CRITICAL reset action that the Application Manager will add.
 */
void            ErrorManager_Process(void);

#endif /* ERROR_MANAGER_H */
