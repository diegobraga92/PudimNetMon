cmake_minimum_required(VERSION 3.20)
project(pudim-agent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Export a compilation database so clangd (and other tools) can resolve the
# protobuf/gRPC headers generated into ${CMAKE_BINARY_DIR} (*.pb.h).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
  # The link target name differs by provider: vcpkg's config package exports
  # SQLite3::SQLite3 and deprecates the SQLite::SQLite3 alias (CMake emits an
  # author warning for it), while CMake's FindSQLite3 module only exports
  # SQLite::SQLite3. Prefer the non-deprecated name when it is available.
  if(TARGET SQLite3::SQLite3)
    set(PUDIM_SQLITE3_TARGET SQLite3::SQLite3)
  else()
    set(PUDIM_SQLITE3_TARGET SQLite::SQLite3)
  endif()
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

option(PUDIM_STATIC_GRPC "Statically link gRPC/protobuf/abseil (Linux release builds)" OFF)

# Everything gRPC/protobuf/abseil is bundled into the executable when PUDIM_STATIC_GRPC=ON.
set(PUDIM_PROTO_LINK_TARGETS protobuf::libprotobuf gRPC::grpc++)

if(PUDIM_STATIC_GRPC)
  if(WIN32 OR APPLE)
    message(FATAL_ERROR "PUDIM_STATIC_GRPC is only supported on Linux")
  endif()
  if(NOT PkgConfig_FOUND)
    find_package(PkgConfig REQUIRED)
  endif()

  # pkg-config --static lists gRPC's full transitive closure (its own libraries,
  # upb, re2, c-ares and every abseil library) in a dependency-safe order.
  execute_process(
    COMMAND pkg-config --libs --static grpc++
    OUTPUT_VARIABLE _pudim_grpc_static_flags
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _pudim_pc_rc)
  if(NOT _pudim_pc_rc EQUAL 0 OR _pudim_grpc_static_flags STREQUAL "")
    message(FATAL_ERROR "pkg-config --libs --static grpc++ failed; install the static gRPC/abseil dev packages")
  endif()
  execute_process(
    COMMAND pkg-config --variable=libdir grpc++
    OUTPUT_VARIABLE _pudim_libdir
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _pudim_libdir)
    set(_pudim_libdir "/usr/lib")
  endif()

  set(_pudim_lib_dirs "${_pudim_libdir}")
  if(CMAKE_LIBRARY_ARCHITECTURE)
    list(APPEND _pudim_lib_dirs "${_pudim_libdir}/${CMAKE_LIBRARY_ARCHITECTURE}")
    list(APPEND _pudim_lib_dirs "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}")
  endif()

  # C++ libraries to bundle statically. Everything else (ssl, crypto, z, zstd,
  # dl, pthread, rt, atomic) stays dynamic so it can be shared with curl.
  set(_pudim_static_re "^-l(grpc\\+\\+|grpc|gpr|upb|address_sorting|protobuf|protobuf-lite|re2|cares|absl_.*)$")
  set(_pudim_grpc_static_libs "")
  set(_pudim_grpc_dynamic_flags "")
  separate_arguments(_pudim_tokens UNIX_COMMAND "${_pudim_grpc_static_flags}")
  foreach(_tok IN LISTS _pudim_tokens)
    if(_tok MATCHES "${_pudim_static_re}")
      string(REGEX REPLACE "^-l" "" _name "${_tok}")

      set(_pudim_arch "")
      foreach(_dir IN LISTS _pudim_lib_dirs)
        if(EXISTS "${_dir}/lib${_name}.a")
          set(_pudim_arch "${_dir}/lib${_name}.a")
          break()
        endif()
      endforeach()
      if(NOT _pudim_arch)
        # Some distros ship no static archive for a dependency (c-ares on Ubuntu
        # 24.04). Fall back to the shared library rather than failing the build
        message(STATUS "  ${_name}: no static archive, linking shared")
        list(APPEND _pudim_grpc_dynamic_flags "-l${_name}")
      else()
        list(APPEND _pudim_grpc_static_libs "${_pudim_arch}")
      endif()
    elseif(_tok MATCHES "^-l:libjitterentropy\\.a$")
      # Only pulled in by a *static* libcrypto; OpenSSL stays dynamic here.
    else()
      list(APPEND _pudim_grpc_dynamic_flags "${_tok}")
    endif()
  endforeach()

  # gRPC's .pc does not list every abseil archive our code pulls in through the
  # generated stubs (e.g. absl::log_internal::LogMessage), so bundle them all.
  set(_pudim_absl_archives "")
  foreach(_dir IN LISTS _pudim_lib_dirs)
    file(GLOB _pudim_absl_globbed "${_dir}/libabsl_*.a")
    if(_pudim_absl_globbed)
      set(_pudim_absl_archives ${_pudim_absl_globbed})
      break()
    endif()
  endforeach()
  if(NOT _pudim_absl_archives)
    message(FATAL_ERROR "no static abseil archives found in ${_pudim_lib_dirs}")
  endif()
  list(FILTER _pudim_absl_archives EXCLUDE REGEX "libabsl_[^/]*(test|testing|mock|matcher)[^/]*\\.a$")
  list(APPEND _pudim_grpc_static_libs ${_pudim_absl_archives})

  # gRPC's .pc omits protobuf (it is a separate package)
  set(_pudim_protobuf_arch "")
  foreach(_dir IN LISTS _pudim_lib_dirs)
    if(EXISTS "${_dir}/libprotobuf.a")
      set(_pudim_protobuf_arch "${_dir}/libprotobuf.a")
      break()
    endif()
  endforeach()
  if(NOT _pudim_protobuf_arch)
    message(STATUS "  protobuf: no static archive, linking shared")
    list(APPEND _pudim_grpc_dynamic_flags -lprotobuf)
  else()
    list(APPEND _pudim_grpc_static_libs "${_pudim_protobuf_arch}")
  endif()
  list(REMOVE_DUPLICATES _pudim_grpc_static_libs)

  # --start-group resolves the cyclic references between the static archives.
  set(PUDIM_PROTO_LINK_TARGETS
    -Wl,--start-group ${_pudim_grpc_static_libs} -Wl,--end-group
    ${_pudim_grpc_dynamic_flags})
  message(STATUS "gRPC/protobuf/abseil: static (${_pudim_libdir})")
else()
  message(STATUS "gRPC/protobuf/abseil: shared")
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
    ${PUDIM_PROTO_LINK_TARGETS}
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
    ${PUDIM_PROTO_LINK_TARGETS}
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
  target_link_libraries(pudim-agent PRIVATE ${PUDIM_SQLITE3_TARGET})
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
    ${PUDIM_PROTO_LINK_TARGETS}
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
  target_link_libraries(test-probes PRIVATE ${PUDIM_SQLITE3_TARGET})
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

# Agent-config resolution unit tests (defaults < config file < CLI flags).
# platform.cpp is required on Windows because agent_config.cpp resolves the
# default config-file/disk-buffer paths through platform::DefaultStateDir().
add_executable(test-agent-config
  tests/test_agent_config.cpp
  src/agent_config.cpp
  src/platform/getopt.cpp
  src/platform/platform.cpp
)
target_link_libraries(test-agent-config PRIVATE ${PUDIM_PLATFORM_LIBS})
target_include_directories(test-agent-config PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test-agent-config PRIVATE -Wall -Wextra)
endif()
add_test(NAME agent-config-tests COMMAND test-agent-config)

# Pre-set command catalog unit tests. platform.cpp is required on Windows
# because commands.cpp resolves the state directory via
# platform::DefaultStateDir() in the disk-usage command.
add_executable(test-commands
  tests/test_commands.cpp
  src/commands.cpp
  src/platform/platform.cpp
)
target_link_libraries(test-commands PRIVATE pudim_proto Threads::Threads ${PUDIM_PLATFORM_LIBS})
target_include_directories(test-commands PRIVATE ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test-commands PRIVATE -Wall -Wextra)
endif()
add_test(NAME command-tests COMMAND test-commands)

# Platform helper unit tests (local-address derivation for the advertised
# diagnostic endpoint). No collector or network traffic is required.
add_executable(test-platform
  tests/test_platform.cpp
  src/platform/platform.cpp
)
target_link_libraries(test-platform PRIVATE ${PUDIM_PLATFORM_LIBS})
target_include_directories(test-platform PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test-platform PRIVATE -Wall -Wextra)
endif()
add_test(NAME platform-tests COMMAND test-platform)

# Install
install(TARGETS pudim-agent DESTINATION /usr/local/bin)
