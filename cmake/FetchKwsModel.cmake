# FetchKwsModel.cmake - Fetch the 小进小进 KWS model to cache
# Sets: KWS_MODEL_DIR
#
# 模型很小（约 3 MB：cFSMN 权重 + 波束系数 + 关键词表），不进 git，构建时下载到
# ~/.cache/models/kws/<name>。默认只使用本地模型；-DKWS_MODEL_FETCH_OFF=OFF 时下载并校验
# SHA256（默认包已预置，其他包须提供 KWS_MODEL_SHA256）。

if(DEFINED _FETCH_KWS_MODEL_LOADED)
  return()
endif()
set(_FETCH_KWS_MODEL_LOADED ON)

# 发布包 archive.spacemit.com/spacemit-ai/model_zoo/kws/<name>.tar.gz；SHA256 只对默认包预置。
set(_KWS_DEFAULT_MODEL_NAME "xiaojin-v1")
set(_KWS_DEFAULT_MODEL_SHA256 "9e3a6f3d2142dcc2f65077539906a840c3e911215febdf631645ecb11e5db5d7")
if(NOT DEFINED KWS_MODEL_NAME)
  set(KWS_MODEL_NAME "${_KWS_DEFAULT_MODEL_NAME}")
endif()
if(NOT KWS_MODEL_NAME MATCHES "^[A-Za-z0-9_-]+$")
  message(FATAL_ERROR "Invalid KWS_MODEL_NAME")
endif()
if(KWS_MODEL_NAME STREQUAL _KWS_DEFAULT_MODEL_NAME)
  set(_kws_sha256_default "${_KWS_DEFAULT_MODEL_SHA256}")
else()
  set(_kws_sha256_default "")
endif()
set(KWS_MODEL_SHA256 "${_kws_sha256_default}" CACHE STRING "SHA256 of the approved KWS model archive")

set(_KWS_MODEL_URL "https://archive.spacemit.com/spacemit-ai/model_zoo/kws/${KWS_MODEL_NAME}.tar.gz")
set(_KWS_ARCHIVE_SUBDIR "${KWS_MODEL_NAME}")
set(_KWS_REQUIRED_FILES "cfsmn.bin;beam_w.bin;keywords.txt")

# Cache root
if(DEFINED ENV{HOME})
  set(_KWS_CACHE_ROOT "$ENV{HOME}/.cache/models/kws")
else()
  set(_KWS_CACHE_ROOT "${CMAKE_BINARY_DIR}/.cache/models/kws")
endif()

set(_KWS_MODEL_DIR "${_KWS_CACHE_ROOT}/${KWS_MODEL_NAME}")

# Check if model exists
set(_need_download OFF)
foreach(_file IN LISTS _KWS_REQUIRED_FILES)
  if(NOT EXISTS "${_KWS_MODEL_DIR}/${_file}")
    set(_need_download ON)
    break()
  endif()
endforeach()

if(_need_download)
  if(DEFINED KWS_MODEL_FETCH_OFF AND KWS_MODEL_FETCH_OFF)
    message(WARNING "KWS model not found at ${_KWS_MODEL_DIR}, fetch disabled (KWS_MODEL_FETCH_OFF). "
                    "Point KwsConfig::model_dir or $KWS_MODEL_DIR at a local copy before running.")
  elseif(NOT KWS_MODEL_SHA256 MATCHES "^[0-9a-fA-F]+$" OR NOT KWS_MODEL_SHA256)
    message(WARNING "KWS model fetch requires KWS_MODEL_SHA256 from the model release. "
                    "Provide a local model directory until a verified release is available.")
  else()
    string(LENGTH "${KWS_MODEL_SHA256}" _hash_length)
    if(NOT _hash_length EQUAL 64)
      message(FATAL_ERROR "KWS_MODEL_SHA256 must contain 64 hex digits")
    endif()
    message(STATUS "Fetching KWS model to ${_KWS_MODEL_DIR} ...")
    file(MAKE_DIRECTORY "${_KWS_MODEL_DIR}")

    set(_archive_path "${_KWS_MODEL_DIR}/${KWS_MODEL_NAME}.tar.gz")

    file(DOWNLOAD
      "${_KWS_MODEL_URL}"
      "${_archive_path}"
      SHOW_PROGRESS
      STATUS _download_status
      TLS_VERIFY ON
      TIMEOUT 60
      INACTIVITY_TIMEOUT 20
    )

    list(GET _download_status 0 _download_code)
    if(NOT _download_code EQUAL 0)
      list(GET _download_status 1 _download_error)
      file(REMOVE "${_archive_path}")
      # 下载失败不该挡住库的编译：没有模型只是跑不了 demo。
      message(WARNING "Failed to download KWS model: ${_download_error}")
    else()
      file(SHA256 "${_archive_path}" _archive_sha256)
      string(TOLOWER "${KWS_MODEL_SHA256}" _expected_sha256)
      if(NOT _archive_sha256 STREQUAL _expected_sha256)
        file(REMOVE "${_archive_path}")
        message(FATAL_ERROR "KWS model archive SHA256 mismatch")
      endif()
      message(STATUS "Extracting KWS model...")
      execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${_archive_path}"
        WORKING_DIRECTORY "${_KWS_MODEL_DIR}"
        RESULT_VARIABLE _extract_result
      )
      if(NOT _extract_result EQUAL 0)
        message(WARNING "Failed to extract KWS model")
      endif()

      # Move files from subdirectory if exists
      if(EXISTS "${_KWS_MODEL_DIR}/${_KWS_ARCHIVE_SUBDIR}")
        file(GLOB _subdir_files "${_KWS_MODEL_DIR}/${_KWS_ARCHIVE_SUBDIR}/*")
        foreach(_file IN LISTS _subdir_files)
          get_filename_component(_filename "${_file}" NAME)
          file(RENAME "${_file}" "${_KWS_MODEL_DIR}/${_filename}")
        endforeach()
        file(REMOVE_RECURSE "${_KWS_MODEL_DIR}/${_KWS_ARCHIVE_SUBDIR}")
      endif()

      file(REMOVE "${_archive_path}")
      set(_model_ready TRUE)
      foreach(_file IN LISTS _KWS_REQUIRED_FILES)
        if(NOT EXISTS "${_KWS_MODEL_DIR}/${_file}")
          set(_model_ready FALSE)
        endif()
      endforeach()
      if(_extract_result EQUAL 0 AND _model_ready)
        message(STATUS "KWS model ready at ${_KWS_MODEL_DIR}")
      else()
        message(WARNING "KWS model extraction incomplete; runtime assets are unavailable")
      endif()
    endif()
  endif()
else()
  message(STATUS "KWS model found at ${_KWS_MODEL_DIR}")
endif()

# Export
set(KWS_MODEL_DIR "${_KWS_MODEL_DIR}" CACHE PATH "KWS model directory" FORCE)
