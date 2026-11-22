# aether — 启动救援 Shell 工具箱
# §11：CMake 只做平台无关的描述逻辑；syscall ABI 由 src/sysnum/*.h 与编译宏承担。
cmake_minimum_required(VERSION 3.13)
project(aether C ASM)

option(USE_HOST_LIBC "Use host libc syscall wrapper (local build). OFF = official nolibc static package (-nostdlib, only x86_64/aarch64/i386/arm/riscv64/loongarch64)" ON)
option(AETHER_BUILD_TESTS "Build host unit tests (libc build only)" ON)
set(AETHER_ARCH "" CACHE STRING "Target arch override: x86_64|aarch64|i386|arm|riscv64|loongarch64 (default: CMAKE_SYSTEM_PROCESSOR, normalized)")
set(AETHER_VERSION "1.0.1" CACHE STRING "Version string embedded in the binary")

# --- 架构归一（§11）：AMD64/x64→x86_64；aarch64/arm64→aarch64；i386/i486/i586/i686→i386；armv*/armhf/arm→arm；riscv64/riscv→riscv64；loongarch64→loongarch64 ---
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _proc)
if(AETHER_ARCH STREQUAL "")
    if(_proc MATCHES "^(x86_64|amd64|x64)$")
        set(AETHER_ARCH x86_64)
    elseif(_proc MATCHES "^(aarch64|arm64)$")
        set(AETHER_ARCH aarch64)
    elseif(_proc MATCHES "^(i386|i486|i586|i686|x86)$")
        set(AETHER_ARCH i386)
    elseif(_proc MATCHES "^(armv[0-9]+.*|armhf|armel|arm)$")
        set(AETHER_ARCH arm)
    elseif(_proc MATCHES "^(riscv64|riscv)$")
        set(AETHER_ARCH riscv64)
    elseif(_proc MATCHES "^(loongarch64|loong64)$")
        set(AETHER_ARCH loongarch64)
    else()
        set(AETHER_ARCH unknown)
    endif()
endif()

set(AETHER_VALID_ARCHES "x86_64;aarch64;i386;arm;riscv64;loongarch64")
if(NOT AETHER_ARCH IN_LIST AETHER_VALID_ARCHES)
    if(USE_HOST_LIBC)
        message(WARNING "aether: 未识别的架构 '${CMAKE_SYSTEM_PROCESSOR}'，按 host 原生 libc 构建（结果可能不符预期）")
    else()
        message(FATAL_ERROR
            "aether: nolibc 官方包不支持架构 '${CMAKE_SYSTEM_PROCESSOR}'（归一化结果：'${AETHER_ARCH}'）。\n"
            "nolibc 仅支持 x86_64 / aarch64 / i386 / arm / riscv64 / loongarch64；其他架构请用 -DUSE_HOST_LIBC=ON。\n"
            "riscv32 / rv32* 亦不支持。")
    endif()
endif()

if(NOT USE_HOST_LIBC AND _proc MATCHES "rv32|riscv32")
    message(FATAL_ERROR "aether: RV32 不支持；riscv64 仅按 RV64G 小端构建。")
endif()

message(STATUS "aether: arch=${AETHER_ARCH}, USE_HOST_LIBC=${USE_HOST_LIBC}")

# clang 用 --target= 交叉时，ASM 也要带同样的 --target，否则 .S 按宿主架构汇编
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS}")
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_FLAGS MATCHES "(^| )--target=([^ ]+)")
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} --target=${CMAKE_MATCH_2}")
    message(STATUS "aether: ASM 使用目标 ${CMAKE_MATCH_2}")
endif()

