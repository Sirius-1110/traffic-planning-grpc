cmake_minimum_required(VERSION 3.14)
project(transit_assignment_cli CXX)
set(CMAKE_CXX_STANDARD 17)

# 外部变量：TRANSIT_ROOT 指向 transitCapTL 项目源码根目录，结构约定：
#   ${TRANSIT_ROOT}/Source/DllProj/transitNet/PTNet.{h,cpp}
#   ${TRANSIT_ROOT}/Source/DllProj/trafficNet/...
#   ${TRANSIT_ROOT}/include/...
# 在 build_on_target.sh 或命令行里通过 -DTRANSIT_ROOT=/path/to/transitCapTL 传入。

if(NOT DEFINED TRANSIT_ROOT)
    message(FATAL_ERROR "TRANSIT_ROOT not set. Pass -DTRANSIT_ROOT=<path to transitCapTL> to cmake.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQ REQUIRED libpq)

set(TRANSIT_DLLPROJ "${TRANSIT_ROOT}/Source/DllProj")
set(TRANSIT_INCLUDE "${TRANSIT_ROOT}/include")

# transit_assignment 只部署无容量分配，因此只编译 PTNET/TEAP 两个核心源文件。
set(TRANSIT_NET_SOURCES
    "${TRANSIT_DLLPROJ}/transitNet/PTNet.cpp"
    "${TRANSIT_DLLPROJ}/transitNet/TEAP.cpp"
    "${TRANSIT_DLLPROJ}/transitNet/MMCapacity.cpp"
)
file(GLOB TRAFFIC_NET_SOURCES  "${TRANSIT_DLLPROJ}/trafficNet/*.cpp")
list(REMOVE_ITEM TRAFFIC_NET_SOURCES "${TRANSIT_DLLPROJ}/trafficNet/TNM.cpp")

add_executable(transit_assignment_cli
    transit_assignment_cli.cpp
    transit_teap_missing.cpp
    ${TRANSIT_NET_SOURCES}
    ${TRAFFIC_NET_SOURCES}
)

target_include_directories(transit_assignment_cli PRIVATE
    ${TRANSIT_DLLPROJ}/transitNet
    ${TRANSIT_DLLPROJ}/trafficNet
    ${TRANSIT_INCLUDE}
    ${LIBPQ_INCLUDE_DIRS}
)

target_compile_definitions(transit_assignment_cli PRIVATE
    _TNM_DLL TNM_EXT_CLASS= TNM_SHUTMYSQL _SHUTTNMMATLAB
)

target_link_libraries(transit_assignment_cli PRIVATE
    ${LIBPQ_LIBRARIES}
    dl
    pthread
)

target_compile_options(transit_assignment_cli PRIVATE -w -fno-operator-names)

install(TARGETS transit_assignment_cli DESTINATION bin)
