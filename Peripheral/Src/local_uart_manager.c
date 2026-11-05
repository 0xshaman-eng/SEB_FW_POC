/**
 * @file local_uart_manager.c
 *
 * @brief Implements the Local UART Manager.
 *
 * This file contains the receive ring, the HAL callbacks and the INIT /
 * READY / TRANSMIT / RECEIVE / ERROR / RECOVERY state machine defined in
 * architecture section 3.2.
 *
 * Every Run() first forwards newly received bytes to the Link TX queue, in
 * every state, so that nothing the local device sends is lost (developer
 * decision). The state machine then decides only when the next frame from
 * the Local TX queue may be sent.
 *
 * Normal cycles (section 3.2):
 *   Main: READY -> RECEIVE -> TRANSMIT -> READY   (SBC request, remote reply)
 *   Sub:  READY -> TRANSMIT -> RECEIVE -> READY   (command, RockBLOCK reply)
 * Main also goes READY -> TRANSMIT when the Local TX queue holds data that
 * arrived without a request, for example a late RockBLOCK result.
 * Errors go to ERROR, then RECOVERY, and back to INIT, TRANSMIT or READY
 * by error type; after SEB_CFG_LOCAL_UART_MAX_RECOVERY attempts without a
 * successful transaction the module stays in ERROR.
 */

#include "local_uart_manager.h"

#include "board_config.h"
#include "error_manager.h"
#include "packet_manager.h"
#include "timer_manager.h"

/** Circular DMA receive ring. */
static uint8_t           rx_ring[SEB_CFG_LOCAL_UART_RX_SIZE];

/** DMA write position in rx_ring, set by the RX event callback. */
static volatile uint16_t rx_head = 0U;

/** Next byte of rx_ring to forward. */
static uint16_t          rx_tail = 0U;

/** Set by the TX complete callback. */
static volatile bool     tx_done = false;

/** Set by the error callback (overrun, framing, noise). */
static volatile bool     rx_error = false;

/** A transmission is in progress. */
static bool              tx_busy = false;

/** Copy of the peeked frame; its payload stays valid while it is sent. */
static Packet_t          tx_packet;

/** Current state. */
static LocalUartState_t  state = LOCAL_UART_STATE_INIT;

/** Board operating mode, from LocalUARTManager_Init(). */
static BoardMode_t       board_mode = BOARD_MODE_MAIN;

/** Address placed in the frames this module queues. */
static uint8_t           own_address = 0U;

/** Response timer; SEB_TIMER_HANDLE_INVALID when no response is awaited. */
static TimerHandle_t     response_timer = SEB_TIMER_HANDLE_INVALID;

/** Error that caused the last ERROR state; selects the recovery. */
static SEB_ErrorCode_t   last_error = SEB_ERROR_NONE;

/** Recoveries since the last successful transaction. */
static uint8_t           attempts = 0U;

/** Recovery attempts exhausted. */
static bool              fatal = false;

/**
 * @brief Records the DMA write position after an idle line, half or full
 *        ring event. Interrupt context: stores the position only.
 */
static void LocalUART_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    (void)huart;
    rx_head = (uint16_t)(size % SEB_CFG_LOCAL_UART_RX_SIZE);
}

/**
 * @brief Flags the end of a transmission. Interrupt context.
 */
static void LocalUART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    (void)huart;
    tx_done = true;
}

/**
 * @brief Flags a UART error. Interrupt context.
 */
static void LocalUART_ErrorCallback(UART_HandleTypeDef *huart)
{
    (void)huart;
    rx_error = true;
}

/**
 * @brief Stops the response timer, if one is running.
 */
static void LocalUART_StopTimer(void)
{
    TimerManager_Stop(response_timer);
    response_timer = SEB_TIMER_HANDLE_INVALID;
}

/**
 * @brief Stores an error and enters the ERROR state.
 *
 * @param error
 *        Error that selects the recovery.
 */
static void LocalUART_Fail(SEB_ErrorCode_t error)
{
    last_error = error;
    LocalUART_StopTimer();
    state = LOCAL_UART_STATE_ERROR;
}

