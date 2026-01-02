
cmake_minimum_required(VERSION 3.0.0)

project(qore-sqlite3-module VERSION 1.2.0)

option(INSTALL_DOCS "Install documentation" OFF)

# Check for C++11.
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
if(COMPILER_SUPPORTS_CXX11)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
else()
    message(FATAL_ERROR "The compiler ${CMAKE_CXX_COMPILER} has no C++11 support. Please use a different C++ compiler.")
endif()

find_package(Qore 1.0 REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

set(CMAKE_THREAD_PREFER_PTHREAD ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
if(CMAKE_USE_PTHREADS_INIT)
message(STATUS "Found POSIX Threads: TRUE")
else(CMAKE_USE_PTHREADS_INIT)
message(STATUS "Found POSIX Threads: FALSE")
message(FATAL_ERROR "POSIX threads does not seem to be supported on this platform, aborting")
endif()

include_directories(${SQLITE3_INCLUDE_DIRS})

check_cxx_compiler_flag(-fvisibility=hidden HAVE_GCC_VISIBILITY)

if(${CMAKE_SYSTEM_NAME} EQUAL "Linux")
add_definitions(-D_GNU_SOURCE)
endif()

configure_file(${CMAKE_SOURCE_DIR}/cmake/config.h.cmake config.h)

set(CPP_SRC
    src/sqlite3connection.cc
    src/sqlite3executor.cc
    src/sqlite3module.cc
)

set(module_name "sqlite3")

set(QORE_DOX_TMPL_SRC
  docs/mainpage.dox.tmpl
)

add_library(${module_name} MODULE ${CPP_SRC})

if (DEFINED ENV{DOXYGEN_EXECUTABLE})
    set(DOXYGEN_EXECUTABLE $ENV{DOXYGEN_EXECUTABLE})
endif()

qore_external_binary_module(${module_name} ${PROJECT_VERSION} ${SQLITE3_LDFLAGS} Threads::Threads)

qore_dist(${PROJECT_VERSION})

qore_config_info()

if (DOXYGEN_FOUND)
    qore_wrap_dox(QORE_DOX_SRC ${QORE_DOX_TMPL_SRC})
    add_custom_target(QORE_MOD_DOX_FILES DEPENDS ${QORE_DOX_SRC})
    add_dependencies(docs-module QORE_MOD_DOX_FILES)
endif()
