cmake_minimum_required(VERSION 3.20)
project(TransitAssignment LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PostgreSQL REQUIRED)

option(BUILD_TRANSIT_STANDALONE
    "Build the local fixed-config transit_assignment executable"
    OFF)

set(TRANSIT_ASSIGNMENT_SOURCES
    src/PTNet.cpp
    src/TEAP.cpp
    src/TransitDatabase.cpp
    src/TransitInput.cpp
    src/TransitOutput.cpp
    src/TNM_utility.cpp
)

if(BUILD_TRANSIT_STANDALONE)
    add_executable(transit_assignment
        src/main.cpp
        ${TRANSIT_ASSIGNMENT_SOURCES}
    )
endif()

add_executable(transit_assignment_new_cli
    src/transit_assignment_new_cli.cpp
    ${TRANSIT_ASSIGNMENT_SOURCES}
)

set(TRANSIT_TARGETS transit_assignment_new_cli)
if(BUILD_TRANSIT_STANDALONE)
    list(APPEND TRANSIT_TARGETS transit_assignment)
endif()

foreach(target ${TRANSIT_TARGETS})
    target_include_directories(${target} PRIVATE include)
    target_link_libraries(${target} PRIVATE PostgreSQL::PostgreSQL)
endforeach()

if(MSVC)
    foreach(target ${TRANSIT_TARGETS})
        target_compile_options(${target} PRIVATE /W3 /permissive-)
    endforeach()
else()
    foreach(target ${TRANSIT_TARGETS})
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wno-deprecated-declarations
            -flto
        )
        target_link_options(${target} PRIVATE -flto)
    endforeach()
endif()
