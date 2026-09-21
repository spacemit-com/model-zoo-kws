# FetchKwsModel.cmake - Fetch the 小进小进 KWS model to cache
# Sets: KWS_MODEL_DIR
#
# 模型很小（约 3 MB：cFSMN 权重 + 波束系数 + 关键词表），不进 git，构建时下载到
# ~/.cache/models/kws/<name>。已有目录则跳过；离线构建用 -DKWS_MODEL_FETCH_OFF=ON，
# 或用 -DKWS_MODEL_NAME=<name> 换一个关键词模型。

if(DEFINED _FETCH_KWS_MODEL_LOADED)
  return()
endif()
set(_FETCH_KWS_MODEL_LOADED ON)

if(NOT DEFINED KWS_MODEL_NAME)
  set(KWS_MODEL_NAME "xiaojin")
endif()

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
  else()
    message(STATUS "Fetching KWS model to ${_KWS_MODEL_DIR} ...")
    file(MAKE_DIRECTORY "${_KWS_MODEL_DIR}")

    set(_archive_path "${_KWS_MODEL_DIR}/${KWS_MODEL_NAME}.tar.gz")

    file(DOWNLOAD
      "${_KWS_MODEL_URL}"
      "${_archive_path}"
      SHOW_PROGRESS
      STATUS _download_status
      TLS_VERIFY OFF
    )

    list(GET _download_status 0 _download_code)
    if(NOT _download_code EQUAL 0)
      list(GET _download_status 1 _download_error)
      file(REMOVE "${_archive_path}")
      # 下载失败不该挡住库的编译：没有模型只是跑不了 demo。
      message(WARNING "Failed to download KWS model: ${_download_error}")
    else()
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
      message(STATUS "KWS model ready at ${_KWS_MODEL_DIR}")
    endif()
  endif()
else()
  message(STATUS "KWS model found at ${_KWS_MODEL_DIR}")
endif()

# Export
set(KWS_MODEL_DIR "${_KWS_MODEL_DIR}" CACHE PATH "KWS model directory" FORCE)
