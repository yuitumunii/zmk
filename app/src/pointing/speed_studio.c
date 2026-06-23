/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Custom Studio RPC subsystem "pyuron_speed" — lets ZMK Studio live-change the
 * trackball cursor speed multiplier (percent) without reflashing.
 * Wire format: proto/pyuron/speed/speed.proto. Modelled after scroll_studio.c.
 */

#include <string.h>
#include <errno.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <zmk/pointing/speed.h>
#include <pyuron/speed/speed.pb.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta speed_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://github.com/yuitumunii/zmk"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(pyuron_speed, &speed_feature_meta, speed_rpc_handle_request);
ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(pyuron_speed, pyuron_speed_Response);

static void fill_info_response(pyuron_speed_Response *resp) {
    struct zmk_speed_config cfg;
    zmk_speed_get(&cfg);
    resp->which_response_type = pyuron_speed_Response_info_tag;
    pyuron_speed_SpeedInfo *info = &resp->response_type.info;
    *info = (pyuron_speed_SpeedInfo)pyuron_speed_SpeedInfo_init_zero;
    info->percent = cfg.percent;
    info->accel_on = cfg.accel_on;
    info->accel_strength = cfg.accel_strength;
}

static bool speed_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                     pb_callback_t *encode_response) {
    pyuron_speed_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(pyuron_speed, encode_response);

    pyuron_speed_Request req = pyuron_speed_Request_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(raw_request->payload.bytes,
                                                  raw_request->payload.size);
    if (!pb_decode(&stream, pyuron_speed_Request_fields, &req)) {
        LOG_WRN("speed_rpc: decode error: %s", PB_GET_ERROR(&stream));
        resp->which_response_type = pyuron_speed_Response_error_tag;
        snprintf(resp->response_type.error.message,
                 sizeof(resp->response_type.error.message), "decode error");
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case pyuron_speed_Request_get_tag:
        break;
    case pyuron_speed_Request_set_tag:
        rc = zmk_speed_set(req.request_type.set.percent);
        if (rc == 0) {
            rc = zmk_speed_set_accel(req.request_type.set.accel_on,
                                     req.request_type.set.accel_strength);
        }
        if (rc == 0) { zmk_speed_save(); }
        break;
    default:
        LOG_WRN("speed_rpc: unknown request type %d", req.which_request_type);
        rc = -ENOTSUP;
    }

    if (rc != 0) {
        resp->which_response_type = pyuron_speed_Response_error_tag;
        snprintf(resp->response_type.error.message,
                 sizeof(resp->response_type.error.message), "error %d", rc);
        return true;
    }

    fill_info_response(resp);
    return true;
}
