cmake_minimum_required(VERSION 3.22)

project(Alchemy VERSION 0.1.0 LANGUAGES C CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_PATH "/Users/user/Documents/Fabric/JUCE" CACHE PATH "Path to a JUCE checkout")
add_subdirectory("${JUCE_PATH}" JUCE)

set(CHUCK_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/chuck/src/core")
set(WELD_RTCMIX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rtcmix" CACHE PATH "Path to an RTcmix checkout with an embedded build")
set(WELD_RTCMIX_LIBRARY "" CACHE FILEPATH "Optional path to librtcmix_embedded.dylib or librtcmix_embedded.so")
set(WELD_CSOUND_LIBRARY "" CACHE FILEPATH "Optional path to libcsound64.dylib, libcsound.dylib, or libcsound.so")
set(WELD_SUPERCOLLIDER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/supercollider" CACHE PATH "Path to a SuperCollider checkout with the Alchemy host audio backend")
set(WELD_SUPERCOLLIDER_LIBRARY "" CACHE FILEPATH "Optional path to a host-audio libscsynth.dylib or libscsynth.so")
set(WELD_SUPERCOLLIDER_LANG_LIBRARY "" CACHE FILEPATH "Optional path to libweldsclang.dylib or libweldsclang.so")

set(WELD_HAS_RTCMIX 0)
set(WELD_RTCMIX_INCLUDE_DIR "${WELD_RTCMIX_ROOT}/src/rtcmix")
set(WELD_RTCMIX_DEFAULT_LIBRARY "")
set(WELD_HAS_CSOUND 1)
set(WELD_CSOUND_DEFAULT_LIBRARY "")
set(WELD_HAS_SUPERCOLLIDER 0)
set(WELD_SUPERCOLLIDER_INCLUDE_DIR "${WELD_SUPERCOLLIDER_ROOT}/include/server")
set(WELD_SUPERCOLLIDER_DEFAULT_LIBRARY "")
set(WELD_SUPERCOLLIDER_DEFAULT_LANG_LIBRARY "")

if (APPLE AND EXISTS "${WELD_RTCMIX_ROOT}/src/rtcmix/librtcmix_embedded.dylib")
    set(WELD_RTCMIX_DEFAULT_LIBRARY "${WELD_RTCMIX_ROOT}/src/rtcmix/librtcmix_embedded.dylib")
elseif (UNIX AND EXISTS "${WELD_RTCMIX_ROOT}/src/rtcmix/librtcmix_embedded.so")
    set(WELD_RTCMIX_DEFAULT_LIBRARY "${WELD_RTCMIX_ROOT}/src/rtcmix/librtcmix_embedded.so")
endif()

if ((WELD_RTCMIX_LIBRARY STREQUAL "" OR WELD_RTCMIX_LIBRARY MATCHES "\\.a$")
    AND NOT WELD_RTCMIX_DEFAULT_LIBRARY STREQUAL "")
    set(WELD_RTCMIX_LIBRARY "${WELD_RTCMIX_DEFAULT_LIBRARY}" CACHE FILEPATH "Optional path to librtcmix_embedded.dylib or librtcmix_embedded.so" FORCE)
endif()

if (EXISTS "${WELD_RTCMIX_INCLUDE_DIR}/RTcmix_API.h")
    set(WELD_HAS_RTCMIX 1)
    if (WELD_RTCMIX_LIBRARY STREQUAL "")
        message(STATUS "RTcmix embedded backend enabled; set WELD_RTCMIX_LIBRARY or WELD_RTCMIX_LIBRARY env var before running to choose the runtime library")
    else()
        message(STATUS "RTcmix embedded backend enabled; runtime library hint: ${WELD_RTCMIX_LIBRARY}")
    endif()
else()
    message(STATUS "RTcmix embedded backend disabled; set WELD_RTCMIX_ROOT after cloning RTcmix")
endif()

if (APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build-csound/CsoundLib64.framework/CsoundLib64")
    set(WELD_CSOUND_DEFAULT_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/build-csound/CsoundLib64.framework/CsoundLib64")
elseif (UNIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build-csound/libcsound64.so")
    set(WELD_CSOUND_DEFAULT_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/build-csound/libcsound64.so")
elseif (UNIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build-csound/libcsound64.dylib")
    set(WELD_CSOUND_DEFAULT_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/build-csound/libcsound64.dylib")
elseif (APPLE AND EXISTS "/opt/homebrew/lib/libcsound64.dylib")
    set(WELD_CSOUND_DEFAULT_LIBRARY "/opt/homebrew/lib/libcsound64.dylib")
elseif (APPLE AND EXISTS "/usr/local/lib/libcsound64.dylib")
    set(WELD_CSOUND_DEFAULT_LIBRARY "/usr/local/lib/libcsound64.dylib")
elseif (APPLE AND EXISTS "/Library/Frameworks/CsoundLib64.framework/CsoundLib64")
    set(WELD_CSOUND_DEFAULT_LIBRARY "/Library/Frameworks/CsoundLib64.framework/CsoundLib64")
elseif (UNIX AND EXISTS "/usr/local/lib/libcsound64.so")
    set(WELD_CSOUND_DEFAULT_LIBRARY "/usr/local/lib/libcsound64.so")
elseif (UNIX AND EXISTS "/usr/lib/libcsound64.so")
    set(WELD_CSOUND_DEFAULT_LIBRARY "/usr/lib/libcsound64.so")
endif()

if (WELD_CSOUND_LIBRARY STREQUAL "" AND NOT WELD_CSOUND_DEFAULT_LIBRARY STREQUAL "")
    set(WELD_CSOUND_LIBRARY "${WELD_CSOUND_DEFAULT_LIBRARY}" CACHE FILEPATH "Optional path to libcsound64.dylib, libcsound.dylib, or libcsound.so" FORCE)
endif()

if (WELD_CSOUND_LIBRARY STREQUAL "")
    message(STATUS "Csound embedded backend enabled; set WELD_CSOUND_LIBRARY or WELD_CSOUND_LIBRARY env var before running to choose libcsound")
else()
    message(STATUS "Csound embedded backend enabled; runtime library hint: ${WELD_CSOUND_LIBRARY}")
endif()

if (APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build-supercollider-host/server/scsynth/libscsynth.dylib")
    set(WELD_SUPERCOLLIDER_DEFAULT_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/build-supercollider-host/server/scsynth/libscsynth.dylib")
elseif (UNIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build-supercollider-host/server/scsynth/libscsynth.so")
    set(WELD_SUPERCOLLIDER_DEFAULT_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/build-supercollider-host/server/scsynth/libscsynth.so")
endif()

if (APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build-supercollider-host/lang/libweldsclang.dylib")
    set(WELD_SUPERCOLLIDER_DEFAULT_LANG_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/build-supercollider-host/lang/libweldsclang.dylib")
elseif (UNIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build-supercollider-host/lang/libweldsclang.so")
    set(WELD_SUPERCOLLIDER_DEFAULT_LANG_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/build-supercollider-host/lang/libweldsclang.so")
endif()

if (WELD_SUPERCOLLIDER_LIBRARY STREQUAL "" AND NOT WELD_SUPERCOLLIDER_DEFAULT_LIBRARY STREQUAL "")
    set(WELD_SUPERCOLLIDER_LIBRARY "${WELD_SUPERCOLLIDER_DEFAULT_LIBRARY}" CACHE FILEPATH "Optional path to a host-audio libscsynth.dylib or libscsynth.so" FORCE)
endif()

if (WELD_SUPERCOLLIDER_LANG_LIBRARY STREQUAL "" AND NOT WELD_SUPERCOLLIDER_DEFAULT_LANG_LIBRARY STREQUAL "")
    set(WELD_SUPERCOLLIDER_LANG_LIBRARY "${WELD_SUPERCOLLIDER_DEFAULT_LANG_LIBRARY}" CACHE FILEPATH "Optional path to libweldsclang.dylib or libweldsclang.so" FORCE)
endif()

if (EXISTS "${WELD_SUPERCOLLIDER_INCLUDE_DIR}/SC_WorldOptions.h")
    set(WELD_HAS_SUPERCOLLIDER 1)
    if (WELD_SUPERCOLLIDER_LIBRARY STREQUAL "")
        message(STATUS "SuperCollider embedded backend enabled; set WELD_SUPERCOLLIDER_LIBRARY or WELD_SUPERCOLLIDER_LIBRARY env var before running to choose the host-audio libscsynth")
    else()
        message(STATUS "SuperCollider embedded backend enabled; runtime library hint: ${WELD_SUPERCOLLIDER_LIBRARY}")
    endif()
    if (WELD_SUPERCOLLIDER_LANG_LIBRARY STREQUAL "")
        message(STATUS "SuperCollider language bridge enabled; set WELD_SUPERCOLLIDER_LANG_LIBRARY or WELD_SUPERCOLLIDER_LANG_LIBRARY env var before running to compile SC source")
    else()
        message(STATUS "SuperCollider language bridge hint: ${WELD_SUPERCOLLIDER_LANG_LIBRARY}")
    endif()
else()
    message(STATUS "SuperCollider embedded backend disabled; set WELD_SUPERCOLLIDER_ROOT after cloning SuperCollider")
endif()

add_library(chuck_core STATIC
    ${CHUCK_CORE_DIR}/chuck_yacc.c
    ${CHUCK_CORE_DIR}/util_network.c
    ${CHUCK_CORE_DIR}/util_raw.c
    ${CHUCK_CORE_DIR}/util_sndfile.c
    ${CHUCK_CORE_DIR}/util_xforms.c
    ${CHUCK_CORE_DIR}/lo/address.c
    ${CHUCK_CORE_DIR}/lo/blob.c
    ${CHUCK_CORE_DIR}/lo/bundle.c
    ${CHUCK_CORE_DIR}/lo/message.c
    ${CHUCK_CORE_DIR}/lo/method.c
    ${CHUCK_CORE_DIR}/lo/pattern_match.c
    ${CHUCK_CORE_DIR}/lo/send.c
    ${CHUCK_CORE_DIR}/lo/server.c
    ${CHUCK_CORE_DIR}/lo/server_thread.c
    ${CHUCK_CORE_DIR}/lo/timetag.c
    ${CHUCK_CORE_DIR}/chuck.cpp
    ${CHUCK_CORE_DIR}/chuck_absyn.cpp
    ${CHUCK_CORE_DIR}/chuck_carrier.cpp
    ${CHUCK_CORE_DIR}/chuck_compile.cpp
    ${CHUCK_CORE_DIR}/chuck_dl.cpp
    ${CHUCK_CORE_DIR}/chuck_emit.cpp
    ${CHUCK_CORE_DIR}/chuck_errmsg.cpp
    ${CHUCK_CORE_DIR}/chuck_frame.cpp
    ${CHUCK_CORE_DIR}/chuck_globals.cpp
    ${CHUCK_CORE_DIR}/chuck_instr.cpp
    ${CHUCK_CORE_DIR}/chuck_io.cpp
    ${CHUCK_CORE_DIR}/chuck_lang.cpp
    ${CHUCK_CORE_DIR}/chuck_oo.cpp
    ${CHUCK_CORE_DIR}/chuck_parse.cpp
    ${CHUCK_CORE_DIR}/chuck_scan.cpp
    ${CHUCK_CORE_DIR}/chuck_stats.cpp
    ${CHUCK_CORE_DIR}/chuck_symbol.cpp
    ${CHUCK_CORE_DIR}/chuck_table.cpp
    ${CHUCK_CORE_DIR}/chuck_type.cpp
    ${CHUCK_CORE_DIR}/chuck_ugen.cpp
    ${CHUCK_CORE_DIR}/chuck_vm.cpp
    ${CHUCK_CORE_DIR}/uana_extract.cpp
    ${CHUCK_CORE_DIR}/uana_xform.cpp
    ${CHUCK_CORE_DIR}/ugen_filter.cpp
    ${CHUCK_CORE_DIR}/ugen_osc.cpp
    ${CHUCK_CORE_DIR}/ugen_stk.cpp
    ${CHUCK_CORE_DIR}/ugen_xxx.cpp
    ${CHUCK_CORE_DIR}/ulib_ai.cpp
    ${CHUCK_CORE_DIR}/ulib_doc.cpp
    ${CHUCK_CORE_DIR}/ulib_machine.cpp
    ${CHUCK_CORE_DIR}/ulib_math.cpp
    ${CHUCK_CORE_DIR}/ulib_opsc.cpp
    ${CHUCK_CORE_DIR}/ulib_std.cpp
    ${CHUCK_CORE_DIR}/util_buffers.cpp
    ${CHUCK_CORE_DIR}/util_console.cpp
    ${CHUCK_CORE_DIR}/util_hid.cpp
    ${CHUCK_CORE_DIR}/util_math.cpp
    ${CHUCK_CORE_DIR}/util_opsc.cpp
    ${CHUCK_CORE_DIR}/util_platforms.cpp
    ${CHUCK_CORE_DIR}/util_serial.cpp
    ${CHUCK_CORE_DIR}/util_string.cpp
    ${CHUCK_CORE_DIR}/util_thread.cpp)

target_include_directories(chuck_core
    SYSTEM PUBLIC
        ${CHUCK_CORE_DIR}
        ${CHUCK_CORE_DIR}/lo)

target_compile_definitions(chuck_core
    PUBLIC
        __MACOSX_CORE__
        __DISABLE_HID__
        __DISABLE_MIDI__
        __DISABLE_SERIAL__
        __DISABLE_SHELL__
        __DISABLE_OTF_SERVER__
        __DISABLE_NETWORK__
        __DISABLE_KBHIT__
        __DISABLE_PROMPTER__
        __USE_CHUCK_YACC__)

target_compile_options(chuck_core
    PRIVATE
        -Wno-deprecated-declarations
        -Wno-format
        -Wno-unused-variable
        -Wno-nontrivial-memcall
        -Wno-deprecated-copy-with-user-provided-copy
        -Wno-zero-as-null-pointer-constant
        -Wno-nested-anon-types
        -Wno-sign-conversion
        -Wno-extra-semi)

add_library(weld_chuck_engine STATIC)
add_library(Alchemy::Engine ALIAS weld_chuck_engine)
add_library(WeldChucK::Engine ALIAS weld_chuck_engine)

target_sources(weld_chuck_engine
    PRIVATE
        Source/WeldChucK/ChucKScoreScript.cpp
        Source/WeldChucK/ChucKScoreScript.h
        Source/WeldChucK/EmbeddedChucKEngine.cpp
        Source/WeldChucK/EmbeddedChucKEngine.h
        Source/WeldChucK/EmbeddedLanguageEngine.cpp
        Source/WeldChucK/EmbeddedLanguageEngine.h
        Source/WeldChucK/EmbeddedPerformanceEngine.cpp
        Source/WeldChucK/EmbeddedPerformanceEngine.h
        Source/WeldChucK/WeldChucKEngine.h)

target_include_directories(weld_chuck_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/WeldChucK
    PRIVATE
        ${JUCE_PATH}/modules)

if (WELD_HAS_RTCMIX)
    target_include_directories(weld_chuck_engine
        PRIVATE
            ${WELD_RTCMIX_INCLUDE_DIR})
endif()

if (WELD_HAS_SUPERCOLLIDER)
    target_include_directories(weld_chuck_engine
        SYSTEM PRIVATE
            ${WELD_SUPERCOLLIDER_INCLUDE_DIR}
            ${WELD_SUPERCOLLIDER_ROOT}/include/common)
endif()

target_link_libraries(weld_chuck_engine
    PRIVATE
        chuck_core
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
    INTERFACE
        juce::juce_audio_basics
        juce::juce_core
        juce::juce_recommended_config_flags)

target_compile_definitions(weld_chuck_engine
    PRIVATE
        WELD_HAS_RTCMIX=${WELD_HAS_RTCMIX}
        WELD_HAS_CSOUND=${WELD_HAS_CSOUND}
        WELD_HAS_SUPERCOLLIDER=${WELD_HAS_SUPERCOLLIDER}
        WELD_RTCMIX_DEFAULT_LIBRARY="${WELD_RTCMIX_LIBRARY}"
        WELD_CSOUND_DEFAULT_LIBRARY="${WELD_CSOUND_LIBRARY}"
        WELD_SUPERCOLLIDER_DEFAULT_LIBRARY="${WELD_SUPERCOLLIDER_LIBRARY}"
        WELD_SUPERCOLLIDER_DEFAULT_LANG_LIBRARY="${WELD_SUPERCOLLIDER_LANG_LIBRARY}"
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
        JUCE_MODULE_AVAILABLE_juce_audio_basics=1
        JUCE_MODULE_AVAILABLE_juce_core=1
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

juce_add_console_app(Alchemy
    PRODUCT_NAME "Alchemy"
    COMPANY_NAME "Codex"
    BUNDLE_ID "com.codex.alchemy")

target_sources(Alchemy
    PRIVATE
        Source/Main.cpp)

target_link_libraries(Alchemy
    PRIVATE
        Alchemy::Engine
        juce::juce_audio_devices
        juce::juce_data_structures
        juce::juce_events
    PUBLIC
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

juce_add_gui_app(AlchemyGui
    PRODUCT_NAME "Alchemy"
    COMPANY_NAME "Codex"
    BUNDLE_ID "com.codex.alchemy.gui")

target_sources(AlchemyGui
    PRIVATE
        Source/Gui/Main.cpp)

target_link_libraries(AlchemyGui
    PRIVATE
        Alchemy::Engine
        juce::juce_audio_devices
        juce::juce_data_structures
        juce::juce_events
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

add_custom_command(TARGET AlchemyGui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/Examples"
            "$<TARGET_BUNDLE_DIR:AlchemyGui>/Contents/Resources/Examples")

add_executable(AlchemyEngineBoundaryTest
    Tests/EngineBoundaryTest.cpp)

target_link_libraries(AlchemyEngineBoundaryTest
    PRIVATE
        Alchemy::Engine
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

add_test(NAME AlchemySelfTest COMMAND Alchemy --self-test)
add_test(NAME AlchemyStressTest COMMAND Alchemy --stress-test)
add_test(NAME AlchemyCallbackTest COMMAND Alchemy --callback-test)
add_test(NAME AlchemyFuzzTest COMMAND Alchemy --fuzz-test)
add_test(NAME AlchemyProgramTest COMMAND Alchemy --program-test)
add_test(NAME AlchemyParameterTest COMMAND Alchemy --parameter-test)
add_test(NAME AlchemyAsyncProgramTest COMMAND Alchemy --async-program-test)
add_test(NAME AlchemyScoreScriptTest COMMAND Alchemy --score-script-test)
add_test(NAME AlchemyBoundaryTest COMMAND Alchemy --boundary-test)
add_test(NAME AlchemyConcurrencyTest COMMAND Alchemy --concurrency-test)
add_test(NAME AlchemyEngineBoundaryTest COMMAND AlchemyEngineBoundaryTest)

set_tests_properties(AlchemySelfTest
                     AlchemyStressTest
                     AlchemyCallbackTest
                     AlchemyFuzzTest
                     AlchemyProgramTest
                     AlchemyParameterTest
                     AlchemyAsyncProgramTest
                     AlchemyScoreScriptTest
                     AlchemyBoundaryTest
                     AlchemyConcurrencyTest
                     AlchemyEngineBoundaryTest
    PROPERTIES
        TIMEOUT 30)

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS Alchemy AlchemyEngineBoundaryTest)
