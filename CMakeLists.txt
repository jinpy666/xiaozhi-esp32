# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(BRAND_PROJECT_NAME "xiaozhi")
set(BRAND_PROJECT_VER "2.5.3")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/product/brand.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/product/brand.cmake")
endif()
# Generated per-build by scripts/build.py --brand <tenant>; overrides the
# checked-in defaults so one checkout can build several branded firmwares.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/product/brand.gen.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/product/brand.gen.cmake")
endif()
set(PROJECT_VER "${BRAND_PROJECT_VER}")
project(${BRAND_PROJECT_NAME})
