# Kernel test kernels: one entry "name:stage" = one test file
# test/kernel/test_<name>.cpp (integration: test/kernel/integration/) =
# one kernel = one image = one QEMU boot = one CTest case. The stage
# names the init-ladder level the test main walks before running cases.
set(CINUX_KTEST_STAGES exceptions irq keyboard mm proc block)
set(CINUX_KTESTS selfcheck:exceptions console:exceptions tick:irq keyboard:keyboard vmm:mm heap:mm aspace:mm cow:mm
    vma:mm demand:mm loader:mm uaccess:proc sched:proc block:block block_error:block ext2:block
    pipe:proc tty:keyboard smp:proc smp_pipe:proc xhci:block net:block)
set(CINUX_KTESTS_INTEGRATION "")
