cmake_minimum_required(VERSION 3.20)
project(pjournal_lvgl LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

option(PJOURNAL_STATIC "Build a static executable" ON)
option(PJOURNAL_TARGET_ARM "Tune for target ARMv7 device" OFF)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(PJOURNAL_OBJCOPY_FORMAT elf32-littlearm)
    set(PJOURNAL_OBJCOPY_ARCH arm)
else()
    set(PJOURNAL_OBJCOPY_FORMAT elf64-x86-64)
    set(PJOURNAL_OBJCOPY_ARCH i386:x86-64)
endif()

set(PJOURNAL_IME_BIN ${CMAKE_CURRENT_BINARY_DIR}/ime_table_pinyin.bin)
set(PJOURNAL_IME_OBJ ${CMAKE_CURRENT_BINARY_DIR}/ime_table_pinyin.o)
set(PJOURNAL_LIANGFEN_BIN ${CMAKE_CURRENT_BINARY_DIR}/liangfen.bin)
set(PJOURNAL_LIANGFEN_OBJ ${CMAKE_CURRENT_BINARY_DIR}/liangfen.o)
set(PJOURNAL_ENGLISH_BIN ${CMAKE_CURRENT_BINARY_DIR}/english_words.txt)
set(PJOURNAL_ENGLISH_OBJ ${CMAKE_CURRENT_BINARY_DIR}/english_words_txt.o)
add_custom_command(
    OUTPUT ${PJOURNAL_IME_OBJ}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/port/esp32_main/ime/ime_table_pinyin.bin ${PJOURNAL_IME_BIN}
    COMMAND ${CMAKE_OBJCOPY} -I binary -O ${PJOURNAL_OBJCOPY_FORMAT} -B ${PJOURNAL_OBJCOPY_ARCH}
            ime_table_pinyin.bin ime_table_pinyin.o
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/port/esp32_main/ime/ime_table_pinyin.bin
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    VERBATIM
)
add_custom_command(
    OUTPUT ${PJOURNAL_LIANGFEN_OBJ}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/port/esp32_main/ime/liangfen.bin ${PJOURNAL_LIANGFEN_BIN}
    COMMAND ${CMAKE_OBJCOPY} -I binary -O ${PJOURNAL_OBJCOPY_FORMAT} -B ${PJOURNAL_OBJCOPY_ARCH}
            liangfen.bin liangfen.o
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/port/esp32_main/ime/liangfen.bin
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    VERBATIM
)
add_custom_command(
    OUTPUT ${PJOURNAL_ENGLISH_OBJ}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/port/esp32_main/ime/english_words.txt ${PJOURNAL_ENGLISH_BIN}
    COMMAND ${CMAKE_OBJCOPY} -I binary -O ${PJOURNAL_OBJCOPY_FORMAT} -B ${PJOURNAL_OBJCOPY_ARCH}
            english_words.txt english_words_txt.o
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/port/esp32_main/ime/english_words.txt
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    VERBATIM
)

add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(FT_DISABLE_ZLIB TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)
add_subdirectory(third_party/freetype EXCLUDE_FROM_ALL)

add_subdirectory(third_party/lvgl EXCLUDE_FROM_ALL)
target_include_directories(lvgl PUBLIC third_party/freetype/include)

add_executable(pjournal-lvgl
    src/main.cpp
    src/safe_file.cpp
    src/process_utils.cpp
    src/hash_utils.cpp
    src/esp32_compat.cpp
    src/settings.cpp
    src/journal_storage.cpp
    src/markdown.cpp
    src/vertical_layout.cpp
    src/outline_model.cpp
    src/font_manager.cpp
    src/wifi_manager.cpp
    src/webdav_sync.cpp
    src/file_manager_server.cpp
    src/app_ui.cpp
    src/text_utils.cpp
    src/linux_ime.cpp
    src/linux_input.cpp
    src/linux_console.cpp
    port/esp32_main/json_utils.cpp
    port/esp32_main/json_parser.cpp
    port/esp32_main/ime/IME.cpp
    ${PJOURNAL_IME_OBJ}
    ${PJOURNAL_LIANGFEN_OBJ}
    ${PJOURNAL_ENGLISH_OBJ}
)

target_include_directories(pjournal-lvgl PRIVATE include third_party/freetype/include port/esp32_main port/esp32_main/ime)
target_link_libraries(pjournal-lvgl PRIVATE lvgl freetype m pthread)

target_compile_options(pjournal-lvgl PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(PJOURNAL_TARGET_ARM)
    target_compile_options(pjournal-lvgl PRIVATE -mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard)
endif()

if(PJOURNAL_STATIC)
    target_link_options(pjournal-lvgl PRIVATE -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS pjournal-lvgl RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(pjournal-tests
        tests/test_utils.cpp
        src/hash_utils.cpp
        src/journal_storage.cpp
        src/process_utils.cpp
        src/safe_file.cpp
        src/settings.cpp
    )
    target_include_directories(pjournal-tests PRIVATE include)
    target_compile_options(pjournal-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME pjournal-tests COMMAND pjournal-tests)
endif()
