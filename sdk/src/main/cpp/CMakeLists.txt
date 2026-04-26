# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 3.10.2)

# Declares and names the project.

project("mdkplayer")

set(MDK_SDK_ARCHIVE_NAME "mdk-sdk-android.7z")
set(MDK_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/mdk-sdk")
set(MDK_SDK_FIND_MODULE "${MDK_SDK_ROOT}/lib/cmake/FindMDK.cmake")
set(MDK_SDK_LOCAL_ARCHIVE "${CMAKE_CURRENT_SOURCE_DIR}/${MDK_SDK_ARCHIVE_NAME}")
set(MDK_SDK_URLS
    "https://sourceforge.net/projects/mdk-sdk/files/nightly/${MDK_SDK_ARCHIVE_NAME}/download"
    "https://downloads.sourceforge.net/project/mdk-sdk/nightly/${MDK_SDK_ARCHIVE_NAME}")

# 支持显式指定本地归档，避免每次配置阶段都依赖 SourceForge。
set(MDK_SDK_ARCHIVE "" CACHE FILEPATH "Path to a pre-downloaded mdk-sdk archive")
set(MDK_SDK_CACHE_DIR "" CACHE PATH "Directory that contains mdk-sdk-android.7z")

function(select_mdk_sdk_archive candidate description remove_if_empty out_var)
  if(EXISTS "${candidate}")
    file(SIZE "${candidate}" candidate_size)
    if(candidate_size GREATER 0)
      message(STATUS "Using cached mdk-sdk archive from ${description}: ${candidate} (${candidate_size} bytes)")
      set(${out_var} "${candidate}" PARENT_SCOPE)
    else()
      message(WARNING "Found empty mdk-sdk archive from ${description}: ${candidate}")
      if(remove_if_empty)
        file(REMOVE "${candidate}")
        message(STATUS "Removed empty mdk-sdk archive: ${candidate}")
      endif()
    endif()
  endif()
endfunction()

if(NOT EXISTS "${MDK_SDK_FIND_MODULE}")
  set(MDK_SDK_ARCHIVE_HINT "${MDK_SDK_ARCHIVE}")
  if("${MDK_SDK_ARCHIVE_HINT}" STREQUAL "" AND DEFINED ENV{MDK_SDK_ARCHIVE} AND NOT "$ENV{MDK_SDK_ARCHIVE}" STREQUAL "")
    set(MDK_SDK_ARCHIVE_HINT "$ENV{MDK_SDK_ARCHIVE}")
  endif()

  set(MDK_SDK_CACHE_DIR_HINT "${MDK_SDK_CACHE_DIR}")
  if("${MDK_SDK_CACHE_DIR_HINT}" STREQUAL "" AND DEFINED ENV{MDK_SDK_CACHE_DIR} AND NOT "$ENV{MDK_SDK_CACHE_DIR}" STREQUAL "")
    set(MDK_SDK_CACHE_DIR_HINT "$ENV{MDK_SDK_CACHE_DIR}")
  endif()

  set(MDK_SDK_ARCHIVE_TO_EXTRACT "")
  if(NOT "${MDK_SDK_ARCHIVE_HINT}" STREQUAL "")
    select_mdk_sdk_archive("${MDK_SDK_ARCHIVE_HINT}" "MDK_SDK_ARCHIVE" FALSE MDK_SDK_ARCHIVE_TO_EXTRACT)
  endif()

  if("${MDK_SDK_ARCHIVE_TO_EXTRACT}" STREQUAL "" AND NOT "${MDK_SDK_CACHE_DIR_HINT}" STREQUAL "")
    select_mdk_sdk_archive("${MDK_SDK_CACHE_DIR_HINT}/${MDK_SDK_ARCHIVE_NAME}" "MDK_SDK_CACHE_DIR" FALSE MDK_SDK_ARCHIVE_TO_EXTRACT)
  endif()

  if("${MDK_SDK_ARCHIVE_TO_EXTRACT}" STREQUAL "")
    select_mdk_sdk_archive("${MDK_SDK_LOCAL_ARCHIVE}" "workspace local cache" TRUE MDK_SDK_ARCHIVE_TO_EXTRACT)
  endif()

  if("${MDK_SDK_ARCHIVE_TO_EXTRACT}" STREQUAL "")
    foreach(MDK_SDK_URL IN LISTS MDK_SDK_URLS)
      message(STATUS "Trying mdk-sdk source: ${MDK_SDK_URL}")
      file(REMOVE "${MDK_SDK_LOCAL_ARCHIVE}")
      file(
        DOWNLOAD "${MDK_SDK_URL}" "${MDK_SDK_LOCAL_ARCHIVE}"
        STATUS MDK_SDK_DOWNLOAD_STATUS
        LOG MDK_SDK_DOWNLOAD_LOG
        SHOW_PROGRESS
        TIMEOUT 120
        INACTIVITY_TIMEOUT 30
      )
      list(GET MDK_SDK_DOWNLOAD_STATUS 0 MDK_SDK_DOWNLOAD_CODE)
      list(GET MDK_SDK_DOWNLOAD_STATUS 1 MDK_SDK_DOWNLOAD_MESSAGE)
      set(MDK_SDK_LOCAL_ARCHIVE_SIZE 0)
      if(EXISTS "${MDK_SDK_LOCAL_ARCHIVE}")
        file(SIZE "${MDK_SDK_LOCAL_ARCHIVE}" MDK_SDK_LOCAL_ARCHIVE_SIZE)
      endif()
      if(MDK_SDK_DOWNLOAD_CODE EQUAL 0 AND MDK_SDK_LOCAL_ARCHIVE_SIZE GREATER 0)
        message(STATUS "Downloaded mdk-sdk archive to ${MDK_SDK_LOCAL_ARCHIVE} (${MDK_SDK_LOCAL_ARCHIVE_SIZE} bytes)")
        set(MDK_SDK_ARCHIVE_TO_EXTRACT "${MDK_SDK_LOCAL_ARCHIVE}")
        break()
      endif()

      if(EXISTS "${MDK_SDK_LOCAL_ARCHIVE}")
        file(REMOVE "${MDK_SDK_LOCAL_ARCHIVE}")
      endif()
      string(REPLACE "\n" "\n    " MDK_SDK_DOWNLOAD_LOG_INDENTED "${MDK_SDK_DOWNLOAD_LOG}")
      message(WARNING
        "Failed to download mdk-sdk from ${MDK_SDK_URL}\n"
        "  status=${MDK_SDK_DOWNLOAD_CODE}\n"
        "  message=${MDK_SDK_DOWNLOAD_MESSAGE}\n"
        "  log=\n    ${MDK_SDK_DOWNLOAD_LOG_INDENTED}"
      )
    endforeach()
  endif()

  if("${MDK_SDK_ARCHIVE_TO_EXTRACT}" STREQUAL "")
    message(FATAL_ERROR
      "Unable to obtain ${MDK_SDK_ARCHIVE_NAME}. "
      "Provide a pre-downloaded archive via MDK_SDK_ARCHIVE, "
      "or set MDK_SDK_CACHE_DIR to a directory containing ${MDK_SDK_ARCHIVE_NAME}, "
      "or place ${MDK_SDK_ARCHIVE_NAME} under ${CMAKE_CURRENT_SOURCE_DIR}."
    )
  endif()

  message(STATUS "Extracting mdk-sdk from ${MDK_SDK_ARCHIVE_TO_EXTRACT}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar "xvf" "${MDK_SDK_ARCHIVE_TO_EXTRACT}" "--format=7zip"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE EXTRACT_RET
    ERROR_VARIABLE EXTRACT_ERR
    OUTPUT_QUIET
  )
  if(NOT EXTRACT_RET EQUAL 0 OR NOT EXISTS "${MDK_SDK_FIND_MODULE}")
    if("${MDK_SDK_ARCHIVE_TO_EXTRACT}" STREQUAL "${MDK_SDK_LOCAL_ARCHIVE}")
      file(REMOVE "${MDK_SDK_LOCAL_ARCHIVE}")
      message(STATUS "Removed invalid downloaded mdk-sdk archive: ${MDK_SDK_LOCAL_ARCHIVE}")
    endif()
    message(FATAL_ERROR
      "Failed to extract mdk-sdk from ${MDK_SDK_ARCHIVE_TO_EXTRACT}.\n"
      "cmake -E tar exit code: ${EXTRACT_RET}\n"
      "stderr: ${EXTRACT_ERR}\n"
      "Download ${MDK_SDK_ARCHIVE_NAME} manually and extract it into ${MDK_SDK_ROOT}, "
      "or set MDK_SDK_ARCHIVE to a valid archive path."
    )
  endif()
endif()
include("${MDK_SDK_FIND_MODULE}")

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.
add_link_options(-Wl,-z,max-page-size=16384)

add_library( # Sets the name of the library.
             mdk-jni

             # Sets the library as a shared library.
             SHARED

             # Provides a relative path to your source file(s).
        jmi/jmi.cpp
        MDKPlayerJNI.cpp)

# Specifies libraries CMake should link to your target library. You
# can link multiple libraries, such as libraries you define in this
# build script, prebuilt third-party libraries, or system libraries.

target_link_libraries( # Specifies the target library.
                       mdk-jni
                       PRIVATE
                       mdk
                       # Links the target library to the log library
                       # included in the NDK.
                       -llog -landroid #-lGLESv2
              )
