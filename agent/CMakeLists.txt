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

# Proto generation
set(PROTO_DIR "${CMAKE_SOURCE_DIR}/../api/proto")
set(PROTO_FILES
  "${PROTO_DIR}/heartbeat.proto"
  "${PROTO_DIR}/metrics.proto"
)

set(PROTO_GEN_SRCS
  "${CMAKE_BINARY_DIR}/heartbeat.pb.cc"
  "${CMAKE_BINARY_DIR}/metrics.pb.cc"
)
set(PROTO_GEN_HDRS
  "${CMAKE_BINARY_DIR}/heartbeat.pb.h"
  "${CMAKE_BINARY_DIR}/metrics.pb.h"
)
set(GRPC_GEN_SRCS
  "${CMAKE_BINARY_DIR}/heartbeat.grpc.pb.cc"
  "${CMAKE_BINARY_DIR}/metrics.grpc.pb.cc"
)
set(GRPC_GEN_HDRS
  "${CMAKE_BINARY_DIR}/heartbeat.grpc.pb.h"
  "${CMAKE_BINARY_DIR}/metrics.grpc.pb.h"
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
  src/metrics/metrics_client.cpp
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
target_include_directories(pudim-agent PRIVATE ${CMAKE_BINARY_DIR})

# Tests
enable_testing()
add_executable(test-probes
  tests/test_probes.cpp
  src/metrics/probes.cpp
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
target_include_directories(test-probes PRIVATE ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/src)
add_test(NAME probe-tests COMMAND test-probes)

# Install
install(TARGETS pudim-agent DESTINATION /usr/local/bin)
