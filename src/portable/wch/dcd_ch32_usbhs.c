/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2022 Greg Davill
 * Copyright (c) 2023 Denis Krasutski
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if CFG_TUD_ENABLED && ((CFG_TUSB_MCU == OPT_MCU_CH32V307) || (CFG_TUSB_MCU == OPT_MCU_CH32F20X))
#include "device/dcd.h"

#include "ch32_usbhs_reg.h"

// Max number of bi-directional endpoints including EP0
#define EP_MAX                  16
#define CH32_USBHS_EP0_MAX_SIZE (64)

typedef struct {
    uint8_t *buffer;
    uint16_t total_len;
    uint16_t queued_len;
    uint16_t max_size;
    bool is_last_packet;
} xfer_ctl_t;

typedef enum {
    EP_RESPONSE_ACK,
    EP_RESPONSE_NAK,
} ep_response_list_t;

#define XFER_CTL_BASE(_ep, _dir) &xfer_status[_ep][_dir]
static xfer_ctl_t xfer_status[EP_MAX][2];

#define EP_TX_LEN(ep)     *(volatile uint16_t *)((volatile uint16_t *)&(USBHSD->UEP0_TX_LEN) + (ep) * 2)
#define EP_TX_CTRL(ep)    *(volatile uint8_t *)((volatile uint8_t *)&(USBHSD->UEP0_TX_CTRL) + (ep) * 4)
#define EP_RX_CTRL(ep)    *(volatile uint8_t *)((volatile uint8_t *)&(USBHSD->UEP0_RX_CTRL) + (ep) * 4)
#define EP_RX_MAX_LEN(ep) *(volatile uint16_t *)((volatile uint16_t *)&(USBHSD->UEP0_MAX_LEN) + (ep) * 2)

#define EP_TX_DMA_ADDR(ep) *(volatile uint32_t *)((volatile uint32_t *)&(USBHSD->UEP1_TX_DMA) + (ep - 1))
#define EP_RX_DMA_ADDR(ep) *(volatile uint32_t *)((volatile uint32_t *)&(USBHSD->UEP1_RX_DMA) + (ep - 1))

/* Endpoint Buffer */
TU_ATTR_ALIGNED(4) static uint8_t ep0_data_in_out_buffer[CH32_USBHS_EP0_MAX_SIZE];

static void ep_set_response_and_toggle(uint8_t ep_addr, ep_response_list_t response_type) {
    uint8_t const ep_num = tu_edpt_number(ep_addr);
    if (ep_addr & TUSB_DIR_IN_MASK) {
        uint8_t response = (response_type == EP_RESPONSE_ACK) ? USBHS_EP_T_RES_ACK : USBHS_EP_T_RES_NAK;
        if (ep_num == 0) {
            if (response_type == EP_RESPONSE_ACK) {
                if (EP_TX_LEN(ep_num) == 0) {
                    EP_TX_CTRL(ep_num) |= USBHS_EP_T_TOG_1;
                } else {
                    EP_TX_CTRL(ep_num) ^= USBHS_EP_T_TOG_1;
                }
            }
        }
        EP_TX_CTRL(ep_num) = (EP_TX_CTRL(ep_num) & ~(USBHS_EP_T_RES_MASK)) | response;
    } else {
        uint8_t response = (response_type == EP_RESPONSE_ACK) ? USBHS_EP_R_RES_ACK : USBHS_EP_R_RES_NAK;
        if (ep_num == 0) {
            if (response_type == EP_RESPONSE_ACK) {
                if (xfer_status[ep_num][TUSB_DIR_OUT].queued_len == 0) {
                    EP_RX_CTRL(ep_num) |= USBHS_EP_R_TOG_1;
                }
            } else {
                EP_RX_CTRL(ep_num) ^= USBHS_EP_R_TOG_1;
            }
        }
        EP_RX_CTRL(ep_num) = (EP_RX_CTRL(ep_num) & ~(USBHS_EP_R_RES_MASK)) | response;
    }
}

