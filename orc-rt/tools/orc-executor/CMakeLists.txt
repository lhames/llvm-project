add_executable(orc-executor
  orc-executor.cpp
)

target_link_libraries(orc-executor PRIVATE
  ORC-RT
  )

