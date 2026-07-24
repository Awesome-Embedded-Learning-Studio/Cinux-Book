/**
 * @file kernel/version.hpp
 * @brief Single source of truth for OS version identity strings.
 *
 * sys_uname / /proc/cpuinfo / (future) /proc/version all draw from here, so the
 * version string lives in exactly one place.  Bump these alongside the VERSION
 * field in the top-level CMakeLists.txt (a configure_file pass to inject
 * PROJECT_VERSION directly is a follow-up; until then the two are kept in sync
 * by hand and grep-checked).
 */

#pragma once

namespace cinux {

/// utsname.sysname / nodename.
constexpr const char* kOsName = "Cinux";

/// utsname.release (matches top-level CMakeLists.txt VERSION).
constexpr const char* kOsVersion = "1.0.0";

/// utsname.version (build-id string).
constexpr const char* kOsRelease = "#1 SMP Cinux";

/// /proc/cpuinfo "model name".
constexpr const char* kCpuModel = "Cinux x86_64";

}  // namespace cinux
