if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(ROOT_CMAKE "${SOURCE_DIR}/CMakeLists.txt")
file(READ "${ROOT_CMAKE}" ROOT_CONTENT)
string(REPLACE
    "update_cached_list(MK_LINK_LIBRARIES \${OPENSSL_LIBRARIES})"
    "update_cached_list(MK_LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)"
    PATCHED_ROOT_CONTENT
    "${ROOT_CONTENT}")
if(PATCHED_ROOT_CONTENT STREQUAL ROOT_CONTENT)
    message(STATUS "ZLMediaKit OpenSSL imported targets already patched")
else()
    file(WRITE "${ROOT_CMAKE}" "${PATCHED_ROOT_CONTENT}")
    message(STATUS "Patched ZLMediaKit OpenSSL links to stay configuration-aware")
endif()

set(TOOLKIT_CMAKE "${SOURCE_DIR}/3rdpart/ZLToolKit/CMakeLists.txt")
file(READ "${TOOLKIT_CMAKE}" TOOLKIT_CONTENT)
string(REPLACE
    "update_cached_list(TK_LINK_LIBRARIES \${OPENSSL_LIBRARIES})"
    "update_cached_list(TK_LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)"
    PATCHED_TOOLKIT_CONTENT
    "${TOOLKIT_CONTENT}")
if(PATCHED_TOOLKIT_CONTENT STREQUAL TOOLKIT_CONTENT)
    message(STATUS "ZLToolKit OpenSSL imported targets already patched")
else()
    file(WRITE "${TOOLKIT_CMAKE}" "${PATCHED_TOOLKIT_CONTENT}")
    message(STATUS "Patched ZLToolKit OpenSSL links to stay configuration-aware")
endif()

set(API_CMAKE "${SOURCE_DIR}/api/CMakeLists.txt")
file(READ "${API_CMAKE}" CONTENT)
string(REPLACE
    "\${CMAKE_SOURCE_DIR}/resource.rc"
    "\${CMAKE_CURRENT_SOURCE_DIR}/../resource.rc"
    PATCHED_CONTENT
    "${CONTENT}")
if(PATCHED_CONTENT STREQUAL CONTENT)
    message(STATUS "ZLMediaKit MSVC resource path already patched")
else()
    file(WRITE "${API_CMAKE}" "${PATCHED_CONTENT}")
    message(STATUS "Patched ZLMediaKit MSVC resource path for subproject builds")
endif()

set(API_PRIVATE_LINK_MARKER
    "target_link_libraries(mk_api PRIVATE -Wl,--start-group")
string(FIND "${PATCHED_CONTENT}" "${API_PRIVATE_LINK_MARKER}" API_PRIVATE_LINK_INDEX)
if(API_PRIVATE_LINK_INDEX EQUAL -1)
    string(REPLACE
        "target_link_libraries(mk_api -Wl,--start-group"
        "target_link_libraries(mk_api PRIVATE -Wl,--start-group"
        PATCHED_API_LINK_CONTENT
        "${PATCHED_CONTENT}")
    string(REPLACE
        "target_link_libraries(mk_api log -Wl,--start-group"
        "target_link_libraries(mk_api PRIVATE log -Wl,--start-group"
        PATCHED_API_LINK_CONTENT
        "${PATCHED_API_LINK_CONTENT}")
    string(REPLACE
        "target_link_libraries(mk_api \${LINK_LIBRARIES})"
        "target_link_libraries(mk_api PRIVATE \${LINK_LIBRARIES})"
        PATCHED_API_LINK_CONTENT
        "${PATCHED_API_LINK_CONTENT}")
else()
    set(PATCHED_API_LINK_CONTENT "${PATCHED_CONTENT}")
endif()
if(PATCHED_API_LINK_CONTENT STREQUAL PATCHED_CONTENT)
    message(STATUS "ZLMediaKit C API private link boundary already patched")
