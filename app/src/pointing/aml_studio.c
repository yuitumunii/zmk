/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Custom Studio RPC subsystem "pyuron_aml" — lets ZMK Studio live-change
 * AutoMouse Layer config (deactivation timeout, prior-idle guard, excluded
 * positions) without reflashing.  Wire format: proto/pyuron/aml/aml.proto.
 */

#include <string.h>
#include <errno.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <zmk/pointing/aml.h>
#include <pyuron/aml/aml.pb.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta aml_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://github.com/yuitumunii/zmk"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(pyuron_aml, &aml_feature_meta, aml_rpc_handle_request);
ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(pyuron_aml, pyuron_aml_Response);

static void fill_config_response(pyuron_aml_Response *resp) {
    struct zmk_aml_config cfg;
    zmk_aml_get(&cfg);

    resp->which_response_type = pyuron_aml_Response_config_tag;
    pyuron_aml_ConfigResponse *cr = &resp->response_type.config;
    *cr = (pyuron_aml_ConfigResponse)pyuron_aml_ConfigResponse_init_zero;
    cr->has_config = true;
    cr->config.deactivation_ms = cfg.deactivation_ms;
    cr->config.prior_idle_ms   = cfg.prior_idle_ms;
    cr->config.extend_ms       = cfg.extend_ms;
    cr->config.excluded_positions_count = cfg.num_excluded;
    for (uint8_t i = 0; i < cfg.num_excluded && i < ZMK_AML_MAX_EXCLUDED; i++) {
        cr->config.excluded_positions[i] = cfg.excluded_positions[i];
    }
}

static bool aml_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                   pb_callback_t *encode_response) {
    pyuron_aml_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(pyuron_aml, encode_response);

    pyuron_aml_Request req = pyuron_aml_Request_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(raw_request->payload.bytes,
                                                  raw_request->payload.size);
    if (!pb_decode(&stream, pyuron_aml_Request_fields, &req)) {
        LOG_WRN("aml_rpc: decode error: %s", PB_GET_ERROR(&stream));
        resp->which_response_type = pyuron_aml_Response_error_tag;
        snprintf(resp->response_type.error.message,
                 sizeof(resp->response_type.error.message),
                 "decode error");
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case pyuron_aml_Request_get_tag:
        break;
    case pyuron_aml_Request_set_deactivation_tag:
        rc = zmk_aml_set_deactivation(req.request_type.set_deactivation.ms);
        if (rc == 0) { zmk_aml_save(); }
        break;
    case pyuron_aml_Request_set_prior_idle_tag:
        rc = zmk_aml_set_prior_idle(req.request_type.set_prior_idle.ms);
        if (rc == 0) { zmk_aml_save(); }
        break;
    case pyuron_aml_Request_set_extend_tag:
        rc = zmk_aml_set_extend(req.request_type.set_extend.ms);
        if (rc == 0) { zmk_aml_save(); }
        break;
    case pyuron_aml_Request_toggle_excluded_tag:
        rc = zmk_aml_toggle_excluded(req.request_type.toggle_excluded.position);
        if (rc == 0) { zmk_aml_save(); }
        break;
    case pyuron_aml_Request_reset_tag:
        rc = zmk_aml_reset();
        break;
    default:
        LOG_WRN("aml_rpc: unknown request type %d", req.which_request_type);
        rc = -ENOTSUP;
    }

    if (rc != 0) {
        resp->which_response_type = pyuron_aml_Response_error_tag;
        snprintf(resp->response_type.error.message,
                 sizeof(resp->response_type.error.message),
                 "error %d", rc);
        return true;
    }

    fill_config_response(resp);
    return true;
}
