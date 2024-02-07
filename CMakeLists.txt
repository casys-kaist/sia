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

# Build Sindex
# foreach(Test IN LISTS TestList)
#     foreach(Index IN LISTS IndexList)
#         foreach(val RANGE ${len_iter})
#             list(GET BenchList ${val} Bench)
#             list(GET KeyList ${val} Key)

#             add_executable(${Test}_${Index}_${Bench}_${Key}
#                 ${CMAKE_CURRENT_SOURCE_DIR}/${Bench}.cpp
#             )
#             string(TOLOWER ${CMAKE_BUILD_TYPE} _type)

#             if (_type STREQUAL release)
#                 set(default_build_type "Release")
#                 message(STATUS "Setting build type to '${default_build_type}' ")
#             else()
#                 message(STATUS "Setting build type to '${default_build_type}' ")
#             endif()

#             target_compile_definitions(${Test}_${Index}_${Bench}_${Key} PRIVATE NDEBUGGING)
#             target_compile_definitions(${Test}_${Index}_${Bench}_${Key} PRIVATE MAX_KEY_SIZE=${Key})
#             target_compile_definitions(${Test}_${Index}_${Bench}_${Key} PRIVATE ${Test})

#             target_include_directories(${Test}_${Index}_${Bench}_${Key}
#             PRIVATE
#                 ${CMAKE_CURRENT_SOURCE_DIR}
#                 ${CMAKE_CURRENT_SOURCE_DIR}/${Index}
#             )

#             target_link_libraries(${Test}_${Index}_${Bench}_${Key}
#                 PRIVATE
#                     mkl_rt
#                     -lpthread
#             )
#         endforeach()
#     endforeach()
# endforeach()

list(APPEND DistList UNIFORM_DIST SEQUENTIAL_DIST LATEST_DIST EXPONENT_DIST ZIPF_DIST HOTSPOT_DIST)
# foreach(Dist IN LISTS DistList)
#     foreach(Index IN LISTS IndexList)
#         add_executable(micro_${Index}_${Dist}
#             ${CMAKE_CURRENT_SOURCE_DIR}/bench.cpp
#         )
#         string(TOLOWER ${CMAKE_BUILD_TYPE} _type)

#         if (_type STREQUAL release)
#             set(default_build_type "Release")
#             message(STATUS "Setting build type to '${default_build_type}' ")
#         else()
#             message(STATUS "Setting build type to '${default_build_type}' ")
#         endif()

#         target_compile_definitions(micro_${Index}_${Dist} PRIVATE NDEBUGGING)
#         target_compile_definitions(micro_${Index}_${Dist} PRIVATE MAX_KEY_SIZE=32)
#         target_compile_definitions(micro_${Index}_${Dist} PRIVATE ${Dist})

#         target_include_directories(micro_${Index}_${Dist}
#         PRIVATE
#             ${CMAKE_CURRENT_SOURCE_DIR}
#             ${CMAKE_CURRENT_SOURCE_DIR}/${Index}
#         )

#         target_link_libraries(micro_${Index}_${Dist}
#         PRIVATE
#             mkl_rt
#             -lpthread
#         )
#     endforeach()
# endforeach()

# # Build cuckoo-trie
# foreach(val RANGE ${len_iter})
#     list(GET BenchList ${val} Bench)
#     list(GET KeyList ${val} Key)
#     add_executable(PERFORMANCE_cuckoo_${Bench}_${Key}
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo_${Bench}.cpp
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code/main.c
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code/util.c
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code/verify_trie.c
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code/random.c
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code/atomics.c
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code/mt_debug.c
#     )
#     target_include_directories(PERFORMANCE_cuckoo_${Bench}_${Key}
#         PRIVATE
#         ${CMAKE_CURRENT_SOURCE_DIR}
#         ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code
#     )
#     target_link_libraries(PERFORMANCE_cuckoo_${Bench}_${Key}
#         PRIVATE
#         -lpthread
#         mkl_rt
#     )
#     target_compile_definitions(PERFORMANCE_cuckoo_${Bench}_${Key} PRIVATE NDEBUGGING)
#     target_compile_definitions(PERFORMANCE_cuckoo_${Bench}_${Key} PRIVATE MAX_KEY_SIZE=${Key})
# endforeach()

