/**
 * @file
 * @brief BACNet secure connect hub function API.
 * @author Kirill Neznamov
 * @date October 2022
 * @section LICENSE
 *
 * Copyright (C) 2022 Legrand North America, LLC
 * as an unpublished work.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bacnet/basic/sys/debug.h"
#include <bacnet/basic/sys/fifo.h>
#include "bacnet/datalink/bsc/bsc-conf.h"
#include "bacnet/datalink/bsc/bvlc-sc.h"
#include "bacnet/datalink/bsc/bsc-socket.h"
#include "bacnet/datalink/bsc/bsc-util.h"
#include "bacnet/datalink/bsc/bsc-mutex.h"
#include "bacnet/datalink/bsc/bsc-event.h"
#include "bacnet/datalink/bsc/bsc-runloop.h"
#include "bacnet/datalink/bsc/bsc-hub-function.h"
#include "bacnet/datalink/bsc/bsc-node.h"
#include "bacnet/bacdef.h"
#include "bacnet/npdu.h"
#include "bacnet/bacenum.h"
#include "bacnet/basic/object/netport.h"
#include "bacnet/basic/object/sc_netport.h"
#include "bacnet/basic/object/bacfile.h"

#define DEBUG_BSC_DATALINK 0

#if DEBUG_BSC_DATALINK == 1
#define DEBUG_PRINTF debug_printf
#else
#undef DEBUG_ENABLED
#define DEBUG_PRINTF(...)
#endif

#define BSC_NEXT_POWER_OF_TWO1(v) \
    ((((unsigned int)v) - 1) | ((((unsigned int)v) - 1) >> 1))
#define BSC_NEXT_POWER_OF_TWO2(v) \
    (BSC_NEXT_POWER_OF_TWO1(v) | BSC_NEXT_POWER_OF_TWO1(v) >> 2)
#define BSC_NEXT_POWER_OF_TWO3(v) \
    (BSC_NEXT_POWER_OF_TWO2(v) | BSC_NEXT_POWER_OF_TWO2(v) >> 4)
#define BSC_NEXT_POWER_OF_TWO4(v) \
    (BSC_NEXT_POWER_OF_TWO3(v) | BSC_NEXT_POWER_OF_TWO3(v) >> 8)
#define BSC_NEXT_POWER_OF_TWO(v) \
    ((BSC_NEXT_POWER_OF_TWO4(v) | BSC_NEXT_POWER_OF_TWO4(v) >> 16)) + 1

static FIFO_BUFFER bsc_fifo = { 0 };
static uint8_t bsc_fifo_buf[BSC_NEXT_POWER_OF_TWO(BVLC_SC_NPDU_SIZE_CONF * 10)];
static BSC_NODE *bsc_node = NULL;
static BSC_NODE_CONF bsc_conf;
static BSC_EVENT *bsc_event = NULL;
static bool bsc_datalink_initialized = false;

static void bsc_node_event(BSC_NODE *node,
    BSC_NODE_EVENT ev,
    BACNET_SC_VMAC_ADDRESS *dest,
    uint8_t *pdu,
    uint16_t pdu_len)
{
    DEBUG_PRINTF("bsc_node_event() >>> ev = %d\n", ev);
    bsc_global_mutex_lock();

    if (ev == BSC_NODE_EVENT_STARTED || ev == BSC_NODE_EVENT_STOPPED) {
        bsc_event_signal(bsc_event);
    } else if (ev == BSC_NODE_EVENT_RECEIVED_NPDU) {
        if (bsc_datalink_initialized) {
            if (FIFO_Available(&bsc_fifo, pdu_len + sizeof(pdu_len))) {
                FIFO_Add(&bsc_fifo, (uint8_t *)&pdu_len, sizeof(pdu_len));
                FIFO_Add(&bsc_fifo, pdu, pdu_len);
            }
#if DEBUG_ENABLED == 1
            else {
                DEBUG_PRINTF("pdu of size %d\n is dropped\n", pdu_len);
            }
#endif
        }
    }
    bsc_global_mutex_unlock();
    DEBUG_PRINTF("bsc_node_event() <<<\n");
}

BACNET_STACK_EXPORT
bool bsc_init(char *ifname)
{
    BSC_SC_RET r;
    bool ret = false;
    DEBUG_PRINTF("bsc_init() >>>\n");
    DEBUG_PRINTF("bsc_init() BACNET/SC datalink configured to use input fifo "
                 "of size %d\n",
        sizeof(bsc_fifo_buf));

    bsc_global_mutex_lock();
    if (!bsc_node && !bsc_datalink_initialized) {
        r = bsc_runloop_start(bsc_global_runloop());
        if (r == BSC_SC_SUCCESS) {
            bsc_event = bsc_event_init();
            if (bsc_event) {
                FIFO_Init(&bsc_fifo, bsc_fifo_buf, sizeof(bsc_fifo_buf));
                bsc_node_conf_fill_from_netport(&bsc_conf, &bsc_node_event);
                r = bsc_node_init(&bsc_conf, &bsc_node);
                if (r != BSC_SC_SUCCESS) {
                    bsc_runloop_stop(bsc_global_runloop());
                    bsc_event_deinit(bsc_event);
                    bsc_event = NULL;
                } else {
                    r = bsc_node_start(bsc_node);
                    if (r == BSC_SC_SUCCESS) {
                        ret = true;
                        bsc_global_mutex_unlock();
                        bsc_event_wait(bsc_event);
                        bsc_global_mutex_lock();
                        bsc_datalink_initialized = true;
                    } else {
                        bsc_runloop_stop(bsc_global_runloop());
                        bsc_node_deinit(bsc_node);
                        bsc_event_deinit(bsc_event);
                        bsc_event = NULL;
                        bsc_node = NULL;
                    }
                }
            }
        }
    }
    bsc_global_mutex_unlock();
    DEBUG_PRINTF("bsc_init() <<< ret = %d\n", ret);
    return ret;
}

BACNET_STACK_EXPORT
void bsc_cleanup(void)
{
    DEBUG_PRINTF("bsc_cleanup() >>>\n");
    bsc_global_mutex_lock();
    if (bsc_node) {
        if (!bsc_datalink_initialized) {
            bsc_global_mutex_unlock();
            bsc_event_wait(bsc_event);
            bsc_global_mutex_lock();
        }
        bsc_node_stop(bsc_node);
        bsc_global_mutex_unlock();
        bsc_event_wait(bsc_event);
        bsc_global_mutex_lock();
        bsc_datalink_initialized = false;
        bsc_runloop_stop(bsc_global_runloop());
        bsc_node_deinit(bsc_node);
        bsc_event_deinit(bsc_event);
        bsc_event = NULL;
        bsc_node = NULL;
    }
    bsc_global_mutex_unlock();
    DEBUG_PRINTF("bsc_cleanup() <<<\n");
}

BACNET_STACK_EXPORT
int bsc_send_pdu(BACNET_ADDRESS *dest,
    BACNET_NPDU_DATA *npdu_data,
    uint8_t *pdu,
    unsigned pdu_len)
{
    (void)npdu_data;
    BSC_SC_RET ret;
    BACNET_SC_VMAC_ADDRESS dest_vmac = { 0xFF };
    int len = -1;
    static uint8_t buf[BVLC_SC_NPDU_SIZE_CONF];

    /* this datalink doesn't need to know the npdu data */
    (void)npdu_data;

    bsc_global_mutex_lock();

    if (dest->net == BACNET_BROADCAST_NETWORK || dest->mac_len == 0) {
        // broadcast message
        memset(&dest_vmac.address[0], 0xFF, BVLC_SC_VMAC_SIZE);
    } else if (dest->mac_len == 6) {
        // unicast
        memcpy(&dest_vmac.address[0], &dest->mac[0], BVLC_SC_VMAC_SIZE);
    } else {
        bsc_global_mutex_unlock();
        DEBUG_PRINTF(
            "bsc_send_pdu() <<< ret = -1, incorrect dest mac address\n");
        return len;
    }

    len = bvlc_sc_encode_encapsulated_npdu(buf, sizeof(buf),
        bsc_get_next_message_id(), NULL, &dest_vmac, pdu, pdu_len);

    ret = bsc_node_send(bsc_node, buf, len);

    if (ret != BSC_SC_SUCCESS) {
        len = -1;
    }

    bsc_global_mutex_unlock();
    DEBUG_PRINTF("bsc_send_pdu() <<< ret = %d\n", len);
    return len;
}

