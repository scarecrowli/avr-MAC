/**
 * @file mac_data_ind.c
 *
 * @brief Implements incoming frame handling in the MAC
 *
 * This file handles parsing of incoming frames from the PHY. The frames are
 * parsed in two passes, whereas the first pass is initiated by the PHY and
 * handles all time critical task necessary to initiate ACk transmission.
 * The second pass handles the further filtering of the frames and calls the
 * corresponding actions within the MAC. As long as the second pass of a valid
 * frame is not finished, no further frame can be processed.
 *
 * $Id: mac_data_ind.c 22673 2010-07-26 11:51:53Z sschneid $
 *
 * @author    Atmel Corporation: http://www.atmel.com
 * @author    Support email: avr@atmel.com
 */
/*
 * Copyright (c) 2009, Atmel Corporation All rights reserved.
 *
 * Licensed under Atmel's Limited License Agreement --> EULA.txt
 */

/* === Includes ============================================================ */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "pal.h"
#include "return_val.h"
#include "bmm.h"
#include "qmm.h"
#include "tal.h"
#include "ieee_const.h"
#include "mac_msg_const.h"
#include "mac_api.h"
#include "mac_msg_types.h"
#include "mac_data_structures.h"
#include "stack_config.h"
#include "mac_internal.h"
#include "mac.h"
#include "mac_config.h"
#include "mac_build_config.h"

/* === Macros =============================================================== */

/*
 * Mask of the GTS descriptor counter
 */
#define GTS_DESCRIPTOR_COUNTER_MASK     (0x07)

/*
 * Extract the PAN Coordinator bit from the Superframe Spec.
 */
#define GET_PAN_COORDINATOR(spec)      (((spec) & 0x4000) >> 14)

#if (MAC_PAN_ID_CONFLICT_NON_PC == 1)
/*
 * PAN-Id conflict notification payload length
 */
#define PAN_ID_CONFLICT_PAYLOAD_LEN     (1)
#endif  /* (MAC_PAN_ID_CONFLICT_NON_PC == 1) */

/* === Globals ============================================================= */


/* === Prototypes ========================================================== */

static uint8_t extract_mhr_addr_information(uint8_t *frame_ptr);
static bool parse_mpdu(frame_info_t *frameptr);
static bool process_data_ind_not_transient(buffer_t *b_ptr, frame_info_t *f_ptr);
#if (MAC_SCAN_SUPPORT == 1)
static bool process_data_ind_scanning(buffer_t *b_ptr);
#endif /* (MAC_SCAN_SUPPORT == 1) */

#ifdef PROMISCUOUS_MODE
static void prom_mode_rx_frame(buffer_t *b_ptr, frame_info_t *f_ptr);
#endif  /* PROMISCUOUS_MODE */

#if (MAC_PAN_ID_CONFLICT_AS_PC == 1)
static void check_for_pan_id_conflict_as_pc(bool in_scan);
#endif  /* (MAC_PAN_ID_CONFLICT_AS_PC == 1) */

#if (MAC_PAN_ID_CONFLICT_NON_PC == 1)
static void check_for_pan_id_conflict_non_pc(bool in_scan);
static bool tx_pan_id_conf_notif(void);
#endif  /* (MAC_PAN_ID_CONFLICT_NON_PC == 1) */

/* === Implementation ====================================================== */

/**
 * @brief Depending on received frame the appropriate function is called
 *
 * @param msg Pointer to the buffer header.
 */
