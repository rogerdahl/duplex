cmake_minimum_required(VERSION 3.9)
project(duplex)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_VERBOSE_MAKEFILE ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(SOURCE_DIR ${CMAKE_SOURCE_DIR}/src)

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
	set(LIBRARIES ${CMAKE_SOURCE_DIR}/libraries/linux)
else()
	set(LIBRARIES ${CMAKE_SOURCE_DIR}/libraries/win64)
endif()

# Boost
if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  # https://cmake.org/cmake/help/v3.0/module/FindBoost.html
  find_package(Boost 1.58.0 REQUIRED)
  set(
    BOOST_LIBRARIES
    boost_filesystem
    boost_program_options
    boost_regex
    boost_system
  )
else ()
  set(
    BOOST_LIBRARIES
    libboost_filesystem-vc140-mt-1_60
    libboost_system-vc140-mt-1_60
  )
  set(BOOST_INCLUDE_DIR ${LIBRARIES}/boost_1_60_0)
  set(BOOST_LINK_DIR ${LIBRARIES}/boost_1_60_0/stage/lib)
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wfatal-errors")
endif ()

set(SOURCE_FILES
  # Precompiled header is compiled as if it's source. Must be first in list
  ${SOURCE_DIR}/pch.h
  ${SOURCE_DIR}/main.cpp
  ${SOURCE_DIR}/junction.cpp
  ${SOURCE_DIR}/md5.cpp
  ${SOURCE_DIR}/fnv_1a_64.cpp
)

include_directories(
#  ${LIBRARIES}/spdlog/include
  ${BOOST_INCLUDE_DIR}
)

link_directories(
#  ${LIBRARIES}/spdlog/build
  ${BOOST_LINK_DIR}
)

link_libraries(
  ${BOOST_LIBRARIES}
  fmt
#  spdlog
  # For Windows, the boost libraries are not required here because Boost
  # supports VS' autolink feature
)

add_executable(
  duplex ${SOURCE_FILES}
)

target_compile_options(
  duplex PRIVATE -Wno-unknown-pragmas
)
