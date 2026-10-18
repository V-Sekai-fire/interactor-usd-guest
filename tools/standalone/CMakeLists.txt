cmake_minimum_required(VERSION 3.16)
project(usd_guest_standalone C CXX)

# usd.elf alone, without the rest of transport-meshing-pen: the same sandbox-api, rd_compute
# and add_stage_elf it uses (its CMakeLists.txt), then cmake/usd.cmake. ELFs land beside this
# file, as <name>.elf.
set(WEFT_ROOT "${CMAKE_SOURCE_DIR}/../../../.." CACHE PATH "Root of the manifest checkout")
get_filename_component(WEFT_ROOT "${WEFT_ROOT}" ABSOLUTE)
set(GUEST_RUNTIME_ROOT ${WEFT_ROOT}/2-contract/guest-runtime)
set(GUEST_COMMON_ROOT ${WEFT_ROOT}/2-contract/guest-common)
set(USD_GUEST_ROOT ${WEFT_ROOT}/3-interactor/usd-guest)
foreach(root GUEST_RUNTIME_ROOT GUEST_COMMON_ROOT USD_GUEST_ROOT)
	add_compile_options("-ffile-prefix-map=${${root}}/=")
endforeach()

set(DOWNLOAD_RUNTIME_API OFF CACHE BOOL "" FORCE)
add_subdirectory(${GUEST_RUNTIME_ROOT}/vendor/sandbox-api/cmake sandbox_api)
add_library(rd_compute STATIC ${GUEST_RUNTIME_ROOT}/guest/rd_compute.cpp)
target_include_directories(rd_compute PUBLIC ${GUEST_RUNTIME_ROOT}/guest)
target_link_libraries(rd_compute PUBLIC sandbox_api)

function(add_stage_elf name)
	add_sandbox_program(${name} ${ARGN})
	target_link_libraries(${name} PRIVATE rd_compute -lm -lstdc++)
	add_custom_command(TARGET ${name} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${name}> ${CMAKE_SOURCE_DIR}/${name}.elf
		COMMENT "Installing ${name}.elf")
endfunction()

include(${USD_GUEST_ROOT}/cmake/usd.cmake)