# foreach(Dist IN LISTS DistList)
#     add_executable(micro_cuckoo_${Dist}
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo_bench.cpp
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code/main.c
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code/util.c
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code/verify_trie.c
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code/random.c
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code/atomics.c
#                     ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code/mt_debug.c
#     )
#     target_include_directories(micro_cuckoo_${Dist}
#         PRIVATE
#         ${CMAKE_CURRENT_SOURCE_DIR}
#         ${CMAKE_CURRENT_SOURCE_DIR}/cuckoo-trie-code
#     )
#     target_link_libraries(micro_cuckoo_${Dist}
#         PRIVATE
#         -lpthread
#         mkl_rt
#     )
#     target_compile_definitions(micro_cuckoo_${Dist} PRIVATE NDEBUGGING)
#     target_compile_definitions(micro_cuckoo_${Dist} PRIVATE MAX_KEY_SIZE=32)
#     target_compile_definitions(micro_cuckoo_${Dist} PRIVATE ${Dist})
# endforeach()

# # Build Wormhole
# foreach(val RANGE ${len_iter})
#     list(GET BenchList ${val} Bench)
#     list(GET KeyList ${val} Key)
#     add_executable(PERFORMANCE_wormhole_${Bench}_${Key}
#         ${CMAKE_CURRENT_SOURCE_DIR}/wormhole_${Bench}.cpp
#         ${CMAKE_CURRENT_SOURCE_DIR}/wormhole/lib.c
#         ${CMAKE_CURRENT_SOURCE_DIR}/wormhole/wh.c
#         ${CMAKE_CURRENT_SOURCE_DIR}/wormhole/kv.c
#     )
#     target_include_directories(PERFORMANCE_wormhole_${Bench}_${Key}
#         PRIVATE
#         ${CMAKE_CURRENT_SOURCE_DIR}
#         ${CMAKE_CURRENT_SOURCE_DIR}/wormhole
#     )
#     target_link_libraries(PERFORMANCE_wormhole_${Bench}_${Key}
#         PRIVATE
#         -lpthread
#         mkl_rt
#         -L${CMAKE_CURRENT_SOURCE_DIR}/wormhole
#     )
#     target_compile_definitions(PERFORMANCE_wormhole_${Bench}_${Key} PRIVATE NDEBUGGING)
#     target_compile_definitions(PERFORMANCE_wormhole_${Bench}_${Key} PRIVATE MAX_KEY_SIZE=${Key})
# endforeach()

# foreach(Dist IN LISTS DistList)
#     add_executable(micro_wormhole_${Dist}
#         ${CMAKE_CURRENT_SOURCE_DIR}/wormhole_${Bench}.cpp
#         ${CMAKE_CURRENT_SOURCE_DIR}/wormhole/lib.c
#         ${CMAKE_CURRENT_SOURCE_DIR}/wormhole/wh.c
#         ${CMAKE_CURRENT_SOURCE_DIR}/wormhole/kv.c
#     )
#     target_include_directories(micro_wormhole_${Dist}
#         PRIVATE
#         ${CMAKE_CURRENT_SOURCE_DIR}
#         ${CMAKE_CURRENT_SOURCE_DIR}/wormhole
#     )
#     target_link_libraries(micro_wormhole_${Dist}
#         PRIVATE
#         -lpthread
#         mkl_rt
#         -L${CMAKE_CURRENT_SOURCE_DIR}/wormhole
#     )
#     target_compile_definitions(micro_wormhole_${Dist} PRIVATE NDEBUGGING)
#     target_compile_definitions(micro_wormhole_${Dist} PRIVATE MAX_KEY_SIZE=32)
#     target_compile_definitions(micro_wormhole_${Dist} PRIVATE ${Dist})
# endforeach()

# Build ALEX
list(APPEND ALEXBenchList ycsb twitter)
foreach(Bench IN LISTS ALEXBenchList)
    add_executable(PERFORMANCE_alex_${Bench}
                    ${CMAKE_CURRENT_SOURCE_DIR}/alex_${Bench}.cpp
    )
    target_include_directories(PERFORMANCE_alex_${Bench}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/alex
    )
    target_link_libraries(PERFORMANCE_alex_${Bench}
        PRIVATE
        -lpthread
        mkl_rt
    )
    target_compile_definitions(PERFORMANCE_alex_${Bench} PRIVATE NDEBUGGING)
endforeach()

foreach(Dist IN LISTS DistList)
    add_executable(micro_alex_${Dist}
        ${CMAKE_CURRENT_SOURCE_DIR}/alex_bench.cpp
    )
    target_include_directories(micro_alex_${Dist}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/alex
    )
    target_link_libraries(micro_alex_${Dist}
        PRIVATE
        -lpthread
        mkl_rt
    )
    target_compile_definitions(micro_alex_${Dist} PRIVATE NDEBUGGING)
    target_compile_definitions(micro_alex_${Dist} PRIVATE MAX_KEY_SIZE=32)
    target_compile_definitions(micro_alex_${Dist} PRIVATE ${Dist})
endforeach()