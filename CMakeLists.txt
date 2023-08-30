cmake_minimum_required (VERSION 3.5)
project (SIA)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(MKL_LINK_DIRECTORY "/opt/intel/mkl/lib/intel64")
set(MKL_INCLUDE_DIRECTORY "/opt/intel/mkl/include")

# Set a default build type if none was specified
set(default_build_type "Release")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
      STRING "Choose the type of build." FORCE)
  # Set the possible values of build type for cmake-gui
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

link_directories(${MKL_LINK_DIRECTORY})
include_directories(${MKL_INCLUDE_DIRECTORY})

add_compile_options(-Wall -fmax-errors=5 -faligned-new -march=native -mtune=native -g)

list(APPEND BenchList bench) # You can Add 'ycsb' 'twitter' option to compile ycsb & twitter cache trace workload version
list(APPEND IndexList original sia-sw ideal)

foreach(Index IN LISTS IndexList)
    foreach(Bench IN LISTS BenchList)
        add_executable(${Index}_${Bench}
            ${CMAKE_CURRENT_SOURCE_DIR}/${Bench}.cpp
        )
        string(TOLOWER ${CMAKE_BUILD_TYPE} _type)

        if (_type STREQUAL release)
            set(default_build_type "Release")
            message(STATUS "Setting build type to '${default_build_type}' ")
            target_compile_definitions(${Index}_${Bench} PRIVATE NDEBUGGING)
        else()
            message(STATUS "Setting build type to '${default_build_type}' ")
        endif()

        target_compile_definitions(${Index}_${Bench} PRIVATE NDEBUGGING)

        target_include_directories(${Index}_${Bench}
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/${Index}
        )

        target_link_libraries(${Index}_${Bench}
            PRIVATE
                mkl_rt
                -lpthread
        )
    endforeach()
endforeach()