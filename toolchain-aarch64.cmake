# docker/toolchain-aarch64.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TARGET aarch64-linux-gnu)
set(SYSROOT /opt/sysroots/ubuntu-arm64 CACHE PATH "Target sysroot")

set(CMAKE_C_COMPILER   ${TARGET}-gcc)
set(CMAKE_CXX_COMPILER ${TARGET}-g++)

# --- SYSROOT ---
set(CMAKE_SYSROOT ${SYSROOT})
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# sysroot를 플래그로도 명시
set(CMAKE_C_FLAGS_INIT   "--sysroot=${SYSROOT}")
set(CMAKE_CXX_FLAGS_INIT "--sysroot=${SYSROOT}")
string(JOIN " " _SYSROOT_RPATH_LINK_FLAGS
    "-Wl,-rpath-link,${SYSROOT}/lib/aarch64-linux-gnu"
    "-Wl,-rpath-link,${SYSROOT}/usr/lib/aarch64-linux-gnu"
    "-Wl,-rpath-link,${SYSROOT}/usr/lib/aarch64-linux-gnu/blas"
    "-Wl,-rpath-link,${SYSROOT}/usr/lib/aarch64-linux-gnu/lapack"
    "-Wl,-rpath-link,${SYSROOT}/usr/lib")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--sysroot=${SYSROOT} ${_SYSROOT_RPATH_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "--sysroot=${SYSROOT} ${_SYSROOT_RPATH_LINK_FLAGS}")

# CMake search root를 sysroot로 고정
set(CMAKE_FIND_ROOT_PATH ${SYSROOT})

# host 경로 검색 차단
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Make CMake package and find_path/find_library queries deterministic.
set(CMAKE_PREFIX_PATH
    "${SYSROOT}/usr"
    "${SYSROOT}/usr/lib/aarch64-linux-gnu"
    "${SYSROOT}/usr/lib/aarch64-linux-gnu/cmake"
    CACHE STRING "Cross package prefix paths" FORCE
)
set(CMAKE_LIBRARY_PATH
    "${SYSROOT}/usr/lib/aarch64-linux-gnu"
    "${SYSROOT}/lib/aarch64-linux-gnu"
    CACHE STRING "Cross library search paths" FORCE
)
set(CMAKE_INCLUDE_PATH
    "${SYSROOT}/usr/include"
    "${SYSROOT}/usr/include/postgresql"
    CACHE STRING "Cross include search paths" FORCE
)

# --- pkg-config도 sysroot만 보게 강제 ---
set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${SYSROOT}/usr/lib/pkgconfig:${SYSROOT}/usr/share/pkgconfig")

# Seed PostgreSQL paths so find_package(PostgreSQL) resolves libpq from sysroot.
set(PostgreSQL_ROOT "${SYSROOT}/usr" CACHE PATH "PostgreSQL root in sysroot" FORCE)
set(PostgreSQL_INCLUDE_DIR "${SYSROOT}/usr/include/postgresql" CACHE PATH "PostgreSQL include dir" FORCE)
set(PostgreSQL_LIBRARY "${SYSROOT}/usr/lib/aarch64-linux-gnu/libpq.so" CACHE FILEPATH "PostgreSQL client library" FORCE)