void mac_process_tal_data_ind(uint8_t *msg)
{
    buffer_t *buf_ptr = (buffer_t *)msg;
    frame_info_t *frameptr = (frame_info_t *)BMM_BUFFER_POINTER(buf_ptr);
    bool processed_tal_data_indication = false;

    mac_parse_data.mpdu_length = frameptr->mpdu[0];

    /* First extract LQI since this is already needed in Promiscuous Mode. */
    mac_parse_data.ppdu_link_quality = frameptr->mpdu[mac_parse_data.mpdu_length + LQI_LEN];

#ifdef PROMISCUOUS_MODE
    if (tal_pib_PromiscuousMode)
    {
        /*
         * In promiscuous mode all received frames are forwarded to the
         * higher layer or application my means of MCPS_DATA.indication
         * primitives.
         */
        prom_mode_rx_frame(buf_ptr, frameptr);
        return;
    }
#endif  /* PROMISCUOUS_MODE */

    if (!parse_mpdu(frameptr))
    {
        /* Frame parsing failed */
        bmm_buffer_free(buf_ptr);
        return;
    }

    /* Check if the MAC is busy processing the previous requests */
    if (mac_busy)
    {
        /*
         * If MAC has to process an incoming frame that requires a response
         * (i.e. beacon request and data request) then process this operation
         * once the MAC has become free. Put the request received back into the
         * MAC internal event queue.
         */
        if (FCF_FRAMETYPE_MAC_CMD == mac_parse_data.frame_type)
        {
            if (DATAREQUEST == mac_parse_data.mac_command ||
                BEACONREQUEST == mac_parse_data.mac_command)
            {
                qmm_queue_append(&tal_mac_q, buf_ptr);
                return;
            }
        }
    }

    switch (mac_poll_state)
    {
        case MAC_POLL_IDLE:
            /*
             * We are in no transient state.
             * Now are either in a non-transient MAC state or scanning.
             */
            if (MAC_SCAN_IDLE == mac_scan_state)
            {
                /*
                 * Continue with handling the "real" non-transient MAC states now.
                 */
                processed_tal_data_indication = process_data_ind_not_transient(buf_ptr, frameptr);
            }
#if (MAC_SCAN_SUPPORT == 1)
            else
            {
                /* Scanning is ongoing. */
                processed_tal_data_indication = process_data_ind_scanning(buf_ptr);
            }
#endif /* (MAC_SCAN_SUPPORT == 1) */
            break;

#if (MAC_INDIRECT_DATA_BASIC == 1)
            /*
             * This is the 'wait for data' state after either
             * explicit poll or implicit poll.
             */
        case MAC_POLL_EXPLICIT:
        case MAC_POLL_IMPLICIT:
            /*
             * Function mac_process_data_response() resets the
             * MAC poll state.
             */
            mac_process_data_response();

            switch (mac_parse_data.frame_type)
            {
                case FCF_FRAMETYPE_MAC_CMD:
                {
                    switch (mac_parse_data.mac_command)
                    {

#if (MAC_ASSOCIATION_INDICATION_RESPONSE == 1)
                        case ASSOCIATIONREQUEST:
                           mac_process_associate_request(buf_ptr);
                           processed_tal_data_indication = true;
                           break;
#endif /* (MAC_ASSOCIATION_INDICATION_RESPONSE == 1) */

#if (MAC_DISASSOCIATION_BASIC_SUPPORT == 1)
                        case DISASSOCIATIONNOTIFICATION:
                            {
                                mac_process_disassociate_notification(buf_ptr);

                                processed_tal_data_indication = true;

                                /*
                                 * Device needs to scan for networks again,
                                 * go into idle mode and reset variables
                                 */
                                mac_idle_trans();
                            }
                            break;
#endif /* (MAC_DISASSOCIATION_BASIC_SUPPORT == 1) */

                        case DATAREQUEST:
#if (MAC_INDIRECT_DATA_FFD == 1)
                            if (indirect_data_q.size > 0)
                            {
                                mac_process_data_request(buf_ptr);
                                processed_tal_data_indication = true;
                            }
                            else
                            {
                                mac_handle_tx_null_data_frame();
                            }
#endif  /*  (MAC_INDIRECT_DATA_FFD == 1) */
                            break;

                        case PANIDCONFLICTNOTIFICAION:
                            break;

#if (MAC_ORPHAN_INDICATION_RESPONSE == 1)
                        case ORPHANNOTIFICATION:
                            mac_process_orphan_notification(buf_ptr);
                            processed_tal_data_indication = true;
                            break;
#endif /* (MAC_ORPHAN_INDICATION_RESPONSE == 1) */

#if (MAC_START_REQUEST_CONFIRM == 1)
                        case BEACONREQUEST:
                            if (MAC_COORDINATOR == mac_state)
                            {
                                /*
                                 * Only a Coordinator can both poll and
                                 * AND answer beacon request frames.
                                 * PAN Coordinators do not poll;
                                 * End devices do not answer beacon requests.
                                 */
                                mac_process_beacon_request(buf_ptr);
                                processed_tal_data_indication = true;
                            }
                            break;
#endif /* (MAC_START_REQUEST_CONFIRM == 1) */

#if (MAC_SYNC_LOSS_INDICATION == 1)
                        case COORDINATORREALIGNMENT:
                            /*
                             * Received coordinator realignment frame for
                             * entire PAN.
                             */
                            mac_process_coord_realign(buf_ptr);
                            processed_tal_data_indication = true;
                            break;
#endif  /* (MAC_SYNC_LOSS_INDICATION == 1) */

                        default:
#if (DEBUG > 0)
                            ASSERT("Unsupported MAC frame in state MAC_EXPLICIT_POLL or MAC_POLL_IMPLICIT" == 0);
#endif
                            break;
                    }
                }
                break; /* case FCF_FRAMETYPE_MAC_CMD: */

                case FCF_FRAMETYPE_DATA:
                    mac_process_data_frame(buf_ptr);
                    processed_tal_data_indication = true;
                    break;

                default:
                    break;
            }
            break;
            /* MAC_POLL_EXPLICIT, MAC_POLL_IMPLICIT */
#endif /* (MAC_INDIRECT_DATA_BASIC == 1) */


#if (MAC_ASSOCIATION_REQUEST_CONFIRM == 1)
        case MAC_AWAIT_ASSOC_RESPONSE:
            /*
             * We are either expecting an association reponse frame
             * or a null data frame.
             */
            if ((FCF_FRAMETYPE_MAC_CMD == mac_parse_data.frame_type) &&
                (ASSOCIATIONRESPONSE == mac_parse_data.mac_command)
               )
            {
                /* This is the expected association response frame. */
                pal_timer_stop(T_Poll_Wait_Time);

#if (DEBUG > 0)
                if (pal_is_timer_running(T_Poll_Wait_Time))
                {
                    ASSERT("T_Poll_Wait_Time tmr during association running" == 0);
                }
#endif
                mac_process_associate_response(buf_ptr);
                processed_tal_data_indication = true;
            }
            else if (FCF_FRAMETYPE_DATA == mac_parse_data.frame_type)
            {
                mac_process_data_frame(buf_ptr);
                processed_tal_data_indication = true;
            }
            break;
            /*  MAC_AWAIT_ASSOC_RESPONSE */
#endif /* (MAC_ASSOCIATION_REQUEST_CONFIRM == 1) */

        default:
#if (DEBUG > 0)
            ASSERT("Received frame in unsupported MAC poll state" == 0);
#endif
            break;
    }

    /* If message is not processed */
    if (!processed_tal_data_indication)
    {
        bmm_buffer_free(buf_ptr);
    }
} /* mac_process_tal_data_ind() */



#if (MAC_SCAN_SUPPORT == 1)
/**
 * @brief Continues processing a data indication from the TAL for
 *        scanning states of the MAC (mac_scan_state == MAC_SCAN_IDLE).
 *
 * @param b_ptr Pointer to the buffer header.
 *
 * @return bool True if frame has been processed, or false otherwise.
 */
static bool process_data_ind_scanning(buffer_t *b_ptr)
{
    bool processed_in_scanning = false;
    /*
     * We are in a scanning process now (mac_scan_state is not MAC_SCAN_IDLE),
     * so continue with the specific scanning states.
     */
    switch (mac_scan_state)
    {
#if (MAC_SCAN_ED_REQUEST_CONFIRM == 1)
        /* Energy Detect scan */
        case MAC_SCAN_ED:
            /*
             * Ignore all frames received while performing ED measurement,
             * or while performing CCA.
             */
            b_ptr = b_ptr;  /* Keep compiler happy. */
            break;
#endif /* (MAC_SCAN_ED_REQUEST_CONFIRM == 1) */


#if ((MAC_SCAN_PASSIVE_REQUEST_CONFIRM == 1) || (MAC_SCAN_ACTIVE_REQUEST_CONFIRM == 1))
        /* Active scan or passive scan */
        case MAC_SCAN_ACTIVE:
        case MAC_SCAN_PASSIVE:
            if (FCF_FRAMETYPE_BEACON == mac_parse_data.frame_type)
            {
#if (MAC_PAN_ID_CONFLICT_AS_PC == 1)
                /* PAN-Id conflict detection as PAN-Coordinator. */
                if (MAC_PAN_COORD_STARTED == mac_state)
                {
                    /* Node is currently scanning. */
                    check_for_pan_id_conflict_as_pc(true);
                }
#endif  /* (MAC_PAN_ID_CONFLICT_AS_PC == 1) */
#if (MAC_PAN_ID_CONFLICT_NON_PC == 1)
                if (mac_pib_macAssociatedPANCoord &&
                    ((MAC_ASSOCIATED == mac_state) || (MAC_COORDINATOR == mac_state))
                   )
                {
                    check_for_pan_id_conflict_non_pc(true);
                }
#endif  /* (MAC_PAN_ID_CONFLICT_NON_PC == 1) */
                mac_process_beacon_frame(b_ptr);
                processed_in_scanning = true;
            }
            break;
#endif /* ((MAC_SCAN_PASSIVE_REQUEST_CONFIRM == 1) || (MAC_SCAN_ACTIVE_REQUEST_CONFIRM == 1)) */


#if (MAC_SCAN_ORPHAN_REQUEST_CONFIRM == 1)
        /* Orphan scan */
        case MAC_SCAN_ORPHAN:
            if (FCF_FRAMETYPE_MAC_CMD == mac_parse_data.frame_type &&
                COORDINATORREALIGNMENT == mac_parse_data.mac_command)
            {
                /*
                 * Received coordinator realignment frame in the middle of
                 * an orphan scan.
                 */
                pal_timer_stop(T_Scan_Duration);

                mac_process_orphan_realign(b_ptr);
                processed_in_scanning = true;
            }
            break;
#endif /* (MAC_SCAN_ORPHAN_REQUEST_CONFIRM == 1) */


        default:
        {
#if (DEBUG > 0)
            ASSERT("Unexpected TAL data indication while checking mac_scan_state" == 0);
#endif
        }
        break;
    }

    return (processed_in_scanning);
}
#endif /* (MAC_SCAN_SUPPORT == 1) */



