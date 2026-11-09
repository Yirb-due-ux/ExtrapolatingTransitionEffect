cmake_minimum_required(VERSION 3.22)
project(ExtrapolatingTransitionEffect VERSION 0.3.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build this Skyrim SE 1.5.97 plugin with MSVC x64.")
endif()

set(COMMONLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/CommonLibSSE")
find_package(Git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${COMMONLIB_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE COMMONLIB_REV OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${COMMONLIB_ROOT}" diff --quiet HEAD -- .
    RESULT_VARIABLE COMMONLIB_DIRTY)
if(NOT COMMONLIB_REV STREQUAL "b5d0fe3588978bbeb3dd39d87d4c97fb7ae7bf89" OR NOT COMMONLIB_DIRTY EQUAL 0)
    message(FATAL_ERROR "Use the pinned, clean ETE CommonLibSSE checkout. See the ETE workflow.")
endif()
set(SKYRIM_SUPPORT_AE OFF CACHE BOOL "Build the verified SE ABI" FORCE)
add_subdirectory("${COMMONLIB_ROOT}" commonlib)
target_compile_definitions(CommonLibSSE PRIVATE _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
set_property(TARGET CommonLibSSE PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${COMMONLIB_ROOT}/include")

add_library(ExtrapolatingTransitionEffect SHARED src/main.cpp src/ClassRegistration.cpp)
target_compile_definitions(ExtrapolatingTransitionEffect PRIVATE _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
target_compile_options(ExtrapolatingTransitionEffect PRIVATE /W4 /WX /external:anglebrackets /external:W0 /permissive- /Zc:preprocessor /EHsc /utf-8)
target_link_libraries(ExtrapolatingTransitionEffect PRIVATE CommonLibSSE::CommonLibSSE)

# The local test sources are optional in a source checkout.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/extrapolate.cpp" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/track.cpp")
    enable_testing()
    add_executable(ExtrapolateTests tests/extrapolate.cpp)
    target_include_directories(ExtrapolateTests PRIVATE src)
    target_compile_options(ExtrapolateTests PRIVATE /W4 /WX /EHsc)
    add_test(NAME extrapolation COMMAND ExtrapolateTests)

    add_executable(DensePoseTrackTests tests/track.cpp)
    target_include_directories(DensePoseTrackTests PRIVATE src)
    target_compile_definitions(DensePoseTrackTests PRIVATE _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_compile_options(DensePoseTrackTests PRIVATE /W4 /WX /EHsc /utf-8)
    target_link_libraries(DensePoseTrackTests PRIVATE CommonLibSSE::CommonLibSSE)
    add_test(NAME dense_pose_track COMMAND DensePoseTrackTests)
endif()
