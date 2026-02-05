cmake_minimum_required(VERSION 3.26)

project(proxy_web_server
    VERSION 1.0
    LANGUAGES CXX
)

# --- Compiler Configuration ---
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXE_LINKER_FLAGS "-static")

#--- 1. GoogleTest Download (Commented) ---
# include(FetchContent)
# FetchContent_Declare(
#   googletest
#   URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
# )
# set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# FetchContent_MakeAvailable(googletest)

# --- 2. Main Target ---
add_executable(proxy_main)

# Updated source paths
target_sources(proxy_main PRIVATE
    proxy_logger.cpp
    proxy_cache.cpp
    proxy_main.cpp
    proxy_handler.cpp
)

#--- GoogleTest Headers (Commented) ---
# if (DEFINED googletest_SOURCE_DIR)
#   target_include_directories(proxy_cache_test PRIVATE
#     ${googletest_SOURCE_DIR}/googletest/include
#   )
# endif()

# --- 3. Dependencies & Linking ---
# target_link_libraries(proxy_cache_test PRIVATE GTest::gtest_main)

if(WIN32)
    target_compile_definitions(proxy_main PRIVATE
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(proxy_main PRIVATE ws2_32)
endif()

# --- 4. Test Discovery (Commented) ---
# include(CTest)
# enable_testing()
# include(GoogleTest)
# gtest_discover_tests(proxy_cache_test)