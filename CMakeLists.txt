cmake_minimum_required(VERSION 3.16)
project(avtrans LANGUAGES CXX)

option(ENABLE_FFMPEG "Enable FFmpeg-based ingest" ON)
option(ENABLE_WEBRTC "Enable native WebRTC publisher integration" OFF)

set(WEBRTC_ROOT "" CACHE PATH "Path to a libwebrtc source tree root, typically .../webrtc/src")
set(WEBRTC_LIBRARY "" CACHE FILEPATH "Path to the built libwebrtc library")
set(WEBRTC_EXTRA_LIBS "" CACHE STRING "Extra libraries needed to link libwebrtc")
set(WEBRTC_OUT_DIR "" CACHE PATH "Path to the libwebrtc GN output directory, typically .../webrtc/src/out/avtrans")

add_executable(avtrans
    src/main.cpp
    src/ffmpeg_ingest.cpp
    src/webrtc_bridge.cpp
    src/signaling_server.cpp
)

target_include_directories(avtrans PRIVATE src)

if(ENABLE_WEBRTC)
    set_target_properties(avtrans PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(avtrans PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

if(ENABLE_FFMPEG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
        libswresample
        libswscale
    )
    target_link_libraries(avtrans PRIVATE PkgConfig::FFMPEG)
    target_compile_definitions(avtrans PRIVATE AVTRANS_HAS_FFMPEG=1)
endif()

if(ENABLE_WEBRTC)
    if(NOT WEBRTC_ROOT)
        message(FATAL_ERROR "ENABLE_WEBRTC=ON requires -DWEBRTC_ROOT=/path/to/webrtc/src")
    endif()
    if(NOT WEBRTC_LIBRARY)
        message(FATAL_ERROR "ENABLE_WEBRTC=ON requires -DWEBRTC_LIBRARY=/path/to/libwebrtc.a or .so")
    endif()
    if(NOT WEBRTC_OUT_DIR)
        message(FATAL_ERROR "ENABLE_WEBRTC=ON requires -DWEBRTC_OUT_DIR=/path/to/webrtc/src/out/...")
    endif()

    set(WEBRTC_CLANG_BIN ${WEBRTC_ROOT}/third_party/llvm-build/Release+Asserts/bin)
    set(WEBRTC_SYSROOT ${WEBRTC_ROOT}/build/linux/debian_bullseye_amd64-sysroot)
    set(WEBRTC_SHIM_SO ${CMAKE_BINARY_DIR}/libavtrans_webrtc.so)
    set(WEBRTC_SHIM_OBJ ${CMAKE_BINARY_DIR}/webrtc_shim.o)
    set(WEBRTC_LIBCXX ${WEBRTC_OUT_DIR}/obj/buildtools/third_party/libc++/libc++.a)
    set(WEBRTC_LIBCXXABI ${WEBRTC_OUT_DIR}/obj/buildtools/third_party/libc++abi/libc++abi.a)
    set(WEBRTC_BUILTIN_VDEC ${WEBRTC_OUT_DIR}/obj/api/video_codecs/libbuiltin_video_decoder_factory.a)
    set(WEBRTC_BUILTIN_VENC ${WEBRTC_OUT_DIR}/obj/api/video_codecs/libbuiltin_video_encoder_factory.a)
    set(WEBRTC_CHECKS ${WEBRTC_OUT_DIR}/obj/rtc_base/libchecks.a)
    set(WEBRTC_LOGGING ${WEBRTC_OUT_DIR}/obj/rtc_base/liblogging.a)
    set(WEBRTC_SEQCHK ${WEBRTC_OUT_DIR}/obj/rtc_base/synchronization/libsequence_checker_internal.a)
    set(WEBRTC_INTERNAL_VCODECS ${WEBRTC_OUT_DIR}/obj/media/librtc_internal_video_codecs.a)
    set(WEBRTC_SIMULCAST ${WEBRTC_OUT_DIR}/obj/media/librtc_simulcast_encoder_adapter.a)
    set(WEBRTC_SW_FALLBACK ${WEBRTC_OUT_DIR}/obj/api/video_codecs/librtc_software_fallback_wrappers.a)

    add_custom_command(
        OUTPUT ${WEBRTC_SHIM_OBJ}
        COMMAND ${WEBRTC_CLANG_BIN}/clang++
            -DAVTRANS_WEBRTC_SHIM_EXPORT=1
            -DWEBRTC_POSIX
            -DWEBRTC_LINUX
            -DWEBRTC_USE_H264
            -DWEBRTC_LIBRARY_IMPL
            -DWEBRTC_ENABLE_PROTOBUF=1
            -DWEBRTC_STRICT_FIELD_TRIALS=0
            -DNDEBUG
            -DNVALGRIND
            -DDYNAMIC_ANNOTATIONS_ENABLED=0
            -DWEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE
            -DRTC_ENABLE_VP9
            -DRTC_ENABLE_H265
            -DRTC_DAV1D_IN_INTERNAL_DECODER_FACTORY
            -DWEBRTC_HAVE_SCTP
            -DWEBRTC_ENCODER_PSNR_STATS
            -DWEBRTC_ENABLE_AVX2
            -DWEBRTC_DEPRECATE_PLAN_B
            -DWEBRTC_NON_STATIC_TRACE_EVENT_HANDLERS=0
            -DABSL_ALLOCATOR_NOTHROW=1
            -D_GNU_SOURCE
            -D_FILE_OFFSET_BITS=64
            -D_LARGEFILE_SOURCE
            -D_LARGEFILE64_SOURCE
            -D__STDC_CONSTANT_MACROS
            -D__STDC_FORMAT_MACROS
            -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE
            -D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS
            -D_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS
            -I${WEBRTC_ROOT}
            -I${WEBRTC_OUT_DIR}/gen
            -I${WEBRTC_ROOT}/third_party/abseil-cpp
            -nostdinc++
            -isystem ${WEBRTC_OUT_DIR}/gen/third_party/libc++/src/include
            -isystem ${WEBRTC_ROOT}/third_party/libc++abi/src/include
            -isystem ${WEBRTC_SYSROOT}/usr/include/x86_64-linux-gnu
            -isystem ${WEBRTC_SYSROOT}/usr/include
            --target=x86_64-unknown-linux-gnu
            --sysroot=${WEBRTC_SYSROOT}
            -std=c++20
            -fPIC
            -fno-exceptions
            -pthread
            -c ${CMAKE_SOURCE_DIR}/src/webrtc_shim.cpp
            -o ${WEBRTC_SHIM_OBJ}
        DEPENDS ${CMAKE_SOURCE_DIR}/src/webrtc_shim.cpp ${CMAKE_SOURCE_DIR}/src/webrtc_shim.h
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${WEBRTC_SHIM_SO}
        COMMAND ${WEBRTC_CLANG_BIN}/clang++
            -shared
            -fuse-ld=lld
            --target=x86_64-unknown-linux-gnu
            --sysroot=${WEBRTC_SYSROOT}
            -o ${WEBRTC_SHIM_SO}
            ${WEBRTC_SHIM_OBJ}
            ${WEBRTC_LIBRARY}
            ${WEBRTC_BUILTIN_VDEC}
            ${WEBRTC_BUILTIN_VENC}
            ${WEBRTC_INTERNAL_VCODECS}
            ${WEBRTC_SIMULCAST}
            ${WEBRTC_SW_FALLBACK}
            ${WEBRTC_CHECKS}
            ${WEBRTC_LOGGING}
            ${WEBRTC_SEQCHK}
            ${WEBRTC_LIBCXX}
            ${WEBRTC_LIBCXXABI}
            -Wl,--exclude-libs,ALL
            -lpthread
            -ldl
            -lm
            -lX11
        DEPENDS ${WEBRTC_SHIM_OBJ} ${WEBRTC_LIBRARY}
        VERBATIM
    )

    add_custom_target(avtrans_webrtc_shim ALL DEPENDS ${WEBRTC_SHIM_SO})

    add_library(avtrans_webrtc SHARED IMPORTED GLOBAL)
    set_target_properties(avtrans_webrtc PROPERTIES IMPORTED_LOCATION ${WEBRTC_SHIM_SO})

    add_dependencies(avtrans avtrans_webrtc_shim)
    target_link_libraries(avtrans PRIVATE avtrans_webrtc)

    target_compile_definitions(avtrans PRIVATE
        AVTRANS_HAS_WEBRTC=1
    )

    if(WEBRTC_EXTRA_LIBS)
        separate_arguments(WEBRTC_EXTRA_LIBS_LIST NATIVE_COMMAND "${WEBRTC_EXTRA_LIBS}")
        set_property(TARGET avtrans_webrtc_shim APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${WEBRTC_EXTRA_LIBS_LIST}")
    endif()
endif()
