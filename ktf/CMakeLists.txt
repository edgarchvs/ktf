FILE(GLOB_RECURSE srcs "*/*.c" "*/*.cpp")
add_custom_target(srcs SOURCES ${srcs})

option(ktf_build_tests "Build all of ktf's own tests." OFF)

option(ktf_build_samples "Build ktf's sample programs." OFF)

option(ktf_disable_pthreads "Disable uses of pthreads in ktf." OFF)

option(
  ktf_hide_internal_symbols
  "Build ktf with internal symbols hidden in shared libraries."
  OFF)

########################################################################
#
# Project-wide settings

# Name of the project.
#
# CMake files in this project can refer to the root source directory
# as ${ktf_SOURCE_DIR} and to the root binary directory as
# ${KTF_BINARY_DIR}.
# Language "C" is required for find_package(Threads).

# Project version:

if (CMAKE_VERSION VERSION_LESS 3.0)
  project(ktf CXX C)
  set(PROJECT_VERSION ${KTF_VERSION})
else()
  cmake_policy(SET CMP0048 NEW)
  project(ktf VERSION ${KTF_VERSION} LANGUAGES CXX C)
endif()
cmake_minimum_required(VERSION 2.6.4)

if (POLICY CMP0063) # Visibility
  cmake_policy(SET CMP0063 NEW)
endif (POLICY CMP0063)

if (COMMAND set_up_hermetic_build)
  set_up_hermetic_build()
endif()

# These commands only run if this is the main project
if(CMAKE_PROJECT_NAME STREQUAL "ktf" OR CMAKE_PROJECT_NAME STREQUAL "ktf-distribution")
else()

  mark_as_advanced(
    ktf_force_shared_crt
    ktf_build_tests
    ktf_build_samples
    ktf_disable_pthreads
    ktf_hide_internal_symbols)

endif()


if (ktf_hide_internal_symbols)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)
endif()

# Define helper functions and macros used by Google Test.
include(cmake/internal_utils.cmake)

config_compiler_and_linker()  # Defined in internal_utils.cmake.

# Create the CMake package file descriptors.
if (INSTALL_KTF)
  include(CMakePackageConfigHelpers)
  set(cmake_package_name ktf)
##333  set(targets_export_name ${cmake_package_name}Targets CACHE INTERNAL "")
  set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated" CACHE INTERNAL "")
  set(cmake_files_install_dir "${CMAKE_INSTALL_LIBDIR}/cmake/${cmake_package_name}")
  set(version_file "${generated_dir}/${cmake_package_name}ConfigVersion.cmake")
  write_basic_package_version_file(${version_file} VERSION ${KTF_VERSION} COMPATIBILITY AnyNewerVersion)
##333  install(EXPORT ${targets_export_name}
##333    NAMESPACE ${cmake_package_name}::
##333    DESTINATION ${cmake_files_install_dir})
  set(config_file "${generated_dir}/${cmake_package_name}Config.cmake")
  configure_package_config_file("${ktf_SOURCE_DIR}/cmake/Config.cmake.in"
    "${config_file}" INSTALL_DESTINATION ${cmake_files_install_dir})
  install(FILES ${version_file} ${config_file}
    DESTINATION ${cmake_files_install_dir})
endif()

# Where Google Test's .h files can be found.
set(ktf_build_include_dirs
  "${ktf_SOURCE_DIR}/lib"
  "${ktf_SOURCE_DIR}")
include_directories(${ktf_build_include_dirs})

########################################################################
#
# Defines the ktf & ktf_main libraries.  User tests should link
# with one of them.

# Google Test libraries.  We build them using more strict warnings than what
# are used for other targets, to ensure that ktf can be compiled by a user
# aggressive about warnings.
cxx_library(ktf "${cxx_strict}" src/ktf-all.cc)
cxx_library(ktf_main "${cxx_strict}" src/ktf_main.cc)
# If the CMake version supports it, attach header directory information
# to the targets for when we are part of a parent build (ie being pulled
# in via add_subdirectory() rather than being a standalone build).
##if (DEFINED CMAKE_VERSION AND NOT "${CMAKE_VERSION}" VERSION_LESS "2.8.11")
##  target_include_directories(ktf SYSTEM INTERFACE
##    "$<BUILD_INTERFACE:${ktf_build_include_dirs}>"
##    "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")
##  target_include_directories(ktf_main SYSTEM INTERFACE
##    "$<BUILD_INTERFACE:${ktf_build_include_dirs}>"
##    "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")
##endif()
##target_link_libraries(ktf_main PUBLIC ktf)

