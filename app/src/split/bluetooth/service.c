/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/drivers/sensor.h>
#include <zephyr/types.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/init.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <drivers/behavior.h>
#include <zmk/stdlib.h>
#include <zmk/behavior.h>
#include <zmk/matrix.h>
#include <zmk/physical_layouts.h>
#include <zmk/split/transport/peripheral.h>
#include <zmk/split/bluetooth/uuid.h>
#include <zmk/split/bluetooth/service.h>

#include "peripheral.h"
#include "relay_event.h"

#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
#include <zmk/events/hid_indicators_changed.h>
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)

#include <zmk/events/sensor_event.h>
#include <zmk/sensors.h>

#if ZMK_KEYMAP_HAS_SENSORS
static struct sensor_event last_sensor_event;

static ssize_t split_svc_sensor_state(struct bt_conn *conn, const struct bt_gatt_attr *attrs,
                                      void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attrs, buf, len, offset, &last_sensor_event,
                             sizeof(last_sensor_event));
}

static void split_svc_sensor_state_ccc(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("value %d", value);
}
#endif /* ZMK_KEYMAP_HAS_SENSORS */

#define POS_STATE_LEN 16

static uint8_t num_of_positions = ZMK_KEYMAP_LEN;
static uint8_t position_state[POS_STATE_LEN];

static struct zmk_split_run_behavior_payload behavior_run_payload;

static ssize_t split_svc_pos_state(struct bt_conn *conn, const struct bt_gatt_attr *attrs,
                                   void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attrs, buf, len, offset, &position_state,
                             sizeof(position_state));
}

static ssize_t split_svc_run_behavior(struct bt_conn *conn, const struct bt_gatt_attr *attrs,
                                      const void *buf, uint16_t len, uint16_t offset,
                                      uint8_t flags);

static ssize_t split_svc_num_of_positions(struct bt_conn *conn, const struct bt_gatt_attr *attrs,
                                          void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attrs, buf, len, offset, attrs->user_data, sizeof(uint8_t));
}

static void split_svc_pos_state_ccc(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("value %d", value);
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)

static void split_svc_relay_event_from_central_callback(struct k_work *work);

K_MSGQ_DEFINE(relay_event_from_central_msgq, sizeof(struct zmk_split_relay_event_payload),
              CONFIG_ZMK_SPLIT_BLE_PERIPHERAL_POSITION_QUEUE_SIZE, 4);
K_WORK_DEFINE(split_svc_relay_event_from_central_work, split_svc_relay_event_from_central_callback);

static void split_svc_relay_event_from_central_callback(struct k_work *work) {
    struct zmk_split_relay_event_payload payload;
    while (k_msgq_get(&relay_event_from_central_msgq, &payload, K_NO_WAIT) == 0) {
        struct zmk_relay_event_received relay_ev;
        relay_ev.source = 0; // Central is always source 0
        relay_ev.event_name = payload.event_type;
        relay_ev.event_data = payload.event_data;
        relay_ev.event_data_size = payload.header.event_data_size;
        raise_zmk_relay_event_received(relay_ev);
    }
}

static struct zmk_split_relay_event_chunk_reassembly_state
    relay_event_from_central_chunk_reassembly_state;

