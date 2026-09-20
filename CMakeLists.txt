cmake_minimum_required(VERSION 3.20)
project(pjournal_lvgl VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

option(PJOURNAL_STATIC "Build a static executable" ON)
option(PJOURNAL_TARGET_ARM "Tune for target ARMv7 device" OFF)
option(PJOURNAL_FCITX5 "Enable the fcitx5+rime input method backend" ON)
option(PJOURNAL_YONG "Enable the yong input method backend" ON)
# yong 的源码树直接引用本地一份,不做 submodule、不 vendor。引擎(mb/*.c + llib/*.c)
# 静态链进主程序,不走 dlopen 设备上那个 libmb.so。
set(PJOURNAL_YONG_DIR "/home/ywz/yong" CACHE PATH "yong source tree")
# 设备上的 yong 数据目录(词库/表);x86 本机试的时候用同名环境变量顶掉。
set(PJOURNAL_YONG_DATA "/root/yong" CACHE STRING "yong data dir on the target")

# 交叉编译时 glib/gio 用目标 rootfs 里那套(包含 .a),不能用主机的 pkg-config。
# rootfs 里的 usr/lib32 就是 usr/lib,所以 pc 文件里的 -L/usr/lib32 也指得对。
if(PJOURNAL_FCITX5)
    find_package(PkgConfig REQUIRED)
    if(CMAKE_CROSSCOMPILING)
        set(PJOURNAL_SYSROOT "/home/ywz/void/rootfs" CACHE PATH
            "Target rootfs providing glib/gio for the fcitx5 backend")
        if(NOT EXISTS "${PJOURNAL_SYSROOT}/usr/lib/pkgconfig/gio-2.0.pc")
            message(FATAL_ERROR "PJOURNAL_SYSROOT=${PJOURNAL_SYSROOT} 里没有 gio-2.0.pc")
        endif()
        set(ENV{PKG_CONFIG_SYSROOT_DIR} "${PJOURNAL_SYSROOT}")
        set(ENV{PKG_CONFIG_LIBDIR}
            "${PJOURNAL_SYSROOT}/usr/lib/pkgconfig:${PJOURNAL_SYSROOT}/usr/share/pkgconfig")
    endif()
    pkg_check_modules(GIO2 REQUIRED IMPORTED_TARGET gio-2.0)
endif()

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

# yong 引擎 + 桥接层。单独一个静态库,是因为那 42 个老 C 文件要一整套自己的编译选项和
# include 路径(-fms-extensions / -include stdint.h / 一堆 yong 目录),蹭到主程序上会
# 污染其它文件。库不存在时 linux_ime.cpp 走「没有 yong 后端」那条分支。
if(PJOURNAL_YONG AND EXISTS "${PJOURNAL_YONG_DIR}/include/yong.h")
    file(GLOB PJOURNAL_YONG_LLIB "${PJOURNAL_YONG_DIR}/llib/*.c")
    set(PJOURNAL_YONG_MB
        mb/mb.c mb/face.c mb/learn.c mb/assoc.c mb/pyzip.c mb/fuzzy.c
        mb/cset.c mb/sentence.c mb/tools.c mb/correct.c
        common/pinyin.c common/trie.c
        # 拆字(笔画)辅助引擎。它 include 了 common/common.h,靠下面的 CFG_NO_GLIB 绕开 glib
        common/bihua.c)
    foreach(f ${PJOURNAL_YONG_MB})
        list(APPEND PJOURNAL_YONG_ENGINE "${PJOURNAL_YONG_DIR}/${f}")
    endforeach()
    add_library(pjournal_yong STATIC
        ${PJOURNAL_YONG_LLIB}
        ${PJOURNAL_YONG_ENGINE}
        src/yong_bridge.c
        src/yong_bihua_compat.c
        src/yong_s2t.c
        src/yong_ime.cpp)
    # 这两个文件 include 的是 yong 的 common/common.h(glib 只在没定义 CFG_NO_GLIB 时拉进来),
    # 顺带它们要的 y_im_* / YongGetPunc 就是 yong_bihua_compat.c 里那份
    set_source_files_properties(
        "${PJOURNAL_YONG_DIR}/common/bihua.c" src/yong_bihua_compat.c
        PROPERTIES COMPILE_OPTIONS "-DCFG_NO_GLIB")
    target_include_directories(pjournal_yong PUBLIC include)
    target_include_directories(pjournal_yong PRIVATE
        "${PJOURNAL_YONG_DIR}"
        "${PJOURNAL_YONG_DIR}/include"
        "${PJOURNAL_YONG_DIR}/common"
        "${PJOURNAL_YONG_DIR}/llib"
        "${PJOURNAL_YONG_DIR}/mb"
        # src/yong_ime.cpp 的英文代理要调内置输入法(IME.h)
        "${CMAKE_CURRENT_SOURCE_DIR}/port/esp32_main"
        "${CMAKE_CURRENT_SOURCE_DIR}/port/esp32_main/ime")
    target_compile_options(pjournal_yong PRIVATE
        -fms-extensions -fno-strict-aliasing -D_GNU_SOURCE -w
        # 一个 SHELL: 元素,不然 CMake 会把两个 -include 去重拼成 "-include a b"
        "SHELL:-include stdint.h -include stdbool.h")
    target_compile_definitions(pjournal_yong PUBLIC
        PJOURNAL_HAS_YONG PJOURNAL_YONG_DATA="${PJOURNAL_YONG_DATA}")
else()
    message(STATUS "yong 后端关闭(${PJOURNAL_YONG_DIR}/include/yong.h 不存在)")
endif()

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
    src/fcitx5_ime.cpp
    port/esp32_main/json_utils.cpp
    port/esp32_main/json_parser.cpp
    port/esp32_main/ime/IME.cpp
    ${PJOURNAL_IME_OBJ}
    ${PJOURNAL_LIANGFEN_OBJ}
    ${PJOURNAL_ENGLISH_OBJ}
)

target_include_directories(pjournal-lvgl PRIVATE include third_party/freetype/include port/esp32_main port/esp32_main/ime)
target_link_libraries(pjournal-lvgl PRIVATE lvgl freetype m pthread)

if(TARGET pjournal_yong)
    target_link_libraries(pjournal-lvgl PRIVATE pjournal_yong)
endif()

if(PJOURNAL_FCITX5)
    target_include_directories(pjournal-lvgl PRIVATE ${GIO2_INCLUDE_DIRS})
    if(PJOURNAL_STATIC)
        # glib/gobject/gio/gmodule 互相有循环依赖,静态链必须套 start-group
        target_link_libraries(pjournal-lvgl PRIVATE
            -Wl,--start-group ${GIO2_STATIC_LDFLAGS} -Wl,--end-group)
    else()
        target_link_libraries(pjournal-lvgl PRIVATE PkgConfig::GIO2)
    endif()
endif()

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
        src/outline_model.cpp
        src/process_utils.cpp
        src/safe_file.cpp
        src/esp32_compat.cpp
        src/settings.cpp
        port/esp32_main/json_parser.cpp
    )
    target_include_directories(pjournal-tests PRIVATE include port/esp32_main)
    target_compile_options(pjournal-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME pjournal-tests COMMAND pjournal-tests)
endif()
