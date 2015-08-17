/***********************************************************************************
Copyright (c) Nordic Semiconductor ASA
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

  3. Neither the name of Nordic Semiconductor ASA nor the names of other
  contributors to this software may be used to endorse or promote products
  derived from this software without specific prior written permission.

  4. This software must only be used in a processor manufactured by Nordic
  Semiconductor ASA, or in a processor manufactured by a third party that
  is used in combination with a processor manufactured by Nordic Semiconductor.


THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
************************************************************************************/

#include "version_handler.h"

#include "transport_control.h"
#include "timeslot_handler.h"
#include "timer_control.h"
#include "event_handler.h"
#include "rbc_mesh_common.h"
#include "trickle.h"

#include "nrf_error.h"
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#define MESH_TRICKLE_I_MAX              (2000)
#define MESH_TRICKLE_K                  (3)

#define MESH_VALUE_LOLLIPOP_LIMIT       (200)

#define TIMESLOT_STARTUP_DELAY_US       (100)



#define MESH_MD_FLAGS_USED_POS          (0) /* Metadata flag: Used */
#define MESH_MD_FLAGS_INITIALIZED_POS   (1) /* Metadata flag: Initialized */
#define MESH_MD_FLAGS_IS_ORIGIN_POS     (2) /* Metadata flag: Is origin */
/******************************************************************************
* Local typedefs
******************************************************************************/
typedef struct 
{
    uint16_t version_number; /* value version */
    uint8_t char_value_handle; /* value handle */
    uint8_t flags; /* value flags */
    uint32_t crc; /* previous value CRC */
    ble_gap_addr_t last_sender_addr; /*previous value originator address */
    trickle_t trickle; /* trickle instance for mesh value */
} metadata_t;

typedef struct
{
    metadata_t* md;
    uint8_t handle_count;
} metadata_set_t;

/******************************************************************************
* Static globals
******************************************************************************/
static metadata_set_t g_md_set;
static bool g_is_initialized = false;

/******************************************************************************
* Static functions
******************************************************************************/
static void transmit_all_instances(uint64_t timestamp);

static void order_next_transmission(uint64_t time_now)
{
    uint64_t ts_begin_time = timeslot_get_global_time();
    uint64_t ts_end_time = timeslot_get_end_time();
    uint64_t earliest = UINT64_MAX;
    for (uint32_t i = 0; i < g_md_set.handle_count; ++i)
    {
        if (g_md_set.md[i].flags & (1 << MESH_MD_FLAGS_USED_POS) &&
            g_md_set.md[i].trickle.t < earliest && 
            g_md_set.md[i].trickle.t > time_now + ts_begin_time /* expired timers should be handled in transmit_all_instances() */
        )
        {
            earliest = g_md_set.md[i].trickle.t;
        }
    }
    if (earliest < ts_end_time)
    {
        timer_order_cb(TIMER_INDEX_VH, earliest - ts_begin_time, transmit_all_instances);
    }
}

static void transmit_all_instances(uint64_t timestamp)
{
    /* the framework continues where it left off */
    static uint32_t handle = 0;
    
    uint64_t ts_begin_time = timeslot_get_global_time();

    for (uint32_t i = 0; i < g_md_set.handle_count; ++i)
    {
        while (handle >= g_md_set.handle_count)
            handle -= g_md_set.handle_count;

        if ((g_md_set.md[handle].flags & (1 << MESH_MD_FLAGS_USED_POS)) &&
            g_md_set.md[handle].trickle.t <= timestamp + ts_begin_time)
        {
            bool do_tx = false;
            trickle_tx_timeout(&g_md_set.md[handle].trickle, &do_tx, timestamp + ts_begin_time);
            if (do_tx)
            {
                if (tc_tx(handle + 1 /* 1-indexed */,
                            g_md_set.md[handle].version_number,
                            &g_md_set.md[handle].last_sender_addr)
                        != NRF_SUCCESS)
                {
                    /* the radio queue is full, tc will notify us when it's available again */
                    TICK_PIN(PIN_TC_QUEUE_FULL);
                    break;
                }
                else
                {
                    /* the handle is queued for transmission */
                    trickle_tx_register(&g_md_set.md[handle].trickle);
                }
            }
        }

        handle++;
    }

    order_next_transmission(timestamp);
}

static void version_increase(uint16_t* version)
{
    if (*version == UINT16_MAX)
    {
        *version = MESH_VALUE_LOLLIPOP_LIMIT;
    }
    else
    {
        (*version)++;
    }
}