/**
 * @brief Continues processing a data indication from the TAL for
 *        non-polling and non-scanning states of the MAC
 *        (mac_poll_state == MAC_POLL_IDLE, mac_scan_state == MAC_SCAN_IDLE).
 *
 * @param b_ptr Pointer to the buffer header.
 * @param f_ptr Pointer to the frame_info_t structure.
 *
 * @return bool True if frame has been processed, or false otherwise.
 */
static bool process_data_ind_not_transient(buffer_t *b_ptr, frame_info_t *f_ptr)
{
    bool processed_in_not_transient = false;
    /*
     * We are in MAC_POLL_IDLE and MAC_SCAN_IDLE now,
     * so continue with the real MAC states.
     */
    switch (mac_state)
    {
#if (MAC_START_REQUEST_CONFIRM == 1)
        case MAC_PAN_COORD_STARTED:
        {
            switch (mac_parse_data.frame_type)
            {
                case FCF_FRAMETYPE_MAC_CMD:
                {
                    switch (mac_parse_data.mac_command)
                    {
#if (MAC_ASSOCIATION_INDICATION_RESPONSE == 1)
                        case ASSOCIATIONREQUEST:
                            mac_process_associate_request(b_ptr);
                            processed_in_not_transient = true;
                            break;
#endif /* (MAC_ASSOCIATION_INDICATION_RESPONSE == 1) */

#if (MAC_DISASSOCIATION_BASIC_SUPPORT == 1)
                        case DISASSOCIATIONNOTIFICATION:
                            mac_process_disassociate_notification(b_ptr);
                            processed_in_not_transient = true;
                            break;
#endif /* (MAC_DISASSOCIATION_BASIC_SUPPORT == 1) */

#if (MAC_INDIRECT_DATA_FFD == 1)
                        case DATAREQUEST:
                            if (indirect_data_q.size > 0)
                            {
                                mac_process_data_request(b_ptr);
                                processed_in_not_transient = true;
                            }
                            else
                            {
                                mac_handle_tx_null_data_frame();
                            }
                            break;
#endif /* (MAC_INDIRECT_DATA_FFD == 1) */

#if (MAC_ORPHAN_INDICATION_RESPONSE == 1)
                        case ORPHANNOTIFICATION:
                            mac_process_orphan_notification(b_ptr);
                            processed_in_not_transient = true;
                            break;
#endif /* (MAC_ORPHAN_INDICATION_RESPONSE == 1) */

                        case BEACONREQUEST:
                            mac_process_beacon_request(b_ptr);
                            processed_in_not_transient = true;
                            break;

#if (MAC_PAN_ID_CONFLICT_AS_PC == 1)
                        case PANIDCONFLICTNOTIFICAION:
                            mac_sync_loss(MAC_PAN_ID_CONFLICT);
                            break;
#endif  /* (MAC_PAN_ID_CONFLICT_AS_PC == 1) */

                        default:
                            break;
                    }
                }
                break;

                case FCF_FRAMETYPE_DATA:
                    mac_process_data_frame(b_ptr);
                    processed_in_not_transient = true;
                    break;

#if (MAC_PAN_ID_CONFLICT_AS_PC == 1)
                case FCF_FRAMETYPE_BEACON:
                    /* PAN-Id conflict detection as PAN-Coordinator. */
                    /* Node is not scanning. */
                    check_for_pan_id_conflict_as_pc(false);
                    break;
#endif  /* (MAC_PAN_ID_CONFLICT_AS_PC == 1) */

                default:
                    break;
             }
        }
        break;
        /* MAC_PAN_COORD_STARTED */
#endif /* (MAC_START_REQUEST_CONFIRM == 1) */

        case MAC_IDLE:
#if (MAC_ASSOCIATION_REQUEST_CONFIRM == 1)
        case MAC_ASSOCIATED:
#endif /* (MAC_ASSOCIATION_REQUEST_CONFIRM == 1) */
#if (MAC_START_REQUEST_CONFIRM == 1)
        case MAC_COORDINATOR:
#endif /* (MAC_START_REQUEST_CONFIRM == 1) */
        {
            /* Is it a Beacon from our parent? */
            switch (mac_parse_data.frame_type)
            {
#if (MAC_SYNC_REQUEST == 1)
                case FCF_FRAMETYPE_BEACON:
                {
                    uint32_t beacon_tx_time_symb;

                    /* Check for PAN-Id conflict being NOT a PAN Corodinator. */
#if (MAC_PAN_ID_CONFLICT_NON_PC == 1)
                    if (mac_pib_macAssociatedPANCoord && (MAC_IDLE != mac_state))
                    {
                        check_for_pan_id_conflict_non_pc(false);
                    }
#endif  /* (MAC_PAN_ID_CONFLICT_NON_PC == 1) */

                    /* Check if the beacon is received from my parent. */
                    if ((mac_parse_data.src_panid == tal_pib_PANId) &&
                        (((mac_parse_data.src_addr_mode == FCF_SHORT_ADDR) &&
                          (mac_parse_data.src_addr.short_address ==
                                mac_pib_macCoordShortAddress)) ||
                         ((mac_parse_data.src_addr_mode == FCF_LONG_ADDR) &&
                          (mac_parse_data.src_addr.long_address ==
                                mac_pib_macCoordExtendedAddress))))
                    {
                        beacon_tx_time_symb = TAL_CONVERT_US_TO_SYMBOLS(f_ptr->time_stamp);

#if (DEBUG > 0)
                        retval_t set_status =
#endif
                        set_tal_pib_internal(macBeaconTxTime, (void *)&beacon_tx_time_symb);
#if (DEBUG > 0)
                        ASSERT(MAC_SUCCESS == set_status);
#endif
                        if ((MAC_SYNC_TRACKING_BEACON == mac_sync_state) ||
                            (MAC_SYNC_BEFORE_ASSOC == mac_sync_state)
                           )
                        {
                            uint32_t nxt_bcn_tm;
                            uint32_t beacon_int_symb;

                            /* Process a received beacon. */
                            mac_process_beacon_frame(b_ptr);

                            /* Initialize beacon tracking timer. */
                            {
                                retval_t tmr_start_res = FAILURE;

#ifdef BEACON_SUPPORT
                                if (tal_pib_BeaconOrder < NON_BEACON_NWK)
                                {
                                    beacon_int_symb =
                                        TAL_GET_BEACON_INTERVAL_TIME(tal_pib_BeaconOrder);
                                }
                                else
#endif /* BEACON_SUPPORT */
                                {
                                    beacon_int_symb =
                                        TAL_GET_BEACON_INTERVAL_TIME(BO_USED_FOR_MAC_PERS_TIME);
                                }

                                pal_timer_stop(T_Beacon_Tracking_Period);

#if (DEBUG > 0)
                                if (pal_is_timer_running(T_Beacon_Tracking_Period))
                                {
                                    ASSERT("Bcn tmr running" == 0);
                                }
#endif

                                do
                                {
                                    /*
                                     * Calculate the time for next beacon
                                     * transmission
                                     */
                                    beacon_tx_time_symb = tal_add_time_symbols(beacon_tx_time_symb,
                                                                               beacon_int_symb);
                                    /*
                                     * Take into account the time taken by
                                     * the radio to wakeup from sleep state
                                     */
                                    nxt_bcn_tm = tal_sub_time_symbols(beacon_tx_time_symb,
                                                                      TAL_RADIO_WAKEUP_TIME_SYM << (tal_pib_BeaconOrder + 2));

                                    tmr_start_res =
                                        pal_timer_start(T_Beacon_Tracking_Period,
                                                        TAL_CONVERT_SYMBOLS_TO_US(nxt_bcn_tm),
                                                        TIMEOUT_ABSOLUTE,
                                                        (FUNC_PTR)mac_t_tracking_beacons_cb,
                                                        NULL);
                                }
                                while (MAC_SUCCESS != tmr_start_res);
                            }

                            /*
                             * Initialize superframe timer if required only
                             * for devices because Superframe timer is already running for
                             * coordinator.
                             */
                            /* TODO */
                            /*
                            if (MAC_COORDINATOR != mac_state)
                            {
                                if (tal_pib_SuperFrameOrder < tal_pib_BeaconOrder)
                                {
                                        pal_timer_start(T_Superframe,
                                                        TAL_CONVERT_SYMBOLS_TO_US(
                                                            TAL_GET_SUPERFRAME_DURATION_TIME(
                                                                tal_pib_SuperFrameOrder)),
                                                        TIMEOUT_RELATIVE,
                                                        (FUNC_PTR)mac_t_start_inactive_device_cb,
                                                        NULL);
                                }
                            }
                            */

                            /* Initialize missed beacon timer. */
                            mac_start_missed_beacon_timer();

                            /* A device that is neither scanning nor polling shall go to sleep now. */
                            if (
                                (MAC_COORDINATOR != mac_state) &&
                                (MAC_SCAN_IDLE == mac_scan_state) &&
                                (MAC_POLL_IDLE == mac_poll_state)
                               )
                            {
                                /*
                                 * If the last received beacon frame from our parent
                                 * has indicated pending broadbast data, we need to
                                 * stay awake, until the broadcast data has been received.
                                 */
                                if (!mac_bc_data_indicated)
                                {
                                    /* Set radio to sleep if allowed */
                                    mac_sleep_trans();
                                }
                            }
                        }
                        else if (MAC_SYNC_ONCE == mac_sync_state)
                        {
                            mac_process_beacon_frame(b_ptr);

                            /* Do this after processing the beacon. */
                            mac_sync_state = MAC_SYNC_NEVER;
                        }
                        else
                        {
                            /* Process the beacon frame */
                            bmm_buffer_free(b_ptr);
                        }

                        processed_in_not_transient = true;
                    }
                    else
                    {
                       /* No action taken, buffer will be freed. */
                    }
                }
                break;
#else   /* (MAC_SYNC_REQUEST == 0) */
                case FCF_FRAMETYPE_BEACON:
                {
                    /* Check for PAN-Id conflict being NOT a PAN Corodinator. */
#if ((MAC_PAN_ID_CONFLICT_NON_PC == 1) && (MAC_ASSOCIATION_REQUEST_CONFIRM == 1))
                    if (mac_pib_macAssociatedPANCoord && (MAC_IDLE != mac_state))
                    {
                        check_for_pan_id_conflict_non_pc(false);
                    }
#endif  /* ((MAC_PAN_ID_CONFLICT_NON_PC == 1) && (MAC_ASSOCIATION_REQUEST_CONFIRM == 1)) */
                }
                break;
#endif /* (MAC_SYNC_REQUEST == 0/1) */



                case FCF_FRAMETYPE_MAC_CMD:
                    switch (mac_parse_data.mac_command)
                    {
#if (MAC_DISASSOCIATION_BASIC_SUPPORT == 1)
                        case DISASSOCIATIONNOTIFICATION:
                            mac_process_disassociate_notification(b_ptr);
                            processed_in_not_transient = true;

                            if (MAC_ASSOCIATED == mac_state)
                            {
                                /*
                                 * Device needs to scan for networks again,
                                 * go into idle mode and reset variables
                                 */
                                mac_idle_trans();
                            }
                            break;
#endif /* (MAC_DISASSOCIATION_BASIC_SUPPORT == 1) */

#if (MAC_SYNC_LOSS_INDICATION == 1)
                        case COORDINATORREALIGNMENT:
                            /*
                             * Received coordinator realignment frame from
                             * coordinator while NOT performing orphan scan.
                             */
                            mac_process_coord_realign(b_ptr);
                            processed_in_not_transient = true;
                            break;
#endif /* (MAC_SYNC_LOSS_INDICATION == 1) */

#if (MAC_START_REQUEST_CONFIRM == 1)
                        case BEACONREQUEST:
                            if (MAC_COORDINATOR == mac_state)
                            {
                                /*
                                 * Only Coordinators (no End devices) answer
                                 * Beacon requests.
                                 */
                                mac_process_beacon_request(b_ptr);
                                processed_in_not_transient = true;
                            }
                            break;
#endif  /* (MAC_START_REQUEST_CONFIRM == 1) */

#if (MAC_ASSOCIATION_INDICATION_RESPONSE == 1)
                        case ASSOCIATIONREQUEST:
                            mac_process_associate_request(b_ptr);
                            processed_in_not_transient = true;
                            break;
#endif /* (MAC_ASSOCIATION_INDICATION_RESPONSE == 1) */

#if (MAC_INDIRECT_DATA_FFD == 1)
                        case DATAREQUEST:
                            if (indirect_data_q.size > 0)
                            {
                                mac_process_data_request(b_ptr);
                                processed_in_not_transient = true;
                            }
                            else
                            {
                                mac_handle_tx_null_data_frame();
                            }
                            break;
#endif /* (MAC_INDIRECT_DATA_FFD == 1) */

#if (MAC_ORPHAN_INDICATION_RESPONSE == 1)
                        case ORPHANNOTIFICATION:
                            mac_process_orphan_notification(b_ptr);
                            processed_in_not_transient = true;
                            break;
#endif /* (MAC_ORPHAN_INDICATION_RESPONSE == 1) */
                        default:
                            break;
                    }
                    break;

                case FCF_FRAMETYPE_DATA:
                    mac_process_data_frame(b_ptr);
                    processed_in_not_transient = true;
                    break;

                default:
                    break;
            }
        }
        break;
        /* MAC_IDLE, MAC_ASSOCIATED, MAC_COORDINATOR */

        default:
        {
#if (DEBUG > 0)
            ASSERT("Unexpected TAL data indication" == 0);
#endif
        }
        break;
    }

    f_ptr = f_ptr;    /* Keep compiler happy. */

    return (processed_in_not_transient);
}



