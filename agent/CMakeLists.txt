cmake_minimum_required(VERSION 3.20)
project(pudim-agent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
# protobuf >= 5.x (as provided by vcpkg on Windows) must be consumed via its
# CMake CONFIG package so the abseil/utf8_range link dependencies are propagated.
# The FindProtobuf module links only libprotobuf, which fails at link time on
# Windows with unresolved utf8_range/absl symbols. Older system protobuf (e.g.
# Ubuntu's 3.21) ships no CONFIG package, so fall back to the module there.
find_package(protobuf CONFIG QUIET)
if(NOT protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()
find_package(gRPC REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
# libpcap is optional: without it the TCP-handshake probe degrades gracefully.
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(PCAP libpcap)
  pkg_check_modules(SYSTEMD libsystemd)
endif()
# SQLite3 (disk buffer) optional: without it the agent degrades to in-memory
# buffering only.
find_package(SQLite3)
if(SQLite3_FOUND)
  add_compile_definitions(HAVE_SQLITE3=1)
  message(STATUS "SQLite3 found: persistent disk buffer enabled")
else()
  message(STATUS "SQLite3 NOT found: persistent disk buffer disabled")
endif()
if(PCAP_FOUND)
  add_compile_definitions(HAVE_LIBPCAP=1)
  message(STATUS "libpcap found: TCP handshake capture enabled")
else()
  message(STATUS "libpcap NOT found: TCP handshake capture disabled")
endif()
if(SYSTEMD_FOUND)
  add_compile_definitions(HAVE_LIBSYSTEMD=1)
  message(STATUS "libsystemd found: sd_notify (Type=notify) enabled")
else()
  message(STATUS "libsystemd NOT found: sd_notify disabled")
endif()

# ---- Windows support (MSVC + vcpkg) ----
# Platform abstraction layer, Windows service integration and the getopt shim.
set(PUDIM_PLATFORM_SOURCES
  src/platform/platform.cpp
  src/platform/win_service.cpp
  src/platform/getopt.cpp
)
if(WIN32)
  # SIO_TCP_INFO / TCP_INFO_v0 require the Windows 10 SDK target.
  add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX)
  set(PUDIM_PLATFORM_LIBS ws2_32 iphlpapi bcrypt advapi32)
else()
  set(PUDIM_PLATFORM_LIBS)
endif()

# Proto generation
set(PROTO_DIR "${CMAKE_SOURCE_DIR}/../api/proto")
set(PROTO_FILES
  "${PROTO_DIR}/heartbeat.proto"
  "${PROTO_DIR}/metrics.proto"
  "${PROTO_DIR}/diagnostic.proto"
)

set(PROTO_GEN_SRCS
  "${CMAKE_BINARY_DIR}/heartbeat.pb.cc"
  "${CMAKE_BINARY_DIR}/metrics.pb.cc"
  "${CMAKE_BINARY_DIR}/diagnostic.pb.cc"
)
set(PROTO_GEN_HDRS
  "${CMAKE_BINARY_DIR}/heartbeat.pb.h"
  "${CMAKE_BINARY_DIR}/metrics.pb.h"
  "${CMAKE_BINARY_DIR}/diagnostic.pb.h"
)
set(GRPC_GEN_SRCS
  "${CMAKE_BINARY_DIR}/heartbeat.grpc.pb.cc"
  "${CMAKE_BINARY_DIR}/metrics.grpc.pb.cc"
  "${CMAKE_BINARY_DIR}/diagnostic.grpc.pb.cc"
)
set(GRPC_GEN_HDRS
  "${CMAKE_BINARY_DIR}/heartbeat.grpc.pb.h"
  "${CMAKE_BINARY_DIR}/metrics.grpc.pb.h"
  "${CMAKE_BINARY_DIR}/diagnostic.grpc.pb.h"
)

add_custom_command(
  OUTPUT ${PROTO_GEN_SRCS} ${PROTO_GEN_HDRS}
  COMMAND protobuf::protoc
    ARGS -I ${PROTO_DIR} --cpp_out=${CMAKE_BINARY_DIR}
    ${PROTO_FILES}
  DEPENDS ${PROTO_FILES}
  COMMENT "Generating protobuf C++ sources"
)

add_custom_command(
  OUTPUT ${GRPC_GEN_SRCS} ${GRPC_GEN_HDRS}
  COMMAND protobuf::protoc
    ARGS -I ${PROTO_DIR} --grpc_out=${CMAKE_BINARY_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
    ${PROTO_FILES}
  DEPENDS ${PROTO_FILES}
  COMMENT "Generating gRPC C++ sources"
)

# Library for generated proto/grpc code
add_library(pudim_proto STATIC
  ${PROTO_GEN_SRCS}
  ${GRPC_GEN_SRCS}
)
target_include_directories(pudim_proto PUBLIC ${CMAKE_BINARY_DIR})
target_link_libraries(pudim_proto
  PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

# Agent executable
add_executable(pudim-agent
  src/main.cpp
  src/agent_config.cpp
  src/metrics/probes.cpp
  src/metrics/pcap_probe.cpp
  src/metrics/metrics_client.cpp
  src/metrics/heartbeat_client.cpp
  src/metrics/ntp_probe.cpp
  src/metrics/failover_client.cpp
  src/diagnostic_service.cpp
  src/commands.cpp
  src/dns_resolver.cpp
  src/tls_context.cpp
  src/probe_runner.cpp
  src/systemd_notify.cpp
  src/trace_context.cpp
  src/disk_buffer.cpp
  src/tls_credentials.cpp
  ${PUDIM_PLATFORM_SOURCES}
)
target_link_libraries(pudim-agent
  PRIVATE
    pudim_proto
    protobuf::libprotobuf
    gRPC::grpc++
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    ${PUDIM_PLATFORM_LIBS}
)
if(PCAP_FOUND)
  target_link_libraries(pudim-agent PRIVATE ${PCAP_LIBRARIES})
  target_include_directories(pudim-agent PRIVATE ${PCAP_INCLUDE_DIRS})
endif()
if(SYSTEMD_FOUND)
  target_link_libraries(pudim-agent PRIVATE ${SYSTEMD_LIBRARIES})
  target_include_directories(pudim-agent PRIVATE ${SYSTEMD_INCLUDE_DIRS})
endif()
if(SQLite3_FOUND)
  target_link_libraries(pudim-agent PRIVATE SQLite::SQLite3)
endif()
target_include_directories(pudim-agent PRIVATE ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/src)

# Warnings for the agent's own sources (proto/grpc code lives in pudim_proto).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(pudim-agent PRIVATE -Wall -Wextra)
endif()

# Tests
enable_testing()
add_executable(test-probes
  tests/test_probes.cpp
  src/metrics/probes.cpp
  src/metrics/pcap_probe.cpp
  src/metrics/ntp_probe.cpp
  src/dns_resolver.cpp
  src/tls_context.cpp
  src/disk_buffer.cpp
  src/platform/platform.cpp
)
target_link_libraries(test-probes
  PRIVATE
    pudim_proto
    protobuf::libprotobuf
    gRPC::grpc++
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    ${PUDIM_PLATFORM_LIBS}
)
if(PCAP_FOUND)
  target_link_libraries(test-probes PRIVATE ${PCAP_LIBRARIES})
  target_include_directories(test-probes PRIVATE ${PCAP_INCLUDE_DIRS})
endif()
if(SQLite3_FOUND)
  target_link_libraries(test-probes PRIVATE SQLite::SQLite3)
endif()
target_include_directories(test-probes PRIVATE ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test-probes PRIVATE -Wall -Wextra)
endif()
add_test(NAME probe-tests COMMAND test-probes)

# Config-file parser unit tests (layered configuration)
add_executable(test-config
  tests/test_config.cpp
)
target_include_directories(test-config PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test-config PRIVATE -Wall -Wextra)
endif()
add_test(NAME config-tests COMMAND test-config)

# Agent-config resolution unit tests (defaults < config file < CLI flags)
add_executable(test-agent-config
  tests/test_agent_config.cpp
  src/agent_config.cpp
  src/platform/getopt.cpp
)
target_include_directories(test-agent-config PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test-agent-config PRIVATE -Wall -Wextra)
endif()
add_test(NAME agent-config-tests COMMAND test-agent-config)

# Pre-set command catalog unit tests
add_executable(test-commands
  tests/test_commands.cpp
  src/commands.cpp
)
target_link_libraries(test-commands PRIVATE pudim_proto Threads::Threads)
target_include_directories(test-commands PRIVATE ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test-commands PRIVATE -Wall -Wextra)
endif()
add_test(NAME command-tests COMMAND test-commands)

# Install
install(TARGETS pudim-agent DESTINATION /usr/local/bin)