/**
 * @brief Starts the response timer.
 *
 * @return true when started; false when the timer pool is empty, in which
 *         case the module enters ERROR.
 */
static bool LocalUART_StartTimer(void)
{
    LocalUART_StopTimer();
    response_timer = TimerManager_Start(SEB_CFG_LOCAL_UART_RESPONSE_TIMEOUT_MS);

    if (response_timer == SEB_TIMER_HANDLE_INVALID)
    {
        LocalUART_Fail(SEB_ERROR_SYSTEM_TIMER_POOL_EMPTY);
    }

    return (response_timer != SEB_TIMER_HANDLE_INVALID);
}

/**
 * @brief Restarts circular DMA reception from the start of the ring.
 *
 * @return The HAL result of starting reception.
 */
static HAL_StatusTypeDef LocalUART_StartRx(void)
{
    rx_head  = 0U;
    rx_tail  = 0U;
    rx_error = false;

    return HAL_UARTEx_ReceiveToIdle_DMA(SEB_BOARD_LOCAL_UART, rx_ring,
                                        (uint16_t)SEB_CFG_LOCAL_UART_RX_SIZE);
}

/**
 * @brief Runs the INIT state functions (section 3.2 INIT).
 *
 * Stops any transfer, registers the callbacks (HAL resets them after a
 * de-initialization) and starts reception.
 */
static void LocalUART_Setup(void)
{
    HAL_StatusTypeDef hal;

    LocalUART_StopTimer();
    tx_busy = false;
    tx_done = false;
    (void)HAL_UART_Abort(SEB_BOARD_LOCAL_UART);

    hal = HAL_UART_RegisterCallback(SEB_BOARD_LOCAL_UART,
                                    HAL_UART_TX_COMPLETE_CB_ID,
                                    LocalUART_TxCpltCallback);
    if (hal == HAL_OK)
    {
        hal = HAL_UART_RegisterCallback(SEB_BOARD_LOCAL_UART,
                                        HAL_UART_ERROR_CB_ID,
                                        LocalUART_ErrorCallback);
    }
    if (hal == HAL_OK)
    {
        hal = HAL_UART_RegisterRxEventCallback(SEB_BOARD_LOCAL_UART,
                                               LocalUART_RxEventCallback);
    }
    if (hal == HAL_OK)
    {
        hal = LocalUART_StartRx();
    }

    if (hal == HAL_OK)
    {
        state = LOCAL_UART_STATE_READY;
    }
    else
    {
        LocalUART_Fail(SEB_ERROR_LOCAL_UART_INIT);
    }
}

/**
 * @brief Forwards newly received bytes to the Link TX queue.
 *
 * Each contiguous run of the ring becomes one UART_DATA frame of at most
 * SEB_CFG_PACKET_MAX_PAYLOAD bytes. A full queue drops the run; the Packet
 * Manager reports it.
 *
 * @return true when any byte was forwarded.
 */
static bool LocalUART_ForwardRx(void)
{
    const uint16_t head      = rx_head;
    bool           forwarded = false;
    uint16_t       run;

    while (rx_tail != head)
    {
        if (head > rx_tail)
        {
            run = (uint16_t)(head - rx_tail);
        }
        else
        {
            run = (uint16_t)(SEB_CFG_LOCAL_UART_RX_SIZE - rx_tail);
        }

        if (run > SEB_CFG_PACKET_MAX_PAYLOAD)
        {
            run = (uint16_t)SEB_CFG_PACKET_MAX_PAYLOAD;
        }

        (void)PacketManager_Packetize(PACKET_QUEUE_LINK_TX, own_address,
                                      PACKET_FUNC_UART_DATA,
                                      &rx_ring[rx_tail], (uint8_t)run);

        rx_tail   = (uint16_t)((rx_tail + run) % SEB_CFG_LOCAL_UART_RX_SIZE);
        forwarded = true;
    }

    return forwarded;
}

/**
 * @brief Sends the payload of the oldest Local TX frame.
 *
 * The frame stays queued (peek) until the transaction succeeds. A frame
 * that cannot be unwrapped, or is empty, is removed so that it cannot block
 * the queue.
 */
