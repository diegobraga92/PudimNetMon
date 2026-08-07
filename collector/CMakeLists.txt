cmake_minimum_required(VERSION 3.20)
project(pudim-collector VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(Threads REQUIRED)
find_package(PostgreSQL REQUIRED)

# Fetch cpp-httplib for the HTTP health/metrics endpoints
include(FetchContent)
FetchContent_Declare(
  cpp-httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.18.2
)
FetchContent_MakeAvailable(cpp-httplib)

# Fetch nlohmann/json for alert rule configuration parsing (header-only)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

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

# Collector executable
add_executable(pudim-collector
  src/main.cpp
  src/storage/timescale_storage.cpp
  src/metrics_service.cpp
  src/alerting/alert_rule.cpp
  src/alerting/alert_manager.cpp
  src/alerting/notifier.cpp
)
target_link_libraries(pudim-collector
  PRIVATE
    pudim_proto
    protobuf::libprotobuf
    gRPC::grpc++
    Threads::Threads
    httplib::httplib
    nlohmann_json::nlohmann_json
    PostgreSQL::PostgreSQL
)
target_include_directories(pudim-collector PRIVATE ${CMAKE_BINARY_DIR})

# Tests
enable_testing()
add_executable(test-storage
  tests/test_storage.cpp
  src/storage/timescale_storage.cpp
)
target_link_libraries(test-storage
  PRIVATE
    pudim_proto
    protobuf::libprotobuf
    gRPC::grpc++
    Threads::Threads
    PostgreSQL::PostgreSQL
)
target_include_directories(test-storage PRIVATE ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/src)
add_test(NAME storage-tests COMMAND test-storage)

# End-to-end gRPC test: collector server + real agent client (cross-directory
# reuse of the agent's MetricsClient). Skips gracefully without TimescaleDB.
add_executable(test-e2e-grpc
  tests/test_e2e_grpc.cpp
  src/metrics_service.cpp
  src/storage/timescale_storage.cpp
  src/alerting/alert_rule.cpp
  src/alerting/alert_manager.cpp
  src/alerting/notifier.cpp
  ../agent/src/metrics/metrics_client.cpp
)
target_link_libraries(test-e2e-grpc
  PRIVATE
    pudim_proto
    protobuf::libprotobuf
    gRPC::grpc++
    Threads::Threads
    httplib::httplib
    nlohmann_json::nlohmann_json
    PostgreSQL::PostgreSQL
)
target_include_directories(test-e2e-grpc
  PRIVATE
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/../agent/src/metrics
)
add_test(NAME e2e-grpc-tests COMMAND test-e2e-grpc)

# Alerting state machine unit tests (no database required)
add_executable(test-alerting
  tests/test_alerting.cpp
  src/alerting/alert_rule.cpp
  src/alerting/alert_manager.cpp
  src/alerting/notifier.cpp
)
target_link_libraries(test-alerting
  PRIVATE
    pudim_proto
    protobuf::libprotobuf
    gRPC::grpc++
    Threads::Threads
    httplib::httplib
    nlohmann_json::nlohmann_json
)
target_include_directories(test-alerting
  PRIVATE
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME alerting-tests COMMAND test-alerting)

# Install
install(TARGETS pudim-collector DESTINATION /usr/local/bin)
