cmake_minimum_required(VERSION 3.20)
project(pudim-agent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Protobuf REQUIRED)
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
  src/metrics/probes.cpp
  src/metrics/pcap_probe.cpp
  src/metrics/metrics_client.cpp
  src/metrics/ntp_probe.cpp
  src/metrics/failover_client.cpp
  src/diagnostic_service.cpp
  src/systemd_notify.cpp
  src/trace_context.cpp
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
)
if(PCAP_FOUND)
  target_link_libraries(pudim-agent PRIVATE ${PCAP_LIBRARIES})
  target_include_directories(pudim-agent PRIVATE ${PCAP_INCLUDE_DIRS})
endif()
if(SYSTEMD_FOUND)
  target_link_libraries(pudim-agent PRIVATE ${SYSTEMD_LIBRARIES})
  target_include_directories(pudim-agent PRIVATE ${SYSTEMD_INCLUDE_DIRS})
endif()
target_include_directories(pudim-agent PRIVATE ${CMAKE_BINARY_DIR})

# Tests
enable_testing()
add_executable(test-probes
  tests/test_probes.cpp
  src/metrics/probes.cpp
  src/metrics/pcap_probe.cpp
  src/metrics/ntp_probe.cpp
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
)
if(PCAP_FOUND)
  target_link_libraries(test-probes PRIVATE ${PCAP_LIBRARIES})
  target_include_directories(test-probes PRIVATE ${PCAP_INCLUDE_DIRS})
endif()
target_include_directories(test-probes PRIVATE ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/src)
add_test(NAME probe-tests COMMAND test-probes)

# Install
install(TARGETS pudim-agent DESTINATION /usr/local/bin)