static void xfer_data_packet(uint8_t ep_addr, xfer_ctl_t *xfer) {
    uint8_t const ep_num = tu_edpt_number(ep_addr);
    tusb_dir_t const dir = tu_edpt_dir(ep_addr);

    if (dir == TUSB_DIR_IN) {
        uint16_t remaining = xfer->total_len - xfer->queued_len;
        uint16_t next_tx_size = TU_MIN(remaining, xfer->max_size);

        if (ep_num == 0) {
            memcpy(ep0_data_in_out_buffer, &xfer->buffer[xfer->queued_len], next_tx_size);
        } else {
            EP_TX_DMA_ADDR(ep_num) = (uint32_t)&xfer->buffer[xfer->queued_len];
        }

        EP_TX_LEN(ep_num) = next_tx_size;
        xfer->queued_len += next_tx_size;
        if (xfer->queued_len == xfer->total_len) {
            xfer->is_last_packet = true;
        }
    } else { /* TUSB_DIR_OUT */
        uint16_t left_to_receive = xfer->total_len - xfer->queued_len;
        uint16_t max_possible_rx_size = TU_MIN(xfer->max_size, left_to_receive);

        if (max_possible_rx_size == left_to_receive) {
            xfer->is_last_packet = true;
        }

        if (ep_num > 0) {
            EP_RX_DMA_ADDR(ep_num) = (uint32_t)&xfer->buffer[xfer->queued_len];
            EP_RX_MAX_LEN(ep_num) = max_possible_rx_size;
        }
    }
    ep_set_response_and_toggle(ep_addr, USBHS_EP_R_RES_ACK);
}

void dcd_init(uint8_t rhport) {
    (void)rhport;

    memset(&xfer_status, 0, sizeof(xfer_status));

    USBHSD->HOST_CTRL = 0x00;
    USBHSD->HOST_CTRL = USBHS_PHY_SUSPENDM;

    USBHSD->CONTROL = 0;

#if TUD_OPT_HIGH_SPEED
    USBHSD->CONTROL = USBHS_DMA_EN | USBHS_INT_BUSY_EN | USBHS_HIGH_SPEED;
#else
    #error OPT_MODE_FULL_SPEED not currently supported on CH32
    USBHSD->CONTROL = USBHS_DMA_EN | USBHS_INT_BUSY_EN | USBHS_FULL_SPEED;
#endif

    USBHSD->INT_EN = 0;
    USBHSD->INT_EN = USBHS_SETUP_ACT_EN | USBHS_TRANSFER_EN | USBHS_DETECT_EN | USBHS_SUSPEND_EN;

    USBHSD->ENDP_CONFIG = USBHS_EP0_T_EN | USBHS_EP0_R_EN;
    USBHSD->ENDP_TYPE = 0x00;
    USBHSD->BUF_MODE = 0x00;

    for (int ep = 0; ep < EP_MAX; ep++) {
        EP_TX_LEN(ep) = 0;
        EP_TX_CTRL(ep) = USBHS_EP_T_AUTOTOG | USBHS_EP_T_RES_NAK;
        EP_RX_CTRL(ep) = USBHS_EP_R_AUTOTOG | USBHS_EP_R_RES_NAK;

        EP_RX_MAX_LEN(ep) = 0;
    }

    USBHSD->UEP0_DMA = (uint32_t)ep0_data_in_out_buffer;
    USBHSD->UEP0_MAX_LEN = CH32_USBHS_EP0_MAX_SIZE;
    xfer_status[0][TUSB_DIR_OUT].max_size = CH32_USBHS_EP0_MAX_SIZE;
    xfer_status[0][TUSB_DIR_IN].max_size = CH32_USBHS_EP0_MAX_SIZE;

    USBHSD->DEV_AD = 0;
    USBHSD->CONTROL |= USBHS_DEV_PU_EN;
}

void dcd_int_enable(uint8_t rhport) {
    (void)rhport;

    NVIC_EnableIRQ(USBHS_IRQn);
}

void dcd_int_disable(uint8_t rhport) {
    (void)rhport;

    NVIC_DisableIRQ(USBHS_IRQn);
}

void dcd_edpt_close_all(uint8_t rhport) {
    (void)rhport;

    for (size_t ep = 1; ep < EP_MAX; ep++) {
        EP_TX_LEN(ep) = 0;
        EP_TX_CTRL(ep) = USBHS_EP_T_AUTOTOG | USBHS_EP_T_RES_NAK;
        EP_RX_CTRL(ep) = USBHS_EP_R_AUTOTOG | USBHS_EP_R_RES_NAK;

        EP_RX_MAX_LEN(ep) = 0;
    }

    USBHSD->ENDP_CONFIG = USBHS_EP0_T_EN | USBHS_EP0_R_EN;
}

