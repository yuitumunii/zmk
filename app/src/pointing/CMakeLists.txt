# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

target_sources_ifdef(CONFIG_ZMK_INPUT_LISTENER app PRIVATE input_listener.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_TRANSFORM app PRIVATE input_processor_transform.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_SCALER app PRIVATE input_processor_scaler.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_PYURON_SCROLL_INVERT app PRIVATE input_processor_pyuron_scroll_invert.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER app PRIVATE input_processor_temp_layer.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_CODE_MAPPER app PRIVATE input_processor_code_mapper.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_BEHAVIORS app PRIVATE input_processor_behaviors.c)
target_sources_ifdef(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING app PRIVATE resolution_multipliers.c)
target_sources_ifdef(CONFIG_ZMK_INPUT_SPLIT app PRIVATE input_split.c)

if(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC OR CONFIG_PYURON_SCROLL_STUDIO_RPC)
    list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
    include(nanopb)
    set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
    set(NANOPB_GENERATE_CPP_STANDALONE OFF)
endif()

if(CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_STUDIO_RPC)
    target_sources(app PRIVATE aml_studio.c)

    # Build the generated proto into its own library.  zephyr_library() must be
    # in the same CMake directory scope as nanopb_generate_cpp so that CMake sees
    # the GENERATED property on .pb.c.  Then target_link_libraries (not
    # add_dependencies) actually links the symbols into app.
    zephyr_library()
    set(AML_PROTO_LIB ${ZEPHYR_CURRENT_LIBRARY})

    file(GLOB_RECURSE AML_PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/pyuron/aml/*.proto)
    nanopb_generate_cpp(aml_proto_srcs aml_proto_hdrs
                        RELPATH ${CMAKE_CURRENT_SOURCE_DIR}/proto
                        ${AML_PROTO_FILES})
    target_include_directories(${AML_PROTO_LIB} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_sources(${AML_PROTO_LIB} PRIVATE ${aml_proto_srcs} ${aml_proto_hdrs})

    target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(app PRIVATE ${AML_PROTO_LIB})
endif()

if(CONFIG_PYURON_SCROLL_STUDIO_RPC)
    target_sources(app PRIVATE scroll_studio.c)

    zephyr_library()
    set(SCROLL_PROTO_LIB ${ZEPHYR_CURRENT_LIBRARY})

    file(GLOB_RECURSE SCROLL_PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/pyuron/scroll/*.proto)
    nanopb_generate_cpp(scroll_proto_srcs scroll_proto_hdrs
                        RELPATH ${CMAKE_CURRENT_SOURCE_DIR}/proto
                        ${SCROLL_PROTO_FILES})
    target_include_directories(${SCROLL_PROTO_LIB} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_sources(${SCROLL_PROTO_LIB} PRIVATE ${scroll_proto_srcs} ${scroll_proto_hdrs})

    target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(app PRIVATE ${SCROLL_PROTO_LIB})
endif()
