/**
 * @file local_uart_manager.h
 *
 * @brief Provides the interface for the Local UART Manager.
 *
 * The Local UART Manager passes raw bytes between the local device (the SBC
 * on the Main board, the RockBLOCK 9603 on the Sub board) and the Packet
 * Manager queues, as defined in architecture section 3.2. It never
 * interprets the bytes, and no Modbus framing appears on the local wire.
 *
 * All functions must be called from the main loop only.
 */

#ifndef LOCAL_UART_MANAGER_H
#define LOCAL_UART_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "status_codes.h"

/** Bytes in the circular DMA receive ring. */
#define SEB_CFG_LOCAL_UART_RX_SIZE              256U

/** Response timeout in ms; covers a RockBLOCK satellite session. */
#define SEB_CFG_LOCAL_UART_RESPONSE_TIMEOUT_MS  60000U

/** Recovery attempts without a successful transaction before fatal. */
#define SEB_CFG_LOCAL_UART_MAX_RECOVERY         3U

/**
 * @brief Defines the Local UART Manager states (architecture section 3.2).
 */
typedef enum
{
    LOCAL_UART_STATE_INIT = 0,  /**< Configuring the UART and reception.  */
    LOCAL_UART_STATE_READY,     /**< Idle, waiting for data to move.      */
    LOCAL_UART_STATE_TRANSMIT,  /**< Sending to, or waiting to send to, the local device. */
    LOCAL_UART_STATE_RECEIVE,   /**< Receiving from the local device.     */
    LOCAL_UART_STATE_ERROR,     /**< An error is being handled.           */
    LOCAL_UART_STATE_RECOVERY   /**< Recovering from the stored error.    */
} LocalUartState_t;

/**
 * @brief Initializes the Local UART Manager.
 *
 * Stores the board mode and address, resets all module state, registers the
 * UART callbacks on SEB_BOARD_LOCAL_UART and starts reception. Must be
 * called after PacketManager_Init() and TimerManager_Init().
 *
 * @param mode
 *        Board operating mode.
 * @param address
 *        SEB address placed in the frames this module queues.
 *
 * @return STATUS_OK and state READY; STATUS_ERROR and state ERROR when a
 *         HAL call failed (recovery then retries the initialization).
 */
Status_t         LocalUARTManager_Init(BoardMode_t mode, uint8_t address);

/**
 * @brief Runs the Local UART Manager.
 *
 * Forwards newly received bytes to the Link TX queue, then performs one
 * step of the state machine. Never blocks. Called once per main-loop pass,
 * after TimerManager_Process() (section 2.5).
 */
void             LocalUARTManager_Run(void);

/**
 * @brief Returns the current state.
 *
 * @return The current LocalUartState_t value.
 */
LocalUartState_t LocalUARTManager_GetState(void);

/**
 * @brief Tells whether the recovery attempts are exhausted.
 *
 * The module then stays in LOCAL_UART_STATE_ERROR until
 * LocalUARTManager_Init() is called again (section 3.2 ERROR).
 *
 * @return true when a fatal error is present.
 */
bool             LocalUARTManager_HasFatalError(void);

#endif /* LOCAL_UART_MANAGER_H */
