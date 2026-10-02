# AEC is a demo-only optional dependency. Reuse an installed package, or the
# existing SDK third-party build used by application/native/omni_agent.
set(KWS_WEBRTC_ROOT "" CACHE PATH "Existing webrtc-audio-processing source/build root")
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT KWS_WEBRTC_ROOT)
    pkg_check_modules(KWS_WEBRTC QUIET IMPORTED_TARGET webrtc-audio-processing-2)
endif()
if(TARGET PkgConfig::KWS_WEBRTC)
    set(KWS_WEBRTC_TARGET PkgConfig::KWS_WEBRTC)
else()
    if(NOT KWS_WEBRTC_ROOT)
        if(DEFINED ENV{SROBOTIS_THIRDPARTY_CACHE})
            set(KWS_WEBRTC_ROOT "$ENV{SROBOTIS_THIRDPARTY_CACHE}/webrtc-audio-processing")
        else()
            set(KWS_WEBRTC_ROOT "$ENV{HOME}/.cache/thirdparty/webrtc-audio-processing")
        endif()
    endif()
    find_path(KWS_WEBRTC_INCLUDE_DIR webrtc/modules/audio_processing/include/audio_processing.h
        HINTS "${KWS_WEBRTC_ROOT}")
    file(GLOB _KWS_ABSL_ROOTS "${KWS_WEBRTC_ROOT}/subprojects/abseil-cpp*")
    find_path(KWS_WEBRTC_ABSL_INCLUDE_DIR absl/base/config.h HINTS ${_KWS_ABSL_ROOTS})
    find_library(KWS_WEBRTC_LIBRARY NAMES webrtc-audio-processing-2
        HINTS "${_KWS_AUDIO_LIB_DIR}"
              "${KWS_WEBRTC_ROOT}/build/webrtc/modules/audio_processing")
    if(NOT KWS_WEBRTC_INCLUDE_DIR OR NOT KWS_WEBRTC_ABSL_INCLUDE_DIR OR NOT KWS_WEBRTC_LIBRARY)
        message(FATAL_ERROR "BUILD_KWS_AEC needs webrtc-audio-processing-2 development files. Install them or set KWS_WEBRTC_ROOT/KWS_WEBRTC_LIBRARY to the existing SDK third-party build. No source is downloaded automatically.")
    endif()
    add_library(kws_webrtc INTERFACE)
    target_include_directories(kws_webrtc INTERFACE
        "${KWS_WEBRTC_INCLUDE_DIR}" "${KWS_WEBRTC_INCLUDE_DIR}/webrtc"
        "${KWS_WEBRTC_ABSL_INCLUDE_DIR}")
    target_compile_definitions(kws_webrtc INTERFACE WEBRTC_POSIX WEBRTC_LIBRARY_IMPL)
    target_link_libraries(kws_webrtc INTERFACE "${KWS_WEBRTC_LIBRARY}")
    set(KWS_WEBRTC_TARGET kws_webrtc)
    # The SDK's source-cache build is not in the system loader search path.
    # Install its real SONAME file alongside the demo's other SDK dependencies.
    get_filename_component(_KWS_WEBRTC_REAL_LIBRARY "${KWS_WEBRTC_LIBRARY}" REALPATH)
    install(FILES "${_KWS_WEBRTC_REAL_LIBRARY}" DESTINATION lib)
    if(EXISTS "${KWS_WEBRTC_ROOT}/COPYING")
        install(FILES "${KWS_WEBRTC_ROOT}/COPYING" DESTINATION share/licenses/kws RENAME WebRTC-COPYING)
    endif()
endif()
