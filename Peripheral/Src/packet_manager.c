/**
 * @file packet_manager.c
 *
 * @brief Implements the Packet Manager.
 *
 * This file contains the two TX queues, the CRC-16/MODBUS calculation, and
 * the frame building and checking defined in architecture section 3.4.
 * The queues hold frames, not communication state: the module is stateless
 * with respect to transactions.
 */

#include "packet_manager.h"

#include <stddef.h>
#include <string.h>

#include "error_manager.h"

/** ADDRESS, FUNCTION and LENGTH bytes before the payload. */
#define PACKET_HEADER_SIZE      3U

/** CRC bytes after the payload. */
#define PACKET_CRC_SIZE         2U

/** Smallest frame: header and CRC with no payload. */
#define PACKET_MIN_FRAME        (PACKET_HEADER_SIZE + PACKET_CRC_SIZE)

/** Byte positions in a frame. */
#define PACKET_POS_ADDRESS      0U
#define PACKET_POS_FUNCTION     1U
#define PACKET_POS_LENGTH       2U
#define PACKET_POS_PAYLOAD      3U

/** CRC-16/MODBUS: initial value and reflected polynomial 0x8005. */
#define PACKET_CRC_INIT         0xFFFFU
#define PACKET_CRC_POLY         0xA001U

/** Frames waiting to be delivered, per queue. */
static Packet_t tx_queue[PACKET_QUEUE_COUNT][SEB_CFG_PACKET_QUEUE_DEPTH];

/** Slot of the oldest frame, per queue. */
static uint8_t  tx_head[PACKET_QUEUE_COUNT];

/** Frames currently held, per queue. */
static uint8_t  tx_count[PACKET_QUEUE_COUNT];

/** Address of this board; 0 until PacketManager_Init() succeeds. */
static uint8_t  own_address = 0U;

/**
 * @brief Calculates the CRC-16/MODBUS of a byte block.
 *
 * A bit loop is used instead of a lookup table: frames are short and rare,
 * and this keeps the code small and easy to check.
 *
 * @param data
 *        Bytes to cover.
 * @param size
 *        Number of bytes.
 *
 * @return The CRC; its low byte is transmitted first.
 */
static uint16_t PacketManager_Crc16(const uint8_t *data, uint16_t size)
{
    uint16_t crc = PACKET_CRC_INIT;
    uint16_t index;
    uint8_t  bit;

    for (index = 0U; index < size; index++)
    {
        crc ^= (uint16_t)data[index];

        for (bit = 0U; bit < 8U; bit++)
        {
            if ((crc & 1U) != 0U)
            {
                crc = (uint16_t)((crc >> 1U) ^ PACKET_CRC_POLY);
            }
            else
            {
                crc = (uint16_t)(crc >> 1U);
            }
        }
    }

    return crc;
}

/**
 * @brief Tells whether a function code is one this module carries.
 *
 * @param function
 *        Function code to check.
 *
 * @return true for PACKET_FUNC_UART_DATA and PACKET_FUNC_GPIO_STATUS.
 */
static bool PacketManager_IsKnownFunction(uint8_t function)
{
    return (function == PACKET_FUNC_UART_DATA) ||
           (function == PACKET_FUNC_GPIO_STATUS);
}

Status_t PacketManager_Init(uint8_t local_address)
{
    Status_t status = STATUS_ERROR;
    uint8_t  queue;

    /* 0 is the Modbus broadcast address, which the SEB link does not use. */
    if ((local_address != 0U) && (local_address <= SEB_CFG_PACKET_ADDRESS_MAX))
    {
        for (queue = 0U; queue < (uint8_t)PACKET_QUEUE_COUNT; queue++)
        {
            tx_head[queue]  = 0U;
            tx_count[queue] = 0U;
        }

        own_address = local_address;
        status      = STATUS_OK;
    }

    return status;
}