/*
 * @brief Parse an MPDU
 *
 * This function parses an MPDU which got from data_indication,
 * and leaves the parse result in mac_parse_data.
 *
 * @param frameptr Pointer to frame received from TAL
 *
 * @return bool True if frame OK, or false if frame is invalid.
 */
static bool parse_mpdu(frame_info_t *rx_frame_ptr)
{
    uint8_t     payload_index;
    uint8_t     temp_byte;
    uint16_t    fcf;
    uint8_t     *temp_frame_ptr = &(rx_frame_ptr->mpdu[1]);
    /* temp_frame_ptr points now to first octet of FCF. */

    /* Extract the FCF. */
    fcf = convert_byte_array_to_16_bit(temp_frame_ptr);
    mac_parse_data.fcf = fcf;
    temp_frame_ptr += 2;

    /* Extract the Sequence Number. */
    mac_parse_data.sequence_number = *temp_frame_ptr++;

    /* Extract the complete address information from the MHR. */
    temp_frame_ptr += extract_mhr_addr_information(temp_frame_ptr);
    /*
     * Note: temp_frame_ptr points now to the first octet of the MAC payload
     * if available.
     */

#ifndef MAC_SECURITY_ZIP
    if (fcf & FCF_SECURITY_ENABLED)
    {
        return false;
    }
#endif

    mac_parse_data.frame_type = FCF_GET_FRAMETYPE(fcf);

    if (FCF_FRAMETYPE_MAC_CMD == mac_parse_data.frame_type)
    {
        mac_parse_data.mac_command = *temp_frame_ptr;
    }

    payload_index = 0;

#ifdef BEACON_SUPPORT
        /* The timestamping is only required for beaconing networks. */
    mac_parse_data.time_stamp = rx_frame_ptr->time_stamp;
#endif  /* BEACON_SUPPORT */

#ifdef MAC_SECURITY_ZIP
    if (fcf & FCF_SECURITY_ENABLED)
    {
        mac_unsecure(&mac_parse_data, &rx_frame_ptr->mpdu[1], temp_frame_ptr, &payload_index);
    }
#endif  /* MAC_SECURITY_ZIP */

    /* temp_frame_ptr still points to the first octet of the MAC payload. */
    switch (mac_parse_data.frame_type)
    {
        case FCF_FRAMETYPE_BEACON:
            /* Get the Superframe specification */
            memcpy(&mac_parse_data.mac_payload_data.beacon_data.superframe_spec,
                   &temp_frame_ptr[payload_index],
                   sizeof(uint16_t));
            payload_index += sizeof(uint16_t);

            /* Get the GTS specification */
            mac_parse_data.mac_payload_data.beacon_data.gts_spec = temp_frame_ptr[payload_index++];

            /*
             * If the GTS specification descriptor count is > 0, then
             * increase the index by the correct GTS field octet number
             * GTS directions and GTS address list will not be parsed
             */
            temp_byte = (mac_parse_data.mac_payload_data.beacon_data.gts_spec &
                         GTS_DESCRIPTOR_COUNTER_MASK);
            if (temp_byte > 0)
            {
                /* 1 octet GTS diresctions + GTS address list */
                payload_index += 1 + temp_byte;
            }

            /* Get the Pending address specification */
            mac_parse_data.mac_payload_data.beacon_data.pending_addr_spec =
                temp_frame_ptr[payload_index++];

            {
            /*
             * If the Pending address specification indicates that the number of
             * short or long addresses is > 0, then get the short and/or
             * long addresses
             */
                uint8_t number_bytes_short_addr = NUM_SHORT_PEND_ADDR(mac_parse_data.mac_payload_data.beacon_data.pending_addr_spec);
                uint8_t number_bytes_long_addr = NUM_LONG_PEND_ADDR(mac_parse_data.mac_payload_data.beacon_data.pending_addr_spec);

                if ((number_bytes_short_addr) || (number_bytes_long_addr))
            {
                    mac_parse_data.mac_payload_data.beacon_data.pending_addr_list =
                            &temp_frame_ptr[payload_index];
            }


                if (number_bytes_short_addr)
            {
                    payload_index += (number_bytes_short_addr * sizeof(uint16_t));
                }


                if (number_bytes_long_addr)
                {
                    payload_index += (number_bytes_long_addr * sizeof(uint64_t));
            }
            }

            /* Is there a beacon payload ? */
            if (mac_parse_data.mac_payload_length > payload_index)
            {
                mac_parse_data.mac_payload_data.beacon_data.beacon_payload_len =
                                mac_parse_data.mac_payload_length - payload_index;

                /* Store pointer to received beacon payload. */
                mac_parse_data.mac_payload_data.beacon_data.beacon_payload =
                                &temp_frame_ptr[payload_index];
            }
            else
            {
                mac_parse_data.mac_payload_data.beacon_data.beacon_payload_len = 0;
            }
            break;

        case FCF_FRAMETYPE_DATA:
            if (mac_parse_data.mac_payload_length)
            {
                /*
                 * In case the device got a frame with a corrupted payload
                 * length
                 */
                if (mac_parse_data.mac_payload_length >= aMaxMACPayloadSize)
                {
                    mac_parse_data.mac_payload_length = aMaxMACPayloadSize;
                }

                /*
                 * Copy the pointer to the data frame payload for
                 * further processing later.
                 */
                mac_parse_data.mac_payload_data.data.payload = &temp_frame_ptr[payload_index];
            }
            else
            {
                mac_parse_data.mac_payload_length = 0;
            }
            break;

        case FCF_FRAMETYPE_MAC_CMD:
            /*
             * Leave the SDL path here.
             *
             * SDL would normally complete parsing the command, then
             * fall back to the caller, which would eventually notice
             * that an ACK needs to be sent (76(154), p. 328). Upon
             * that, it would walk a lot of state transitions until it
             * could eventually transmit the ACK frame. This is very
             * likely to happen way too late to be in time for an ACK
             * frame, and it's somewhat funny why that part of SDL
             * would actually go all that way down as all other ACK
             * frames are pushed out onto the queue really quickly,
             * without taking care to await all the response message
             * ping-pong first.
             */
            payload_index = 1;

            switch (mac_parse_data.mac_command)
            {
#if (MAC_ASSOCIATION_INDICATION_RESPONSE == 1)
            case ASSOCIATIONREQUEST:
                mac_parse_data.mac_payload_data.assoc_req_data.capability_info =
                    temp_frame_ptr[payload_index++];
                break;
#endif /* (MAC_ASSOCIATION_INDICATION_RESPONSE == 1) */

#if (MAC_ASSOCIATION_REQUEST_CONFIRM == 1)
            case ASSOCIATIONRESPONSE:
                memcpy(&mac_parse_data.mac_payload_data.assoc_response_data.short_addr,
                       &temp_frame_ptr[payload_index],
                       sizeof(uint16_t));
                payload_index += sizeof(uint16_t);
                mac_parse_data.mac_payload_data.assoc_response_data.assoc_status =
                    temp_frame_ptr[payload_index];
                break;
#endif /* (MAC_ASSOCIATION_REQUEST_CONFIRM == 1) */

#if (MAC_DISASSOCIATION_BASIC_SUPPORT == 1)
            case DISASSOCIATIONNOTIFICATION:
                mac_parse_data.mac_payload_data.disassoc_req_data.disassoc_reason =
                    temp_frame_ptr[payload_index++];
                break;
#endif /* (MAC_DISASSOCIATION_BASIC_SUPPORT == 1) */

            case COORDINATORREALIGNMENT:
                memcpy(&mac_parse_data.mac_payload_data.coord_realign_data.pan_id,
                       &temp_frame_ptr[payload_index],
                       sizeof(uint16_t));
                payload_index += sizeof(uint16_t);

                memcpy(&mac_parse_data.mac_payload_data.coord_realign_data.coord_short_addr,
                       &temp_frame_ptr[payload_index],
                       sizeof(uint16_t));
                payload_index += sizeof(uint16_t);

                mac_parse_data.mac_payload_data.coord_realign_data.logical_channel =
                    temp_frame_ptr[payload_index++];

                memcpy(&mac_parse_data.mac_payload_data.coord_realign_data.short_addr,
                       &temp_frame_ptr[payload_index],
                       sizeof(uint16_t));
                payload_index += sizeof(uint16_t);

                /*
                 * If frame version subfield indicates a 802.15.4-2006 compatible frame,
                 * the channel page is appended as additional information element.
                 */
                if (fcf & FCF_FRAME_VERSION_2006)
                {
                    mac_parse_data.mac_payload_data.coord_realign_data.channel_page =
                        temp_frame_ptr[payload_index++];
                }
                break;

#if (MAC_ORPHAN_INDICATION_RESPONSE == 1)
            case ORPHANNOTIFICATION:
#endif /* (MAC_ORPHAN_INDICATION_RESPONSE == 1) */
            case DATAREQUEST:
            case BEACONREQUEST:
#if (MAC_PAN_ID_CONFLICT_AS_PC == 1)
            case PANIDCONFLICTNOTIFICAION:
#endif  /* (MAC_PAN_ID_CONFLICT_AS_PC == 1) */
                break;

            default:
#if (DEBUG > 0)
                ASSERT("Unsupported MAC command in parse_MPDU" == 0);
#endif
                return false;
            }
            break;

        default:
            return false;
    }
    return true;
} /* parse_mpdu() */