########################################################################
#
# Install rules
install_project(ktf ktf_main)

########################################################################
#
# Samples on how to link user tests with ktf or ktf_main.
#
# They are not built by default.  To build them, set the
# ktf_build_samples option to ON.  You can do it by running ccmake
# or specifying the -Dktf_build_samples=ON flag when running cmake.

if (ktf_build_samples)
  cxx_executable(sample1_unittest samples ktf_main samples/sample1.cc)
  cxx_executable(sample2_unittest samples ktf_main samples/sample2.cc)
  cxx_executable(sample3_unittest samples ktf_main)
  cxx_executable(sample4_unittest samples ktf_main samples/sample4.cc)
  cxx_executable(sample5_unittest samples ktf_main samples/sample1.cc)
  cxx_executable(sample6_unittest samples ktf_main)
  cxx_executable(sample7_unittest samples ktf_main)
  cxx_executable(sample8_unittest samples ktf_main)
  cxx_executable(sample9_unittest samples ktf)
  cxx_executable(sample10_unittest samples ktf)
endif()

########################################################################
#
# Google Test's own tests.
#
# You can skip this section if you aren't interested in testing
# Google Test itself.
#
# The tests are not built by default.  To build them, set the
# ktf_build_tests option to ON.  You can do it by running ccmake
# or specifying the -Dktf_build_tests=ON flag when running cmake.

if (ktf_build_tests)
  # This must be set in the root directory for the tests to be run by
  # 'make test' or ctest.
  enable_testing()

  if (WIN32)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/RunTest.ps1"
         CONTENT
"$project_bin = \"${CMAKE_BINARY_DIR}/bin/$<CONFIG>\"
$env:Path = \"$project_bin;$env:Path\"
& $args")
  elseif (MINGW)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/RunTest.ps1"
         CONTENT