void dcd_set_address(uint8_t rhport, uint8_t dev_addr) {
    (void)dev_addr;

    // Response with zlp status
    dcd_edpt_xfer(rhport, 0x80, NULL, 0);
}

void dcd_remote_wakeup(uint8_t rhport) {
    (void)rhport;
}

void dcd_edpt0_status_complete(uint8_t rhport, tusb_control_request_t const *request) {
    (void)rhport;

    if (request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE &&
        request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
        request->bRequest == TUSB_REQ_SET_ADDRESS) {
        USBHSD->DEV_AD = (uint8_t)request->wValue;
    }

    EP_TX_CTRL(0) = USBHS_EP_T_RES_NAK | USBHS_EP_T_TOG_0;
    EP_RX_CTRL(0) = USBHS_EP_R_RES_NAK | USBHS_EP_R_TOG_0;
}

bool dcd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const *desc_edpt) {
    (void)rhport;

    uint8_t const ep_num = tu_edpt_number(desc_edpt->bEndpointAddress);
    tusb_dir_t const dir = tu_edpt_dir(desc_edpt->bEndpointAddress);

    TU_ASSERT(ep_num < EP_MAX);

    if (ep_num == 0) {
        return true;
    }

    xfer_ctl_t *xfer = XFER_CTL_BASE(ep_num, dir);
    xfer->max_size = tu_edpt_packet_size(desc_edpt);

    bool is_iso = (desc_edpt->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS);
    if (dir == TUSB_DIR_OUT) {
        USBHSD->ENDP_CONFIG |= (USBHS_EP0_R_EN << ep_num);
        EP_RX_CTRL(ep_num) = USBHS_EP_R_AUTOTOG | USBHS_EP_R_RES_NAK;
        if (is_iso == true) {
            USBHSD->ENDP_TYPE |= (USBHS_EP0_R_TYP << ep_num);
        }
        EP_RX_MAX_LEN(ep_num) = xfer->max_size;
    } else {
        USBHSD->ENDP_CONFIG |= (USBHS_EP0_T_EN << ep_num);
        if (is_iso == true) {
            USBHSD->ENDP_TYPE |= (USBHS_EP0_T_TYP << ep_num);
        }
        EP_TX_LEN(ep_num) = 0;
        EP_TX_CTRL(ep_num) = USBHS_EP_T_AUTOTOG | USBHS_EP_T_RES_NAK | USBHS_EP_T_TOG_0;
    }

    return true;
}

void dcd_edpt_close(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;

    uint8_t const ep_num = tu_edpt_number(ep_addr);
    tusb_dir_t const dir = tu_edpt_dir(ep_addr);

    if (dir == TUSB_DIR_OUT) {
        EP_RX_CTRL(ep_num) = USBHS_EP_R_AUTOTOG | USBHS_EP_R_RES_NAK;
        EP_RX_MAX_LEN(ep_num) = 0;
        USBHSD->ENDP_TYPE &= ~(USBHS_EP0_R_TYP << ep_num);
        USBHSD->ENDP_CONFIG &= ~(USBHS_EP0_R_EN << ep_num);
    } else { // TUSB_DIR_IN
        EP_TX_CTRL(ep_num) = USBHS_EP_T_AUTOTOG | USBHS_EP_T_RES_NAK | USBHS_EP_T_TOG_0;
        EP_TX_LEN(ep_num) = 0;
        USBHSD->ENDP_TYPE &= ~(USBHS_EP0_T_TYP << ep_num);
        USBHSD->ENDP_CONFIG &= ~(USBHS_EP0_T_EN << ep_num);
    }
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;

    uint8_t const ep_num = tu_edpt_number(ep_addr);
    tusb_dir_t const dir = tu_edpt_dir(ep_addr);

    if (dir == TUSB_DIR_OUT) {
        EP_RX_CTRL(ep_num) = USBHS_EP_R_RES_STALL;
    } else {
        EP_TX_LEN(0) = 0;
        EP_TX_CTRL(ep_num) = USBHS_EP_T_RES_STALL;
    }
}