else()
    file(WRITE "${API_CMAKE}" "${PATCHED_API_LINK_CONTENT}")
    message(STATUS "Patched ZLMediaKit C API dependencies to stay private")
endif()

set(EXT_CODEC_CMAKE "${SOURCE_DIR}/ext-codec/CMakeLists.txt")
file(READ "${EXT_CODEC_CMAKE}" EXT_CODEC_CONTENT)
set(PATCHED_EXT_CODEC_CONTENT "${EXT_CODEC_CONTENT}")
set(NOMINMAX_BLOCK
    "if(MSVC)\n  target_compile_definitions(ext-codec PRIVATE NOMINMAX)\nendif()")
set(DUPLICATE_NOMINMAX_BLOCK "${NOMINMAX_BLOCK}\n${NOMINMAX_BLOCK}")
string(FIND "${PATCHED_EXT_CODEC_CONTENT}" "${NOMINMAX_BLOCK}" NOMINMAX_INDEX)
if(NOMINMAX_INDEX EQUAL -1)
    string(REPLACE
        "target_compile_definitions(ext-codec PUBLIC \${COMPILE_DEFINITIONS})"
        "target_compile_definitions(ext-codec PUBLIC \${COMPILE_DEFINITIONS})\n${NOMINMAX_BLOCK}"
        PATCHED_EXT_CODEC_CONTENT
        "${PATCHED_EXT_CODEC_CONTENT}")
else()
    while(TRUE)
        string(FIND
            "${PATCHED_EXT_CODEC_CONTENT}"
            "${DUPLICATE_NOMINMAX_BLOCK}"
            DUPLICATE_NOMINMAX_INDEX)
        if(DUPLICATE_NOMINMAX_INDEX EQUAL -1)
            break()
        endif()
        string(REPLACE
            "${DUPLICATE_NOMINMAX_BLOCK}"
            "${NOMINMAX_BLOCK}"
            PATCHED_EXT_CODEC_CONTENT
            "${PATCHED_EXT_CODEC_CONTENT}")
    endwhile()
endif()

string(FIND
    "${PATCHED_EXT_CODEC_CONTENT}"
    "media-server/libflv/include"
    LIBFLV_INCLUDE_INDEX)
if(LIBFLV_INCLUDE_INDEX EQUAL -1)
    string(REPLACE
        "\"$<BUILD_INTERFACE:\${CMAKE_CURRENT_SOURCE_DIR}>\"\n        PUBLIC"
        "\"$<BUILD_INTERFACE:\${CMAKE_CURRENT_SOURCE_DIR}>\"\n        \"${SOURCE_DIR}/3rdpart/media-server/libflv/include\"\n        PUBLIC"
        PATCHED_EXT_CODEC_CONTENT
        "${PATCHED_EXT_CODEC_CONTENT}")
endif()
if(PATCHED_EXT_CODEC_CONTENT STREQUAL EXT_CODEC_CONTENT)
    message(STATUS "ZLMediaKit ext-codec integration already patched")
else()
    file(WRITE "${EXT_CODEC_CMAKE}" "${PATCHED_EXT_CODEC_CONTENT}")
    message(STATUS "Patched ZLMediaKit ext-codec integration for static MSVC builds")
endif()

set(THIRDPARTY_CMAKE "${SOURCE_DIR}/3rdpart/CMakeLists.txt")
file(READ "${THIRDPARTY_CMAKE}" THIRDPARTY_CONTENT)
string(FIND
    "${THIRDPARTY_CONTENT}"
    "Build libflv for ext-codec even when MP4 recording is disabled."
    LIBFLV_FALLBACK_INDEX)
