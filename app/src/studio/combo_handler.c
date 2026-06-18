/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Custom Studio RPC subsystem (pyuron_combo) for live editing of fixed combo
 * slots (6). A combo's key set changes the lookup table, so each slot is set
 * atomically with a single set request. The app probes get_count then
 * get(0..count-1). Modeled 1:1 on src/studio/timing_handler.c.
 */

#include <string.h>
#include <errno.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <zmk/combo_tuning.h>
#include <pyuron/combo/combo.pb.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta combo_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://github.com/yuitumunii/zmk"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(pyuron_combo, &combo_feature_meta, combo_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(pyuron_combo, pyuron_combo_Response);

static int fill_combo_info(uint32_t id, pyuron_combo_ComboInfo *info) {
    struct pyuron_combo_slot s;
    int ret = pyuron_combo_get((uint8_t)id, &s);
    if (ret < 0) {
        return ret;
    }

    *info = (pyuron_combo_ComboInfo)pyuron_combo_ComboInfo_init_zero;
    info->id = id;
    info->key_positions_count = s.key_len;
    for (int i = 0; i < s.key_len && i < PYURON_COMBO_MAX_KEYS; i++) {
        info->key_positions[i] = (uint32_t)s.key_positions[i];
    }
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    info->behavior_id = s.behavior.local_id;
#else
    info->behavior_id = 0;
#endif
    info->param1 = (int32_t)s.behavior.param1;
    info->param2 = (int32_t)s.behavior.param2;
    info->timeout_ms = (uint32_t)s.timeout_ms;
    info->layer_mask = s.layer_mask;
    info->enabled = s.enabled;
    return 0;
}

static int handle_get_count(const pyuron_combo_GetCountRequest *req,
                            pyuron_combo_Response *resp) {
    ARG_UNUSED(req);
    resp->which_response_type = pyuron_combo_Response_get_count_tag;
    resp->response_type.get_count.count = (uint32_t)pyuron_combo_get_count();
    return 0;
}

static int handle_get(const pyuron_combo_GetRequest *req, pyuron_combo_Response *resp) {
    resp->which_response_type = pyuron_combo_Response_get_tag;
    resp->response_type.get = (pyuron_combo_GetResponse)pyuron_combo_GetResponse_init_zero;
    resp->response_type.get.has_info = true;
    return fill_combo_info(req->id, &resp->response_type.get.info);
}

static int handle_set(const pyuron_combo_SetRequest *req, pyuron_combo_Response *resp) {
    if (req->id >= PYURON_COMBO_SLOTS || req->key_positions_count > PYURON_COMBO_MAX_KEYS) {
        return -EINVAL;
    }
    int32_t keys[PYURON_COMBO_MAX_KEYS] = {0};
    for (pb_size_t i = 0; i < req->key_positions_count && i < PYURON_COMBO_MAX_KEYS; i++) {
        keys[i] = (int32_t)req->key_positions[i];
    }
    int ret = pyuron_combo_set((uint8_t)req->id, keys, (uint8_t)req->key_positions_count,
                               req->behavior_id, req->param1, req->param2, req->timeout_ms,
                               req->layer_mask, req->enabled);
    if (ret < 0) {
        return ret;
    }
    resp->which_response_type = pyuron_combo_Response_set_tag;
    resp->response_type.set = (pyuron_combo_SetResponse)pyuron_combo_SetResponse_init_zero;
    resp->response_type.set.has_info = true;
    return fill_combo_info(req->id, &resp->response_type.set.info);
}

static int handle_clear(const pyuron_combo_ClearRequest *req, pyuron_combo_Response *resp) {
    int ret = pyuron_combo_clear((uint8_t)req->id);
    if (ret < 0) {
        return ret;
    }
    resp->which_response_type = pyuron_combo_Response_clear_tag;
    resp->response_type.clear = (pyuron_combo_ClearResponse)pyuron_combo_ClearResponse_init_zero;
    resp->response_type.clear.has_info = true;
    return fill_combo_info(req->id, &resp->response_type.clear.info);
}

static bool combo_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                     pb_callback_t *encode_response) {
    pyuron_combo_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(pyuron_combo, encode_response);

    pyuron_combo_Request req = pyuron_combo_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, pyuron_combo_Request_fields, &req)) {
        LOG_WRN("Failed to decode combo request: %s", PB_GET_ERROR(&req_stream));
        pyuron_combo_ErrorResponse err = pyuron_combo_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = pyuron_combo_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case pyuron_combo_Request_get_count_tag:
        rc = handle_get_count(&req.request_type.get_count, resp);
        break;
    case pyuron_combo_Request_get_tag:
        rc = handle_get(&req.request_type.get, resp);
        break;
    case pyuron_combo_Request_set_tag:
        rc = handle_set(&req.request_type.set, resp);
        break;
    case pyuron_combo_Request_clear_tag:
        rc = handle_clear(&req.request_type.clear, resp);
        break;
    default:
        LOG_WRN("Unsupported combo request type: %d", req.which_request_type);
        rc = -1;
    }

    if (rc != 0) {
        pyuron_combo_ErrorResponse err = pyuron_combo_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to process request (%d)", rc);
        resp->which_response_type = pyuron_combo_Response_error_tag;
        resp->response_type.error = err;
    }
    return true;
}
