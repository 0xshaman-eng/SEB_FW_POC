/**
 * @file timer_manager.h
 *
 * @brief Provides the interface for the Timer Manager.
 *
 * The Timer Manager maintains a fixed pool of software timers used by the
 * SEB modules for communication timeouts, retry timing, periodic intervals
 * and elapsed time measurement, as defined in architecture section 3.10.
 */

#ifndef TIMER_MANAGER_H
#define TIMER_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "status_codes.h"

/**
 * Number of software timers the Timer Manager owns.
 *
 * Ten timers can be active at once in the Main-board worst case derived from
 * the section 3.10.2 consumer table, plus two spare. The full derivation is
 * in the Timer Manager feature plan (specs/001-timer-manager/plan.md in the
 * original SEB_FW project). The section 3.10.5 timing parameters are
 * deliberately not defined here: section 3.10.5 marks every value TBD until
 * integration, and each belongs to its consuming module. Callers pass the
 * interval to TimerManager_Start().
 */
#define SEB_CFG_TIMER_MAX_COUNT         12U

/**
 * Reference to a software timer, issued by TimerManager_Start().
 *
 * A handle is only valid until the timer is stopped. TimerManager_Stop()
 * returns the timer to the pool, from where it may be issued again to a
 * different module, so the caller must set its own copy of the handle to
 * SEB_TIMER_HANDLE_INVALID at that point. A handle kept past the stop
 * refers to whatever now occupies that slot.
 */
typedef uint8_t TimerHandle_t;

/** Returned by TimerManager_Start() when no timer could be allocated. */
#define SEB_TIMER_HANDLE_INVALID        ((TimerHandle_t)0xFFU)

/**
 * @brief Initializes the Timer Manager.
 *
 * Clears every software timer to the idle state and makes timer services
 * available to the other modules. Uses the STM32 HAL millisecond tick that
 * HAL_Init() has already started; the system tick is not reconfigured.
 * Calling this function again re-clears the pool.
 *
 * @return STATUS_OK when the Timer Manager is ready for use.
 */
Status_t      TimerManager_Init(void);

/**
 * @brief Starts a software timer.
 *
 * Claims a free timer and begins timing the requested interval. A new timer
 * is always allocated; this function does not restart an existing one. To
 * run a further interval, stop the previous handle first.
 *
 * @param timeout
 *        Interval in milliseconds.
 *
 * @return A handle identifying the started timer, or
 *         SEB_TIMER_HANDLE_INVALID when no timer is free or the Timer
 *         Manager has not been initialized.
 */
TimerHandle_t TimerManager_Start(uint32_t timeout);

/**
 * @brief Stops a software timer and releases it.
 *
 * Cancels a running timer, and is also how an expired timer is released.
 * The timer returns to the idle state and becomes available for reuse. An
 * unknown handle is ignored.
 *
 * The caller must discard the handle once this function returns, by setting
 * its own copy to SEB_TIMER_HANDLE_INVALID. The slot may be reissued to
 * another module by the next TimerManager_Start().
 *
 * @param timer
 *        Handle returned by TimerManager_Start().
 */
void          TimerManager_Stop(TimerHandle_t timer);

/**
 * @brief Reports whether a software timer has expired.
 *
 * This is a query only: it never changes the state of the timer, so it is
 * safe to call on every pass of the main loop. An expired timer keeps
 * reporting expiry until TimerManager_Stop() releases it.
 *
 * @param timer
 *        Handle returned by TimerManager_Start().
 *
 * @return true when the timer has expired; false when it is still running,
 *         is idle, or the handle is unknown.
 */
bool          TimerManager_IsExpired(TimerHandle_t timer);

/**
 * @brief Reports the time elapsed since a software timer was started.
 *
 * @param timer
 *        Handle returned by TimerManager_Start().
 *
 * The value keeps rising once the timer has expired, because it reports the
 * age of the timer rather than the time remaining. It is read from the tick
 * at the moment of the call, so it does not depend on
 * TimerManager_Process() having run during this loop iteration.
 *
 * @return Milliseconds elapsed since the timer was started, correct across
 *         the rollover of the millisecond tick; 0 when the timer is idle or
 *         the handle is unknown. This matches TimerManager_IsExpired(),
 *         which reports false in the same cases.
 */
uint32_t      TimerManager_GetElapsed(TimerHandle_t timer);

/**
 * @brief Performs the periodic processing of the Timer Manager.
 *
 * Reads the millisecond tick once and moves every timer that has reached
 * its interval to the expired state. Taking a single reading per call is
 * what lets all modules evaluate timing against the same instant within one
 * main loop iteration (architecture section 2.5). This function never
 * blocks.
 */
void          TimerManager_Process(void);

#endif /* TIMER_MANAGER_H */
