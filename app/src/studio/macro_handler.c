/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Custom Studio RPC subsystem (pyuron_macro) for live editing of fixed macro
 * slots (&pmac0..&pmac5). The app probes get_count then get(0..count-1) and
 * edits step-by-step (set_step / set_len / clear). Built on the custom
 * subsystem support in the custom-studio-protocol ZMK base; modeled 1:1 on
 * src/studio/timing_handler.c.
 */

#include <string.h>
#include <errno.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <zmk/behaviors/macro_tuning.h>
#include <pyuron/macro/macro.pb.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta macro_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://github.com/yuitumunii/zmk"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(pyuron_macro, &macro_feature_meta, macro_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(pyuron_macro, pyuron_macro_Response);

// Fill a protobuf MacroInfo from the live RAM state of slot `id`.
static int fill_macro_info(uint32_t id, pyuron_macro_MacroInfo *info) {
    struct pyuron_macro_slot s;
    int ret = pyuron_macro_get((uint8_t)id, &s);
    if (ret < 0) {
        return ret;
    }

    *info = (pyuron_macro_MacroInfo)pyuron_macro_MacroInfo_init_zero;
    info->id = id;
    info->step_count = s.step_count;
    info->steps_count = s.step_count; // nanopb repeated count
    for (int i = 0; i < s.step_count && i < PYURON_MACRO_MAX_STEPS; i++) {
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

static int handle_get_count(const pyuron_macro_GetCountRequest *req,
                            pyuron_macro_Response *resp) {
    ARG_UNUSED(req);
    resp->which_response_type = pyuron_macro_Response_get_count_tag;
    resp->response_type.get_count.count = (uint32_t)pyuron_macro_get_count();
    return 0;
}

static int handle_get(const pyuron_macro_GetRequest *req, pyuron_macro_Response *resp) {
    resp->which_response_type = pyuron_macro_Response_get_tag;
    resp->response_type.get = (pyuron_macro_GetResponse)pyuron_macro_GetResponse_init_zero;
    resp->response_type.get.has_info = true;
    return fill_macro_info(req->id, &resp->response_type.get.info);
}

static int handle_set_step(const pyuron_macro_SetStepRequest *req, pyuron_macro_Response *resp) {
    if (req->id >= PYURON_MACRO_SLOTS || req->index >= PYURON_MACRO_MAX_STEPS) {
        return -EINVAL;
    }
    int ret = pyuron_macro_set_step((uint8_t)req->id, (uint8_t)req->index, req->behavior_id,
                                    req->param1, req->param2);
    if (ret < 0) {
        return ret;
    }
    resp->which_response_type = pyuron_macro_Response_set_step_tag;
    resp->response_type.set_step =
        (pyuron_macro_SetStepResponse)pyuron_macro_SetStepResponse_init_zero;
    resp->response_type.set_step.has_info = true;
    return fill_macro_info(req->id, &resp->response_type.set_step.info);
}

static int handle_set_len(const pyuron_macro_SetLenRequest *req, pyuron_macro_Response *resp) {
    if (req->id >= PYURON_MACRO_SLOTS || req->len > PYURON_MACRO_MAX_STEPS) {
        return -EINVAL;
    }
    int ret = pyuron_macro_set_len((uint8_t)req->id, (uint8_t)req->len);
    if (ret < 0) {
        return ret;
    }
    resp->which_response_type = pyuron_macro_Response_set_len_tag;
    resp->response_type.set_len =
        (pyuron_macro_SetLenResponse)pyuron_macro_SetLenResponse_init_zero;
    resp->response_type.set_len.has_info = true;
    return fill_macro_info(req->id, &resp->response_type.set_len.info);
}

static int handle_clear(const pyuron_macro_ClearRequest *req, pyuron_macro_Response *resp) {
    int ret = pyuron_macro_clear((uint8_t)req->id);
    if (ret < 0) {
        return ret;
    }
    resp->which_response_type = pyuron_macro_Response_clear_tag;
    resp->response_type.clear = (pyuron_macro_ClearResponse)pyuron_macro_ClearResponse_init_zero;
    resp->response_type.clear.has_info = true;
    return fill_macro_info(req->id, &resp->response_type.clear.info);
}

static bool macro_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                     pb_callback_t *encode_response) {
    pyuron_macro_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(pyuron_macro, encode_response);

    pyuron_macro_Request req = pyuron_macro_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, pyuron_macro_Request_fields, &req)) {
        LOG_WRN("Failed to decode macro request: %s", PB_GET_ERROR(&req_stream));
        pyuron_macro_ErrorResponse err = pyuron_macro_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = pyuron_macro_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case pyuron_macro_Request_get_count_tag:
        rc = handle_get_count(&req.request_type.get_count, resp);
        break;
    case pyuron_macro_Request_get_tag:
        rc = handle_get(&req.request_type.get, resp);
        break;
    case pyuron_macro_Request_set_step_tag:
        rc = handle_set_step(&req.request_type.set_step, resp);
        break;
    case pyuron_macro_Request_set_len_tag:
        rc = handle_set_len(&req.request_type.set_len, resp);
        break;
    case pyuron_macro_Request_clear_tag:
        rc = handle_clear(&req.request_type.clear, resp);
        break;
    default:
        LOG_WRN("Unsupported macro request type: %d", req.which_request_type);
        rc = -1;
    }

    if (rc != 0) {
        pyuron_macro_ErrorResponse err = pyuron_macro_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to process request (%d)", rc);
        resp->which_response_type = pyuron_macro_Response_error_tag;
        resp->response_type.error = err;
    }
    return true;
}
