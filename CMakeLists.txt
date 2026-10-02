cmake_minimum_required(VERSION 3.16)
project(KWS VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable Position Independent Code for all targets
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -------------------------------------------------------
# Build Options
# -------------------------------------------------------
option(BUILD_KWS_EXAMPLES "Build KWS example programs" ON)
option(BUILD_KWS_PYTHON "Build Python bindings" ON)
option(BUILD_KWS_TESTS "Build deterministic KWS inference regression tests" OFF)
option(KWS_MODEL_FETCH_OFF "Do not download the model at configure time" ON)
set(_KWS_MICROPHONE_DEFAULT OFF)
if(DEFINED ENV{SROBOTIS_OUTPUT_ROOT} AND NOT "$ENV{SROBOTIS_OUTPUT_ROOT}" STREQUAL "")
    set(_KWS_MICROPHONE_DEFAULT ON)
endif()
option(BUILD_KWS_MICROPHONE "Enable SDK audio capture/playback in kws_stream_demo" ${_KWS_MICROPHONE_DEFAULT})
option(BUILD_KWS_AEC "Enable optional WebRTC AEC3 full-duplex mode in the stream demo" OFF)
if(BUILD_KWS_AEC AND (NOT BUILD_KWS_MICROPHONE OR NOT BUILD_KWS_EXAMPLES))
    message(FATAL_ERROR "BUILD_KWS_AEC requires BUILD_KWS_MICROPHONE and BUILD_KWS_EXAMPLES")
endif()

# -------------------------------------------------------
# Model
# -------------------------------------------------------
# 权重与波束系数下载到 ~/.cache/models/kws/xiaojin-v1，运行时由 KwsConfig::model_dir
# （或环境变量 KWS_MODEL_DIR）指向；库本身不嵌入模型。
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchKwsModel.cmake)

