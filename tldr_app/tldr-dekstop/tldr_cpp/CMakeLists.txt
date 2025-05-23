# CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(tldr_cpp)

set(SOURCE_DIR ${CMAKE_SOURCE_DIR}/src)
include_directories(${SOURCE_DIR})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OpenMP_C_FLAGS "-Xpreprocessor -fopenmp -I/opt/homebrew/include")
set(OpenMP_C_LIB_NAMES "omp")
set(OpenMP_omp_LIBRARY "/opt/homebrew/lib/libomp.a")
set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp -I/opt/homebrew/include")
set(OpenMP_CXX_LIB_NAMES "omp")

find_package(OpenMP REQUIRED)

# Force static libraries
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

# Set position independent code globally
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

#set(JSON_BuildTests OFF CACHE INTERNAL "")

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER REQUIRED poppler-cpp)
find_package(PostgreSQL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(SQLiteCpp REQUIRED)


# Create static library
add_library(tldr STATIC
    ${SOURCE_DIR}/lib_tldr/lib_tldr.cpp
    ${SOURCE_DIR}/lib_tldr/lib_tldr.h
    ${SOURCE_DIR}/lib_tldr/constants.h
    ${SOURCE_DIR}/lib_tldr/db/sqlite_database.cpp
    ${SOURCE_DIR}/lib_tldr/db/sqlite_database.h
    ${SOURCE_DIR}/lib_tldr/db/database.h
    ${SOURCE_DIR}/lib_tldr/db/postgres_database.cpp
    ${SOURCE_DIR}/lib_tldr/db/postgres_database.h
    ${SOURCE_DIR}/lib_tldr/db/connection_pool.h
    ${SOURCE_DIR}/lib_tldr/tldr_api.cpp
    ${SOURCE_DIR}/lib_tldr/tldr_api.h
    ${SOURCE_DIR}/lib_tldr/llm/llm-wrapper.cpp
    ${SOURCE_DIR}/lib_tldr/llm/llm-wrapper.h
    ${SOURCE_DIR}/lib_tldr/llm/LlmChat.cpp
    ${SOURCE_DIR}/lib_tldr/llm/LlmChat.h
    ${SOURCE_DIR}/lib_tldr/llm/LlmEmbeddings.cpp
    ${SOURCE_DIR}/lib_tldr/llm/LlmEmbeddings.h
    ${SOURCE_DIR}/lib_tldr/llm/LlmContextPool.cpp
    ${SOURCE_DIR}/lib_tldr/llm/LlmContextPool.h
    ${SOURCE_DIR}/lib_tldr/vec_dump.cpp
    ${SOURCE_DIR}/lib_tldr/vec_dump.h
    ${SOURCE_DIR}/lib_tldr/file_hashes.cpp
)

# Include directories for the library
target_include_directories(tldr PRIVATE
    ${PostgreSQL_INCLUDE_DIRS}
    ${PQXX_INCLUDE_DIRS}
    ${SQLite3_INCLUDE_DIRS}
    /opt/homebrew/opt/libpq/include
    /opt/homebrew/opt/libpqxx/include
    /opt/homebrew/opt/poppler/include
    /opt/homebrew/opt/curl/include
    /opt/homebrew/opt/nlohmann-json/include
    /opt/homebrew/opt/sqlitecpp/include
    /opt/homebrew/opt/libomp/include
    /Users/manu/proj_tldr/tldr-dekstop/release-products/include
    /Users/manu/proj_tldr/tldr-dekstop/release-products/include/llama.cpp/
    /opt/homebrew/opt/openssl/include
)

# Set POSITION_INDEPENDENT_CODE to allow linking into Swift
set_property(TARGET tldr PROPERTY POSITION_INDEPENDENT_CODE ON)

# Always prefer static libraries
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" CACHE STRING "Library suffixes")

# Link libraries for the static library with explicit static library paths
target_link_libraries(tldr PUBLIC
        /opt/homebrew/opt/libpq/lib/libpq.a
        /opt/homebrew/opt/libpqxx/lib/libpqxx.a
        /opt/homebrew/opt/poppler/lib/libpoppler.a
        /opt/homebrew/opt/curl/lib/libcurl.a
        /opt/homebrew/opt/openssl/lib/libssl.a
        /opt/homebrew/opt/openssl/lib/libcrypto.a
        /opt/homebrew/opt/libomp/lib/libomp.a

        /Users/manu/proj_tldr/tldr-dekstop/release-products/libs/libnpu-accelerator.a
        /Users/manu/proj_tldr/tldr-dekstop/release-products/libs/llama.cpp/libcommon.a
        /Users/manu/proj_tldr/tldr-dekstop/release-products/libs/llama.cpp/libggml-base.a
        /Users/manu/proj_tldr/tldr-dekstop/release-products/libs/llama.cpp/libggml-blas.a
        /Users/manu/proj_tldr/tldr-dekstop/release-products/libs/llama.cpp/libggml-cpu.a
        /Users/manu/proj_tldr/tldr-dekstop/release-products/libs/llama.cpp/libggml-metal.a
        /Users/manu/proj_tldr/tldr-dekstop/release-products/libs/llama.cpp/libggml.a
        /Users/manu/proj_tldr/tldr-dekstop/release-products/libs/llama.cpp/libllama.a
        /Users/manu/proj_tldr/tldr-dekstop/release-products/libs/llama.cpp/libllava_static.a
        "-framework Accelerate"
        "-framework Metal"
        "-framework Foundation"
)

target_include_directories(tldr PUBLIC ${SOURCE_DIR})

# Add the executable
add_executable(tldr_cpp ${SOURCE_DIR}/main.cpp)

# Link the static library with the executable
target_link_libraries(tldr_cpp PUBLIC tldr
        /opt/homebrew/opt/libpq/lib/libpq.a
        /opt/homebrew/opt/libpqxx/lib/libpqxx.a
        /opt/homebrew/opt/poppler/lib/libpoppler.a
        /opt/homebrew/opt/curl/lib/libcurl.a
)

target_link_options(tldr_cpp PUBLIC
        -L/opt/homebrew/opt/libpq/lib/
        -L/opt/homebrew/opt/libpqxx/lib/
        -L/opt/homebrew/opt/poppler/lib/
        -lpq
        -lpoppler-cpp
        -lcurl
)
# Include directories for the executable
target_include_directories(tldr_cpp PRIVATE ${SOURCE_DIR})

# Copy the output file to a custom directory after build
add_custom_command(TARGET tldr POST_BUILD COMMAND ../copy-release-products.sh)