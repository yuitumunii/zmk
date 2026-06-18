/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Custom Studio RPC subsystem (pyuron_tapdance) for live editing of fixed
 * tap-dance slots (&ptd0..&ptd3). The app probes get_count then get(0..count-1)
 * and edits step-by-step (set_step / set_term / set_len / clear). Modeled 1:1 on
 * src/studio/timing_handler.c.
 */

#include <string.h>
#include <errno.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <zmk/behaviors/tapdance_tuning.h>
#include <pyuron/tapdance/tapdance.pb.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta tapdance_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://github.com/yuitumunii/zmk"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(pyuron_tapdance, &tapdance_feature_meta, tapdance_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(pyuron_tapdance, pyuron_tapdance_Response);

static int fill_tapdance_info(uint32_t id, pyuron_tapdance_TapDanceInfo *info) {
    struct pyuron_td_slot s;
    int ret = pyuron_td_get((uint8_t)id, &s);
    if (ret < 0) {
        return ret;
    }

    *info = (pyuron_tapdance_TapDanceInfo)pyuron_tapdance_TapDanceInfo_init_zero;
    info->id = id;
    info->tapping_term_ms = s.tapping_term_ms;
    info->count_len = s.count_len;
    info->steps_count = s.count_len; // nanopb repeated count
    for (int i = 0; i < s.count_len && i < PYURON_TD_MAX_COUNT; i++) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        info->steps[i].behavior_id = s.steps[i].local_id;
#else
        info->steps[i].behavior_id = 0;
#endif
        info->steps[i].param1 = (int32_t)s.steps[i].param1;
        info->steps[i].param2 = (int32_t)s.steps[i].param2;
    }
    return 0;
}

static int handle_get_count(const pyuron_tapdance_GetCountRequest *req,
                            pyuron_tapdance_Response *resp) {
    ARG_UNUSED(req);
    resp->which_response_type = pyuron_tapdance_Response_get_count_tag;
    resp->response_type.get_count.count = (uint32_t)pyuron_td_get_count();
    return 0;
}

static int handle_get(const pyuron_tapdance_GetRequest *req, pyuron_tapdance_Response *resp) {
    resp->which_response_type = pyuron_tapdance_Response_get_tag;
    resp->response_type.get = (pyuron_tapdance_GetResponse)pyuron_tapdance_GetResponse_init_zero;
    resp->response_type.get.has_info = true;
    return fill_tapdance_info(req->id, &resp->response_type.get.info);
}

static int handle_set_step(const pyuron_tapdance_SetStepRequest *req,
                           pyuron_tapdance_Response *resp) {
    if (req->id >= PYURON_TD_SLOTS || req->index >= PYURON_TD_MAX_COUNT) {
        return -EINVAL;
    }
    int ret = pyuron_td_set_step((uint8_t)req->id, (uint8_t)req->index, req->behavior_id,
                                 req->param1, req->param2);
    if (ret < 0) {
        return ret;
    }
    resp->which_response_type = pyuron_tapdance_Response_set_step_tag;
    resp->response_type.set_step =
        (pyuron_tapdance_SetStepResponse)pyuron_tapdance_SetStepResponse_init_zero;
    resp->response_type.set_step.has_info = true;
    return fill_tapdance_info(req->id, &resp->response_type.set_step.info);
}

static int handle_set_term(const pyuron_tapdance_SetTermRequest *req,
                           pyuron_tapdance_Response *resp) {
    if (req->id >= PYURON_TD_SLOTS) {
        return -EINVAL;
    }
    int ret = pyuron_td_set_term((uint8_t)req->id, req->tapping_term_ms);
    if (ret < 0) {
        return ret;
    }
    resp->which_response_type = pyuron_tapdance_Response_set_term_tag;
    resp->response_type.set_term =
        (pyuron_tapdance_SetTermResponse)pyuron_tapdance_SetTermResponse_init_zero;
    resp->response_type.set_term.has_info = true;
    return fill_tapdance_info(req->id, &resp->response_type.set_term.info);
}

static int handle_set_len(const pyuron_tapdance_SetLenRequest *req,
                          pyuron_tapdance_Response *resp) {
    if (req->id >= PYURON_TD_SLOTS || req->len < 1 || req->len > PYURON_TD_MAX_COUNT) {
        return -EINVAL;
    }
    int ret = pyuron_td_set_len((uint8_t)req->id, (uint8_t)req->len);
    if (ret < 0) {
        return ret;
    }
    resp->which_response_type = pyuron_tapdance_Response_set_len_tag;
    resp->response_type.set_len =
        (pyuron_tapdance_SetLenResponse)pyuron_tapdance_SetLenResponse_init_zero;
    resp->response_type.set_len.has_info = true;
    return fill_tapdance_info(req->id, &resp->response_type.set_len.info);
}

static int handle_clear(const pyuron_tapdance_ClearRequest *req, pyuron_tapdance_Response *resp) {
    int ret = pyuron_td_clear((uint8_t)req->id);
    if (ret < 0) {
        return ret;
    }
    resp->which_response_type = pyuron_tapdance_Response_clear_tag;
    resp->response_type.clear =
        (pyuron_tapdance_ClearResponse)pyuron_tapdance_ClearResponse_init_zero;
    resp->response_type.clear.has_info = true;
    return fill_tapdance_info(req->id, &resp->response_type.clear.info);
}

static bool tapdance_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                        pb_callback_t *encode_response) {
    pyuron_tapdance_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(pyuron_tapdance, encode_response);

    pyuron_tapdance_Request req = pyuron_tapdance_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, pyuron_tapdance_Request_fields, &req)) {
        LOG_WRN("Failed to decode tapdance request: %s", PB_GET_ERROR(&req_stream));
        pyuron_tapdance_ErrorResponse err = pyuron_tapdance_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = pyuron_tapdance_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case pyuron_tapdance_Request_get_count_tag:
        rc = handle_get_count(&req.request_type.get_count, resp);
        break;
    case pyuron_tapdance_Request_get_tag:
        rc = handle_get(&req.request_type.get, resp);
        break;
    case pyuron_tapdance_Request_set_step_tag:
        rc = handle_set_step(&req.request_type.set_step, resp);
        break;
    case pyuron_tapdance_Request_set_term_tag:
        rc = handle_set_term(&req.request_type.set_term, resp);
        break;
    case pyuron_tapdance_Request_set_len_tag:
        rc = handle_set_len(&req.request_type.set_len, resp);
        break;
    case pyuron_tapdance_Request_clear_tag:
        rc = handle_clear(&req.request_type.clear, resp);
        break;
    default:
        LOG_WRN("Unsupported tapdance request type: %d", req.which_request_type);
        rc = -1;
    }

    if (rc != 0) {
        pyuron_tapdance_ErrorResponse err = pyuron_tapdance_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to process request (%d)", rc);
        resp->which_response_type = pyuron_tapdance_Response_error_tag;
        resp->response_type.error = err;
    }
    return true;
}
