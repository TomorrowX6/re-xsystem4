cmake_minimum_required(VERSION 3.10)
project(archive_jni C)

find_library(LIBARCHIVE_LIB NAMES archive PATHS "${CMAKE_FIND_ROOT_PATH}/lib" "${CMAKE_INSTALL_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(LIBLZMA_LIB NAMES lzma PATHS "${CMAKE_FIND_ROOT_PATH}/lib" "${CMAKE_INSTALL_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
find_package(ZLIB REQUIRED)

add_library(archive_jni SHARED archive_jni.c)
target_include_directories(archive_jni PRIVATE
  "${CMAKE_FIND_ROOT_PATH}/include"
  "${CMAKE_INSTALL_PREFIX}/include"
)
target_compile_definitions(archive_jni PRIVATE LIBARCHIVE_STATIC)
target_link_libraries(archive_jni PRIVATE
  "${LIBARCHIVE_LIB}"
  "${LIBLZMA_LIB}"
  ZLIB::ZLIB
)
if(ANDROID)
  target_link_libraries(archive_jni PRIVATE log)
endif()
target_link_options(archive_jni PRIVATE "-Wl,--gc-sections")

install(TARGETS archive_jni LIBRARY DESTINATION lib)
