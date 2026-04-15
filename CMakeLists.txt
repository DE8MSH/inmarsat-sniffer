cmake_minimum_required(VERSION 3.9)
project(inmarsat-sniffer C CXX)
enable_language(CXX)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/Modules" ${CMAKE_MODULE_PATH})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-common -Wall -Wextra -Wno-unused-parameter -std=c99 -Werror=implicit-function-declaration")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter -std=c++11")

set(CMAKE_C_FLAGS_DEBUG "-ggdb3 -g3 -O0 -fsanitize=address")
set(CMAKE_C_FLAGS_MINSIZEREL "-Os")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O3 -march=native -ggdb3 -g3")
set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
      "Choose the type of build: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# SDR backends (all optional)
find_package(SoapySDR)
find_package(SDRplay)
find_package(RTLSDR)

set(HAVE_ANY_SDR FALSE)
if(SOAPYSDR_FOUND)
    set(HAVE_ANY_SDR TRUE)
endif()
if(SDRPLAY_FOUND)
    set(HAVE_ANY_SDR TRUE)
endif()
if(RTLSDR_FOUND)
    set(HAVE_ANY_SDR TRUE)
endif()

if(NOT HAVE_ANY_SDR)
    message(WARNING "No SDR backend found. Live capture will not be available.\n"
                    "Install SoapySDR: sudo apt install libsoapysdr-dev")
endif()

include_directories(${PROJECT_SOURCE_DIR})

set(SOURCES
    ${PROJECT_SOURCE_DIR}/main.c
    ${PROJECT_SOURCE_DIR}/options.c
    ${PROJECT_SOURCE_DIR}/satellites.c
    ${PROJECT_SOURCE_DIR}/channelizer.c
    ${PROJECT_SOURCE_DIR}/demod_dbpsk.c
    ${PROJECT_SOURCE_DIR}/viterbi.c
    ${PROJECT_SOURCE_DIR}/stdc_decode.c
    ${PROJECT_SOURCE_DIR}/feed.c
    ${PROJECT_SOURCE_DIR}/web.c
    ${PROJECT_SOURCE_DIR}/vita49.c
    ${PROJECT_SOURCE_DIR}/acars_position.c
    ${PROJECT_SOURCE_DIR}/waypoint_db.c
    ${PROJECT_SOURCE_DIR}/learned_waypoints.c
    ${PROJECT_SOURCE_DIR}/basestation.c
    ${PROJECT_SOURCE_DIR}/aircraft_db.c
    ${PROJECT_SOURCE_DIR}/simd_generic.c
)

# SIMD kernels — platform-specific, runtime-detected (ported from iridium-sniffer)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|x86|i[3-6]86")
    set_source_files_properties(${PROJECT_SOURCE_DIR}/simd_sse42.c
        PROPERTIES COMPILE_FLAGS "-msse4.2")
    set_source_files_properties(${PROJECT_SOURCE_DIR}/simd_avx2.c
        PROPERTIES COMPILE_FLAGS "-mavx2 -mfma")
    list(APPEND SOURCES
        ${PROJECT_SOURCE_DIR}/simd_sse42.c
        ${PROJECT_SOURCE_DIR}/simd_avx2.c)
    message(STATUS "SIMD: SSE4.2 + AVX2+FMA kernels enabled (runtime-detected)")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|ARM|aarch64|AARCH64")
    list(APPEND SOURCES
        ${PROJECT_SOURCE_DIR}/simd_neon.c)
    message(STATUS "SIMD: NEON kernels enabled (AArch64)")
else()
    message(STATUS "SIMD: scalar only (unrecognised platform)")
endif()

# JAERO DSP library (C++ with C API, stripped of Qt)
set(JAERO_DSP_SOURCES
    ${PROJECT_SOURCE_DIR}/jaero_dsp/DSP.cpp
    ${PROJECT_SOURCE_DIR}/jaero_dsp/burstmskdemodulator.cpp
    ${PROJECT_SOURCE_DIR}/jaero_dsp/burstoqpskdemodulator.cpp
    ${PROJECT_SOURCE_DIR}/jaero_dsp/mskdemodulator.cpp
    ${PROJECT_SOURCE_DIR}/jaero_dsp/coarsefreqestimate.cpp
    ${PROJECT_SOURCE_DIR}/jaero_dsp/fftrwrapper.cpp
    ${PROJECT_SOURCE_DIR}/jaero_dsp/fftwrapper.cpp
    ${PROJECT_SOURCE_DIR}/jaero_dsp/jfft.cpp
    ${PROJECT_SOURCE_DIR}/jaero_dsp/jconvolutionalcodec.cpp
    ${PROJECT_SOURCE_DIR}/jaero_dsp/aerol.cpp
    ${PROJECT_SOURCE_DIR}/jaero_dsp/jaero_demod.cpp
)
list(APPEND SOURCES ${JAERO_DSP_SOURCES})
include_directories(${PROJECT_SOURCE_DIR}/jaero_dsp)

