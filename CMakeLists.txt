cmake_minimum_required(VERSION 3.15)
project(NokiaKlattSAPI VERSION 1.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(NK_X64 TRUE)
    message(STATUS "Nokia Klatt: x64 half - engine host, x64 SAPI, config utility, tools")
else()
    set(NK_X64 FALSE)
    message(STATUS "Nokia Klatt: x86 half - 32-bit SAPI engine only")
endif()

set(NK_COMMON_DEFS _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX)
set(NK_TARGETS)

# ---------------------------------------------------------------------------
# nk_shared - what both architectures need: logging, the voice catalogue, the
# settings file, the DSP that supplies the rate, pitch and volume the ROM
# engine refuses to apply, and the client half of the host protocol.
# ---------------------------------------------------------------------------
add_library(nk_shared STATIC
    src/nk/log.cpp
    src/nk/catalog.cpp
    src/nk/settings.cpp
    src/nk/dsp.cpp
    src/nk/client.cpp
    src/nk/pipe_io.cpp
)
target_include_directories(nk_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(nk_shared PRIVATE ${NK_COMMON_DEFS})
target_link_libraries(nk_shared PUBLIC shlwapi advapi32)
list(APPEND NK_TARGETS nk_shared)

# ---------------------------------------------------------------------------
# The SAPI5 engine. Built for both architectures from the same sources; each
# one is a thin client of the x64 host.
# ---------------------------------------------------------------------------
add_library(NokiaKlattSAPI SHARED
    src/sapi/nk_sapi_main.cpp
    src/sapi/nk_engine.cpp
    src/sapi/nk_enum.cpp
    src/sapi/nk_token.cpp
    src/sapi/nk_voices.cpp
    src/sapi/ISpDataKeyImpl.cpp
    src/sapi/com.cpp
    src/sapi/registry.cpp
    src/sapi/nk_sapi.def
)
target_include_directories(NokiaKlattSAPI PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sapi
)
target_compile_definitions(NokiaKlattSAPI PRIVATE ${NK_COMMON_DEFS})
target_link_libraries(NokiaKlattSAPI PRIVATE nk_shared ole32 oleaut32 advapi32 shell32)
list(APPEND NK_TARGETS NokiaKlattSAPI)

# Drives the SAPI DLL through its own class objects, so both architectures can
# be tested without the administrator rights registration needs.
add_executable(nk_sapitest src/tools/nk_sapitest.cpp)
target_include_directories(nk_sapitest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(nk_sapitest PRIVATE ${NK_COMMON_DEFS})
target_link_libraries(nk_sapitest PRIVATE ole32 oleaut32 sapi)
list(APPEND NK_TARGETS nk_sapitest)

if(NK_X64)
    # -----------------------------------------------------------------------
    # nk_core - the Symbian emulation: ROM mapping, the EPOC kernel surface,
    # the file server, the E32 loader and the speech engine. x64 only, because
    # the vendored unicorn.dll is x64 only.
    # -----------------------------------------------------------------------
    add_library(nk_core STATIC
        src/nk/uc_api.cpp
        src/nk/rom.cpp
        src/nk/emu.cpp
        src/nk/f32.cpp
        src/nk/e32load.cpp
        src/nk/engine.cpp
    )
    target_include_directories(nk_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(nk_core PRIVATE ${NK_COMMON_DEFS})
    target_link_libraries(nk_core PUBLIC nk_shared)
    list(APPEND NK_TARGETS nk_core)

    add_executable(NokiaKlattHost
        src/host/nk_host.cpp
        src/host/engine_pool.cpp
    )
    target_compile_definitions(NokiaKlattHost PRIVATE ${NK_COMMON_DEFS})
    target_link_libraries(NokiaKlattHost PRIVATE nk_core)
    list(APPEND NK_TARGETS NokiaKlattHost)

    add_executable(nk_render src/tools/nk_render.cpp)
    target_compile_definitions(nk_render PRIVATE ${NK_COMMON_DEFS})
    target_link_libraries(nk_render PRIVATE nk_core)
    list(APPEND NK_TARGETS nk_render)

    add_executable(nk_speak src/tools/nk_speak.cpp)
    target_compile_definitions(nk_speak PRIVATE ${NK_COMMON_DEFS})
    target_link_libraries(nk_speak PRIVATE nk_shared)
    list(APPEND NK_TARGETS nk_speak)

    # The configuration utility, and the desktop icon that starts it.
    add_executable(NokiaKlattConfig WIN32
        src/config/nk_config.cpp
        src/config/nk_config.rc
    )
    target_include_directories(NokiaKlattConfig PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    )
    target_compile_definitions(NokiaKlattConfig PRIVATE ${NK_COMMON_DEFS})
    target_link_libraries(NokiaKlattConfig PRIVATE
        nk_shared comctl32 winmm ole32 shell32)
    list(APPEND NK_TARGETS NokiaKlattConfig)

    # unicorn.dll has to sit beside whatever loads it.
    add_custom_command(TARGET NokiaKlattHost POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/bin/_nokia/lib/unicorn/lib/unicorn.dll"
            "$<TARGET_FILE_DIR:NokiaKlattHost>/unicorn.dll"
        COMMENT "Copying unicorn.dll beside the host")
endif()

if(MSVC)
    foreach(target ${NK_TARGETS})
        target_compile_options(${target} PRIVATE /W3 /MP)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()
