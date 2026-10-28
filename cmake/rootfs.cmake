set(CINUX_EXT2_IMG ${CMAKE_BINARY_DIR}/boot/ext2.img)
set(CINUX_BUSYBOX_GLIBC ${CMAKE_BINARY_DIR}/busybox-glibc/busybox)
set(CINUX_HELLO_GLIBC ${CMAKE_BINARY_DIR}/glibc/hello-glibc)
add_custom_command(
    OUTPUT ${CINUX_BUSYBOX_GLIBC}
    COMMAND ${CMAKE_COMMAND} -E env
            "CINUX_ROOTFS_CACHE=${CMAKE_SOURCE_DIR}/.cache/rootfs"
            bash ${CMAKE_SOURCE_DIR}/scripts/build-busybox-glibc.sh ${CINUX_BUSYBOX_GLIBC}
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/build-busybox-glibc.sh
            ${CMAKE_SOURCE_DIR}/user/rootfs/busybox/busybox.config
    COMMENT "glibc busybox for the ecosystem volume")
add_custom_target(busybox_glibc DEPENDS ${CINUX_BUSYBOX_GLIBC})
add_library(selfhost_seed_checks OBJECT EXCLUDE_FROM_ALL
    scripts/ext2_seed/hello.cpp scripts/ext2_seed/execprobe.c scripts/ext2_seed/sendfileprobe.c scripts/ext2_seed/nativeprobe.c)
target_link_libraries(selfhost_seed_checks PRIVATE cinux_warnings)
set_source_files_properties(scripts/ext2_seed/nativeprobe.c PROPERTIES COMPILE_DEFINITIONS _GNU_SOURCE)
set(CINUX_TOOLCHAIN_STAGING ${CMAKE_BINARY_DIR}/toolchain-staging)
add_custom_command(
    OUTPUT ${CINUX_TOOLCHAIN_STAGING}/hello.s
    COMMAND ${CMAKE_COMMAND} -E env
            "CINUX_ROOTFS_CACHE=${CMAKE_SOURCE_DIR}/.cache/rootfs"
            bash ${CMAKE_SOURCE_DIR}/scripts/build-toolchain-staging.sh ${CINUX_TOOLCHAIN_STAGING}
    DEPENDS selfhost_seed_checks
            ${CMAKE_SOURCE_DIR}/scripts/build-toolchain-staging.sh
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/hello.c
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/hello.cpp
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/execprobe.c
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/sendfileprobe.c
    COMMENT "GCC/G++ toolchain staging for the self-host volume")
add_custom_target(toolchain_staging DEPENDS ${CINUX_TOOLCHAIN_STAGING}/hello.s)
add_custom_command(
    OUTPUT ${CINUX_HELLO_GLIBC}
    COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/glibc/build-hello-glibc.sh ${CINUX_HELLO_GLIBC}
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/glibc/build-hello-glibc.sh
    COMMENT "glibc hello for the ecosystem volume")
add_custom_target(hello_glibc DEPENDS ${CINUX_HELLO_GLIBC})
set(CINUX_VESPER_ROOT "${CMAKE_SOURCE_DIR}/third_party/vesper")
file(GLOB_RECURSE CINUX_VESPER_INPUTS CONFIGURE_DEPENDS
    "${CINUX_VESPER_ROOT}/src/*.cpp" "${CINUX_VESPER_ROOT}/src/*.hpp"
    "${CINUX_VESPER_ROOT}/apps/terminal/*.cpp" "${CINUX_VESPER_ROOT}/apps/terminal/*.hpp"
    "${CINUX_VESPER_ROOT}/assets/*.hpp"
    "${CINUX_VESPER_ROOT}/cmake/*.cmake"
    "${CINUX_VESPER_ROOT}/third_party/visor/src/*.cpp"
    "${CINUX_VESPER_ROOT}/third_party/visor/include/*.hpp")
