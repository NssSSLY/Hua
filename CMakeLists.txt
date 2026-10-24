cmake_minimum_required(VERSION 3.20)
project(Hua VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_library(hua_frontend
    src/source.cpp src/token.cpp src/lexer.cpp src/parser.cpp src/ast_printer.cpp)
target_include_directories(hua_frontend PUBLIC include)
if(MSVC)
    target_compile_options(hua_frontend PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(hua_frontend PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_library(hua_semantics src/entry.cpp src/resume.cpp src/resources.cpp src/file_stdlib.cpp src/gc.cpp src/simd.cpp src/tasks.cpp src/task_engines.cpp src/concurrency_lowering.cpp src/lowering.cpp src/enum_value.cpp src/numeric.cpp src/array.cpp src/record_fields.cpp
    src/plain_record.cpp src/value.cpp src/sema.cpp src/runtime.cpp src/interpreter.cpp src/bytecode.cpp src/vm.cpp src/module.cpp src/external.cpp src/archive.cpp src/stdlib.cpp src/text_stdlib.cpp src/collections_stdlib.cpp src/callback_stdlib.cpp src/error_stdlib.cpp src/json.cpp src/binary_stdlib.cpp src/algorithm_stdlib.cpp src/system_stdlib.cpp src/filesystem_stdlib.cpp src/hir.cpp src/mir.cpp src/ir_verify.cpp src/ir_bytecode.cpp)
find_package(Threads REQUIRED)
target_link_libraries(hua_semantics PUBLIC hua_frontend Threads::Threads)
option(HUA_SEMA_PROFILE "Build diagnostic host semantic scale counters" OFF)
if(HUA_SEMA_PROFILE)
    target_compile_definitions(hua_semantics PUBLIC HUA_SEMA_PROFILE=1)
    add_executable(hua_sema_profile_tests tests/sema_profile_tests.cpp)
    target_link_libraries(hua_sema_profile_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_sema_profile_tests PRIVATE -static)
    endif()
    add_test(NAME sema_profile COMMAND hua_sema_profile_tests)
    if(WIN32)
        add_executable(hua_sema_scale_probe tests/sema_scale_probe.cpp)
        target_link_libraries(hua_sema_scale_probe PRIVATE hua_semantics psapi)
        if(MINGW)
            target_link_options(hua_sema_scale_probe PRIVATE -municode -static)
        endif()
    endif()
endif()
option(HUA_INTERNAL_PROFILE "Build internal runtime counters and diagnostic driver" OFF)
if(HUA_INTERNAL_PROFILE)
    target_compile_definitions(hua_semantics PUBLIC HUA_INTERNAL_PROFILE=1)
    add_executable(hua_performance_probe tests/performance_probe.cpp)
    target_link_libraries(hua_performance_probe PRIVATE hua_semantics)
    add_executable(hua_memory_probe tests/performance_probe.cpp tests/support/native_allocations.cpp)
    target_compile_definitions(hua_memory_probe PRIVATE HUA_MEMORY_AUDIT=1)
    target_link_libraries(hua_memory_probe PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_memory_probe PRIVATE -municode -static)
    endif()

    if(MINGW)
        target_link_options(hua_performance_probe PRIVATE -municode -static)
    endif()
endif()
# Fine GC diagnostics remain out of ordinary runtime and counter-only builds.
option(HUA_GC_COST_PROFILE "Build current-thread GC cost diagnostics" OFF)
if(HUA_GC_COST_PROFILE)
    if(NOT HUA_INTERNAL_PROFILE)
        message(FATAL_ERROR "HUA_GC_COST_PROFILE requires HUA_INTERNAL_PROFILE")
    endif()
    set_property(SOURCE src/gc.cpp APPEND PROPERTY COMPILE_DEFINITIONS HUA_GC_COST_PROFILE=1)
    target_compile_definitions(hua_performance_probe PRIVATE HUA_GC_COST_PROFILE=1)
    target_compile_definitions(hua_memory_probe PRIVATE HUA_GC_COST_PROFILE=1)
    add_executable(hua_gc_cost_tests tests/gc_cost_tests.cpp)
    target_compile_definitions(hua_gc_cost_tests PRIVATE HUA_GC_COST_PROFILE=1)
    target_link_libraries(hua_gc_cost_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_gc_cost_tests PRIVATE -static)
    endif()
    add_test(NAME gc_cost_diagnostics COMMAND hua_gc_cost_tests)
    set_tests_properties(gc_cost_diagnostics PROPERTIES TIMEOUT 30 LABELS "diagnostic-only")
endif()
set(WASMTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.tools/wasmtime-v49.0.2-x86_64-windows-c-api" CACHE PATH "Wasmtime C API SDK")
find_path(WASMTIME_INCLUDE wasmtime.h PATHS "${WASMTIME_ROOT}/include" REQUIRED NO_DEFAULT_PATH)
if(WIN32)
    set(WASMTIME_LIBRARY "${WASMTIME_ROOT}/lib/wasmtime.dll.lib")
else()
    find_library(WASMTIME_LIBRARY NAMES wasmtime PATHS "${WASMTIME_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
endif()
target_include_directories(hua_semantics SYSTEM PRIVATE "${WASMTIME_INCLUDE}")
target_link_libraries(hua_semantics PUBLIC "${WASMTIME_LIBRARY}" ${CMAKE_DL_LIBS})
if(WIN32)
    configure_file("${WASMTIME_ROOT}/lib/wasmtime.dll" "${CMAKE_CURRENT_BINARY_DIR}/wasmtime.dll" COPYONLY)
    configure_file("${WASMTIME_ROOT}/LICENSE" "${CMAKE_CURRENT_BINARY_DIR}/wasmtime-LICENSE.txt" COPYONLY)
endif()

if(MSVC)
    target_compile_options(hua_semantics PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(hua_semantics PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(hua src/main.cpp)
target_link_libraries(hua PRIVATE hua_semantics)
if(MINGW)
    target_link_options(hua PRIVATE -municode -static)
endif()
include(CTest)
if(BUILD_TESTING)
    add_executable(hua_frontend_tests tests/frontend_tests.cpp)
    target_link_libraries(hua_frontend_tests PRIVATE hua_frontend)
    if(MINGW)
        target_link_options(hua_frontend_tests PRIVATE -static)
    endif()
    target_compile_definitions(hua_frontend_tests PRIVATE HUA_TEST_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/tests/spec")
    add_test(NAME frontend COMMAND hua_frontend_tests)
    add_executable(hua_entry_boundary_tests tests/entry_boundary_tests.cpp)
    target_link_libraries(hua_entry_boundary_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_entry_boundary_tests PRIVATE -static)
    endif()
    add_test(NAME entry_boundary COMMAND hua_entry_boundary_tests)
    set_tests_properties(entry_boundary PROPERTIES TIMEOUT 10)

    add_executable(hua_runtime_tests tests/runtime_tests.cpp)
    target_link_libraries(hua_runtime_tests PRIVATE hua_semantics)
    target_compile_definitions(hua_runtime_tests PRIVATE HUA_RUNTIME_TEST_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime")
    if(MINGW)
        target_link_options(hua_runtime_tests PRIVATE -static)
    endif()
    add_test(NAME semantics_runtime COMMAND hua_runtime_tests)
    add_test(NAME cli_hello_run COMMAND hua run ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello.hua)
    set_tests_properties(cli_hello_run PROPERTIES PASS_REGULAR_EXPRESSION "Hua")
    add_test(NAME cli_version COMMAND hua version)
    set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "Hua 0.1.0-dev")
    add_test(NAME cli_hello_check COMMAND hua check ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello.hua)
    set_tests_properties(cli_hello_check PROPERTIES PASS_REGULAR_EXPRESSION "OK")
    add_test(NAME cli_hello_ast COMMAND hua ast ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello.hua)
    set_tests_properties(cli_hello_ast PROPERTIES PASS_REGULAR_EXPRESSION "Function main")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME vm_modules COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/module_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME cli_contract COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME entry_contract COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/entry_contract_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(entry_contract PROPERTIES TIMEOUT 180)
        add_test(NAME compiler_entry COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_entry_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(compiler_entry PROPERTIES TIMEOUT 600)


    endif()
endif()

# Local extension demonstration (owned source, no third-party native plugin execution).
add_library(hua_native_demo SHARED examples/extensions/native_math.cpp)
target_include_directories(hua_native_demo PRIVATE include)
set_target_properties(hua_native_demo PROPERTIES PREFIX "" OUTPUT_NAME "native_math"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/extensions"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/extensions")
if(MINGW)
    target_link_options(hua_native_demo PRIVATE -static)
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/extensions")
configure_file(examples/extensions/native_math.huam extensions/native_math.huam COPYONLY)
configure_file(examples/extensions/main.hua extensions/main.hua COPYONLY)
if(Python3_Interpreter_FOUND)
    if(WIN32)
        set(WASMTIME_RUNTIME "${WASMTIME_ROOT}/lib/wasmtime.dll")
    else()
        set(WASMTIME_RUNTIME "${WASMTIME_LIBRARY}")
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/extensions/wasm_math.wasm"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/make_wasm.py"
        "${WASMTIME_RUNTIME}" "${CMAKE_CURRENT_SOURCE_DIR}/examples/extensions/wasm_math.wat"
        "${CMAKE_CURRENT_BINARY_DIR}/extensions/wasm_math.wasm"
        DEPENDS scripts/make_wasm.py examples/extensions/wasm_math.wat)
    add_custom_target(hua_wasm_demo ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/extensions/wasm_math.wasm")
endif()

if(BUILD_TESTING)
    foreach(kind RANGE 1 5)
        add_library(hua_native_bad_${kind} SHARED tests/native_bad.cpp)
        target_include_directories(hua_native_bad_${kind} PRIVATE include)
        target_compile_definitions(hua_native_bad_${kind} PRIVATE BAD_KIND=${kind})
        set_target_properties(hua_native_bad_${kind} PROPERTIES PREFIX "" OUTPUT_NAME "bad${kind}"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/extensions"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/extensions")
        if(MINGW)
            target_link_options(hua_native_bad_${kind} PRIVATE -static)
        endif()
    endforeach()
    if(Python3_Interpreter_FOUND)
        add_test(NAME binary_collections COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/binary_collection_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        add_test(NAME er0_inventory COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/er0_inventory_tests.py ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME error_foundation COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/error_foundation_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        add_test(NAME stdlib_input COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/stdlib_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        add_test(NAME strings_p0 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/strings_p0_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        add_test(NAME callbacks_l01 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/callbacks_l01_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        add_test(NAME collections_p0 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/collections_p0_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        add_test(NAME archives_extensions COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_extension_tests.py $<TARGET_FILE:hua>
            ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${WASMTIME_RUNTIME})
    endif()
endif()

# Optional bridge: the core executable never links Python.
option(HUA_ENABLE_PYTHON_BRIDGE "Build the optional embedded CPython 3.12 Native module" OFF)
if(HUA_ENABLE_PYTHON_BRIDGE)
    find_package(Python3 3.12 EXACT REQUIRED COMPONENTS Interpreter Development.Embed)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import sys; print(sys.base_prefix)"
        OUTPUT_VARIABLE HUA_PYTHON_HOME OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    file(TO_CMAKE_PATH "${HUA_PYTHON_HOME}" HUA_PYTHON_HOME)
    set(HUA_PYTHON_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/python_bridge")
    file(MAKE_DIRECTORY "${HUA_PYTHON_BUNDLE}")
    configure_file(bridges/python/python_config.hpp.in python_config.hpp @ONLY)
    configure_file(bridges/python/python.hua python_bridge/python.hua COPYONLY)
    configure_file(bridges/python/python_native.huam python_bridge/python_native.huam COPYONLY)
    configure_file(bridges/python/example.hua python_bridge/main.hua COPYONLY)
    configure_file(bridges/python/packaging_example.hua python_bridge/packaging_example.hua COPYONLY)
    add_library(hua_python_bridge SHARED bridges/python/python_bridge.cpp)
    target_include_directories(hua_python_bridge PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(hua_python_bridge PRIVATE Python3::Python ${CMAKE_DL_LIBS})
    set_target_properties(hua_python_bridge PROPERTIES PREFIX "" OUTPUT_NAME "python_native"
        RUNTIME_OUTPUT_DIRECTORY "${HUA_PYTHON_BUNDLE}" LIBRARY_OUTPUT_DIRECTORY "${HUA_PYTHON_BUNDLE}")
    if(MINGW)
        target_link_options(hua_python_bridge PRIVATE -static)
    endif()
    if(WIN32)
        foreach(runtime IN ITEMS python312.dll vcruntime140.dll vcruntime140_1.dll)
            if(EXISTS "${HUA_PYTHON_HOME}/${runtime}")
                configure_file("${HUA_PYTHON_HOME}/${runtime}" "${HUA_PYTHON_BUNDLE}/${runtime}" COPYONLY)
            endif()
        endforeach()
    endif()
    if(EXISTS "${HUA_PYTHON_HOME}/LICENSE.txt")
        configure_file("${HUA_PYTHON_HOME}/LICENSE.txt" python_bridge/python-LICENSE.txt COPYONLY)
    endif()
    if(BUILD_TESTING)
        add_test(NAME python_bridge COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_bridge_tests.py $<TARGET_FILE:hua>
            ${CMAKE_CURRENT_BINARY_DIR} "${HUA_PYTHON_BUNDLE}" "${HUA_PYTHON_HOME}")
    endif()
endif()

if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME language_structure COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/language_structure_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_BINARY_DIR})
endif()

# Tree-walk calls use native frames; reserve enough stack to reach Hua's 128-call diagnostic on Windows.
if(WIN32)
    foreach(hua_executable IN ITEMS hua hua_runtime_tests)
        if(TARGET ${hua_executable})
            if(MSVC)
                target_link_options(${hua_executable} PRIVATE /STACK:8388608)
            elseif(MINGW)
                target_link_options(${hua_executable} PRIVATE -Wl,--stack,8388608)
            endif()
        endif()
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(hua_runtime_storage_probe tests/runtime_storage_probe.cpp tests/support/native_allocations.cpp)
    target_link_libraries(hua_runtime_storage_probe PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_runtime_storage_probe PRIVATE -static -municode)
    endif()
    add_executable(hua_plain_record_tests tests/plain_record_tests.cpp tests/support/native_allocations.cpp)
    target_link_libraries(hua_plain_record_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_plain_record_tests PRIVATE -static)
    endif()
    add_test(NAME plain_records COMMAND hua_plain_record_tests)
    set_tests_properties(plain_records PROPERTIES TIMEOUT 30 LABELS "runtime-contract")
    if(Python3_Interpreter_FOUND)
        add_test(NAME plain_record_execution COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/plain_record_execution_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/plain-record-execution)
        set_tests_properties(plain_record_execution PROPERTIES TIMEOUT 180 LABELS "runtime-contract")
    endif()
    add_executable(hua_gc_generation_tests tests/gc_generation_tests.cpp tests/support/native_allocations.cpp)
    target_link_libraries(hua_gc_generation_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_gc_generation_tests PRIVATE -static)
    endif()
    add_test(NAME gc_generations COMMAND hua_gc_generation_tests)
    set_tests_properties(gc_generations PROPERTIES TIMEOUT 30 LABELS "runtime-contract;gc-contract")
    add_executable(hua_gc_tests tests/gc_tests.cpp)
    target_link_libraries(hua_gc_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_gc_tests PRIVATE -static)
    endif()
    add_test(NAME managed_gc COMMAND hua_gc_tests)
    add_executable(hua_gc_graph_tests tests/gc_graph_tests.cpp)
    add_executable(hua_gc_graph_reference tests/gc_graph_tests.cpp tests/reference/gc_nested_edges.cpp)
    foreach(gc_graph_target IN ITEMS hua_gc_graph_tests hua_gc_graph_reference)
        target_link_libraries(${gc_graph_target} PRIVATE hua_semantics)
        if(MINGW)
            target_link_options(${gc_graph_target} PRIVATE -static)
        endif()
    endforeach()
    add_executable(hua_gc_index_tests tests/gc_index_tests.cpp)
    target_link_libraries(hua_gc_index_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_gc_index_tests PRIVATE -static)
    endif()
    add_executable(hua_gc_allocation_probe tests/gc_allocation_probe.cpp tests/support/native_allocations.cpp)
    target_link_libraries(hua_gc_allocation_probe PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_gc_allocation_probe PRIVATE -static)
    endif()
    add_test(NAME gc_allocation_contract COMMAND hua_gc_allocation_probe)
    set_tests_properties(gc_allocation_contract PROPERTIES TIMEOUT 30 LABELS "runtime-contract;gc-contract")
    add_test(NAME gc_index_contract COMMAND hua_gc_index_tests)
    set_tests_properties(gc_index_contract PROPERTIES TIMEOUT 30 LABELS "runtime-contract;gc-contract")
    add_test(NAME gc_graph_contract COMMAND hua_gc_graph_tests)
    set_tests_properties(gc_graph_contract PROPERTIES TIMEOUT 30 LABELS "runtime-contract;gc-contract")

endif()

if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME concurrency COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(BUILD_TESTING)
    add_executable(hua_task_tests tests/task_runtime_tests.cpp)
    target_link_libraries(hua_task_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_task_tests PRIVATE -static)
    endif()
    add_test(NAME task_runtime COMMAND hua_task_tests)
endif()

# Hua-written bootstrap frontend remains independently checked against the C++ lexer.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_executable(hua_bootstrap_oracle tests/bootstrap_lexer_oracle.cpp)
    target_link_libraries(hua_bootstrap_oracle PRIVATE hua_frontend)
    if(MINGW)
        target_link_options(hua_bootstrap_oracle PRIVATE -municode -static)
    endif()
    add_test(NAME selfhost_lexer COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfhost_lexer_tests.py $<TARGET_FILE:hua>
        $<TARGET_FILE:hua_bootstrap_oracle> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
endif()
find_program(HUA_NODE_EXECUTABLE NAMES node)
if(BUILD_TESTING AND HUA_NODE_EXECUTABLE)
    add_test(NAME vscode_extension COMMAND ${HUA_NODE_EXECUTABLE} --test ${CMAKE_CURRENT_SOURCE_DIR}/tests/vscode_extension_tests.cjs)
    set_tests_properties(vscode_extension PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME algorithms_config COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/algorithm_stdlib_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(BUILD_TESTING)
    add_executable(hua_algorithm_limits_tests tests/algorithm_limits_tests.cpp)
    target_link_libraries(hua_algorithm_limits_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_algorithm_limits_tests PRIVATE -static)
    endif()
    add_test(NAME algorithm_limits COMMAND hua_algorithm_limits_tests)
endif()


if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME filesystem_stdlib COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/filesystem_stdlib_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
endif()
if(BUILD_TESTING)
    add_executable(hua_filesystem_limits_tests tests/filesystem_limits_tests.cpp)
    target_link_libraries(hua_filesystem_limits_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_filesystem_limits_tests PRIVATE -static)
    endif()
    add_test(NAME filesystem_limits COMMAND hua_filesystem_limits_tests ${CMAKE_CURRENT_BINARY_DIR})
endif()


# E1 is an independent internal pipeline. The public CLI keeps its existing compiler.
if(BUILD_TESTING)
    add_executable(hua_ir_probe tests/ir_driver.cpp)
    target_link_libraries(hua_ir_probe PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_ir_probe PRIVATE -municode -static)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(hua_ir_tests tests/ir_tests.cpp)
    target_link_libraries(hua_ir_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_ir_tests PRIVATE -static)
    endif()
    add_test(NAME ir_validation COMMAND hua_ir_tests)
    if(Python3_Interpreter_FOUND)
        add_test(NAME ir_pipeline COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_pipeline_tests.py $<TARGET_FILE:hua>
            $<TARGET_FILE:hua_ir_probe> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    endif()
endif()

# D002-a internal resource protocol, deliberately separate from public Hua resource APIs.
if(BUILD_TESTING)
    add_executable(hua_resource_tests tests/resource_runtime_tests.cpp)
    target_link_libraries(hua_resource_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_resource_tests PRIVATE -static)
    endif()
    add_test(NAME resource_runtime COMMAND hua_resource_tests)
endif()

# D002-b synchronous public File and deterministic native fault seam.
if(BUILD_TESTING)
    add_executable(hua_file_resource_tests tests/file_resource_tests.cpp)
    target_link_libraries(hua_file_resource_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_file_resource_tests PRIVATE -static)
    endif()
    add_test(NAME file_resource_runtime COMMAND hua_file_resource_tests)
    if(Python3_Interpreter_FOUND)
        add_test(NAME file_stdlib COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/file_stdlib_tests.py $<TARGET_FILE:hua>
            ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    endif()
endif()

# D002-c continuation and bounded blocking integration.
if(BUILD_TESTING)
    add_executable(hua_task_resume_tests tests/task_resume_tests.cpp)
    target_link_libraries(hua_task_resume_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_task_resume_tests PRIVATE -static)
    endif()
    add_test(NAME task_resume_single COMMAND hua_task_resume_tests)
    set_tests_properties(task_resume_single PROPERTIES ENVIRONMENT "HUA_TASK_WORKERS=1" TIMEOUT 30)
    add_test(NAME task_resume_migration COMMAND hua_task_resume_tests)
    set_tests_properties(task_resume_migration PROPERTIES ENVIRONMENT "HUA_TASK_WORKERS=2" TIMEOUT 30 SKIP_RETURN_CODE 77)
    if(Python3_Interpreter_FOUND)
        add_test(NAME task_resume_public COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/task_resume_tests.py $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(task_resume_public PROPERTIES TIMEOUT 120)
    endif()
endif()

# A01 capability audit and SB1 guard; no change to the public compiler pipeline.
if(BUILD_TESTING)
    add_executable(hua_subset_probe tests/a01_subset_driver.cpp)
    target_link_libraries(hua_subset_probe PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_subset_probe PRIVATE -municode -static)
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME language_baseline COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/language_baseline_tests.py $<TARGET_FILE:hua>
            $<TARGET_FILE:hua_subset_probe> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(language_baseline PROPERTIES TIMEOUT 180)
    endif()
endif()

# A05 Hua AST representations and independent C++ source/AST reference.
if(BUILD_TESTING)
    add_executable(hua_ast_pool_oracle tests/a05_ast_oracle.cpp)
    target_link_libraries(hua_ast_pool_oracle PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_ast_pool_oracle PRIVATE -municode -static)
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME selfhost_ast_pool COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfhost_ast_pool_tests.py $<TARGET_FILE:hua>
            $<TARGET_FILE:hua_subset_probe> $<TARGET_FILE:hua_ast_pool_oracle>
            ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(selfhost_ast_pool PROPERTIES TIMEOUT 180)
    endif()
endif()

# C01 contract and C02 Hua Parser slice, with an independent C++ reference.
if(BUILD_TESTING)
    add_executable(hua_parser_oracle tests/c02_parser_oracle.cpp)
    target_link_libraries(hua_parser_oracle PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_parser_oracle PRIVATE -municode -static)
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME selfhost_parser COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfhost_parser_tests.py $<TARGET_FILE:hua>
            $<TARGET_FILE:hua_subset_probe> $<TARGET_FILE:hua_parser_oracle>
            ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(selfhost_parser PROPERTIES TIMEOUT 180)
    endif()
endif()

# C02-P2/C03 Hua front-end and independent host name/module reference.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME selfhost_frontend_extended COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfhost_frontend_extended_tests.py $<TARGET_FILE:hua>
        $<TARGET_FILE:hua_subset_probe> $<TARGET_FILE:hua_parser_oracle>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(selfhost_frontend_extended PROPERTIES TIMEOUT 300)
endif()

# A11 bounded compiler execution / C04 Hua type and capability reference.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME selfhost_types COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfhost_type_tests.py $<TARGET_FILE:hua>
        $<TARGET_FILE:hua_subset_probe> $<TARGET_FILE:hua_parser_oracle>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(selfhost_types PROPERTIES TIMEOUT 600)
endif()

# C05 typed VM core lowering; the C++ core driver is a test adapter, not a Hua emitter.
if(BUILD_TESTING)
    add_executable(hua_core_driver tests/c05_core_driver.cpp)
    target_link_libraries(hua_core_driver PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_core_driver PRIVATE -municode -static)
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME selfhost_lowering COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfhost_lowering_tests.py $<TARGET_FILE:hua>
            $<TARGET_FILE:hua_subset_probe> $<TARGET_FILE:hua_parser_oracle>
            $<TARGET_FILE:hua_core_driver> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(selfhost_lowering PROPERTIES TIMEOUT 600)
    endif()
endif()

# C06 Hua instructions; test adapter decodes without invoking BytecodeCompiler.
if(BUILD_TESTING)
    add_executable(hua_instruction_driver tests/c06_instruction_driver.cpp)
    target_link_libraries(hua_instruction_driver PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_instruction_driver PRIVATE -municode -static)
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME selfhost_bytecode COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfhost_bytecode_tests.py $<TARGET_FILE:hua>
            $<TARGET_FILE:hua_subset_probe> $<TARGET_FILE:hua_parser_oracle>
            $<TARGET_FILE:hua_core_driver> $<TARGET_FILE:hua_instruction_driver>
            ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(selfhost_bytecode PROPERTIES TIMEOUT 900)
    endif()
endif()

# A07 readonly descendant provenance and legal record copies.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME capability_provenance COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/capability_provenance_tests.py $<TARGET_FILE:hua>
        $<TARGET_FILE:hua_subset_probe> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(capability_provenance PROPERTIES TIMEOUT 600)
endif()

# C04 consumed temporary copies: ownership, readonly descendants, layout and GC.
if(BUILD_TESTING)
    add_executable(hua_value_copy_tests tests/value_copy_tests.cpp)
    target_link_libraries(hua_value_copy_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_value_copy_tests PRIVATE -static)
    endif()
    add_test(NAME value_copy COMMAND hua_value_copy_tests)
endif()

# C06 bounded report transport compatibility and rejection gate.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME selfhost_transport COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfhost_transport_tests.py $<TARGET_FILE:hua>
        $<TARGET_FILE:hua_instruction_driver> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(selfhost_transport PROPERTIES TIMEOUT 600)
endif()

# C07 complete Hua HUAB encoding and atomic publication gate.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME selfhost_archive COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfhost_archive_tests.py $<TARGET_FILE:hua>
        $<TARGET_FILE:hua_subset_probe> $<TARGET_FILE:hua_instruction_driver>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(selfhost_archive PROPERTIES TIMEOUT 900)
endif()

# A02 numeric/type/binding boundaries, independent Hua analysis and legacy HUAB.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME type_contract COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/a02_type_contract_tests.py $<TARGET_FILE:hua>
        $<TARGET_FILE:hua_subset_probe> $<TARGET_FILE:hua_parser_oracle>
        $<TARGET_FILE:hua_instruction_driver> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(type_contract PROPERTIES TIMEOUT 900)
    # VM changes need the runtime type corpus, not repeated Hua compiler preparation.
    add_test(NAME vm_type_contract COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/a02_type_contract_tests.py $<TARGET_FILE:hua>
        $<TARGET_FILE:hua_subset_probe> $<TARGET_FILE:hua_parser_oracle>
        $<TARGET_FILE:hua_instruction_driver> ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/vm-type-contract --native-only --cases-only)
    set_tests_properties(vm_type_contract PROPERTIES TIMEOUT 90 LABELS "runtime-contract;type-contract")
endif()

# Array sizes have one source spelling; legacy archive spelling stays reader-compatible.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME array_size_syntax COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/array_size_syntax_tests.py $<TARGET_FILE:hua>
        $<TARGET_FILE:hua_parser_oracle> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(array_size_syntax PROPERTIES TIMEOUT 120)
endif()

# A03 basic syntax stage: only native parsing and static checking, never VM execution.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME container_syntax COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/container_syntax_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(container_syntax PROPERTIES TIMEOUT 60 LABELS "frontend-only")
endif()

# A04 scope/call/return/loop-exit gate: source parser and static analysis only.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME function_control_syntax COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/function_control_syntax_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(function_control_syntax PROPERTIES TIMEOUT 60 LABELS "frontend-only")
endif()

# A03/A04 execution gates: bounded local programs across AST, VM and source-free HUAB.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME container_execution COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/container_control_execution_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} --section a03)
    add_test(NAME control_cleanup_execution COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/container_control_execution_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} --section a04)
    set_tests_properties(container_execution control_cleanup_execution PROPERTIES TIMEOUT 240 LABELS "execution-contract")
endif()
if(BUILD_TESTING)
    add_executable(hua_container_limits_tests tests/container_limits_tests.cpp)
    target_link_libraries(hua_container_limits_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_container_limits_tests PRIVATE -static)
    endif()
    add_test(NAME container_limits COMMAND hua_container_limits_tests)
    set_tests_properties(container_limits PROPERTIES TIMEOUT 10 LABELS "execution-contract")
endif()

# A08 native module identity, names, visibility, ordering and bounded graphs.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME module_contract COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/module_contract_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(module_contract PROPERTIES TIMEOUT 180 LABELS "module-contract")
endif()

# A09 resource graph inspection, compiler roots and deterministic failure release.
if(BUILD_TESTING)
    add_executable(hua_lifetime_tests tests/lifetime_tests.cpp)
    target_link_libraries(hua_lifetime_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_lifetime_tests PRIVATE -static)
    endif()
    add_test(NAME value_lifetime COMMAND hua_lifetime_tests)
    set_tests_properties(value_lifetime PROPERTIES TIMEOUT 30 LABELS "lifetime-contract")
endif()

if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME lifetime_contract COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/lifetime_contract_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(lifetime_contract PROPERTIES TIMEOUT 180 LABELS "lifetime-contract")
endif()

# A10 normalized source positions and bounded diagnostic/failure provenance.
if(BUILD_TESTING)
    add_executable(hua_diagnostic_tests tests/diagnostic_tests.cpp)
    target_link_libraries(hua_diagnostic_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_diagnostic_tests PRIVATE -static)
    endif()
    add_test(NAME diagnostic_source COMMAND hua_diagnostic_tests)
    set_tests_properties(diagnostic_source PROPERTIES TIMEOUT 10 LABELS "diagnostic-contract")
endif()

if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME diagnostic_contract COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostic_contract_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(diagnostic_contract PROPERTIES TIMEOUT 180 LABELS "diagnostic-contract")
endif()

# A07 per-field return/effect provenance and reused analyzer state.
if(BUILD_TESTING)
    add_executable(hua_capability_metadata_tests tests/capability_metadata_tests.cpp)
    target_link_libraries(hua_capability_metadata_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_capability_metadata_tests PRIVATE -static)
    endif()
    add_test(NAME capability_metadata COMMAND hua_capability_metadata_tests)
    set_tests_properties(capability_metadata PROPERTIES TIMEOUT 10 LABELS "capability-contract")
endif()
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME capability_contract COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/capability_contract_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(capability_contract PROPERTIES TIMEOUT 180 LABELS "capability-contract")
endif()

# Small C04 capability parity only; compiler own-source/generation pipelines stay separate.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_executable(hua_capability_prepared_execution tests/c04_prepared_execution_probe.cpp)
    target_link_libraries(hua_capability_prepared_execution PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_capability_prepared_execution PRIVATE -static -municode -Wl,--stack,8388608)
    endif()
    add_test(NAME capability_c04 COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/capability_c04_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
        --subset-probe $<TARGET_FILE:hua_subset_probe>
        --type-oracle $<TARGET_FILE:hua_parser_oracle>
        --extra-cases ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/c04_call_cases.json
        --execution-probe $<TARGET_FILE:hua_capability_prepared_execution>
        --batch-size 64 --child-seconds 60 --prepare-seconds 120)
    set_tests_properties(capability_c04 PROPERTIES TIMEOUT 900 LABELS "capability-contract;c04-small-inputs")
    add_test(NAME capability_instantiation COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/capability_instantiation_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/capability-instantiation)
    set_tests_properties(capability_instantiation PROPERTIES TIMEOUT 60 LABELS "capability-contract;c04-small-inputs")
    add_test(NAME capability_dependencies COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/capability_dependency_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/capability-dependencies)
    set_tests_properties(capability_dependencies PROPERTIES TIMEOUT 60 LABELS "capability-contract;c04-small-inputs")
    add_test(NAME capability_prepared_supervisor COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/capability_prepared_support_tests.py)
    set_tests_properties(capability_prepared_supervisor PROPERTIES TIMEOUT 10 LABELS "capability-contract;c04-small-inputs")
    add_test(NAME capability_symbolic COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/capability_symbolic_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/capability-symbolic
        --source ${CMAKE_CURRENT_SOURCE_DIR}/tests/experimental/c04-symbolic/capabilities.hua)
    set_tests_properties(capability_symbolic PROPERTIES TIMEOUT 60 LABELS "capability-contract;c04-small-inputs;c04-experimental")
    add_test(NAME capability_transforms COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/capability_transform_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/capability-transforms
        --source ${CMAKE_CURRENT_SOURCE_DIR}/tests/experimental/c04-r1-r2/capabilities.hua)
    set_tests_properties(capability_transforms PROPERTIES TIMEOUT 60 LABELS "capability-contract;c04-small-inputs;c04-experimental")
    add_test(NAME capability_flow_state COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/capability_flow_state_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/capability-flow-state
        --source ${CMAKE_CURRENT_SOURCE_DIR}/tests/experimental/c04-r1-r2/capabilities.hua)
    set_tests_properties(capability_flow_state PROPERTIES TIMEOUT 60 LABELS "capability-contract;c04-small-inputs;c04-experimental")
    add_test(NAME capability_parameter_origins COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/capability_seed_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/capability-seed)
    set_tests_properties(capability_parameter_origins PROPERTIES TIMEOUT 180 LABELS "capability-contract;c04-small-inputs")
    add_test(NAME capability_flow_metadata COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/capability_metadata_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/capability-metadata)
    set_tests_properties(capability_flow_metadata PROPERTIES TIMEOUT 60 LABELS "capability-contract;c04-small-inputs")
    add_test(NAME bytecode_verifier_metadata COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_verifier_tests.py $<TARGET_FILE:hua>
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/bytecode-verifier-metadata
        --probe $<TARGET_FILE:hua_subset_probe>)
    set_tests_properties(bytecode_verifier_metadata PROPERTIES TIMEOUT 300 LABELS "bytecode-contract;c06-small-images")
endif()

# Per-invocation VM metadata must never survive the AST/model used for that run.
if(BUILD_TESTING)
    add_executable(hua_runtime_metadata_tests tests/runtime_metadata_tests.cpp)
    target_link_libraries(hua_runtime_metadata_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_runtime_metadata_tests PRIVATE -static)
    endif()
    add_test(NAME runtime_metadata COMMAND hua_runtime_metadata_tests)
    set_tests_properties(runtime_metadata PROPERTIES TIMEOUT 30 LABELS "runtime-contract")
    add_executable(hua_vm_slot_tests tests/vm_slot_tests.cpp)
    target_link_libraries(hua_vm_slot_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_vm_slot_tests PRIVATE -static)
    endif()
    add_test(NAME vm_slots COMMAND hua_vm_slot_tests)
    set_tests_properties(vm_slots PROPERTIES TIMEOUT 30 LABELS "runtime-contract")
endif()


# Active enum instances retain verified layout identity and existing value capabilities.
if(BUILD_TESTING)
    add_executable(hua_enum_layout_tests tests/enum_layout_tests.cpp)
    target_link_libraries(hua_enum_layout_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_enum_layout_tests PRIVATE -static)
    endif()
    add_test(NAME enum_layout COMMAND hua_enum_layout_tests)
    set_tests_properties(enum_layout PROPERTIES TIMEOUT 30 LABELS "runtime-contract")
endif()

if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME enum_contract COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/enum_contract_tests.py
        $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(enum_contract PROPERTIES TIMEOUT 180 LABELS "runtime-contract")
endif()

if(BUILD_TESTING)
    add_executable(hua_record_layout_tests tests/record_layout_tests.cpp tests/support/native_allocations.cpp)
    target_link_libraries(hua_record_layout_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_record_layout_tests PRIVATE -static)
    endif()
    add_test(NAME record_layout COMMAND hua_record_layout_tests)
    set_tests_properties(record_layout PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME enum_dispatch COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/enum_dispatch_tests.py
        $<TARGET_FILE:hua> ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/enum-dispatch-tests --unique-output)
    set_tests_properties(enum_dispatch PROPERTIES TIMEOUT 180 LABELS "runtime-contract")
endif()

if(BUILD_TESTING)
    add_executable(hua_vm_observer_tests tests/vm_observer_tests.cpp)
    target_link_libraries(hua_vm_observer_tests PRIVATE hua_semantics)
    if(MINGW)
        target_link_options(hua_vm_observer_tests PRIVATE -static)
    endif()
    add_test(NAME vm_observer COMMAND hua_vm_observer_tests)
    set_tests_properties(vm_observer PROPERTIES TIMEOUT 30 LABELS "diagnostic-only")
endif()