static ssize_t split_svc_relay_event_from_central(struct bt_conn *conn,
                                                  const struct bt_gatt_attr *attr, const void *buf,
                                                  uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset != 0 || len <= sizeof(struct relay_event_header)) {
        LOG_WRN("Invalid relay event chunk write from central: len=%u offset=%u flags=0x%02x", len,
                offset, flags);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    struct relay_event_header header;
    memcpy(&header, buf, sizeof(header));
    const uint8_t *chunk_data = (const uint8_t *)buf + sizeof(struct relay_event_header);
    uint16_t chunk_data_len = len - sizeof(struct relay_event_header);
    bool is_end = (header.sequence & ZMK_SPLIT_RELAY_EVENT_SEQUENCE_END) != 0;

    LOG_DBG("Received relay event chunk from central: seq=%u end=%d chunk_len=%u frame_len=%u "
            "type_len=%u state_received=%u",
            header.sequence & ZMK_SPLIT_RELAY_EVENT_SEQUENCE_MASK, is_end, chunk_data_len, len,
            header.event_type_size, relay_event_from_central_chunk_reassembly_state.received_size);

    int result = zmk_split_relay_event_chunk_reassembly_accept(
        &relay_event_from_central_chunk_reassembly_state, &header, chunk_data, chunk_data_len);
    if (result < 0) {
        LOG_WRN("Malformed relay event chunk from central: seq=%u end=%d chunk_len=%u "
                "frame_len=%u type_len=%u err=%d",
                header.sequence & ZMK_SPLIT_RELAY_EVENT_SEQUENCE_MASK, is_end, chunk_data_len, len,
                header.event_type_size, result);
        return len;
    }

    if (result == 0) {
        return len;
    }

    struct zmk_split_relay_event_payload *payload =
        &relay_event_from_central_chunk_reassembly_state.payload;

    k_msgq_put(&relay_event_from_central_msgq, payload, K_NO_WAIT);
    k_work_submit(&split_svc_relay_event_from_central_work);

    return len;
}

static void split_svc_relay_event_ccc(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("value %d", value);
}

#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)

static zmk_hid_indicators_t hid_indicators = 0;

static void split_svc_update_indicators_callback(struct k_work *work) {
    LOG_DBG("Raising HID indicators changed event: %x", hid_indicators);
    raise_zmk_hid_indicators_changed(
        (struct zmk_hid_indicators_changed){.indicators = hid_indicators});
}

static K_WORK_DEFINE(split_svc_update_indicators_work, split_svc_update_indicators_callback);

static ssize_t split_svc_update_indicators(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                           const void *buf, uint16_t len, uint16_t offset,
                                           uint8_t flags) {
    if (offset + len > sizeof(zmk_hid_indicators_t)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy((uint8_t *)&hid_indicators + offset, buf, len);

    k_work_submit(&split_svc_update_indicators_work);

    return len;
}

#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)

static uint8_t selected_phys_layout = 0;

static void split_svc_select_phys_layout_callback(struct k_work *work) {
    LOG_DBG("Selecting physical layout after GATT write of %d", selected_phys_layout);
    zmk_physical_layouts_select(selected_phys_layout);
}

static K_WORK_DEFINE(split_svc_select_phys_layout_work, split_svc_select_phys_layout_callback);

static ssize_t split_svc_select_phys_layout(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                            const void *buf, uint16_t len, uint16_t offset,
                                            uint8_t flags) {
    if (offset + len > sizeof(uint8_t) || len == 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    selected_phys_layout = *(uint8_t *)buf;

    k_work_submit(&split_svc_select_phys_layout_work);

    return len;
}

static ssize_t split_svc_get_selected_phys_layout(struct bt_conn *conn,
                                                  const struct bt_gatt_attr *attrs, void *buf,
                                                  uint16_t len, uint16_t offset) {
    int selected_ret = zmk_physical_layouts_get_selected();
    if (selected_ret < 0) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    uint8_t selected = (uint8_t)selected_ret;

    return bt_gatt_attr_read(conn, attrs, buf, len, offset, &selected, sizeof(selected));
}

#if IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)

static void split_input_events_ccc(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("value %d", value);
}

// Duplicated from Zephyr, since it is internal there
struct gatt_cpf {
    uint8_t format;
    int8_t exponent;
    uint16_t unit;
    uint8_t name_space;
    uint16_t description;
} __packed;

ssize_t bt_gatt_attr_read_input_split_cpf(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                          void *buf, uint16_t len, uint16_t offset) {
    uint16_t reg = (uint16_t)(uint32_t)attr->user_data;
    struct gatt_cpf value;

    value.format = 0x1B; // Struct
    value.exponent = 0;
    value.unit = sys_cpu_to_le16(0x2700); // Unitless
    value.name_space = 0x01;              // Bluetooth SIG
    value.description = sys_cpu_to_le16(reg);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
}

#define INPUT_SPLIT_CHARS(node_id)                                                                 \
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_INPUT_EVENT_UUID),                     \
                           BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT, NULL, NULL, NULL),      \
        BT_GATT_CCC(split_input_events_ccc,                                                        \
                    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),                       \
        BT_GATT_DESCRIPTOR(BT_UUID_GATT_CPF, BT_GATT_PERM_READ, bt_gatt_attr_read_input_split_cpf, \
                           NULL, (void *)DT_REG_ADDR(node_id)),