set(CINUX_VESPER_DESK ${CMAKE_BINARY_DIR}/glibc/vesper-desk)
add_custom_command(
    OUTPUT ${CINUX_VESPER_DESK}
    COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/build-vesper-desk.sh ${CINUX_VESPER_DESK}
            ${CMAKE_BINARY_DIR}/third_party/vesper ${CINUX_VESPER_ROOT}
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/build-vesper-desk.sh
            ${CINUX_VESPER_ROOT}/CMakeLists.txt
            ${CINUX_VESPER_ROOT}/examples/CMakeLists.txt
            ${CINUX_VESPER_ROOT}/apps/terminal/CMakeLists.txt
            ${CINUX_VESPER_ROOT}/examples/fbdev_desktop.cpp
            ${CINUX_VESPER_INPUTS}
    COMMENT "vesper fbdev desk, static")
add_custom_target(vesper_desk DEPENDS ${CINUX_VESPER_DESK})
add_custom_command(
    OUTPUT ${CINUX_EXT2_IMG}
    COMMAND ${CMAKE_COMMAND} -E touch ${CINUX_EXT2_IMG}
    COMMAND ${CMAKE_COMMAND} -E env "CINUX_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "CINUX_TOOLCHAIN_STAGING=${CINUX_TOOLCHAIN_STAGING}"
            "CINUX_HELLO_ELF=$<TARGET_FILE:user_hello>"
            "CINUX_SHELL_ELF=$<TARGET_FILE:user_shell>"
            "CINUX_MAPPER_ELF=$<TARGET_FILE:user_mapper>"
            "CINUX_SIGDEMO_ELF=$<TARGET_FILE:user_sigdemo>"
            "CINUX_SIGCATCH_ELF=$<TARGET_FILE:user_sigcatch>"
            "CINUX_NXDEMO_ELF=$<TARGET_FILE:user_nxdemo>"
            "CINUX_EFAULTER_ELF=$<TARGET_FILE:user_efaulter>"
            "CINUX_SMEPDEMO_ELF=$<TARGET_FILE:user_smepdemo>"
            "CINUX_ASLRDEMO_ELF=$<TARGET_FILE:user_aslrdemo>"
            "CINUX_TLSDEMO_ELF=$<TARGET_FILE:user_tlsdemo>"
            "CINUX_THREADDEMO_ELF=$<TARGET_FILE:user_threaddemo>"
            "CINUX_TTYDEMO_ELF=$<TARGET_FILE:user_ttydemo>"
            "CINUX_SMPSTRESS_ELF=$<TARGET_FILE:user_smpstress>"
            "CINUX_VESPER_DESK=${CINUX_VESPER_DESK}"
            bash ${CMAKE_SOURCE_DIR}/scripts/create_ext2_disk.sh ${CINUX_EXT2_IMG}
    DEPENDS user_hello user_shell user_mapper user_sigdemo user_sigcatch user_nxdemo user_ecotest
            user_efaulter user_smepdemo user_aslrdemo
            user_tlsdemo user_threaddemo user_ttydemo user_smpstress
            busybox_glibc hello_glibc toolchain_staging vesper_desk
            ${CINUX_VESPER_DESK}
            ${CINUX_TOOLCHAIN_STAGING}/hello.s
            ${CMAKE_SOURCE_DIR}/scripts/create_ext2_disk.sh
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/fstab
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/inittab
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/passwd
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/group
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/smp-stress.sh
    COMMENT "teaching ext2 volume for the block and fs experiments")
add_custom_target(ext2_img DEPENDS ${CINUX_EXT2_IMG})
set(CINUX_EXT2_KTEST_IMG ${CMAKE_BINARY_DIR}/boot/ext2_ktest.img)
add_custom_target(ext2_ktest_img
    COMMAND ${CMAKE_COMMAND} -E rm -f ${CINUX_EXT2_KTEST_IMG}
    COMMAND ${CMAKE_COMMAND} -E copy ${CINUX_EXT2_IMG} ${CINUX_EXT2_KTEST_IMG}
    DEPENDS ext2_img
    COMMENT "scratch copy of the ext2 volume for kernel tests")

add_custom_target(kernel_selfhost_volume
    COMMAND ${CINUX_PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/prepare-kernel-selfhost.py
            --build-dir ${CMAKE_BINARY_DIR}
    DEPENDS ext2_img boot_image
    COMMENT "Source-only kernel build volume for compilation inside Cinux"
    VERBATIM)