/*
 * @brief Helper function to extract the complete address information
 *        of the received frame
 *
 * @param frame_ptr Pointer to first octet of Addressing fields of received frame
 *        (See IEEE 802.15.4-2006 Figure 41)
 *
 * @return bool Length of Addressing fields
 */
static uint8_t extract_mhr_addr_information(uint8_t *frame_ptr)
{
    uint16_t fcf = mac_parse_data.fcf;
    uint8_t src_addr_mode = (fcf >> FCF_SOURCE_ADDR_OFFSET) & FCF_ADDR_MASK;
    uint8_t dst_addr_mode = (fcf >> FCF_DEST_ADDR_OFFSET) & FCF_ADDR_MASK;
    bool intra_pan = fcf & FCF_PAN_ID_COMPRESSION;
    uint8_t addr_field_len = 0;

    if (dst_addr_mode != 0)
    {
        mac_parse_data.dest_panid = convert_byte_array_to_16_bit(frame_ptr);
        frame_ptr += PAN_ID_LEN;
        addr_field_len += PAN_ID_LEN;

        if (FCF_SHORT_ADDR == dst_addr_mode)
        {
            /*
             * First initialize the complete long address with zero, since
             * later only 16 bit are actually written.
             */
            mac_parse_data.dest_addr.long_address = 0;
            mac_parse_data.dest_addr.short_address = convert_byte_array_to_16_bit(frame_ptr);
            frame_ptr += SHORT_ADDR_LEN;
            addr_field_len += SHORT_ADDR_LEN;
        }
        else if (FCF_LONG_ADDR == dst_addr_mode)
        {
            mac_parse_data.dest_addr.long_address = convert_byte_array_to_64_bit(frame_ptr);
            frame_ptr += EXT_ADDR_LEN;
            addr_field_len += EXT_ADDR_LEN;
        }
    }

    if (src_addr_mode != 0)
    {
        if (!intra_pan)
        {
            /*
             * Source PAN ID is present in the frame only if the intra-PAN bit
             * is zero and src_addr_mode is non zero.
             */
            mac_parse_data.src_panid = convert_byte_array_to_16_bit(frame_ptr);
            frame_ptr += PAN_ID_LEN;
            addr_field_len += PAN_ID_LEN;
        }
        else
        {
            /*
             * The received frame does not contain a source PAN ID, hence
             * source PAN ID is updated with the destination PAN ID.
             */
            mac_parse_data.src_panid = mac_parse_data.dest_panid;
        }

        /* The source address is updated. */
        if (FCF_SHORT_ADDR == src_addr_mode)
        {
            /*
             * First initialize the complete long address with zero, since
             * later only 16 bit are actually written.
             */
            mac_parse_data.src_addr.long_address = 0;
            mac_parse_data.src_addr.short_address = convert_byte_array_to_16_bit(frame_ptr);
            frame_ptr += SHORT_ADDR_LEN;

            addr_field_len += SHORT_ADDR_LEN;
        }
        else if (FCF_LONG_ADDR == src_addr_mode)
        {
            mac_parse_data.src_addr.long_address = convert_byte_array_to_64_bit(frame_ptr);
            frame_ptr += EXT_ADDR_LEN;

            addr_field_len += EXT_ADDR_LEN;
        }
    }

    /*
     * The length of the Addressing Field is known, so the length of the
     * MAC payload can be calcluated.
     * The actual MAC payload length is calculated from
     * the length of the mpdu minus 2 octets FCS, minus 1 octet sequence
     * number, minus the length of the addressing fields, minus 2 octet FCS.
     */
    mac_parse_data.mac_payload_length = mac_parse_data.mpdu_length -
                                        FCF_LEN -
                                        SEQ_NUM_LEN -
                                        addr_field_len -
                                        FCS_LEN;

    mac_parse_data.src_addr_mode = src_addr_mode;
    mac_parse_data.dest_addr_mode = dst_addr_mode;

    return (addr_field_len);
}



