cmake_minimum_required(VERSION 3.20)

project(LabScheduler
    VERSION 1.0.0
    DESCRIPTION "Vendor neutral C++20 placement scheduler for heterogeneous lab resources"
    LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(LAB_SCHEDULER_BUILD_TESTS "Build Lab Scheduler tests" ON)
option(LAB_SCHEDULER_BUILD_EXAMPLES "Build Lab Scheduler examples" ON)
option(LAB_SCHEDULER_BUILD_BENCHMARKS "Build Lab Scheduler benchmarks" ON)
option(LAB_SCHEDULER_BUILD_TOOLS "Build Lab Scheduler coordinator, worker and inspection tools" ON)
option(LAB_SCHEDULER_BUILD_CUDA_PROOF "Build the CUDA scheduling proof when a toolkit is available" ON)
option(LAB_SCHEDULER_ENABLE_ADDRESS_SANITIZER "Build with AddressSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Every first-party target is compiled with strict warnings and warnings as
# errors. There is no suppression list: a warning is a defect.
function(lab_scheduler_strict target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

if(LAB_SCHEDULER_ENABLE_ADDRESS_SANITIZER)
    if(MSVC)
        add_compile_options(/fsanitize=address)
        add_link_options(/INCREMENTAL:NO)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address)
    endif()
endif()

set(LAB_SCHEDULER_SOURCES
    src/canonical.cpp
    src/cluster.cpp
    src/capability.cpp
    src/codec.cpp
    src/durable.cpp
    src/engine_accounting.cpp
    src/engine_lifecycle.cpp
    src/engine_recovery.cpp
    src/engine_resources.cpp
    src/engine_scheduling.cpp
    src/engine_state.cpp
    src/error.cpp
    src/explanation_render.cpp
    src/identity.cpp
    src/inspection.cpp
    src/lab_profiles.cpp
    src/net.cpp
    src/persistence.cpp
    src/process.cpp
    src/protocol.cpp
    src/record_codec.cpp
    src/placement.cpp
    src/request.cpp
    src/resource.cpp
    src/topology.cpp
)

add_library(lab_scheduler ${LAB_SCHEDULER_SOURCES})
add_library(LabScheduler::lab_scheduler ALIAS lab_scheduler)
target_include_directories(lab_scheduler
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(lab_scheduler PUBLIC cxx_std_20)
lab_scheduler_strict(lab_scheduler)
if(WIN32)
    target_link_libraries(lab_scheduler PUBLIC ws2_32)
endif()
set_target_properties(lab_scheduler PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON)

install(TARGETS lab_scheduler
    EXPORT LabSchedulerTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT LabSchedulerTargets
    FILE LabSchedulerTargets.cmake
    NAMESPACE LabScheduler::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LabScheduler)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LabSchedulerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/LabSchedulerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LabScheduler)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/LabSchedulerConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/LabSchedulerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/LabSchedulerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LabScheduler)
# --- reference architecture: coordinator, worker, inspection CLI -------------
if(LAB_SCHEDULER_BUILD_TOOLS)
    add_executable(lab-scheduler-coordinator apps/coordinator_main.cpp)
    target_include_directories(lab-scheduler-coordinator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
    target_link_libraries(lab-scheduler-coordinator PRIVATE LabScheduler::lab_scheduler)
    lab_scheduler_strict(lab-scheduler-coordinator)

    add_executable(lab-scheduler-worker apps/worker_main.cpp)
    target_include_directories(lab-scheduler-worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
    target_link_libraries(lab-scheduler-worker PRIVATE LabScheduler::lab_scheduler)
    lab_scheduler_strict(lab-scheduler-worker)

    add_executable(lab-scheduler-cli apps/cli_main.cpp)
    target_include_directories(lab-scheduler-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
    target_link_libraries(lab-scheduler-cli PRIVATE LabScheduler::lab_scheduler)
    lab_scheduler_strict(lab-scheduler-cli)

    foreach(tool lab-scheduler-coordinator lab-scheduler-worker lab-scheduler-cli)
        set_target_properties(${tool} PROPERTIES WIN32_EXECUTABLE OFF)
    endforeach()

    install(TARGETS lab-scheduler-coordinator lab-scheduler-worker lab-scheduler-cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(lab-scheduler-cluster-proof apps/cluster_proof_main.cpp)
    target_include_directories(lab-scheduler-cluster-proof PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
    target_link_libraries(lab-scheduler-cluster-proof PRIVATE LabScheduler::lab_scheduler)
    lab_scheduler_strict(lab-scheduler-cluster-proof)
    set_target_properties(lab-scheduler-cluster-proof PROPERTIES WIN32_EXECUTABLE OFF)
endif()

# --- tests -------------------------------------------------------------------
if(LAB_SCHEDULER_BUILD_TESTS)
    enable_testing()

    add_executable(lab-scheduler-tests
        tests/test_main.cpp
        tests/test_identity.cpp
        tests/test_scheduling.cpp
        tests/test_lifecycle.cpp
        tests/test_persistence.cpp
        tests/test_protocol.cpp
        tests/test_concurrency.cpp
        tests/test_property.cpp
        tests/test_adversarial.cpp)
    target_include_directories(lab-scheduler-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(lab-scheduler-tests PRIVATE LabScheduler::lab_scheduler)
    lab_scheduler_strict(lab-scheduler-tests)
    set_target_properties(lab-scheduler-tests PROPERTIES WIN32_EXECUTABLE OFF)

    add_test(NAME lab_scheduler_tests COMMAND lab-scheduler-tests)
endif()
# --- examples and benchmarks -------------------------------------------------
if(LAB_SCHEDULER_BUILD_EXAMPLES)
    add_executable(lab-scheduler-examples examples/examples_main.cpp)
    target_include_directories(lab-scheduler-examples PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/examples
        ${CMAKE_CURRENT_SOURCE_DIR}/apps)
    target_link_libraries(lab-scheduler-examples PRIVATE LabScheduler::lab_scheduler)
    lab_scheduler_strict(lab-scheduler-examples)
    set_target_properties(lab-scheduler-examples PROPERTIES WIN32_EXECUTABLE OFF)
endif()

if(LAB_SCHEDULER_BUILD_BENCHMARKS)
    add_executable(lab-scheduler-bench benchmarks/bench_main.cpp)
    target_include_directories(lab-scheduler-bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks
        ${CMAKE_CURRENT_SOURCE_DIR}/examples
        ${CMAKE_CURRENT_SOURCE_DIR}/apps)
    target_link_libraries(lab-scheduler-bench PRIVATE LabScheduler::lab_scheduler)
    lab_scheduler_strict(lab-scheduler-bench)
    set_target_properties(lab-scheduler-bench PROPERTIES WIN32_EXECUTABLE OFF)
endif()
# --- real accelerator proof (optional, only when a CUDA toolkit is present) --
# The device kernel is compiled by nvcc to an object file through a custom
# command so that the proof does not depend on CUDA toolchain integration in
# the host generator.
if(LAB_SCHEDULER_BUILD_CUDA_PROOF)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND AND CUDAToolkit_NVCC_EXECUTABLE)
        set(LAB_SCHEDULER_CUDA_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/cuda_proof_kernel.obj)
        add_custom_command(
            OUTPUT ${LAB_SCHEDULER_CUDA_OBJECT}
            COMMAND ${CUDAToolkit_NVCC_EXECUTABLE} -c
                    ${CMAKE_CURRENT_SOURCE_DIR}/apps/cuda_proof_kernel.cu
                    -o ${LAB_SCHEDULER_CUDA_OBJECT}
                    -I${CUDAToolkit_INCLUDE_DIRS}
                    -arch=native -O2 -std=c++17 "-Xcompiler=$<IF:$<CONFIG:Debug>,/MDd,/MD>" -DNDEBUG
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/apps/cuda_proof_kernel.cu
            COMMENT "Compiling the Lab Scheduler accelerator proof kernel"
            VERBATIM)
        set_source_files_properties(${LAB_SCHEDULER_CUDA_OBJECT} PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

        add_executable(lab-scheduler-cuda-proof
            apps/cuda_proof_main.cpp
            ${LAB_SCHEDULER_CUDA_OBJECT})
        target_include_directories(lab-scheduler-cuda-proof PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/apps
            ${CMAKE_CURRENT_SOURCE_DIR}/examples
            ${CUDAToolkit_INCLUDE_DIRS})
        target_link_libraries(lab-scheduler-cuda-proof PRIVATE LabScheduler::lab_scheduler CUDA::cudart)
        lab_scheduler_strict(lab-scheduler-cuda-proof)
        set_target_properties(lab-scheduler-cuda-proof PROPERTIES WIN32_EXECUTABLE OFF)
        message(STATUS "Lab Scheduler: CUDA proof enabled with toolkit ${CUDAToolkit_VERSION}")
    else()
        message(STATUS "Lab Scheduler: CUDA toolkit not found, accelerator proof is UNSUPPORTED")
    endif()
endif()
