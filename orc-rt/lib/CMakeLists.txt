set(ORC_COMMON_SOURCES
  debug.cpp
  dlfcn_wrapper.cpp
  extensible_rtti.cpp
  log_error_to_stderr.cpp
  orc-rt.cpp
  run_program_wrapper.cpp
  )

if (APPLE)
  set(ORC_SOURCES
    ${ORC_COMMON_SOURCES}
    macho_platform.cpp
    )
elseif (WIN32)
  set(ORC_SOURCES
    ${ORC_COMMON_SOURCES}
    coff_platform.cpp
    coff_platform.per_jd.cpp
    )
else()
  set(ORC_SOURCES
    ${ORC_COMMON_SOURCES}
    elfnix_platform.cpp
    )
endif()

add_library(ORC-RT
  ${ORC_SOURCES}
  )