Status_t PacketManager_Packetize(QueueId_t queue, uint8_t address,
                                 uint8_t function, const uint8_t *payload,
                                 uint8_t length)
{
    Status_t  status = STATUS_ERROR;
    Packet_t *packet;
    uint8_t   slot;
    uint16_t  crc_pos;
    uint16_t  crc;

    if ((queue < PACKET_QUEUE_COUNT) &&
        PacketManager_IsKnownFunction(function) &&
        (length <= SEB_CFG_PACKET_MAX_PAYLOAD) &&
        ((payload != NULL) || (length == 0U)))
    {
        if (tx_count[queue] >= SEB_CFG_PACKET_QUEUE_DEPTH)
        {
            ErrorManager_Report(SEB_ERROR_PACKET_QUEUE_FULL);
        }
        else
        {
            /* The frame is built in place, so no second buffer is needed. */
            slot   = (uint8_t)((tx_head[queue] + tx_count[queue]) %
                               SEB_CFG_PACKET_QUEUE_DEPTH);
            packet = &tx_queue[queue][slot];

            packet->frame[PACKET_POS_ADDRESS]  = address;
            packet->frame[PACKET_POS_FUNCTION] = function;
            packet->frame[PACKET_POS_LENGTH]   = length;

            if (length > 0U)
            {
                (void)memcpy(&packet->frame[PACKET_POS_PAYLOAD], payload,
                             length);
            }

            crc_pos = (uint16_t)(length + PACKET_HEADER_SIZE);
            crc     = PacketManager_Crc16(packet->frame, crc_pos);

            packet->frame[crc_pos]      = (uint8_t)(crc & 0xFFU);
            packet->frame[crc_pos + 1U] = (uint8_t)(crc >> 8U);
            packet->size                = (uint16_t)(crc_pos + PACKET_CRC_SIZE);

            tx_count[queue]++;
            status = STATUS_OK;
        }
    }

    return status;
}

bool PacketManager_IsDataAvailable(QueueId_t queue)
{
    return (queue < PACKET_QUEUE_COUNT) && (tx_count[queue] > 0U);
}

Status_t PacketManager_Peek(QueueId_t queue, Packet_t *packet)
{
    Status_t        status = STATUS_ERROR;
    const Packet_t *oldest;

    if ((queue < PACKET_QUEUE_COUNT) && (packet != NULL) &&
        (tx_count[queue] > 0U))
    {
        oldest       = &tx_queue[queue][tx_head[queue]];
        packet->size = oldest->size;
        (void)memcpy(packet->frame, oldest->frame, oldest->size);
        status = STATUS_OK;
    }

    return status;
}

Status_t PacketManager_Commit(QueueId_t queue)
{
    Status_t status = STATUS_ERROR;

    if ((queue < PACKET_QUEUE_COUNT) && (tx_count[queue] > 0U))
    {
        tx_head[queue] = (uint8_t)((tx_head[queue] + 1U) %
                                   SEB_CFG_PACKET_QUEUE_DEPTH);
        tx_count[queue]--;
        status = STATUS_OK;
    }

    return status;
}

Status_t PacketManager_Depacketize(const uint8_t *frame, uint16_t size,
                                   PacketData_t *data)
{
    Status_t        status = STATUS_ERROR;
    SEB_ErrorCode_t fault  = SEB_ERROR_NONE;
    uint16_t        crc_pos;
    uint16_t        received_crc;

    if ((frame != NULL) && (data != NULL))
    {
        /* Checks run in the order of architecture section 3.4. */
        if ((size < PACKET_MIN_FRAME) || (size > SEB_CFG_PACKET_MAX_FRAME))
        {
            fault = SEB_ERROR_PACKET_LENGTH;
        }
        else if ((frame[PACKET_POS_ADDRESS] != own_address) ||
                 (own_address == 0U))
        {
            /* For another board: discarded without a report, because it is
             * not a fault (section 3.4, Packet Address Validation). */
        }
        else if (!PacketManager_IsKnownFunction(frame[PACKET_POS_FUNCTION]))
        {
            fault = SEB_ERROR_PACKET_FUNCTION;
        }
        else if (size != ((uint16_t)frame[PACKET_POS_LENGTH] + PACKET_MIN_FRAME))
        {
            fault = SEB_ERROR_PACKET_LENGTH;
        }
        else
        {
            crc_pos      = (uint16_t)(size - PACKET_CRC_SIZE);
            received_crc = (uint16_t)(((uint16_t)frame[crc_pos + 1U] << 8U) |
                                      (uint16_t)frame[crc_pos]);

            if (PacketManager_Crc16(frame, crc_pos) != received_crc)
            {
                fault = SEB_ERROR_PACKET_CRC;
            }
            else
            {
                data->function = frame[PACKET_POS_FUNCTION];
                data->length   = frame[PACKET_POS_LENGTH];
                data->payload  = &frame[PACKET_POS_PAYLOAD];
                status         = STATUS_OK;
            }
        }

        if (fault != SEB_ERROR_NONE)
        {
            ErrorManager_Report(fault);
        }
    }

    return status;
}
