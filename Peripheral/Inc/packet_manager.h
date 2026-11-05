/**
 * @file packet_manager.h
 *
 * @brief Provides the interface for the Packet Manager.
 *
 * The Packet Manager builds the frames exchanged between the Main and Sub
 * SEBs, keeps them in two TX queues until they are delivered, and checks
 * received frames, as defined in architecture section 3.4. It is stateless:
 * UART transfer, timeouts and retries belong to the UART managers.
 *
 * Frame: [ADDRESS][FUNCTION][LENGTH][PAYLOAD][CRC low][CRC high], with
 * CRC-16/MODBUS over every byte before the CRC.
 *
 * All functions must be called from the main loop only.
 */

#ifndef PACKET_MANAGER_H
#define PACKET_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "status_codes.h"

/** Function code: raw UART data, never interpreted (Modbus user range). */
#define PACKET_FUNC_UART_DATA           0x41U

/** Function code: logical state of the SLP/NET/RI signals. */
#define PACKET_FUNC_GPIO_STATUS         0x42U

/** Largest frame in bytes (Modbus RTU ADU limit). */
#define SEB_CFG_PACKET_MAX_FRAME        256U

/** Largest payload: the frame minus ADDRESS, FUNCTION, LENGTH and CRC. */
#define SEB_CFG_PACKET_MAX_PAYLOAD      251U

/** Frames per TX queue: one being delivered, one waiting. */
#define SEB_CFG_PACKET_QUEUE_DEPTH      2U

/** Highest local address (three DIP switches, section 1.4.3). */
#define SEB_CFG_PACKET_ADDRESS_MAX      8U

/**
 * @brief Identifies a TX queue.
 */
typedef enum
{
    PACKET_QUEUE_LOCAL_TX = 0,  /**< Frames for the Local UART.          */
    PACKET_QUEUE_LINK_TX,       /**< Frames for the Link UART.           */
    PACKET_QUEUE_COUNT          /**< Number of queues; not a queue.      */
} QueueId_t;

/**
 * @brief Holds one finished frame, ready to transmit byte for byte.
 */
typedef struct
{
    uint16_t size;                              /**< Bytes used in frame. */
    uint8_t  frame[SEB_CFG_PACKET_MAX_FRAME];   /**< Frame bytes.         */
} Packet_t;

/**
 * @brief Holds the content of a valid received frame.
 */
typedef struct
{
    uint8_t        function;    /**< PACKET_FUNC_UART_DATA or _GPIO_STATUS. */
    uint8_t        length;      /**< Payload bytes.                         */
    const uint8_t *payload;     /**< Points into the caller's frame buffer. */
} PacketData_t;

/**
 * @brief Initializes the Packet Manager.
 *
 * Empties both TX queues and stores the address of this board, used to
 * accept or discard received frames.
 *
 * @param local_address
 *        DIP switch address, 1 to SEB_CFG_PACKET_ADDRESS_MAX.
 *
 * @return STATUS_OK; STATUS_ERROR when the address is 0 or too high, in
 *         which case nothing changes.
 */
Status_t PacketManager_Init(uint8_t local_address);

/**
 * @brief Builds a frame and adds it to a TX queue.
 *
 * Invalid arguments are a caller error and are not reported. A full queue
 * is reported as SEB_ERROR_PACKET_QUEUE_FULL.
 *
 * @param queue
 *        Queue that receives the frame.
 * @param address
 *        Destination SEB address.
 * @param function
 *        PACKET_FUNC_UART_DATA or PACKET_FUNC_GPIO_STATUS.
 * @param payload
 *        Payload bytes; may be NULL only when length is 0.
 * @param length
 *        Payload bytes, 0 to SEB_CFG_PACKET_MAX_PAYLOAD.
 *
 * @return STATUS_OK when queued; STATUS_ERROR when nothing was queued.
 */
Status_t PacketManager_Packetize(QueueId_t queue, uint8_t address,
                                 uint8_t function, const uint8_t *payload,
                                 uint8_t length);

/**
 * @brief Tells whether a TX queue holds a frame.
 *
 * @param queue
 *        Queue to check.
 *
 * @return true when at least one frame is queued; false when the queue is
 *         empty or invalid.
 */
bool     PacketManager_IsDataAvailable(QueueId_t queue);

/**
 * @brief Copies the oldest frame of a TX queue without removing it.
 *
 * The frame stays queued so that it can be retransmitted until
 * PacketManager_Commit() is called.
 *
 * @param queue
 *        Queue to read.
 * @param packet
 *        Receives the frame.
 *
 * @return STATUS_OK; STATUS_ERROR when the queue is empty or invalid, or
 *         packet is NULL.
 */
Status_t PacketManager_Peek(QueueId_t queue, Packet_t *packet);

/**
 * @brief Removes the oldest frame of a TX queue.
 *
 * Called only after the frame's transaction has completed successfully.
 *
 * @param queue
 *        Queue to update.
 *
 * @return STATUS_OK; STATUS_ERROR when the queue is empty or invalid.
 */
Status_t PacketManager_Commit(QueueId_t queue);

/**
 * @brief Checks a received frame and returns its content.
 *
 * Checks, in this order: size, address, function, LENGTH, CRC. A frame for
 * another address is discarded without a report (section 3.4); every other
 * failed check is reported to the Error Manager.
 *
 * @param frame
 *        Received bytes.
 * @param size
 *        Number of received bytes.
 * @param data
 *        Receives the function, length and a payload pointer into frame.
 *        Not valid when the function fails.
 *
 * @return STATUS_OK for a valid frame for this board; STATUS_ERROR
 *         otherwise.
 */
Status_t PacketManager_Depacketize(const uint8_t *frame, uint16_t size,
                                   PacketData_t *data);

#endif /* PACKET_MANAGER_H */