#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_CPI_FORWARD)

// Payload written by the central: | sensor_id:u8 | cpi_lo:u8 | cpi_hi:u8 |
struct zmk_split_cpi_forward_payload {
    uint8_t sensor_id;
    uint16_t cpi; // little-endian
} __packed;

// Weak implementation — returns -ENOTSUP unless a module (e.g. zmk-pmw3610-driver
// with CONFIG_PMW3610_ALT_SPLIT_CPI_PERIPHERAL=y) provides the strong override.
int __attribute__((weak)) zmk_split_peripheral_cpi_apply(uint8_t sensor_id, uint16_t cpi) {
    return -ENOTSUP;
}

static void cpi_forward_work_cb(struct k_work *work);
static K_WORK_DEFINE(cpi_forward_work, cpi_forward_work_cb);
static uint8_t cpi_fwd_sensor_id;
static uint16_t cpi_fwd_cpi;

static void cpi_forward_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    int ret = zmk_split_peripheral_cpi_apply(cpi_fwd_sensor_id, cpi_fwd_cpi);
    if (ret < 0) {
        LOG_WRN("zmk_split_peripheral_cpi_apply(%u, %u) failed: %d",
                cpi_fwd_sensor_id, cpi_fwd_cpi, ret);
    }
}

static ssize_t split_svc_cpi_forward(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                     const void *buf, uint16_t len, uint16_t offset,
                                     uint8_t flags) {
    if (offset != 0 || len != sizeof(struct zmk_split_cpi_forward_payload)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    const struct zmk_split_cpi_forward_payload *p =
        (const struct zmk_split_cpi_forward_payload *)buf;
    cpi_fwd_sensor_id = p->sensor_id;
    cpi_fwd_cpi = sys_le16_to_cpu(p->cpi);
    k_work_submit(&cpi_forward_work);
    return (ssize_t)len;
}

#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_CPI_FORWARD)

BT_GATT_SERVICE_DEFINE(
    split_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_POSITION_STATE_UUID),
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT,
                           split_svc_pos_state, NULL, &position_state),
    BT_GATT_CCC(split_svc_pos_state_ccc, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_RUN_BEHAVIOR_UUID),
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP, BT_GATT_PERM_WRITE_ENCRYPT, NULL,
                           split_svc_run_behavior, &behavior_run_payload),
    BT_GATT_DESCRIPTOR(BT_UUID_NUM_OF_DIGITALS, BT_GATT_PERM_READ, split_svc_num_of_positions, NULL,
                       &num_of_positions),
#if ZMK_KEYMAP_HAS_SENSORS
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_SENSOR_STATE_UUID),
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT,
                           split_svc_sensor_state, NULL, &last_sensor_event),
    BT_GATT_CCC(split_svc_sensor_state_ccc, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
#endif /* ZMK_KEYMAP_HAS_SENSORS */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_RELAY_EVENT_UUID),
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, split_svc_relay_event_from_central,
                           NULL),
    BT_GATT_CCC(split_svc_relay_event_ccc, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
#endif
    DT_FOREACH_STATUS_OKAY(zmk_input_split, INPUT_SPLIT_CHARS)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
        BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_UPDATE_HID_INDICATORS_UUID),
                               BT_GATT_CHRC_WRITE_WITHOUT_RESP, BT_GATT_PERM_WRITE_ENCRYPT, NULL,
                               split_svc_update_indicators, NULL),
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SELECT_PHYS_LAYOUT_UUID),
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_READ,
                           BT_GATT_PERM_WRITE_ENCRYPT | BT_GATT_PERM_READ_ENCRYPT,
                           split_svc_get_selected_phys_layout, split_svc_select_phys_layout,
                           NULL),
