cmake_minimum_required(VERSION 3.14)
project(tna_grpc_server CXX)
set(CMAKE_CXX_STANDARD 17)

set(TNA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../Source/TNA")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPC REQUIRED grpc++ grpc protobuf)
pkg_check_modules(LIBPQ REQUIRED libpq)

include_directories(
    ${TNA_ROOT}/TNA
    ${TNA_ROOT}/TNA/header
)

set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_SRC "${PROTO_DIR}/tool_estimation.proto")
set(PB_CC  "${CMAKE_CURRENT_BINARY_DIR}/tool_estimation.pb.cc")
set(PB_H   "${CMAKE_CURRENT_BINARY_DIR}/tool_estimation.pb.h")
set(GRPC_CC "${CMAKE_CURRENT_BINARY_DIR}/tool_estimation.grpc.pb.cc")
set(GRPC_H  "${CMAKE_CURRENT_BINARY_DIR}/tool_estimation.grpc.pb.h")

find_program(PROTOC protoc)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)

add_custom_command(
    OUTPUT ${PB_CC} ${PB_H} ${GRPC_CC} ${GRPC_H}
    COMMAND ${PROTOC}
        --proto_path=${PROTO_DIR}
        --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
        --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        ${PROTO_SRC}
    DEPENDS ${PROTO_SRC}
    COMMENT "Generating protobuf/gRPC sources for tool_estimation"
)

set(TNA_SOURCES
    "${TNA_ROOT}/TNA/TNM_Algorithm.cpp"
    "${TNA_ROOT}/TNA/TNM_Greedy_dijk_addon.cpp"
    "${TNA_ROOT}/TNA/TNM_Net.cpp"
    "${TNA_ROOT}/TNA/TNM_EnumIO.cpp"
    "${TNA_ROOT}/TNA/TNM_utility.cpp"
    "${TNA_ROOT}/TNA/TNM.cpp"
    "${TNA_ROOT}/TNA/stdafx.cpp"
)

set(SERVER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/grpc_server_main.cpp")
set(TOOL_HELPER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/tool_estimation_cli.cpp")

add_executable(tna_grpc_server
    ${SERVER_SRC}
    ${TNA_SOURCES}
    ${PB_CC} ${GRPC_CC}
)

add_executable(tool_estimation_cli
    ${TOOL_HELPER_SRC}
    ${TNA_SOURCES}
)

set(OD_LOCAL_HELPER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/od_estimate_local_cli.cpp")

add_executable(od_estimate_local_cli
    ${OD_LOCAL_HELPER_SRC}
    ${TNA_SOURCES}
)

target_include_directories(tna_grpc_server PRIVATE
    ${TNA_ROOT}/TNA
    ${TNA_ROOT}/TNA/header
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${GRPC_INCLUDE_DIRS}
    ${LIBPQ_INCLUDE_DIRS}
)

target_include_directories(tool_estimation_cli PRIVATE
    ${TNA_ROOT}/TNA
    ${TNA_ROOT}/TNA/header
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBPQ_INCLUDE_DIRS}
)

target_include_directories(od_estimate_local_cli PRIVATE
    ${TNA_ROOT}/TNA
    ${TNA_ROOT}/TNA/header
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBPQ_INCLUDE_DIRS}
)

target_compile_definitions(tna_grpc_server PRIVATE _TNM_DLL TNM_EXT_CLASS= TNM_SHUTMYSQL _SHUTTNMMATLAB)
target_compile_definitions(tool_estimation_cli PRIVATE _TNM_DLL TNM_EXT_CLASS= TNM_SHUTMYSQL _SHUTTNMMATLAB)
target_compile_definitions(od_estimate_local_cli PRIVATE _TNM_DLL TNM_EXT_CLASS= TNM_SHUTMYSQL _SHUTTNMMATLAB)

target_link_libraries(tna_grpc_server PRIVATE
    ${GRPC_LIBRARIES}
    ${LIBPQ_LIBRARIES}
    pthread
)

target_link_libraries(tool_estimation_cli PRIVATE
    ${LIBPQ_LIBRARIES}
    pthread
)

target_link_libraries(od_estimate_local_cli PRIVATE
    ${LIBPQ_LIBRARIES}
    pthread
)

target_compile_options(tna_grpc_server PRIVATE -w)
target_compile_options(tool_estimation_cli PRIVATE -w)
target_compile_options(od_estimate_local_cli PRIVATE -w)

install(TARGETS tna_grpc_server DESTINATION bin)
install(TARGETS tool_estimation_cli DESTINATION bin)
install(TARGETS od_estimate_local_cli DESTINATION bin)
