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

list(APPEND BenchList ycsb ycsb twitter twitter twitter twitter)
list(APPEND KeyList   12   128  44      19      47      82)
list(APPEND IndexList original sia-sw ideal)
list(APPEND TestList PERFORMANCE LATENCY_BREAKDOWN)

list(LENGTH BenchList len_ent)
math(EXPR len_iter "${len_ent} - 1")

foreach(Test IN LISTS TestList)
    foreach(Index IN LISTS IndexList)
        foreach(val RANGE ${len_iter})
            list(GET BenchList ${val} Bench)
            list(GET KeyList ${val} Key)

            add_executable(${Test}_${Index}_${Bench}_${Key}
                ${CMAKE_CURRENT_SOURCE_DIR}/${Bench}.cpp
            )
            string(TOLOWER ${CMAKE_BUILD_TYPE} _type)

            if (_type STREQUAL release)
                set(default_build_type "Release")
                message(STATUS "Setting build type to '${default_build_type}' ")
            else()
                message(STATUS "Setting build type to '${default_build_type}' ")
            endif()

            target_compile_definitions(${Test}_${Index}_${Bench}_${Key} PRIVATE NDEBUGGING)
            target_compile_definitions(${Test}_${Index}_${Bench}_${Key} PRIVATE MAX_KEY_SIZE=${Key})
            target_compile_definitions(${Test}_${Index}_${Bench}_${Key} PRIVATE ${Test})

            target_include_directories(${Test}_${Index}_${Bench}_${Key}
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/${Index}
            )

            target_link_libraries(${Test}_${Index}_${Bench}_${Key}
                PRIVATE
                    mkl_rt
                    -lpthread
            )
        endforeach()
    endforeach()
endforeach()

list(APPEND DistList UNIFORM_DIST SEQUENTIAL_DIST LATEST_DIST EXPONENTIAL_DIST ZIPF_DIST HOTSPOT_DIST)
foreach(Dist IN LISTS DistList)
    foreach(Index IN LISTS IndexList)
        add_executable(micro_${Index}_${Dist}
            ${CMAKE_CURRENT_SOURCE_DIR}/bench.cpp
        )
        string(TOLOWER ${CMAKE_BUILD_TYPE} _type)

        if (_type STREQUAL release)
            set(default_build_type "Release")
            message(STATUS "Setting build type to '${default_build_type}' ")
        else()
            message(STATUS "Setting build type to '${default_build_type}' ")
        endif()

        target_compile_definitions(micro_${Index}_${Dist} PRIVATE NDEBUGGING)
        target_compile_definitions(micro_${Index}_${Dist} PRIVATE MAX_KEY_SIZE=32)
        target_compile_definitions(micro_${Index}_${Dist} PRIVATE ${Dist})

        target_include_directories(micro_${Index}_${Dist}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/${Index}
        )

        target_link_libraries(micro_${Index}_${Dist}
        PRIVATE
            mkl_rt
            -lpthread
        )
    endforeach()
endforeach()