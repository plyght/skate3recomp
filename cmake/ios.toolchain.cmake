# Cross-compiles for iOS devices (arm64) with upstream LLVM Clang - ReXGlue
# rejects Apple Clang - against Xcode's iPhoneOS SDK. Requires a Mac with
# Xcode (for the SDK, ld and codesign) and Homebrew LLVM 18+.

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
if(NOT SKATE3_IOS_DEPLOYMENT_TARGET)
    set(SKATE3_IOS_DEPLOYMENT_TARGET "26.0")
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "${SKATE3_IOS_DEPLOYMENT_TARGET}" CACHE STRING "" FORCE)

execute_process(
    COMMAND xcrun --sdk iphoneos --show-sdk-path
    OUTPUT_VARIABLE _skate3_ios_sdk
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _skate3_ios_sdk_result)
if(NOT _skate3_ios_sdk_result EQUAL 0 OR NOT EXISTS "${_skate3_ios_sdk}")
    message(FATAL_ERROR "iPhoneOS SDK not found; install Xcode and run xcode-select -s")
endif()
set(CMAKE_OSX_SYSROOT "${_skate3_ios_sdk}" CACHE PATH "" FORCE)

if(NOT SKATE3_LLVM_ROOT)
    set(SKATE3_LLVM_ROOT "/opt/homebrew/opt/llvm")
endif()
set(CMAKE_C_COMPILER "${SKATE3_LLVM_ROOT}/bin/clang")
set(CMAKE_CXX_COMPILER "${SKATE3_LLVM_ROOT}/bin/clang++")
set(CMAKE_OBJC_COMPILER "${SKATE3_LLVM_ROOT}/bin/clang")
set(CMAKE_OBJCXX_COMPILER "${SKATE3_LLVM_ROOT}/bin/clang++")
set(CMAKE_ASM_COMPILER "${SKATE3_LLVM_ROOT}/bin/clang")
set(CMAKE_AR "${SKATE3_LLVM_ROOT}/bin/llvm-ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${SKATE3_LLVM_ROOT}/bin/llvm-ranlib" CACHE FILEPATH "" FORCE)

set(_skate3_ios_target "arm64-apple-ios${SKATE3_IOS_DEPLOYMENT_TARGET}")
# Use the SDK's libc++ headers, not Homebrew's: the app links the system
# libc++ dylib, so headers must match what iOS ships. Xcode's ld is used for
# linking (Homebrew clang finds it through xcrun's /usr/bin/ld shim).
set(_skate3_ios_flags
    "--target=${_skate3_ios_target} -isysroot ${_skate3_ios_sdk}")
set(_skate3_ios_cxx_flags
    "${_skate3_ios_flags} -nostdinc++ -isystem ${_skate3_ios_sdk}/usr/include/c++/v1")
set(CMAKE_C_FLAGS_INIT "${_skate3_ios_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_skate3_ios_flags}")
set(CMAKE_OBJC_FLAGS_INIT "${_skate3_ios_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_skate3_ios_cxx_flags}")
set(CMAKE_OBJCXX_FLAGS_INIT "${_skate3_ios_cxx_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--target=${_skate3_ios_target} -isysroot ${_skate3_ios_sdk}")

# Configure checks must not try to link and sign iOS executables.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH "${_skate3_ios_sdk}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