static void LocalUART_StartTransmit(void)
{
    PacketData_t data;

    if (PacketManager_Peek(PACKET_QUEUE_LOCAL_TX, &tx_packet) == STATUS_OK)
    {
        /* data.length is read only when Depacketize succeeded (|| order). */
        if ((PacketManager_Depacketize(tx_packet.frame, tx_packet.size,
                                       &data) != STATUS_OK) ||
            (data.length == 0U))
        {
            (void)PacketManager_Commit(PACKET_QUEUE_LOCAL_TX);
        }
        else
        {
            tx_done = false;

            if (HAL_UART_Transmit_IT(SEB_BOARD_LOCAL_UART, data.payload,
                                     (uint16_t)data.length) == HAL_OK)
            {
                tx_busy = true;
            }
            else
            {
                LocalUART_Fail(SEB_ERROR_LOCAL_UART_TX);
            }
        }
    }
}

/**
 * @brief READY state (section 3.2 READY).
 *
 * @param burst
 *        true when data from the local device was forwarded in this pass.
 */
static void LocalUART_Ready(bool burst)
{
    if ((board_mode == BOARD_MODE_MAIN) && burst)
    {
        state = LOCAL_UART_STATE_RECEIVE;
    }
    else if (PacketManager_IsDataAvailable(PACKET_QUEUE_LOCAL_TX))
    {
        state = LOCAL_UART_STATE_TRANSMIT;
    }
    else
    {
        /* Nothing to do. */
    }
}

/**
 * @brief RECEIVE state (section 3.2 RECEIVE).
 *
 * @param burst
 *        true when data from the local device was forwarded in this pass.
 */
static void LocalUART_Receive(bool burst)
{
    if (board_mode == BOARD_MODE_MAIN)
    {
        /* The SBC request is already in the Link TX queue: await the reply. */
        if (LocalUART_StartTimer())
        {
            state = LOCAL_UART_STATE_TRANSMIT;
        }
    }
    else if (burst)
    {
        /* The RockBLOCK answered: the command is delivered. */
        (void)PacketManager_Commit(PACKET_QUEUE_LOCAL_TX);
        LocalUART_StopTimer();
        attempts = 0U;
        state    = LOCAL_UART_STATE_READY;
    }
    else if (TimerManager_IsExpired(response_timer))
    {
        LocalUART_Fail(SEB_ERROR_LOCAL_UART_TIMEOUT);
    }
    else
    {
        /* Still waiting for the response. */
    }
}

/**
 * @brief TRANSMIT state (section 3.2 TRANSMIT).
 */
static void LocalUART_Transmit(void)
{
    if (tx_busy)
    {
        if (tx_done)
        {
            tx_busy = false;

            if (board_mode == BOARD_MODE_MAIN)
            {
                (void)PacketManager_Commit(PACKET_QUEUE_LOCAL_TX);
                LocalUART_StopTimer();
                attempts = 0U;
                state    = LOCAL_UART_STATE_READY;
            }
            else if (LocalUART_StartTimer())
            {
                /* Kept queued until the RockBLOCK answers. */
                state = LOCAL_UART_STATE_RECEIVE;
            }
            else
            {
                /* Timer pool empty: already in ERROR. */
            }
        }
    }
    else if (PacketManager_IsDataAvailable(PACKET_QUEUE_LOCAL_TX))
    {
        LocalUART_StartTransmit();
    }
    else if (response_timer == SEB_TIMER_HANDLE_INVALID)
    {
        /* Nothing to send and no reply awaited. */
        state = LOCAL_UART_STATE_READY;
    }
    else if (TimerManager_IsExpired(response_timer))
    {
        /* Main: the remote SEB did not reply. Free the SBC side rather than
         * wait forever (developer decision); the Link UART owns retries. */
        ErrorManager_Report(SEB_ERROR_LOCAL_UART_TIMEOUT);
        LocalUART_StopTimer();
        state = LOCAL_UART_STATE_READY;
    }
    else
    {
        /* Main: reply not ready yet. */
    }
}