/**
 * @brief Callback function called by TAL on recption of any frame.
 *
 * This function pushes an event into the TAL-MAC queue, indicating a
 * frame reception.
 *
 * @param frame Pointer to recived frame
 */
void tal_rx_frame_cb(frame_info_t *frame)
{
    frame->msg_type = (frame_msgtype_t)TAL_DATA_INDICATION;

    if (NULL == frame->buffer_header)
    {
#if (DEBUG > 0)
        ASSERT("Null frame From TAL" == 0);
#endif
        return;
    }

    qmm_queue_append(&tal_mac_q, frame->buffer_header);
}



#ifdef PROMISCUOUS_MODE
static void prom_mode_rx_frame(buffer_t *b_ptr, frame_info_t *f_ptr)
{
    mcps_data_ind_t *mdi =
        (mcps_data_ind_t *)BMM_BUFFER_POINTER(b_ptr);

    /*
     * In promiscous mode the MCPS_DATA.indication is used as container
     * for the received frame.
     */

    /*
     * Since both f_ptr and mdi point to the same data storage place,
     * we need to save all required data first.
     * The time stamp has already been saved into .
     * So lets save the payload now.
     */

    /* Set payload pointer to MPDU of received frame. */
    mdi->msdu = &f_ptr->mpdu[1];
    mdi->msduLength = f_ptr->mpdu[0];

    /* Build the MLME_Data_indication parameters */
    mdi->DSN = 0;
#ifdef ENABLE_TSTAMP
    mdi->Timestamp = f_ptr->time_stamp;
#endif  /* ENABLE_TSTAMP */

    /* Source address mode is 0. */
    mdi->SrcAddrMode = FCF_NO_ADDR;
    mdi->SrcPANId = 0;
    mdi->SrcAddr = 0;

    /* Destination address mode is 0.*/
    mdi->DstAddrMode = FCF_NO_ADDR;
    mdi->DstPANId = 0;
    mdi->DstAddr = 0;

    mdi->mpduLinkQuality = mac_parse_data.ppdu_link_quality;
    mdi->cmdcode = MCPS_DATA_INDICATION;

    /* Append MCPS data indication to MAC-NHLE queue */
    qmm_queue_append(&mac_nhle_q, b_ptr);
}
#endif  /* PROMISCUOUS_MODE */



