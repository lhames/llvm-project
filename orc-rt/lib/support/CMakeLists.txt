# Support: system-portability and general-purpose utilities that are not
# ORC-specific. Bedrock (and, in future, SPIRE) embed this library.

set(ORC_RT_SUPPORT_SOURCES
  Environment.cpp
  Error.cpp
  Logging.cpp
  RTTI.cpp
  )

# The printf logging backend needs a runtime implementation; the none backend
# is header-only and the os_log backend is not yet implemented.
if(ORC_RT_LOG_BACKEND STREQUAL "printf")
  list(APPEND ORC_RT_SUPPORT_SOURCES Logging_printf.cpp)
elseif(ORC_RT_LOG_BACKEND STREQUAL "os_log")
  list(APPEND ORC_RT_SUPPORT_SOURCES Logging_oslog.cpp)
endif()

# System implementations are composed, not merely selected: a target gets one
# source list per capability it has. POSIX systems share sys/posix/, and add
# their OS-specific list on top; Windows shares nothing with them.
#
# A file belongs in a shared list only if it is uniform across every member of
# it. Where a function needs an OS conditional, it moves down into the
# per-OS lists instead of growing an #ifdef here -- or, if it wants to inline,
# becomes a dispatch header under include/orc-rt-internal/ (see
# support/sys/CacheControl.h).

set(ORC_RT_SUPPORT_POSIX_SOURCES
  sys/posix/DynamicLibrary.cpp
  sys/posix/Errno.cpp
  sys/posix/Memory.cpp
  sys/posix/PageSize.cpp
)

set(ORC_RT_SUPPORT_DARWIN_SOURCES
  sys/darwin/CPUFeatures.cpp
  sys/darwin/TargetTriple.cpp
)

set(ORC_RT_SUPPORT_LINUX_SOURCES
  sys/linux/CPUFeatures.cpp
  sys/linux/TargetTriple.cpp
)

# TODO: Windows has no counterparts for the sys/posix/ sources yet
# (DynamicLibrary, Errno, Memory, PageSize), so Windows builds are incomplete.
set(ORC_RT_SUPPORT_WINDOWS_SOURCES
  sys/windows/CPUFeatures.cpp
  sys/windows/TargetTriple.cpp
)

if (APPLE)
  list(APPEND ORC_RT_SUPPORT_SOURCES
    ${ORC_RT_SUPPORT_POSIX_SOURCES} ${ORC_RT_SUPPORT_DARWIN_SOURCES})
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND ORC_RT_SUPPORT_SOURCES
    ${ORC_RT_SUPPORT_POSIX_SOURCES} ${ORC_RT_SUPPORT_LINUX_SOURCES})
elseif (WIN32)
  list(APPEND ORC_RT_SUPPORT_SOURCES ${ORC_RT_SUPPORT_WINDOWS_SOURCES})
else()
  message(WARNING
    "orc-rt: no system implementation for ${CMAKE_SYSTEM_NAME}; "
    "add an orc-rt/lib/support/sys/<system>/ directory and compose it above")
endif()

# Support is always an object library: it is embedded into the libraries that
# ship, never shipped on its own.
add_library(orc-rt-support OBJECT ${ORC_RT_SUPPORT_SOURCES})

target_link_libraries(orc-rt-support PUBLIC orc-rt-headers)

# Apply RTTI and exceptions compile flags
target_compile_options(orc-rt-support PRIVATE ${ORC_RT_COMPILE_FLAGS})
