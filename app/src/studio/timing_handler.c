/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Custom Studio RPC subsystem for live tuning of hold-tap (&mt / &lt) timing:
 * tapping-term-ms / quick-tap-ms / flavor. Lets the ZMK Studio app fix
 * "hold became a tap / tap became a hold" at runtime, without reflashing. Built
 * on the custom subsystem support in the (cormoran) custom-studio-protocol ZMK
 * base.
 *
 * Each response carries at most one HoldTapInfo so it fits the Studio RPC TX
 * buffer; the app probes get_count then get(0..count-1) instead of receiving
 * a list, so we need no async notifications here.
 */

#include <string.h>
#include <errno.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <zmk/behaviors/hold_tap_tuning.h>
#include <pyuron/timing/timing.pb.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta timing_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://github.com/yuitumunii/zmk"),
    // Unsecured so the desktop app can tune without unlocking; these are
    // comfort params, not security-sensitive.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(pyuron_timing, &timing_feature_meta, timing_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(pyuron_timing, pyuron_timing_Response);

// Fill a protobuf HoldTapInfo from the live state of instance `id`.
static int fill_hold_tap_info(uint32_t id, pyuron_timing_HoldTapInfo *info) {
    struct hold_tap_tuning_info src;
    int ret = hold_tap_tuning_get_info(id, &src);
    if (ret < 0) {
        return ret;
    }

    *info = (pyuron_timing_HoldTapInfo)pyuron_timing_HoldTapInfo_init_zero;
    info->id = id;
    strncpy(info->name, src.name ? src.name : "", sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->tapping_term_ms = src.tapping_term_ms;
    info->quick_tap_ms = src.quick_tap_ms;
    info->flavor = src.flavor;
    return 0;
}

static int handle_get_count(const pyuron_timing_GetCountRequest *req,
                            pyuron_timing_Response *resp) {
    ARG_UNUSED(req);
    resp->which_response_type = pyuron_timing_Response_get_count_tag;
    resp->response_type.get_count.count = (uint32_t)hold_tap_tuning_get_count();
    return 0;
}

static int handle_get(const pyuron_timing_GetRequest *req, pyuron_timing_Response *resp) {
    resp->which_response_type = pyuron_timing_Response_get_tag;
    resp->response_type.get = (pyuron_timing_GetResponse)pyuron_timing_GetResponse_init_zero;
    resp->response_type.get.has_info = true; // nanopb: submessage presence flag
    return fill_hold_tap_info(req->id, &resp->response_type.get.info);
}

static int handle_set_param(const pyuron_timing_SetParamRequest *req,
                            pyuron_timing_Response *resp) {
    if (req->param < pyuron_timing_Param_PARAM_TAPPING_TERM_MS ||
        req->param > pyuron_timing_Param_PARAM_FLAVOR) {
        return -EINVAL;
    }

    LOG_DBG("set timing id=%u param=%d value=%d", req->id, (int)req->param, (int)req->value);

    int ret = hold_tap_tuning_set_param(req->id, (enum hold_tap_tuning_param)req->param, req->value);
    if (ret < 0) {
        return ret;
    }
    // Persist so the tuned value survives reboot.
    hold_tap_tuning_save_param(req->id, (enum hold_tap_tuning_param)req->param, req->value);

    resp->which_response_type = pyuron_timing_Response_set_param_tag;
    resp->response_type.set_param =
        (pyuron_timing_SetParamResponse)pyuron_timing_SetParamResponse_init_zero;
    resp->response_type.set_param.has_info = true;
    return fill_hold_tap_info(req->id, &resp->response_type.set_param.info);
}

static int handle_reset(const pyuron_timing_ResetRequest *req, pyuron_timing_Response *resp) {
    int ret = hold_tap_tuning_reset(req->id);
    if (ret < 0) {
        return ret;
    }

    resp->which_response_type = pyuron_timing_Response_reset_tag;
    resp->response_type.reset = (pyuron_timing_ResetResponse)pyuron_timing_ResetResponse_init_zero;
    resp->response_type.reset.has_info = true;
    return fill_hold_tap_info(req->id, &resp->response_type.reset.info);
}

static bool timing_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                      pb_callback_t *encode_response) {
    pyuron_timing_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(pyuron_timing, encode_response);

    pyuron_timing_Request req = pyuron_timing_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, pyuron_timing_Request_fields, &req)) {
        LOG_WRN("Failed to decode timing request: %s", PB_GET_ERROR(&req_stream));
        pyuron_timing_ErrorResponse err = pyuron_timing_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = pyuron_timing_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case pyuron_timing_Request_get_count_tag:
        rc = handle_get_count(&req.request_type.get_count, resp);
        break;
    case pyuron_timing_Request_get_tag:
        rc = handle_get(&req.request_type.get, resp);
        break;
    case pyuron_timing_Request_set_param_tag:
        rc = handle_set_param(&req.request_type.set_param, resp);
        break;
    case pyuron_timing_Request_reset_tag:
        rc = handle_reset(&req.request_type.reset, resp);
        break;
    default:
        LOG_WRN("Unsupported timing request type: %d", req.which_request_type);
        rc = -1;
    }

    if (rc != 0) {
        pyuron_timing_ErrorResponse err = pyuron_timing_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to process request (%d)", rc);
        resp->which_response_type = pyuron_timing_Response_error_tag;
        resp->response_type.error = err;
    }
    return true;
}