static void bsc_remove_packet(uint16_t packet_size)
{
    int i;
    for (i = 0; i < packet_size; i++) {
        FIFO_Get(&bsc_fifo);
    }
}

BACNET_STACK_EXPORT
uint16_t bsc_receive(
    BACNET_ADDRESS *src, uint8_t *pdu, uint16_t max_pdu, unsigned timeout)
{
    uint16_t pdu_len = 0;
    uint16_t npdu_len = 0;
    BVLC_SC_DECODED_MESSAGE dm;
    BACNET_ERROR_CODE error;
    BACNET_ERROR_CLASS class;
    const char *err_desc = NULL;
    static uint8_t buf[BVLC_SC_NPDU_SIZE_CONF];

    // TODO: implement timeout

    //    DEBUG_PRINTF("bsc_receive() >>>\n");
    bsc_global_mutex_lock();
    if (FIFO_Count(&bsc_fifo) > sizeof(uint16_t)) {
        DEBUG_PRINTF("bsc_receive() processing data...\n");
        FIFO_Pull(&bsc_fifo, (uint8_t *)&npdu_len, sizeof(npdu_len));

        if (sizeof(buf) < npdu_len) {
            DEBUG_PRINTF("bsc_receive() pdu of size %d is dropped\n", pdu_len);
            // remove packet from FIFO
            bsc_remove_packet(npdu_len);
        } else {
            FIFO_Pull(&bsc_fifo, buf, npdu_len);
            if (!bvlc_sc_decode_message(
                    buf, npdu_len, &dm, &error, &class, &err_desc)) {
                DEBUG_PRINTF("bsc_receive() pdu of size %d is dropped because "
                             "of err = %d, class %d, desc = %s\n",
                    npdu_len, error, class, err_desc);
                // remove packet from FIFO
                bsc_remove_packet(npdu_len);
            } else {
                if (dm.hdr.origin &&
                    dm.hdr.bvlc_function == BVLC_SC_ENCAPSULATED_NPDU &&
                    max_pdu >= dm.payload.encapsulated_npdu.npdu_len) {
                    src->mac_len = BVLC_SC_VMAC_SIZE;
                    memcpy(src->mac, dm.hdr.origin, BVLC_SC_VMAC_SIZE);
                    memcpy(pdu, dm.payload.encapsulated_npdu.npdu,
                        dm.payload.encapsulated_npdu.npdu_len);
                    pdu_len = dm.payload.encapsulated_npdu.npdu_len;
                }
#if DEBUG_ENABLED == 1
                else {
                    DEBUG_PRINTF("bsc_receive() pdu of size %d is dropped "
                                 "because origin addr is absent\n",
                        npdu_len);
                }
#endif
            }
        }
        DEBUG_PRINTF("bsc_receive() pdu_len = %d\n", npdu_len);
    }
    bsc_global_mutex_unlock();
    // DEBUG_PRINTF("bsc_receive() <<< ret = %d\n", pdu_len);
    return pdu_len;
}
