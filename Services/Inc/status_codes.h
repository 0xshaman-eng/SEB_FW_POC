/**
 * @file status_codes.h
 *
 * @brief Provides the shared operation status type for the SEB modules.
 *
 * This header defines the generic pass/fail result returned by module
 * interfaces. The 16-bit error code categories belong to the Error Manager
 * (architecture section 3.8) and are added by that module, not here.
 */

#ifndef STATUS_CODES_H
#define STATUS_CODES_H

#include <stdint.h>

/**
 * @brief 16-bit error code. Categories are defined by the Error Manager
 *        (architecture section 3.8.5).
 */
typedef uint16_t SEB_ErrorCode_t;

/**
 * @brief Defines the generic result of a module operation.
 */
typedef enum
{
    STATUS_OK = 0,
    STATUS_ERROR
} Status_t;

#endif /* STATUS_CODES_H */
