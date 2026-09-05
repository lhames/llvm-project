
set(ORC_RT_BEDROCK_SOURCES
  BootstrapInfo.cpp
  ExecutorProcessInfo.cpp
  GDBJITRegistrar.cpp
  InProcessControllerAccess.cpp
  NativeDylibManager.cpp
  Service.cpp
  Session.cpp
  SimpleNativeMemoryMap.cpp
  SimpleSymbolTable.cpp
  StandaloneMachOUnwindInfoRegistrar.cpp
  ThreadPoolRunner.cpp
  sps/AllSPSCI.cpp
  sps/CallSPSCI.cpp
  sps/GDBJITRegistrarSPSCI.cpp
  sps/MemoryAccessSPSCI.cpp
  sps/NativeDylibManagerSPSCI.cpp
  sps/SimpleNativeMemoryMapSPSCI.cpp
  sps/SimpleRemoteCA.cpp
  sps/StandaloneMachOUnwindInfoRegistrarSPSCI.cpp
  )

# Bedrock has no per-system source lists of its own: the system-portability
# layer lives in Support (see ../support/CMakeLists.txt). Bedrock's own
# system-touching code -- GDBJITRegistrar, StandaloneMachOUnwindInfoRegistrar --
# is ORC-specific rather than a portability shim, so it is organized by feature
# and guards internally.

add_library(orc-rt-bedrock STATIC
  ${ORC_RT_BEDROCK_SOURCES}
  $<TARGET_OBJECTS:orc-rt-support>
  )

target_link_libraries(orc-rt-bedrock PUBLIC orc-rt-headers)

# Apply RTTI and exceptions compile flags
target_compile_options(orc-rt-bedrock PRIVATE ${ORC_RT_COMPILE_FLAGS})
# TODO: Use common runtimes infrastructure for output and install paths
install(TARGETS orc-rt-bedrock
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          COMPONENT OrcRT_Development
  PUBLIC_HEADER DESTINATION include COMPONENT OrcRT_Development
)