#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_CPI_FORWARD)
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CPI_FORWARD_UUID),
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP, BT_GATT_PERM_WRITE_ENCRYPT,
                           NULL, split_svc_cpi_forward, NULL),
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_CPI_FORWARD)
    );

K_THREAD_STACK_DEFINE(service_q_stack, CONFIG_ZMK_SPLIT_BLE_PERIPHERAL_STACK_SIZE);

struct k_work_q service_work_q;

K_MSGQ_DEFINE(position_state_msgq, sizeof(char[POS_STATE_LEN]),
              CONFIG_ZMK_SPLIT_BLE_PERIPHERAL_POSITION_QUEUE_SIZE, 4);

void send_position_state_callback(struct k_work *work) {
    uint8_t state[POS_STATE_LEN];

    while (k_msgq_get(&position_state_msgq, &state, K_NO_WAIT) == 0) {
        int err = bt_gatt_notify(NULL, &split_svc.attrs[1], &state, sizeof(state));
        if (err) {
            LOG_DBG("Error notifying %d", err);
        }
    }
};

K_WORK_DEFINE(service_position_notify_work, send_position_state_callback);

int send_position_state() {
    int err = k_msgq_put(&position_state_msgq, position_state, K_MSEC(100));
    if (err) {
        switch (err) {
        case -EAGAIN: {
            LOG_WRN("Position state message queue full, popping first message and queueing again");
            uint8_t discarded_state[POS_STATE_LEN];
            k_msgq_get(&position_state_msgq, &discarded_state, K_NO_WAIT);
            return send_position_state();
        }
        default:
            LOG_WRN("Failed to queue position state to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&service_work_q, &service_position_notify_work);

    return 0;
}

static int zmk_split_bt_position_pressed(uint8_t position) {
    WRITE_BIT(position_state[position / 8], position % 8, true);
    return send_position_state();
}

static int zmk_split_bt_position_released(uint8_t position) {
    WRITE_BIT(position_state[position / 8], position % 8, false);
    return send_position_state();
}

#if ZMK_KEYMAP_HAS_SENSORS
K_MSGQ_DEFINE(sensor_state_msgq, sizeof(struct sensor_event),
              CONFIG_ZMK_SPLIT_BLE_PERIPHERAL_POSITION_QUEUE_SIZE, 4);

void send_sensor_state_callback(struct k_work *work) {
    while (k_msgq_get(&sensor_state_msgq, &last_sensor_event, K_NO_WAIT) == 0) {
        int err = bt_gatt_notify(NULL, &split_svc.attrs[8], &last_sensor_event,
                                 sizeof(last_sensor_event));
        if (err) {
            LOG_DBG("Error notifying %d", err);
        }
    }
};

K_WORK_DEFINE(service_sensor_notify_work, send_sensor_state_callback);

int send_sensor_state(struct sensor_event ev) {
    int err = k_msgq_put(&sensor_state_msgq, &ev, K_MSEC(100));
    if (err) {
        // retry...
        switch (err) {
        case -EAGAIN: {
            LOG_WRN("Sensor state message queue full, popping first message and queueing again");
            struct sensor_event discarded_state;
            k_msgq_get(&sensor_state_msgq, &discarded_state, K_NO_WAIT);
            return send_sensor_state(ev);
        }
        default:
            LOG_WRN("Failed to queue sensor state to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&service_work_q, &service_sensor_notify_work);
    return 0;
}

static int zmk_split_bt_sensor_triggered(uint8_t sensor_index,
                                         const struct zmk_sensor_channel_data channel_data[],
                                         size_t channel_data_size) {
    if (channel_data_size > ZMK_SENSOR_EVENT_MAX_CHANNELS) {
        return -EINVAL;
    }

    struct sensor_event ev =
        (struct sensor_event){.sensor_index = sensor_index, .channel_data_size = channel_data_size};
    memcpy(ev.channel_data, channel_data,
           channel_data_size * sizeof(struct zmk_sensor_channel_data));
    return send_sensor_state(ev);
}
#endif /* ZMK_KEYMAP_HAS_SENSORS */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)

static struct zmk_split_relay_event_payload last_relay_event;
static uint8_t relay_event_tx_sequence;
static uint8_t relay_event_frame[sizeof(struct relay_event_header) +
                                 CONFIG_ZMK_SPLIT_RELAY_EVENT_TYPE_NAME_LEN +
                                 CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN];
K_MSGQ_DEFINE(relay_event_msgq, sizeof(struct zmk_split_relay_event_payload),
              CONFIG_ZMK_SPLIT_BLE_PERIPHERAL_POSITION_QUEUE_SIZE, 4);

static void split_svc_find_first_conn(struct bt_conn *conn, void *data) {
    struct bt_conn **found = data;

    if (*found == NULL) {
        *found = bt_conn_ref(conn);
    }
}

static struct bt_conn *split_svc_current_conn(void) {
    struct bt_conn *conn = NULL;

    bt_conn_foreach(BT_CONN_TYPE_LE, split_svc_find_first_conn, &conn);
    return conn;
}

static int
split_svc_notify_relay_event_chunks(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    const struct zmk_split_relay_event_payload *payload) {
    uint16_t total_size;
    int err = zmk_split_relay_event_payload_total_size(payload, &total_size);
    if (err < 0) {
        LOG_WRN("Invalid relay event payload (err %d)", err);
        return err;
    }

    uint16_t mtu = bt_gatt_get_mtu(conn);
    if (mtu <= ZMK_SPLIT_RELAY_EVENT_ATT_VALUE_OVERHEAD + sizeof(struct relay_event_header)) {
        LOG_WRN("ATT MTU %u too small for relay event chunk header", mtu);
        return -EMSGSIZE;
    }

    uint16_t max_frame_len = mtu - ZMK_SPLIT_RELAY_EVENT_ATT_VALUE_OVERHEAD;
    uint16_t max_chunk_data_len = max_frame_len - sizeof(struct relay_event_header);
    max_chunk_data_len = MIN(max_chunk_data_len, UINT8_MAX);

    uint16_t sent_size = 0;

    LOG_DBG("Prepared relay event of total size %u bytes (name %u, data %u)", total_size,
            payload->header.event_type_size, payload->header.event_data_size);

    while (sent_size < total_size) {
        uint16_t chunk_data_len = MIN(max_chunk_data_len, total_size - sent_size);
        bool is_end = (sent_size + chunk_data_len) == total_size;
        struct relay_event_header *header = (struct relay_event_header *)relay_event_frame;

        *header = (struct relay_event_header){
            .sequence = relay_event_tx_sequence & ZMK_SPLIT_RELAY_EVENT_SEQUENCE_MASK,
            .event_type_size = payload->header.event_type_size,
            .event_data_size = chunk_data_len,
        };
        if (is_end) {
            header->sequence |= ZMK_SPLIT_RELAY_EVENT_SEQUENCE_END;
        }
        relay_event_tx_sequence =
            (relay_event_tx_sequence + 1) & ZMK_SPLIT_RELAY_EVENT_SEQUENCE_MASK;

        zmk_split_relay_event_copy_payload_data(
            payload, sent_size, relay_event_frame + sizeof(struct relay_event_header),
            chunk_data_len);

        uint16_t frame_len = sizeof(struct relay_event_header) + chunk_data_len;
        LOG_DBG("Notifying relay event chunk to central: seq=%u end=%d chunk_len=%u "
                "frame_len=%u sent=%u total=%u mtu=%u",
                header->sequence & ZMK_SPLIT_RELAY_EVENT_SEQUENCE_MASK, is_end, chunk_data_len,
                frame_len, sent_size, total_size, mtu);
        err = bt_gatt_notify(conn, attr, relay_event_frame, frame_len);
        if (err < 0) {
            LOG_WRN("Error notifying relay event chunk: seq=%u end=%d err=%d",
                    header->sequence & ZMK_SPLIT_RELAY_EVENT_SEQUENCE_MASK, is_end, err);
            return err;
        }

        sent_size += chunk_data_len;
    }

    return 0;
}

void send_relay_event_callback(struct k_work *work) {
    while (k_msgq_get(&relay_event_msgq, &last_relay_event, K_NO_WAIT) == 0) {
        LOG_DBG("Dequeued relay event to notify: type_len=%u data_len=%u",
                last_relay_event.header.event_type_size, last_relay_event.header.event_data_size);

        // Find the relay event characteristic attribute index
        size_t relay_attr_index = 0;
        for (size_t i = 0; i < split_svc.attr_count; i++) {
            if (bt_uuid_cmp(split_svc.attrs[i].uuid,
                            BT_UUID_DECLARE_128(ZMK_SPLIT_BT_RELAY_EVENT_UUID)) == 0) {
                relay_attr_index = i;
                break;
            }
        }

        if (relay_attr_index > 0) {
            struct bt_conn *conn = split_svc_current_conn();
            if (!conn) {
                LOG_WRN("No central connection to notify relay event");
                continue;
            }

            int err = split_svc_notify_relay_event_chunks(conn, &split_svc.attrs[relay_attr_index],
                                                          &last_relay_event);
            bt_conn_unref(conn);

            if (!err) {
                LOG_DBG("Notified relay event: type_len=%u data_len=%u",
                        last_relay_event.header.event_type_size,
                        last_relay_event.header.event_data_size);
            }
        } else {
            LOG_WRN("Could not find relay event characteristic to send relay event");
        }
    }
};

K_WORK_DEFINE(service_relay_event_notify_work, send_relay_event_callback);

int zmk_split_bt_send_relay_event(const struct zmk_split_relay_event_payload *ev) {
    int err = k_msgq_put(&relay_event_msgq, ev, K_MSEC(100));
    if (err) {
        switch (err) {
        case -EAGAIN: {
            LOG_WRN("Relay event message queue full, popping first message and queueing again");
            struct zmk_split_relay_event_payload discarded_ev;
            k_msgq_get(&relay_event_msgq, &discarded_ev, K_NO_WAIT);
            return zmk_split_bt_send_relay_event(ev);
        }
        default:
            LOG_WRN("Failed to queue relay event to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&service_work_q, &service_relay_event_notify_work);
    return 0;
}

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT) */

#if IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)

static int zmk_split_bt_report_input(uint8_t reg, uint8_t type, uint16_t code, int32_t value,
                                     bool sync) {

    for (size_t i = 0; i < split_svc.attr_count; i++) {
        if (bt_uuid_cmp(split_svc.attrs[i].uuid,
                        BT_UUID_DECLARE_128(ZMK_SPLIT_BT_INPUT_EVENT_UUID)) == 0 &&
            (uint8_t)(uint32_t)split_svc.attrs[i + 2].user_data == reg) {
            struct zmk_split_input_event_payload payload = {
                .type = type,
                .code = code,
                .value = value,
                .sync = sync ? 1 : 0,
            };

            return bt_gatt_notify(NULL, &split_svc.attrs[i], &payload, sizeof(payload));
        }
    }
    return -ENODEV;
}

#endif /* IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT) */

static int service_init(void) {
    static const struct k_work_queue_config queue_config = {
        .name = "Split Peripheral Notification Queue"};
    k_work_queue_start(&service_work_q, service_q_stack, K_THREAD_STACK_SIZEOF(service_q_stack),
                       CONFIG_ZMK_SPLIT_BLE_PERIPHERAL_PRIORITY, &queue_config);

    return 0;
}

SYS_INIT(service_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);

int zmk_split_transport_peripheral_bt_report_event(
    const struct zmk_split_transport_peripheral_event *ev) {
    switch (ev->type) {
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT:
        if (ev->data.key_position_event.pressed) {
            zmk_split_bt_position_pressed(ev->data.key_position_event.position);
        } else {
            zmk_split_bt_position_released(ev->data.key_position_event.position);
        }
        break;
#if ZMK_KEYMAP_HAS_SENSORS
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT:
        zmk_split_bt_sensor_triggered(ev->data.sensor_event.sensor_index,
                                      &ev->data.sensor_event.channel_data, 1);

        break;
#endif

#if IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT)
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT:
        return zmk_split_bt_report_input(ev->data.input_event.reg, ev->data.input_event.type,
                                         ev->data.input_event.code, ev->data.input_event.value,
                                         ev->data.input_event.sync);
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT:
        // The BLE transport uses standard BAS service for propagation, so just return success
        // here.
        return 0;
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_RELAY_EVENT:
        struct zmk_split_relay_event_payload relay_ev = {
            .header = ev->data.relay_event.header,
        };
        relay_ev.header.event_type_size =
            strnlen(ev->data.relay_event.event_type, CONFIG_ZMK_SPLIT_RELAY_EVENT_TYPE_NAME_LEN);
        memcpy(relay_ev.event_type, ev->data.relay_event.event_type,
               relay_ev.header.event_type_size);
        relay_ev.event_type[relay_ev.header.event_type_size] = '\0';
        memcpy(relay_ev.event_data, ev->data.relay_event.event_data,
               ev->data.relay_event.header.event_data_size);
        return zmk_split_bt_send_relay_event(&relay_ev);
#endif
    default:
        LOG_WRN("Unhandled event type %d", ev->type);
        return -ENOTSUP;
    }

    return 0;
}

static ssize_t split_svc_run_behavior(struct bt_conn *conn, const struct bt_gatt_attr *attrs,
                                      const void *buf, uint16_t len, uint16_t offset,
                                      uint8_t flags) {
    struct zmk_split_run_behavior_payload *payload = attrs->user_data;
    uint16_t end_addr = offset + len;

    LOG_DBG("offset %d len %d", offset, len);

    if (end_addr > sizeof(struct zmk_split_run_behavior_payload)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(payload + offset, buf, len);

    // We run if:
    // 1: We've gotten all the position/state/param data.
    // 2: We have a null terminated string for the behavior device label.
    const size_t behavior_dev_offset =
        offsetof(struct zmk_split_run_behavior_payload, behavior_dev);
    if ((end_addr > sizeof(struct zmk_split_run_behavior_data)) &&
        payload->behavior_dev[end_addr - behavior_dev_offset - 1] == '\0') {

        struct zmk_split_transport_central_command cmd = {
            .type = ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR,
            .data = {.invoke_behavior = {
                         .param1 = payload->data.param1,
                         .param2 = payload->data.param2,
                         .position = payload->data.position,
                         .state = payload->data.state,
                     }}};

        const size_t payload_dev_size = sizeof(cmd.data.invoke_behavior.behavior_dev);
        if (strlcpy(cmd.data.invoke_behavior.behavior_dev, payload->behavior_dev,
                    payload_dev_size) >= payload_dev_size) {
            LOG_ERR("Truncated behavior label %s to %s before invoking peripheral behavior",
                    payload->behavior_dev, cmd.data.invoke_behavior.behavior_dev);
        }

        LOG_DBG("%s with params %d %d: pressed? %d", cmd.data.invoke_behavior.behavior_dev,
                cmd.data.invoke_behavior.param1, cmd.data.invoke_behavior.param2,
                cmd.data.invoke_behavior.state);

        int err = zmk_split_transport_peripheral_command_handler(
            zmk_split_transport_peripheral_bt(), cmd);

        if (err) {
            LOG_ERR("Failed to invoke behavior %s: %d", payload->behavior_dev, err);
        }
    }

    return len;
}