#if (MAC_PAN_ID_CONFLICT_AS_PC == 1)
/**
 * @brief PAN-Id conflict detection as PAN-Coordinator.
 *
 * This function handles the detection of PAN-Id Conflict detection
 * in case the node is a PAN Coordinator.
 *
 * @param in_scan Indicates whether node is currently scanning
 */
static void check_for_pan_id_conflict_as_pc(bool in_scan)
{
    /*
     * Check whether the received frame has the PAN Coordinator bit set
     * in the Superframe Specification field of the beacon frame, and
     * whether the received beacon frame has the same PAN-Id as the current
     * network.
     */
    if (GET_PAN_COORDINATOR(mac_parse_data.mac_payload_data.beacon_data.superframe_spec))
    {
        if (
#if ((MAC_SCAN_ACTIVE_REQUEST_CONFIRM == 1) || (MAC_SCAN_PASSIVE_REQUEST_CONFIRM == 1))
            ((!in_scan) && (mac_parse_data.src_panid == tal_pib_PANId)) ||
            (mac_parse_data.src_panid == mac_scan_orig_panid)
#else
            (!in_scan) && (mac_parse_data.src_panid == tal_pib_PANId)
#endif /* ((MAC_SCAN_ACTIVE_REQUEST_CONFIRM == 1) || (MAC_SCAN_PASSIVE_REQUEST_CONFIRM == 1)) */
           )
        {
            mac_sync_loss(MAC_PAN_ID_CONFLICT);
        }
    }
}
#endif  /* (MAC_PAN_ID_CONFLICT_AS_PC == 1) */



#if (MAC_PAN_ID_CONFLICT_NON_PC == 1)
/**
 * @brief PAN-Id conflict detection NOT as PAN-Coordinator.
 *
 * This function handles the detection of PAN-Id Conflict detection
 * in case the node is NOT a PAN Coordinator.
 *
 * @param in_scan Indicates whether node is currently scanning
 */