if(LIBFLV_FALLBACK_INDEX EQUAL -1)
    set(LIBFLV_FALLBACK [=[
# Build libflv for ext-codec even when MP4 recording is disabled.
if(NOT ENABLE_MP4)
  set(MediaServer_FLV_ROOT ${MediaServer_ROOT}/libflv)
  aux_source_directory(${MediaServer_FLV_ROOT}/include FLV_SRC_LIST)
  aux_source_directory(${MediaServer_FLV_ROOT}/source FLV_SRC_LIST)
  add_library(flv STATIC ${FLV_SRC_LIST})
  add_library(MediaServer::flv ALIAS flv)
  target_compile_options(flv PRIVATE ${COMPILE_OPTIONS_DEFAULT})
  target_include_directories(flv
          PRIVATE
          "$<BUILD_INTERFACE:${MediaServer_FLV_ROOT}/include>"
          PUBLIC
          "$<BUILD_INTERFACE:${MediaServer_FLV_ROOT}/include>")
  update_cached_list(MK_LINK_LIBRARIES MediaServer::flv)
endif()

]=])
    string(REPLACE
        "# 添加 mpeg 用于支持 ts 生成"
        "${LIBFLV_FALLBACK}# 添加 mpeg 用于支持 ts 生成"
        PATCHED_THIRDPARTY_CONTENT
        "${THIRDPARTY_CONTENT}")
else()
    set(PATCHED_THIRDPARTY_CONTENT "${THIRDPARTY_CONTENT}")
endif()
if(PATCHED_THIRDPARTY_CONTENT STREQUAL THIRDPARTY_CONTENT)
    message(STATUS "ZLMediaKit libflv fallback already patched")
else()
    file(WRITE "${THIRDPARTY_CMAKE}" "${PATCHED_THIRDPARTY_CONTENT}")
    message(STATUS "Patched ZLMediaKit libflv fallback for ENABLE_MP4=OFF")
endif()

set(MK_RECORDER_SOURCE "${SOURCE_DIR}/api/source/mk_recorder.cpp")
file(READ "${MK_RECORDER_SOURCE}" MK_RECORDER_CONTENT)
set(PATCHED_MK_RECORDER_CONTENT "${MK_RECORDER_CONTENT}")
set(MUXER_INCLUDE "#include \"Common/MultiMediaSourceMuxer.h\"")
set(DUPLICATE_MUXER_INCLUDE "${MUXER_INCLUDE}\n${MUXER_INCLUDE}")
string(FIND "${PATCHED_MK_RECORDER_CONTENT}" "${MUXER_INCLUDE}" MUXER_INCLUDE_INDEX)
if(MUXER_INCLUDE_INDEX EQUAL -1)
    string(REPLACE
        "#include \"Record/Recorder.h\"\n"
        "#include \"Record/Recorder.h\"\n${MUXER_INCLUDE}\n"
        PATCHED_MK_RECORDER_CONTENT
        "${PATCHED_MK_RECORDER_CONTENT}")
else()
    while(TRUE)
        string(FIND
            "${PATCHED_MK_RECORDER_CONTENT}"
            "${DUPLICATE_MUXER_INCLUDE}"
            DUPLICATE_MUXER_INCLUDE_INDEX)
        if(DUPLICATE_MUXER_INCLUDE_INDEX EQUAL -1)
            break()
        endif()
        string(REPLACE
            "${DUPLICATE_MUXER_INCLUDE}"
            "${MUXER_INCLUDE}"
            PATCHED_MK_RECORDER_CONTENT
            "${PATCHED_MK_RECORDER_CONTENT}")
    endwhile()
endif()
if(PATCHED_MK_RECORDER_CONTENT STREQUAL MK_RECORDER_CONTENT)
    message(STATUS "ZLMediaKit recorder includes already patched")
else()
    file(WRITE "${MK_RECORDER_SOURCE}" "${PATCHED_MK_RECORDER_CONTENT}")
    message(STATUS "Patched ZLMediaKit recorder includes for ENABLE_MP4=OFF")
endif()

set(MK_RTP_SERVER_SOURCE "${SOURCE_DIR}/api/source/mk_rtp_server.cpp")
file(READ "${MK_RTP_SERVER_SOURCE}" MK_RTP_SERVER_CONTENT)
set(PATCHED_MK_RTP_SERVER_CONTENT "${MK_RTP_SERVER_CONTENT}")
string(FIND
    "${PATCHED_MK_RTP_SERVER_CONTENT}"
    "#include <memory>"
    MK_RTP_MEMORY_INCLUDE_INDEX)
