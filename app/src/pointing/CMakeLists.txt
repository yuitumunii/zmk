# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

target_sources_ifdef(CONFIG_ZMK_INPUT_LISTENER app PRIVATE input_listener.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_TRANSFORM app PRIVATE input_processor_transform.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_SCALER app PRIVATE input_processor_scaler.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER app PRIVATE input_processor_temp_layer.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_CODE_MAPPER app PRIVATE input_processor_code_mapper.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_BEHAVIORS app PRIVATE input_processor_behaviors.c)
target_sources_ifdef(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING app PRIVATE resolution_multipliers.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_SPLIT app PRIVATE input_split.c)

if(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC)
    target_sources(app PRIVATE aml_studio.c)

    list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
    include(nanopb)
    set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
    set(NANOPB_GENERATE_CPP_STANDALONE OFF)

    zephyr_library()
    file(GLOB_RECURSE AML_PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/pyuron/aml/*.proto)
    nanopb_generate_cpp(aml_proto_srcs aml_proto_hdrs
                        RELPATH ${CMAKE_CURRENT_SOURCE_DIR}/proto
                        ${AML_PROTO_FILES})
    target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${aml_proto_srcs} ${aml_proto_hdrs})

    target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
endif()