# --- 编译器与目标架构一致性校验：裸传交叉编译器时 CMAKE_SYSTEM_PROCESSOR 仍是宿主机值，
#     必须以 -dumpmachine 为准防止拿错 start_*.S（如本机的 x86_64 .S 被拿去喂给 aarch64 编译器） ---
separate_arguments(_cflags UNIX_COMMAND "${CMAKE_C_FLAGS}")
execute_process(COMMAND ${CMAKE_C_COMPILER} ${_cflags} -dumpmachine
                OUTPUT_VARIABLE AETHER_COMPILER_TUPLE
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(TOLOWER "${AETHER_COMPILER_TUPLE}" _tuple)
if(_tuple STREQUAL "")
    message(STATUS "aether: 无法通过 ${CMAKE_C_COMPILER} -dumpmachine 识别目标，跳过一致性校验")
else()
    set(_tuple_ok FALSE)
    if(AETHER_ARCH STREQUAL "i386"   AND _tuple MATCHES "x86_64|amd64" AND CMAKE_C_FLAGS MATCHES "(^| )-m32($| )")
        # -m32（32 位模式）：gcc 的 -m32 不改变 -dumpmachine 输出，仍为 x86_64-pc-linux-gnu
        set(_tuple_ok TRUE)
    elseif(AETHER_ARCH STREQUAL "x86_64"  AND _tuple MATCHES "x86_64|amd64")
        set(_tuple_ok TRUE)
    elseif(AETHER_ARCH STREQUAL "aarch64" AND _tuple MATCHES "aarch64|arm64")
        set(_tuple_ok TRUE)
    elseif(AETHER_ARCH STREQUAL "i386"    AND _tuple MATCHES "i386|i486|i586|i686")
        set(_tuple_ok TRUE)
    elseif(AETHER_ARCH STREQUAL "riscv64" AND _tuple MATCHES "riscv64")
        set(_tuple_ok TRUE)
    elseif(AETHER_ARCH STREQUAL "loongarch64" AND _tuple MATCHES "loongarch64")
        set(_tuple_ok TRUE)
    elseif(AETHER_ARCH STREQUAL "arm" AND _tuple MATCHES "arm")
        set(_tuple_ok TRUE)
    endif()
    if(NOT _tuple_ok)
        message(FATAL_ERROR
            "aether: 编译器目标（${AETHER_COMPILER_TUPLE}）与 -DAETHER_ARCH=${AETHER_ARCH} 不一致。\n"
            "交叉构建请显式传 -DAETHER_ARCH=<x86_64|aarch64|i386|arm|riscv64|loongarch64>，或用 toolchain 文件。")
    endif()
endif()

# --- 编译选项：-Os 静态链接取向（§3）；nolibc 加 -nostdlib -ffreestanding ---
# gnu11：Linux 下需要 _DEFAULT_SOURCE 等特性宏（如 syscall(3) 声明）；方言差异对本项目无影响
set(AETHER_COMMON_FLAGS -Os -Wall -Wextra -std=gnu11 -fno-stack-protector -fno-ident)

add_library(aether_core STATIC
    src/elog.c
    src/mem.c
    src/argparse.c
    src/syscall_shim.c
    src/util.c
    src/applets.c
    src/ttyutil.c
    src/sysfsutil.c
    src/boot.c
    src/tools/fpath.c
    src/shell/pda.c
    src/shell/builtin.c
    src/shell/jobs.c
    src/shell/lineedit.c
    src/shell/shell.c
    src/tools/dmesg.c
    src/tools/free.c
    src/tools/ps.c
    src/tools/lsblk.c
    src/uevent.c
    src/modload.c
    src/tools/hotplug.c
    src/tools/net.c
    src/tools/filetools.c
    src/tools/text_stream.c
    src/tools/cmp_diff.c
    src/tools/hex.c
    src/tools/find.c
    src/tools/syslock.c
    src/tools/swap.c
    src/tools/decompress.c
    src/tools/compress.c
    src/tools/regex.c
    src/tools/grep.c
    src/tools/sysinfo.c
    src/tools/less.c
    src/tools/editor.c
    src/tools/efibootmgr.c
)
target_include_directories(aether_core PUBLIC src)
target_compile_definitions(aether_core PUBLIC AETHER_VERSION="${AETHER_VERSION}")

if(USE_HOST_LIBC)
    # 本地编译分支：宿主 libc 承担 ABI/编号差异；可选 gc-sections 丢未用代码
    target_compile_definitions(aether_core PUBLIC USE_LIBC_SYSCALL)
    target_compile_options(aether_core PRIVATE ${AETHER_COMMON_FLAGS})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_options(aether_core PRIVATE -ffunction-sections -fdata-sections)
    endif()
else()
    # nolibc 官方包分支：完全 freestanding，自带 _start / asm 模板 / sysnum 表
    target_sources(aether_core PRIVATE src/nolibc_mini.c src/nolibc/udiv64.c)
    target_compile_options(aether_core PRIVATE ${AETHER_COMMON_FLAGS}
        -ffreestanding -fno-builtin -fno-pie -fno-pic)
endif()

add_executable(aether src/main.c)
target_link_libraries(aether PRIVATE aether_core)

if(USE_HOST_LIBC)
    target_compile_options(aether PRIVATE ${AETHER_COMMON_FLAGS})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(aether PRIVATE -Wl,--gc-sections -Wl,--strip-all -Wl,--build-id=none)
    endif()
else()
    target_sources(aether PRIVATE src/nolibc/start_${AETHER_ARCH}.S)
    # x86_64/i386/arm 需要信号返回 trampoline（aether_sig_restorer）；aarch64/riscv64/loongarch64 由内核提供
    if(AETHER_ARCH STREQUAL "x86_64" OR AETHER_ARCH STREQUAL "i386" OR AETHER_ARCH STREQUAL "arm")
        target_sources(aether PRIVATE src/nolibc/sigreturn_${AETHER_ARCH}.S)
    endif()
    target_compile_options(aether PRIVATE ${AETHER_COMMON_FLAGS}
        -ffreestanding -fno-builtin -fno-pie -fno-pic)
    # 默认去符号/去 build-id（体积优先）；构建可复现、initramfs 友好
    target_link_options(aether PRIVATE -nostdlib -static
        -Wl,--strip-all -Wl,--build-id=none -Wl,--gc-sections)
endif()

# 默认移除编译器标识/注释段
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(AETHER_STRIP_TOOL NAMES ${CMAKE_STRIP} strip)
    if(AETHER_STRIP_TOOL)
        add_custom_command(TARGET aether POST_BUILD
            COMMAND ${AETHER_STRIP_TOOL} --remove-section=.comment $<TARGET_FILE:aether>
            COMMENT "Removing .comment from aether"
            VERBATIM)
    endif()
endif()

# --- 单元测试（仅 host/libc 构建）---
if(AETHER_BUILD_TESTS AND USE_HOST_LIBC)
    enable_testing()
    foreach(t test_argparse test_mem)
        add_executable(${t} tests/${t}.c)
        target_link_libraries(${t} PRIVATE aether_core)
        target_compile_options(${t} PRIVATE ${AETHER_COMMON_FLAGS})
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
endif()