if(MK_RTP_MEMORY_INCLUDE_INDEX EQUAL -1)
    string(REPLACE
        "#include \"Util/logger.h\""
        "#include \"Util/logger.h\"\n#include <memory>"
        PATCHED_MK_RTP_SERVER_CONTENT
        "${PATCHED_MK_RTP_SERVER_CONTENT}")
endif()
string(REPLACE
    "RtpServer::Ptr *server = new RtpServer::Ptr(new RtpServer);"
    "std::unique_ptr<RtpServer::Ptr> server(new RtpServer::Ptr(new RtpServer));"
    PATCHED_MK_RTP_SERVER_CONTENT
    "${PATCHED_MK_RTP_SERVER_CONTENT}")
string(REPLACE
    "return (mk_rtp_server)server;"
    "return (mk_rtp_server)server.release();"
    PATCHED_MK_RTP_SERVER_CONTENT
    "${PATCHED_MK_RTP_SERVER_CONTENT}")
if(PATCHED_MK_RTP_SERVER_CONTENT STREQUAL MK_RTP_SERVER_CONTENT)
    message(STATUS "ZLMediaKit RTP C API exception ownership already patched")
else()
    file(WRITE "${MK_RTP_SERVER_SOURCE}" "${PATCHED_MK_RTP_SERVER_CONTENT}")
    message(STATUS "Patched ZLMediaKit RTP C API exception ownership")
endif()

set(MK_COMMON_SOURCE "${SOURCE_DIR}/api/source/mk_common.cpp")
file(READ "${MK_COMMON_SOURCE}" MK_COMMON_CONTENT)
set(PATCHED_MK_COMMON_CONTENT "${MK_COMMON_CONTENT}")
string(FIND
    "${PATCHED_MK_COMMON_CONTENT}"
    "Drain asynchronous server destruction before returning"
    MK_COMMON_DRAIN_INDEX)
if(MK_COMMON_DRAIN_INDEX EQUAL -1)
    string(REPLACE
        "    stopAllTcpServer();\n}"
        [=[    stopAllTcpServer();

    // Drain asynchronous server destruction before returning. TcpServer and
    // UdpServer clone deleters post their final delete to the owning poller;
    // returning before those tasks run lets process shutdown race live objects.
    const auto drain = [](TaskExecutorGetterImp &pool) {
        pool.for_each([](const TaskExecutor::Ptr &executor) {
            const auto poller = std::static_pointer_cast<EventPoller>(executor);
            if (!poller->isCurrentThread())
                poller->sync([]() {});
        });
    };
    drain(EventPollerPool::Instance());
    drain(WorkThreadPool::Instance());
    // Destructors executed by the first barrier can enqueue cross-pool cleanup.
    drain(EventPollerPool::Instance());
    drain(WorkThreadPool::Instance());
}]=]
        PATCHED_MK_COMMON_CONTENT
        "${PATCHED_MK_COMMON_CONTENT}")
endif()
string(FIND
    "${PATCHED_MK_COMMON_CONTENT}"
    "Drain asynchronous server destruction before returning"
    PATCHED_MK_COMMON_DRAIN_INDEX)
if(PATCHED_MK_COMMON_DRAIN_INDEX EQUAL -1)
    message(FATAL_ERROR "Unable to patch ZLMediaKit synchronous server shutdown")
endif()
if(PATCHED_MK_COMMON_CONTENT STREQUAL MK_COMMON_CONTENT)
    message(STATUS "ZLMediaKit synchronous server shutdown already patched")
else()
    file(WRITE "${MK_COMMON_SOURCE}" "${PATCHED_MK_COMMON_CONTENT}")
    message(STATUS "Patched ZLMediaKit synchronous server shutdown")
endif()