/******************************************************************************
* Interface functions
******************************************************************************/
uint32_t vh_init(uint8_t handle_count, uint32_t min_interval_us)
{
    if (handle_count == 0 || handle_count > MAX_VALUE_COUNT)
        return NRF_ERROR_INVALID_PARAM;

    /* allocate and zero-initialize metadata */
    g_md_set.md = calloc(handle_count, sizeof(metadata_t));
    
    if (g_md_set.md == NULL)
        return NRF_ERROR_NO_MEM;

    g_is_initialized = true;
    g_md_set.handle_count = handle_count;
    
    trickle_setup(min_interval_us, MESH_TRICKLE_I_MAX, MESH_TRICKLE_K);

    return NRF_SUCCESS;
}

vh_data_status_t vh_compare_metadata(uint8_t handle, uint16_t version, uint32_t crc, bool origin_is_me)
{
    if (!g_is_initialized)
        return VH_DATA_STATUS_UNKNOWN;
    if (handle > g_md_set.handle_count || handle == 0)
        return VH_DATA_STATUS_UNKNOWN;

    metadata_t* p_md = &g_md_set.md[handle-1]; /* to zero-indexed */

    bool uninitialized = !(p_md->flags & (1 << MESH_MD_FLAGS_INITIALIZED_POS));
    uint16_t separation = (version >= p_md->version_number)?
        (version - p_md->version_number) :
        (version - p_md->version_number - MESH_VALUE_LOLLIPOP_LIMIT);

    if (version == p_md->version_number)
    {
        ble_gap_addr_t my_addr;
        sd_ble_gap_address_get(&my_addr);

        if ((origin_is_me && (memcmp(&p_md->last_sender_addr, &my_addr, sizeof(ble_gap_addr_t)) == 0)) || 
            (crc == p_md->crc) ||
            (version == 0))
        {
            return VH_DATA_STATUS_SAME;
        }
        else
        {
            return VH_DATA_STATUS_CONFLICTING;
        }
    }
    else if (uninitialized)
    {
        return VH_DATA_STATUS_NEW;
    }
    else if (
        (   
            p_md->version_number < MESH_VALUE_LOLLIPOP_LIMIT && 
            version > p_md->version_number
        ) 
        ||
        (
            p_md->version_number >= MESH_VALUE_LOLLIPOP_LIMIT && 
            version >= MESH_VALUE_LOLLIPOP_LIMIT && 
            separation < (UINT16_MAX - MESH_VALUE_LOLLIPOP_LIMIT)/2
        ) 
    )
    {
        return VH_DATA_STATUS_UPDATED;
    }
    else
    {
        return VH_DATA_STATUS_OLD;
    }
}

uint32_t vh_rx_register(vh_data_status_t status, uint8_t handle, uint16_t version, uint32_t crc, ble_gap_addr_t* origin, uint64_t timestamp)
{
    if (!g_is_initialized)
        return NRF_ERROR_INVALID_STATE;
    if (handle > g_md_set.handle_count || handle == 0)
        return NRF_ERROR_INVALID_ADDR;

    metadata_t* p_md = &g_md_set.md[handle-1]; /* to zero-indexed */
    uint64_t ts_start_time = timeslot_get_global_time();

    switch (status)
    {
        case VH_DATA_STATUS_NEW:
            vh_order_update(timestamp);
            /* deliberate fallthrough */
        
        case VH_DATA_STATUS_UPDATED:
            p_md->version_number = version;
            p_md->crc = crc;
            memcpy(&p_md->last_sender_addr, origin, sizeof(ble_gap_addr_t));
            /* deliberate fallthrough */

        case VH_DATA_STATUS_OLD:
        case VH_DATA_STATUS_CONFLICTING:
            p_md->flags |= (1 << MESH_MD_FLAGS_INITIALIZED_POS);
            trickle_rx_inconsistent(&p_md->trickle, ts_start_time + timestamp);
            break;

        case VH_DATA_STATUS_SAME:
            p_md->crc = crc; /* if we previously had an invalid crc because of a local update */
            trickle_rx_consistent(&p_md->trickle, ts_start_time + timestamp); 
            break;

        case VH_DATA_STATUS_UNKNOWN:
            return NRF_ERROR_INVALID_PARAM;
    }

    return NRF_SUCCESS;
}

