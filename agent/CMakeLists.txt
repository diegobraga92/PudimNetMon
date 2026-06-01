cmake_minimum_required(VERSION 3.20)
project(pudim-agent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(Threads REQUIRED)

# Proto generation
set(PROTO_DIR "${CMAKE_SOURCE_DIR}/../api/proto")
set(PROTO_SRC "${PROTO_DIR}/heartbeat.proto")

set(PROTO_GEN_SRCS "${CMAKE_BINARY_DIR}/heartbeat.pb.cc")
set(PROTO_GEN_HDRS "${CMAKE_BINARY_DIR}/heartbeat.pb.h")
set(GRPC_GEN_SRCS "${CMAKE_BINARY_DIR}/heartbeat.grpc.pb.cc")
set(GRPC_GEN_HDRS "${CMAKE_BINARY_DIR}/heartbeat.grpc.pb.h")

add_custom_command(
  OUTPUT ${PROTO_GEN_SRCS} ${PROTO_GEN_HDRS}
  COMMAND protobuf::protoc
    ARGS -I ${PROTO_DIR} --cpp_out=${CMAKE_BINARY_DIR}
    ${PROTO_SRC}
  DEPENDS ${PROTO_SRC}
  COMMENT "Generating protobuf C++ sources"
)

add_custom_command(
  OUTPUT ${GRPC_GEN_SRCS} ${GRPC_GEN_HDRS}
  COMMAND protobuf::protoc
    ARGS -I ${PROTO_DIR} --grpc_out=${CMAKE_BINARY_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
    ${PROTO_SRC}
  DEPENDS ${PROTO_SRC}
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
add_executable(pudim-agent src/main.cpp)
target_link_libraries(pudim-agent
  PRIVATE
    pudim_proto
    protobuf::libprotobuf
    gRPC::grpc++
    Threads::Threads
)
target_include_directories(pudim-agent PRIVATE ${CMAKE_BINARY_DIR})

# Install
install(TARGETS pudim-agent DESTINATION /usr/local/bin)