cmake_minimum_required(VERSION 3.21)
project(QuirewiseVector VERSION 0.1.0 LANGUAGES C)
option(QUIREWISE_BUILD_SHARED "Build Quirewise libraries as shared libraries" OFF)
if(QUIREWISE_BUILD_SHARED)
    set(QW_LIBRARY_TYPE SHARED)
else()
    set(QW_LIBRARY_TYPE STATIC)
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT TARGET Quirewise::Data)
    find_package(QuirewiseData CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Compression)
    find_package(QuirewiseCompression CONFIG REQUIRED)
endif()
file(GLOB_RECURSE QW_SOURCES CONFIGURE_DEPENDS src/*.c src/*.cpp)
add_library(quirewise_vector ${QW_LIBRARY_TYPE} ${QW_SOURCES})
add_library(Quirewise::Vector ALIAS quirewise_vector)
set_target_properties(quirewise_vector PROPERTIES EXPORT_NAME Vector POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_vector PUBLIC c_std_11)
target_include_directories(quirewise_vector PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src/formats ${CMAKE_CURRENT_SOURCE_DIR}/src/backends)
target_link_libraries(quirewise_vector PRIVATE
    Quirewise::Data Quirewise::Compression
    "$<$<NOT:$<PLATFORM_ID:Windows>>:m>")
install(TARGETS quirewise_vector EXPORT QuirewiseVectorTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT QuirewiseVectorTargets FILE QuirewiseVectorTargets.cmake NAMESPACE Quirewise:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseVector")
configure_package_config_file(cmake/QuirewiseVectorConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseVectorConfig.cmake" INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseVector")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/QuirewiseVectorConfigVersion.cmake" VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseVectorConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseVectorConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseVector")

if(QUIREWISE_BUILD_SHARED AND WIN32)
    set_target_properties(
        quirewise_vector
        PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