static void check_for_pan_id_conflict_non_pc(bool in_scan)
{
    /*
     * Check whether the received frame has the PAN Coordinator bit set
     * in the Superframe Specification field of the beacon frame.
     */
    if (GET_PAN_COORDINATOR(mac_parse_data.mac_payload_data.beacon_data.superframe_spec))
    {
        /*
         * The received beacon frame is from a PAN Coordinator
         * (not necessarily ours).
         * Now check if the PAN-Id is ours.
         */
        if (
#if ((MAC_SCAN_ACTIVE_REQUEST_CONFIRM == 1) || (MAC_SCAN_PASSIVE_REQUEST_CONFIRM == 1))
            ((!in_scan) && (mac_parse_data.src_panid == tal_pib_PANId)) ||
            (mac_parse_data.src_panid == mac_scan_orig_panid)
#else
            (!in_scan) && (mac_parse_data.src_panid == tal_pib_PANId)
#endif /* ((MAC_SCAN_ACTIVE_REQUEST_CONFIRM == 1) || (MAC_SCAN_PASSIVE_REQUEST_CONFIRM == 1)) */
           )
        {
            /* This beacon frame has our own PAN-Id.
             * If the address of the source is different from our own
             * parent, a PAN-Id conflict has been detected.
             */
            if (
                (mac_parse_data.src_addr.short_address != mac_pib_macCoordShortAddress) &&
                (mac_parse_data.src_addr.long_address != mac_pib_macCoordExtendedAddress)
               )
            {
                tx_pan_id_conf_notif();
            }
        }
    }
}
#endif /* (MAC_PAN_ID_CONFLICT_NON_PC == 1) */



#if (MAC_PAN_ID_CONFLICT_NON_PC == 1)
/**
 * @brief Sends a PAN-Id conflict detection command frame
 *
 * This function is called in case a device (that is associated to a
 * PAN Coordinator) detects a PAN-Id conflict situation.
 *
 * @return True if the PAN-Id conflict notification is sent successfully,
 *         false otherwise
 */
static bool tx_pan_id_conf_notif(void)
{
    retval_t tal_tx_status;
    uint8_t frame_len;
    uint8_t *frame_ptr;
    uint16_t fcf;

    buffer_t *pan_id_conf_buffer = bmm_buffer_alloc(LARGE_BUFFER_SIZE);

    if (NULL == pan_id_conf_buffer)
    {
        return false;
    }

    frame_info_t *pan_id_conf_frame =
        (frame_info_t *)BMM_BUFFER_POINTER(pan_id_conf_buffer);


    pan_id_conf_frame->msg_type = PANIDCONFLICTNOTIFICAION;

    /*
     * The buffer header is stored as a part of frame_info_t structure before the
     * frame is given to the TAL. After the transmission of the frame, reuse
     * the buffer using this pointer.
     */
    pan_id_conf_frame->buffer_header = pan_id_conf_buffer;

    /* Update the payload length. */
    frame_len = PAN_ID_CONFLICT_PAYLOAD_LEN +
                2 + // Add 2 octets for FCS
                8 + // 8 octets for long Source Address
                8 + // 8 octets for long Destination Address
                2 + // 2 octets for Destination PAN-Id
                3;  // 3 octets DSN and FCF

    /* Get the payload pointer. */
    frame_ptr = (uint8_t *)pan_id_conf_frame +
                LARGE_BUFFER_SIZE -
                PAN_ID_CONFLICT_PAYLOAD_LEN - 2;   /* Add 2 octets for FCS. */

    /*
     * Build the command frame id.
     * This is actually being written into "transmit_frame->layload[0]".
     */
    *frame_ptr = PANIDCONFLICTNOTIFICAION;

    /* Source Address */
    frame_ptr -= 8;
    convert_64_bit_to_byte_array(tal_pib_IeeeAddress, frame_ptr);

    /* Destination Address */
    frame_ptr -= 8;
    convert_64_bit_to_byte_array(mac_pib_macCoordExtendedAddress, frame_ptr);

    /* Destination PAN-Id */
    frame_ptr -= 2;
    convert_16_bit_to_byte_array(tal_pib_PANId, frame_ptr);


    /* Set DSN. */
    frame_ptr--;
    *frame_ptr = mac_pib_macDSN++;


    /* Build the FCF. */
    fcf = FCF_SET_FRAMETYPE(FCF_FRAMETYPE_MAC_CMD) |
        FCF_ACK_REQUEST |
        FCF_PAN_ID_COMPRESSION |
        FCF_SET_SOURCE_ADDR_MODE(FCF_LONG_ADDR) |
        FCF_SET_DEST_ADDR_MODE(FCF_LONG_ADDR);

    /* Set the FCF. */
    frame_ptr -= 2;
    convert_16_bit_to_byte_array(fcf, frame_ptr);


    /* First element shall be length of PHY frame. */
    frame_ptr--;
    *frame_ptr = frame_len;

    /* Finished building of frame. */
    pan_id_conf_frame->mpdu = frame_ptr;


    /* Transmission should be done with CSMA-CA and frame retries. */
#ifdef BEACON_SUPPORT
    /*
     * Now it gets tricky:
     * In Beacon network the frame is sent with slotted CSMA-CA only if:
     * 1) the node is associated, or
     * 2) the node is idle, but synced before association,
     * 3) the node is a Coordinator (we assume, that coordinators are always
     *    in sync with their parents).
     *
     * In all other cases, the frame has to be sent using unslotted CSMA-CA.
     */
    csma_mode_t cur_csma_mode;

    if (NON_BEACON_NWK != tal_pib_BeaconOrder)
    {
        if (
            ((MAC_IDLE == mac_state) && (MAC_SYNC_BEFORE_ASSOC == mac_sync_state)) ||
#if (MAC_START_REQUEST_CONFIRM == 1)
            (MAC_ASSOCIATED == mac_state) ||
            (MAC_COORDINATOR == mac_state)
#else
            (MAC_ASSOCIATED == mac_state)
#endif /* MAC_START_REQUEST_CONFIRM */
           )
        {
            cur_csma_mode = CSMA_SLOTTED;
        }
        else
        {
            cur_csma_mode = CSMA_UNSLOTTED;
        }
    }
    else
    {
        /* In Nonbeacon network the frame is sent with unslotted CSMA-CA. */
        cur_csma_mode = CSMA_UNSLOTTED;
    }

    tal_tx_status = tal_tx_frame(pan_id_conf_frame, cur_csma_mode, true);
#else   /* No BEACON_SUPPORT */
    /* In Nonbeacon build the frame is sent with unslotted CSMA-CA. */
    tal_tx_status = tal_tx_frame(pan_id_conf_frame, CSMA_UNSLOTTED, true);
#endif  /* BEACON_SUPPORT */

    if (MAC_SUCCESS == tal_tx_status)
    {
        MAKE_MAC_BUSY();
        return true;
    }
    else
    {
        /* TAL is busy, hence the data request could not be transmitted */
        bmm_buffer_free(pan_id_conf_buffer);

        return false;
    }
} /* tx_pan_id_conf_notif() */
#endif  /* (MAC_PAN_ID_CONFLICT_NON_PC == 1) */

/* EOF */