void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;

    uint8_t const ep_num = tu_edpt_number(ep_addr);
    tusb_dir_t const dir = tu_edpt_dir(ep_addr);

    if (dir == TUSB_DIR_OUT) {
        EP_RX_CTRL(ep_num) = USBHS_EP_R_AUTOTOG | USBHS_EP_R_RES_NAK;
    } else {
        EP_TX_CTRL(ep_num) = USBHS_EP_T_AUTOTOG | USBHS_EP_R_RES_NAK;
    }
}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes) {
    (void)rhport;
    uint8_t const ep_num = tu_edpt_number(ep_addr);
    tusb_dir_t const dir = tu_edpt_dir(ep_addr);

    xfer_ctl_t *xfer = XFER_CTL_BASE(ep_num, dir);
    xfer->buffer = buffer;
    xfer->total_len = total_bytes;
    xfer->queued_len = 0;
    xfer->is_last_packet = false;

    xfer_data_packet(ep_addr, xfer);

    return true;
}

void dcd_int_handler(uint8_t rhport) {
    (void)rhport;

    uint8_t int_flag = USBHSD->INT_FG;
    uint8_t int_status = USBHSD->INT_ST;

    if (int_flag & USBHS_TRANSFER_FLAG) {

        uint8_t ep_num = int_status & MASK_UIS_ENDP;
        uint8_t rx_token = int_status & MASK_UIS_TOKEN;

        uint8_t ep_addr = (rx_token == USBHS_TOKEN_PID_IN) ? (TUSB_DIR_IN_MASK | ep_num) : ep_num;

        xfer_ctl_t *xfer = XFER_CTL_BASE(ep_num, tu_edpt_dir(ep_addr));

        if (rx_token == USBHS_TOKEN_PID_OUT) {
            uint16_t rx_len = USBHSD->RX_LEN;

            if (ep_num == 0) {
                memcpy(&xfer->buffer[xfer->queued_len], ep0_data_in_out_buffer, rx_len);
            }

            xfer->queued_len += rx_len;
            if (rx_len < xfer->max_size) {
                xfer->is_last_packet = true;
            }

        } else if (rx_token == USBHS_TOKEN_PID_IN) {
            // Do nothing, no need to update xfer->is_last_packet, it is already updated in xfer_data_packet
            // Common processing below
        }

        if (xfer->is_last_packet == true) {
            ep_set_response_and_toggle(ep_addr, EP_RESPONSE_NAK);
            dcd_event_xfer_complete(0, ep_addr, xfer->queued_len, XFER_RESULT_SUCCESS, true);
        } else {
            /* prepare next part of packet to xref */
            xfer_data_packet(ep_addr, xfer);
        }

        USBHSD->INT_FG = USBHS_TRANSFER_FLAG; /* Clear flag */
    } else if (int_flag & USBHS_SETUP_FLAG) {
        ep_set_response_and_toggle(0x80, EP_RESPONSE_NAK);
        ep_set_response_and_toggle(0x00, EP_RESPONSE_NAK);
        dcd_event_setup_received(0, ep0_data_in_out_buffer, true);

        USBHSD->INT_FG = USBHS_SETUP_FLAG; /* Clear flag */
    } else if (int_flag & USBHS_DETECT_FLAG) {

        dcd_event_bus_reset(0, TUSB_SPEED_HIGH, true);

        USBHSD->DEV_AD = 0;
        EP_RX_CTRL(0) = USBHS_EP_R_RES_ACK | USBHS_EP_R_TOG_0;
        EP_TX_CTRL(0) = USBHS_EP_T_RES_NAK | USBHS_EP_T_TOG_0;

        USBHSD->INT_FG = USBHS_DETECT_FLAG; /* Clear flag */
    } else if (int_flag & USBHS_SUSPEND_FLAG) {
        dcd_event_t event = { .rhport = rhport, .event_id = DCD_EVENT_SUSPEND };
        dcd_event_handler(&event, true);

        USBHSD->INT_FG = USBHS_SUSPEND_FLAG; /* Clear flag */
    }
}

#endif