"$project_bin = (cygpath --windows ${CMAKE_BINARY_DIR}/bin)
$env:Path = \"$project_bin;$env:Path\"
& $args")
  endif()

  ############################################################
  # C++ tests built with standard compiler flags.

  cxx_test(googletest-death-test-test ktf_main)
  cxx_test(ktf_environment_test ktf)
  cxx_test(googletest-filepath-test ktf_main)
  cxx_test(googletest-listener-test ktf_main)
  cxx_test(ktf_main_unittest ktf_main)
  cxx_test(googletest-message-test ktf_main)
  cxx_test(ktf_no_test_unittest ktf)
  cxx_test(googletest-options-test ktf_main)
  cxx_test(googletest-param-test-test ktf
    test/googletest-param-test2-test.cc)
  cxx_test(googletest-port-test ktf_main)
  cxx_test(ktf_pred_impl_unittest ktf_main)
  cxx_test(ktf_premature_exit_test ktf
    test/ktf_premature_exit_test.cc)
  cxx_test(googletest-printers-test ktf_main)
  cxx_test(ktf_prod_test ktf_main
    test/production.cc)
  cxx_test(ktf_repeat_test ktf)
  cxx_test(ktf_sole_header_test ktf_main)
  cxx_test(ktf_stress_test ktf)
  cxx_test(googletest-test-part-test ktf_main)
  cxx_test(ktf_throw_on_failure_ex_test ktf)
  cxx_test(ktf-typed-test_test ktf_main
    test/ktf-typed-test2_test.cc)
  cxx_test(ktf_unittest ktf_main)
  cxx_test(ktf-unittest-api_test ktf)
  cxx_test(ktf_skip_in_environment_setup_test ktf_main)
  cxx_test(ktf_skip_test ktf_main)

  ############################################################
  # C++ tests built with non-standard compiler flags.

  # MSVC 7.1 does not support STL with exceptions disabled.
  if (NOT MSVC OR MSVC_VERSION GREATER 1310)
    cxx_library(ktf_no_exception "${cxx_no_exception}"
      src/ktf-all.cc)
    cxx_library(ktf_main_no_exception "${cxx_no_exception}"
      src/ktf-all.cc src/ktf_main.cc)
  endif()
  cxx_library(ktf_main_no_rtti "${cxx_no_rtti}"
    src/ktf-all.cc src/ktf_main.cc)

  cxx_test_with_flags(ktf-death-test_ex_nocatch_test
    "${cxx_exception} -DKTF_ENABLE_CATCH_EXCEPTIONS_=0"
    ktf test/googletest-death-test_ex_test.cc)
  cxx_test_with_flags(ktf-death-test_ex_catch_test
    "${cxx_exception} -DKTF_ENABLE_CATCH_EXCEPTIONS_=1"
    ktf test/googletest-death-test_ex_test.cc)

  cxx_test_with_flags(ktf_no_rtti_unittest "${cxx_no_rtti}"
    ktf_main_no_rtti test/ktf_unittest.cc)

  cxx_shared_library(ktf_dll "${cxx_default}"
    src/ktf-all.cc src/ktf_main.cc)

  cxx_executable_with_flags(ktf_dll_test_ "${cxx_default}"
    ktf_dll test/ktf_all_test.cc)
  set_target_properties(ktf_dll_test_
                        PROPERTIES
                        COMPILE_DEFINITIONS "ktf_LINKED_AS_SHARED_LIBRARY=1")

  ############################################################
  # Python tests.

  cxx_executable(googletest-break-on-failure-unittest_ test ktf)
  py_test(googletest-break-on-failure-unittest)

  py_test(ktf_skip_environment_check_output_test)

  # Visual Studio .NET 2003 does not support STL with exceptions disabled.
  if (NOT MSVC OR MSVC_VERSION GREATER 1310)  # 1310 is Visual Studio .NET 2003
    cxx_executable_with_flags(
      googletest-catch-exceptions-no-ex-test_
      "${cxx_no_exception}"
      ktf_main_no_exception
      test/googletest-catch-exceptions-test_.cc)
  endif()

  cxx_executable_with_flags(
    googletest-catch-exceptions-ex-test_
    "${cxx_exception}"
    ktf_main
    test/googletest-catch-exceptions-test_.cc)
  py_test(googletest-catch-exceptions-test)

  cxx_executable(googletest-color-test_ test ktf)
  py_test(googletest-color-test)

  cxx_executable(googletest-env-var-test_ test ktf)
  py_test(googletest-env-var-test)

  cxx_executable(googletest-filter-unittest_ test ktf)
  py_test(googletest-filter-unittest)

  cxx_executable(ktf_help_test_ test ktf_main)
  py_test(ktf_help_test)

  cxx_executable(googletest-list-tests-unittest_ test ktf)
  py_test(googletest-list-tests-unittest)

  cxx_executable(googletest-output-test_ test ktf)
  py_test(googletest-output-test --no_stacktrace_support)

  cxx_executable(googletest-shuffle-test_ test ktf)
  py_test(googletest-shuffle-test)

  # MSVC 7.1 does not support STL with exceptions disabled.
  if (NOT MSVC OR MSVC_VERSION GREATER 1310)
    cxx_executable(googletest-throw-on-failure-test_ test ktf_no_exception)
    set_target_properties(googletest-throw-on-failure-test_
      PROPERTIES
      COMPILE_FLAGS "${cxx_no_exception}")
    py_test(googletest-throw-on-failure-test)
  endif()

  cxx_executable(googletest-uninitialized-test_ test ktf)
  py_test(googletest-uninitialized-test)

  cxx_executable(ktf_xml_outfile1_test_ test ktf_main)
  cxx_executable(ktf_xml_outfile2_test_ test ktf_main)
  py_test(ktf_xml_outfiles_test)
  py_test(googletest-json-outfiles-test)

  cxx_executable(ktf_xml_output_unittest_ test ktf)
  py_test(ktf_xml_output_unittest --no_stacktrace_support)
  py_test(googletest-json-output-unittest --no_stacktrace_support)
endif()