# Optional ZMQ for JAERO-compatible audio output
find_library(ZMQ_LIB zmq)
find_path(ZMQ_INC zmq.h)
if(ZMQ_LIB AND ZMQ_INC)
    set(ZMQ_FOUND TRUE)
    list(APPEND SOURCES ${PROJECT_SOURCE_DIR}/zmq_audio.c)
    include_directories(${ZMQ_INC})
endif()

if(SOAPYSDR_FOUND)
    list(APPEND SOURCES ${PROJECT_SOURCE_DIR}/soapysdr.c)
    include_directories(${SOAPYSDR_INCLUDE_DIR})
endif()

if(SDRPLAY_FOUND)
    list(APPEND SOURCES ${PROJECT_SOURCE_DIR}/sdrplay.c)
    include_directories(${SDRPLAY_INCLUDE_DIR})
endif()

if(RTLSDR_FOUND)
    list(APPEND SOURCES ${PROJECT_SOURCE_DIR}/rtlsdr.c)
    include_directories(${RTLSDR_INCLUDE_DIR})
endif()

add_executable(inmarsat-sniffer ${SOURCES})

target_link_libraries(inmarsat-sniffer PRIVATE
    Threads::Threads
    m
    stdc++
    correct
)

if(ZMQ_FOUND)
    target_link_libraries(inmarsat-sniffer PRIVATE ${ZMQ_LIB})
    target_compile_definitions(inmarsat-sniffer PRIVATE HAVE_ZMQ)
    message(STATUS "ZMQ: enabled (JAERO audio output)")
else()
    message(STATUS "ZMQ: not found (JAERO output disabled)")
endif()

if(LIBACARS_FOUND)
    target_link_libraries(inmarsat-sniffer PRIVATE ${LIBACARS_LIBRARIES})
    target_include_directories(inmarsat-sniffer PRIVATE ${LIBACARS_INCLUDE_DIRS})
    target_compile_definitions(inmarsat-sniffer PRIVATE HAVE_LIBACARS)
    message(STATUS "libacars: enabled")
else()
    message(STATUS "libacars: not found (ACARS sublayer decoding disabled)")
endif()

# Optional MQTT for publishing ACARS messages
find_library(MOSQUITTO_LIB mosquitto)
find_path(MOSQUITTO_INC mosquitto.h)
if(MOSQUITTO_LIB AND MOSQUITTO_INC)
    set(MOSQUITTO_FOUND TRUE)
    list(APPEND SOURCES ${PROJECT_SOURCE_DIR}/mqtt.c)
    include_directories(${MOSQUITTO_INC})
endif()

if(MOSQUITTO_FOUND)
    target_link_libraries(inmarsat-sniffer PRIVATE ${MOSQUITTO_LIB})
    target_compile_definitions(inmarsat-sniffer PRIVATE HAVE_MQTT)
    message(STATUS "MQTT: enabled")
else()
    message(STATUS "MQTT: not found (install libmosquitto-dev)")
endif()

if(SOAPYSDR_FOUND)
    target_link_libraries(inmarsat-sniffer PRIVATE ${SOAPYSDR_LIBRARIES})
    target_compile_definitions(inmarsat-sniffer PRIVATE HAVE_SOAPYSDR)
    message(STATUS "SoapySDR: enabled")
else()
    message(STATUS "SoapySDR: not found")
endif()

if(SDRPLAY_FOUND)
    target_link_libraries(inmarsat-sniffer PRIVATE ${SDRPLAY_LIBRARIES})
    target_compile_definitions(inmarsat-sniffer PRIVATE HAVE_SDRPLAY)
    message(STATUS "SDRplay: enabled")
else()
    message(STATUS "SDRplay: not found")
endif()

if(RTLSDR_FOUND)
    target_link_libraries(inmarsat-sniffer PRIVATE ${RTLSDR_LIBRARIES})
    target_compile_definitions(inmarsat-sniffer PRIVATE HAVE_RTLSDR)
    message(STATUS "RTL-SDR: enabled")
else()
    message(STATUS "RTL-SDR: not found")
endif()

set_property(TARGET inmarsat-sniffer PROPERTY C_STANDARD 99)
set_property(TARGET inmarsat-sniffer PROPERTY CXX_STANDARD 11)

install(TARGETS inmarsat-sniffer DESTINATION bin)