/**
 * @brief ERROR state (section 3.2 ERROR): report, then recover or stop.
 */
static void LocalUART_Error(void)
{
    if (!fatal)
    {
        ErrorManager_Report(last_error);

        if (attempts < SEB_CFG_LOCAL_UART_MAX_RECOVERY)
        {
            attempts++;
            state = LOCAL_UART_STATE_RECOVERY;
        }
        else
        {
            ErrorManager_Report(SEB_ERROR_LOCAL_UART_FATAL);
            fatal = true;
        }
    }
}

/**
 * @brief RECOVERY state (section 3.2 RECOVERY).
 *
 * Any transfer in progress is stopped; a queued frame is kept and sent
 * again from TRANSMIT.
 */
static void LocalUART_Recovery(void)
{
    LocalUartState_t  next = LOCAL_UART_STATE_TRANSMIT;
    HAL_StatusTypeDef hal  = HAL_OK;

    if (tx_busy)
    {
        hal     = HAL_UART_AbortTransmit(SEB_BOARD_LOCAL_UART);
        tx_busy = false;
    }

    if (last_error == SEB_ERROR_LOCAL_UART_INIT)
    {
        /* INIT registers the callbacks again; HAL_UART_Init resets them. */
        if (hal == HAL_OK)
        {
            hal = HAL_UART_DeInit(SEB_BOARD_LOCAL_UART);
        }
        if (hal == HAL_OK)
        {
            hal = HAL_UART_Init(SEB_BOARD_LOCAL_UART);
        }
        next = LOCAL_UART_STATE_INIT;
    }
    else if (last_error == SEB_ERROR_LOCAL_UART_RX)
    {
        if (hal == HAL_OK)
        {
            hal = HAL_UART_AbortReceive(SEB_BOARD_LOCAL_UART);
        }
        if (hal == HAL_OK)
        {
            hal = LocalUART_StartRx();
        }
        if (hal == HAL_OK)
        {
            /* Line noise gives many errors in a few ms; a working restart
             * loses nothing, so it must not use up the attempts. */
            attempts = 0U;
        }
        next = LOCAL_UART_STATE_READY;
    }
    else
    {
        /* Transmit error, response timeout or no timer: resend. */
    }

    if (hal == HAL_OK)
    {
        state = next;
    }
    else
    {
        LocalUART_Fail(SEB_ERROR_LOCAL_UART_INIT);
    }
}

Status_t LocalUARTManager_Init(BoardMode_t mode, uint8_t address)
{
    board_mode  = mode;
    own_address = address;
    attempts    = 0U;
    fatal       = false;
    last_error  = SEB_ERROR_NONE;

    LocalUART_Setup();

    return (state == LOCAL_UART_STATE_READY) ? STATUS_OK : STATUS_ERROR;
}

void LocalUARTManager_Run(void)
{
    const bool burst = LocalUART_ForwardRx();

    if (rx_error &&
        ((state == LOCAL_UART_STATE_READY) ||
         (state == LOCAL_UART_STATE_TRANSMIT) ||
         (state == LOCAL_UART_STATE_RECEIVE)))
    {
        LocalUART_Fail(SEB_ERROR_LOCAL_UART_RX);
    }

    switch (state)
    {
        case LOCAL_UART_STATE_INIT:
            LocalUART_Setup();
            break;

        case LOCAL_UART_STATE_READY:
            LocalUART_Ready(burst);
            break;

        case LOCAL_UART_STATE_RECEIVE:
            LocalUART_Receive(burst);
            break;

        case LOCAL_UART_STATE_TRANSMIT:
            LocalUART_Transmit();
            break;

        case LOCAL_UART_STATE_ERROR:
            LocalUART_Error();
            break;

        case LOCAL_UART_STATE_RECOVERY:
            LocalUART_Recovery();
            break;

        default:
            /* Unreachable: every state is handled above. */
            break;
    }
}

LocalUartState_t LocalUARTManager_GetState(void)
{
    return state;
}

bool LocalUARTManager_HasFatalError(void)
{
    return fatal;
}
