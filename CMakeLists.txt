cmake_minimum_required(VERSION 2.8.8)

project(ktf-distribution C CXX)
set(KTF_VERSION 1.0.0)

if (CMAKE_VERSION VERSION_LESS "3.1")
  add_definitions(-std=c++11)
else()
  set(CMAKE_CXX_STANDARD 11)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  if(NOT CYGWIN)
    set(CMAKE_CXX_EXTENSIONS OFF)
  endif()
endif()

if (POLICY CMP0048)
  cmake_policy(SET CMP0048 NEW)
endif (POLICY CMP0048)

enable_testing()

include(CMakeDependentOption)
include(GNUInstallDirs)

option(INSTALL_KTF "Enable installation of KTF. (Projects embedding KTF may want to turn this OFF.)" ON)

if(INSTALL_KTF)
  add_subdirectory( ktf )
endif()