# -------------------------------------------------------
# KWS Library
# -------------------------------------------------------
# 数值内核：浮点归约要允许重排才能向量化。CTC 搜索单独编译，保持严格 IEEE 语义，
# 否则打分会和 python 参照实现对不上。
set(KWS_FAST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/fft.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/fbank.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/beam.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/cfsmn/fsmn.cpp
)
set(KWS_STRICT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/cfsmn/cfsmn_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/cfsmn/ctc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/kws_engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/kws_backend_factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/kws_presets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model_fetch.cpp
)

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-ffast-math" KWS_HAS_FAST_MATH)
if(KWS_HAS_FAST_MATH)
    set_source_files_properties(${KWS_FAST_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffast-math")
endif()

add_library(kws STATIC ${KWS_FAST_SOURCES} ${KWS_STRICT_SOURCES})

target_include_directories(kws PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(kws PRIVATE pthread)

set_target_properties(kws PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)

message(STATUS "KWS library enabled (cFSMN backend, no external inference engine)")

if(BUILD_KWS_TESTS)
    enable_testing()
    add_executable(kws_inference_test tests/kws_inference_test.cpp)
    target_link_libraries(kws_inference_test PRIVATE kws)
    add_test(NAME kws-inference-regression COMMAND kws_inference_test)
    set_tests_properties(kws-inference-regression PROPERTIES TIMEOUT 90)
    add_executable(kws_async_test tests/kws_async_test.cpp src/kws_engine.cpp src/kws_presets.cpp)
    target_include_directories(kws_async_test PRIVATE include src)
    target_link_libraries(kws_async_test PRIVATE pthread)
    add_test(NAME kws-async-contract COMMAND kws_async_test)
    set_tests_properties(kws-async-contract PROPERTIES TIMEOUT 30)

endif()

# -------------------------------------------------------
# Install rules
# -------------------------------------------------------
install(TARGETS kws
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(FILES include/kws_service.h DESTINATION include)

# -------------------------------------------------------
# Examples
# -------------------------------------------------------
if(BUILD_KWS_EXAMPLES)
    foreach(demo kws_file_demo kws_stream_demo)
        add_executable(${demo} ${CMAKE_CURRENT_SOURCE_DIR}/examples/${demo}.cpp)
        target_include_directories(${demo} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(${demo} PRIVATE kws pthread m)
        set_target_properties(${demo} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        )
        install(TARGETS ${demo} RUNTIME DESTINATION bin)
    endforeach()

    if(BUILD_KWS_MICROPHONE)
        find_path(KWS_AUDIO_INCLUDE_DIR audio_base.hpp)
        find_library(KWS_AUDIO_LIBRARY spacemit_audio)
        if(NOT KWS_AUDIO_INCLUDE_DIR OR NOT KWS_AUDIO_LIBRARY)
            message(FATAL_ERROR "Build/install the audio component first, or set BUILD_KWS_MICROPHONE=OFF for --stdin only")
        endif()
        # Prefer the PortAudio installed with the SDK audio component.
        get_filename_component(_KWS_AUDIO_LIB_DIR "${KWS_AUDIO_LIBRARY}" DIRECTORY)
        find_library(KWS_PORTAUDIO_LIBRARY portaudio HINTS "${_KWS_AUDIO_LIB_DIR}")
        if(NOT KWS_PORTAUDIO_LIBRARY)
            message(FATAL_ERROR "PortAudio from the audio component was not found")
        endif()
        target_compile_definitions(kws_stream_demo PRIVATE KWS_HAS_AUDIO=1)
        target_include_directories(kws_stream_demo PRIVATE "${KWS_AUDIO_INCLUDE_DIR}")
        target_link_libraries(kws_stream_demo PRIVATE "${KWS_AUDIO_LIBRARY}" "${KWS_PORTAUDIO_LIBRARY}")
        if(APPLE)
            set(_KWS_AUDIO_INSTALL_RPATH "@loader_path/../lib")
        else()
            set(_KWS_AUDIO_INSTALL_RPATH "$ORIGIN/../lib")
        endif()
        set_target_properties(kws_stream_demo PROPERTIES INSTALL_RPATH "${_KWS_AUDIO_INSTALL_RPATH}")
    endif()

    if(BUILD_KWS_AEC)
        include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/KwsWebRtc.cmake)
        add_library(kws_demo_aec STATIC examples/kws_aec_audio.cpp)
        # staging may contain an older kws_service.h; prefer this checkout.
        target_include_directories(kws_demo_aec BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_include_directories(kws_demo_aec PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/examples"
            PRIVATE "${KWS_AUDIO_INCLUDE_DIR}")
        target_link_libraries(kws_demo_aec PUBLIC kws PRIVATE "${KWS_AUDIO_LIBRARY}"
            "${KWS_PORTAUDIO_LIBRARY}" ${KWS_WEBRTC_TARGET})
        target_compile_definitions(kws_stream_demo PRIVATE KWS_HAS_AEC=1)
        target_link_libraries(kws_stream_demo PRIVATE kws_demo_aec)
        if(BUILD_KWS_TESTS)
            add_executable(kws_aec_test tests/kws_aec_test.cpp)
            target_link_libraries(kws_aec_test PRIVATE kws_demo_aec)
            add_test(NAME kws-aec-regression COMMAND kws_aec_test)
            set_tests_properties(kws-aec-regression PROPERTIES TIMEOUT 60)
        endif()
    endif()

    if(BUILD_KWS_TESTS)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME kws-stream-cli
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stream_cli.py
                    $<TARGET_FILE:kws_stream_demo> $<TARGET_FILE:kws_file_demo>)
            set_tests_properties(kws-stream-cli PROPERTIES TIMEOUT 30)
        endif()
    endif()
endif()

# -------------------------------------------------------
# Python Bindings (optional)
# -------------------------------------------------------
if(BUILD_KWS_PYTHON)
    set(KWS_BINDINGS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/python/kws_bindings.cpp)
    if(NOT EXISTS ${KWS_BINDINGS_SRC})
        message(STATUS "Python bindings disabled (kws_bindings.cpp not found)")
        set(BUILD_KWS_PYTHON OFF)
    endif()
endif()

if(BUILD_KWS_PYTHON)
    find_package(Python3 COMPONENTS Interpreter Development QUIET)

    if(Python3_FOUND)
        message(STATUS "Python found: ${Python3_EXECUTABLE} (${Python3_VERSION})")
    endif()

    find_package(pybind11 CONFIG QUIET)

    if(NOT pybind11_FOUND AND Python3_FOUND)
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -c "import pybind11; print(pybind11.get_cmake_dir())"
            OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE PYBIND11_RESULT
        )
        if(PYBIND11_RESULT EQUAL 0)
            list(APPEND CMAKE_PREFIX_PATH "${PYBIND11_CMAKE_DIR}")
            find_package(pybind11 CONFIG QUIET)
        endif()
    endif()

    if(pybind11_FOUND)
        pybind11_add_module(_spacemit_kws ${KWS_BINDINGS_SRC})

        target_include_directories(_spacemit_kws PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(_spacemit_kws PRIVATE kws)

        if(BUILD_KWS_TESTS)
            add_test(NAME kws-python-bindings
                COMMAND ${CMAKE_COMMAND} -E env
                    "PYTHONPATH=$<TARGET_FILE_DIR:_spacemit_kws>"
                    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python_bindings.py)
            set_tests_properties(kws-python-bindings PROPERTIES TIMEOUT 30)
        endif()

        # Only set default output dir if not specified externally (e.g., by setup.py)
        if(NOT DEFINED CMAKE_LIBRARY_OUTPUT_DIRECTORY)
            set_target_properties(_spacemit_kws PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/python
            )
        endif()

        message(STATUS "Python bindings enabled (_spacemit_kws module for Python ${Python3_VERSION})")

        set(KWS_WHEEL_BUILD_ROOT "${CMAKE_BINARY_DIR}/python_wheel")
        set(KWS_WHEEL_OUTPUT_DIR "${KWS_WHEEL_BUILD_ROOT}/dist")
        set(KWS_WHEEL_CMAKE_BUILD_DIR "${KWS_WHEEL_BUILD_ROOT}/cmake")
        set(KWS_WHEEL_SOURCE_DIR "${KWS_WHEEL_BUILD_ROOT}/src")
        if(DEFINED ENV{SROBOTIS_OUTPUT_ROOT} AND NOT "$ENV{SROBOTIS_OUTPUT_ROOT}" STREQUAL "")
            set(_kws_output_root "$ENV{SROBOTIS_OUTPUT_ROOT}")
        elseif(DEFINED ENV{OUTPUT_ROOT} AND NOT "$ENV{OUTPUT_ROOT}" STREQUAL "")
            set(_kws_output_root "$ENV{OUTPUT_ROOT}")
        else()
            get_filename_component(_kws_output_root "${CMAKE_INSTALL_PREFIX}/.." ABSOLUTE)
        endif()
        set(KWS_DIST_INSTALL_DIR "${_kws_output_root}/dist")
        set(KWS_WHEEL_STAMP "${KWS_WHEEL_BUILD_ROOT}/wheel.stamp")
        file(MAKE_DIRECTORY "${KWS_WHEEL_OUTPUT_DIR}")

        add_custom_command(
            OUTPUT ${KWS_WHEEL_STAMP}
            COMMAND ${CMAKE_COMMAND} -E rm -rf ${KWS_WHEEL_OUTPUT_DIR}
            COMMAND ${CMAKE_COMMAND} -E rm -rf ${KWS_WHEEL_SOURCE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${KWS_WHEEL_OUTPUT_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${KWS_WHEEL_CMAKE_BUILD_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/python ${KWS_WHEEL_SOURCE_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/README.md ${KWS_WHEEL_SOURCE_DIR}/README.md
            COMMAND ${CMAKE_COMMAND} -E chdir ${KWS_WHEEL_SOURCE_DIR}
                ${CMAKE_COMMAND} -E env
                SPACEMIT_PREBUILT_EXTENSION=$<TARGET_FILE:_spacemit_kws>
                SPACEMIT_CMAKE_BUILD_DIR=${KWS_WHEEL_CMAKE_BUILD_DIR}
                ${Python3_EXECUTABLE} -m pip wheel . --no-deps --wheel-dir ${KWS_WHEEL_OUTPUT_DIR}
            COMMAND ${CMAKE_COMMAND} -E touch ${KWS_WHEEL_STAMP}
            DEPENDS
                _spacemit_kws
                ${CMAKE_CURRENT_SOURCE_DIR}/README.md
                ${CMAKE_CURRENT_SOURCE_DIR}/python/setup.py
                ${CMAKE_CURRENT_SOURCE_DIR}/python/pyproject.toml
                ${CMAKE_CURRENT_SOURCE_DIR}/python/spacemit_kws/__init__.py
            VERBATIM
            COMMENT "Building spacemit_kws wheel into ${KWS_WHEEL_OUTPUT_DIR}"
        )

        add_custom_target(kws-python-wheel ALL DEPENDS ${KWS_WHEEL_STAMP})

        execute_process(
            COMMAND ${Python3_EXECUTABLE} -c "import sysconfig; print(sysconfig.get_path('purelib'))"
            OUTPUT_VARIABLE PYTHON_SITE_PACKAGES
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        add_custom_target(kws-install-python
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${PYTHON_SITE_PACKAGES}/spacemit_kws
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/python/spacemit_kws ${PYTHON_SITE_PACKAGES}/spacemit_kws
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:_spacemit_kws> ${PYTHON_SITE_PACKAGES}/spacemit_kws/
            DEPENDS _spacemit_kws
            COMMENT "Installing spacemit_kws to ${PYTHON_SITE_PACKAGES}/spacemit_kws/"
        )

        install(
            DIRECTORY ${KWS_WHEEL_OUTPUT_DIR}/
            DESTINATION ${KWS_DIST_INSTALL_DIR}
            FILES_MATCHING PATTERN "*.whl"
        )
    else()
        message(STATUS "Python bindings disabled (pybind11 not found)")
    endif()
endif()

# -------------------------------------------------------
# Summary
# -------------------------------------------------------
message(STATUS "")
message(STATUS "=== KWS Build Configuration ===")
message(STATUS "  BUILD_KWS_EXAMPLES:  ${BUILD_KWS_EXAMPLES}")
message(STATUS "  BUILD_KWS_PYTHON:    ${BUILD_KWS_PYTHON}")
message(STATUS "  Model dir:           ${KWS_MODEL_DIR}")
if(BUILD_KWS_PYTHON AND pybind11_FOUND)
message(STATUS "  Wheel output:        ${KWS_WHEEL_OUTPUT_DIR}")
message(STATUS "  Wheel install dir:   ${KWS_DIST_INSTALL_DIR}")
endif()
message(STATUS "")
