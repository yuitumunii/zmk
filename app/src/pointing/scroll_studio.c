/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Custom Studio RPC subsystem "pyuron_scroll" — lets ZMK Studio live-toggle
 * scroll direction inversion (vertical / horizontal) without reflashing.
 * Wire format: proto/pyuron/scroll/scroll.proto.
 *
 * Modelled after aml_studio.c (same subsystem registration pattern).
 */

#include <string.h>
#include <errno.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <zmk/pointing/scroll_invert.h>
#include <pyuron/scroll/scroll.pb.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta scroll_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://github.com/yuitumunii/zmk"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(pyuron_scroll, &scroll_feature_meta, scroll_rpc_handle_request);
ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(pyuron_scroll, pyuron_scroll_Response);

static void fill_info_response(pyuron_scroll_Response *resp) {
    struct zmk_scroll_invert_config cfg;
    zmk_scroll_invert_get(&cfg);

    resp->which_response_type = pyuron_scroll_Response_info_tag;
    pyuron_scroll_ScrollInfo *info = &resp->response_type.info;
    *info = (pyuron_scroll_ScrollInfo)pyuron_scroll_ScrollInfo_init_zero;
    info->invert_v = cfg.invert_v;
    info->invert_h = cfg.invert_h;
}

static bool scroll_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                      pb_callback_t *encode_response) {
    pyuron_scroll_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(pyuron_scroll, encode_response);

    pyuron_scroll_Request req = pyuron_scroll_Request_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(raw_request->payload.bytes,
                                                  raw_request->payload.size);
    if (!pb_decode(&stream, pyuron_scroll_Request_fields, &req)) {
        LOG_WRN("scroll_rpc: decode error: %s", PB_GET_ERROR(&stream));
        resp->which_response_type = pyuron_scroll_Response_error_tag;
        snprintf(resp->response_type.error.message,
                 sizeof(resp->response_type.error.message),
                 "decode error");
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case pyuron_scroll_Request_get_tag:
        /* Nothing to do — fill_info_response below covers the reply. */
        break;
    case pyuron_scroll_Request_set_tag: {
        const pyuron_scroll_SetRequest *s = &req.request_type.set;
        rc = zmk_scroll_invert_set(s->invert_v, s->invert_h);
        if (rc == 0) {
            zmk_scroll_invert_save();
        }
        break;
    }
    default:
        LOG_WRN("scroll_rpc: unknown request type %d", req.which_request_type);
        rc = -ENOTSUP;
    }

    if (rc != 0) {
        resp->which_response_type = pyuron_scroll_Response_error_tag;
        snprintf(resp->response_type.error.message,
                 sizeof(resp->response_type.error.message),
                 "error %d", rc);
        return true;
    }

    fill_info_response(resp);
    return true;
}