vh_data_status_t vh_local_update(uint8_t handle)
{
    if (!g_is_initialized)
        return VH_DATA_STATUS_UNKNOWN;
    if (handle > g_md_set.handle_count || handle == 0)
        return VH_DATA_STATUS_UNKNOWN;

    metadata_t* p_md = &g_md_set.md[handle-1]; /* to zero-indexed */

    vh_data_status_t status;
    uint64_t time_now = timer_get_timestamp() + timeslot_get_global_time();

    if (p_md->flags & (1 << MESH_MD_FLAGS_INITIALIZED_POS))
    {
        status = VH_DATA_STATUS_UPDATED;
        trickle_rx_inconsistent(&p_md->trickle, time_now);
    }
    else
    {
        status = VH_DATA_STATUS_NEW;
        trickle_timer_reset(&g_md_set.md[handle - 1].trickle, time_now);
    }
    p_md->flags |= (1 << MESH_MD_FLAGS_INITIALIZED_POS) |
        (1 << MESH_MD_FLAGS_USED_POS);

    version_increase(&p_md->version_number);

    p_md->crc = 0xFFFFFFFF; /* invalid crc */
    ble_gap_addr_t my_addr;
    sd_ble_gap_address_get(&my_addr);
    memcpy(&p_md->last_sender_addr, &my_addr, sizeof(ble_gap_addr_t));

    vh_order_update(0);

    return status;
}

uint32_t vh_on_timeslot_begin(void)
{
    return vh_order_update(TIMESLOT_STARTUP_DELAY_US);
}

uint32_t vh_order_update(uint64_t time_now)
{
    /* handle expired trickle timers */
    async_event_t tx_event;
    tx_event.type = EVENT_TYPE_TIMER;
    tx_event.callback.timer.cb = transmit_all_instances;
    tx_event.callback.timer.timestamp = time_now;
    return event_handler_push(&tx_event);
}

uint32_t vh_set_gatts_handle(uint8_t value_handle, uint8_t gatts_handle)
{
    if (!g_is_initialized)
        return NRF_ERROR_INVALID_STATE;
    if (value_handle > g_md_set.handle_count || value_handle == 0)
        return NRF_ERROR_INVALID_ADDR;

    g_md_set.md[value_handle - 1].char_value_handle = gatts_handle;

    return NRF_SUCCESS;
}

uint32_t vh_get_gatts_handle(uint8_t value_handle, uint8_t* gatts_handle)
{
    if (!g_is_initialized)
        return NRF_ERROR_INVALID_STATE;
    if (value_handle > g_md_set.handle_count || value_handle == 0)
        return NRF_ERROR_INVALID_ADDR;
    if (gatts_handle == NULL)
        return NRF_ERROR_NULL;

    *gatts_handle = g_md_set.md[value_handle - 1].char_value_handle;
    return NRF_SUCCESS;
}

uint32_t vh_get_origin_addr(uint8_t handle, ble_gap_addr_t* addr)
{
    if (!g_is_initialized)
        return NRF_ERROR_INVALID_STATE;
    if (handle > g_md_set.handle_count || handle == 0)
        return NRF_ERROR_INVALID_ADDR;
    
    memcpy(addr, &g_md_set.md[handle].last_sender_addr, sizeof(ble_gap_addr_t));
    return NRF_SUCCESS;
}

uint32_t vh_value_enable(uint8_t handle)
{
    if (!g_is_initialized)
        return NRF_ERROR_INVALID_STATE;
    if (handle > g_md_set.handle_count || handle == 0)
        return NRF_ERROR_INVALID_ADDR;
    
    vh_order_update(0);
    
    trickle_timer_reset(&g_md_set.md[handle - 1].trickle, timer_get_timestamp() + timeslot_get_global_time());

    g_md_set.md[handle - 1].flags |=
        (1 << MESH_MD_FLAGS_INITIALIZED_POS) |
        (1 << MESH_MD_FLAGS_USED_POS);

    return NRF_SUCCESS;
}


uint32_t vh_value_disable(uint8_t handle)
{
    if (!g_is_initialized)
        return NRF_ERROR_INVALID_STATE;
    if (handle >= g_md_set.handle_count || handle == 0)
        return NRF_ERROR_INVALID_ADDR;

    g_md_set.md[handle - 1].flags &=
        ~(1 << MESH_MD_FLAGS_USED_POS);

    return NRF_SUCCESS;
}