# Keep track if we have all dependencies.
set(ORC_RT_LLVM_TOOLS_AVAILABLE TRUE)

# Find executables.
if (TARGET utils/llvm-lit/all)
  list(APPEND utils/llvm-lit/all)
else()
  find_program(ORC_RT_LLVM_LIT_EXECUTABLE
    NAMES llvm-lit.py llvm-lit lit.py lit
    PATHS ${ORC_RT_LLVM_TOOLS_DIR})
  if (NOT ORC_RT_LLVM_LIT_EXECUTABLE)
    message(STATUS "Cannot find llvm-lit. Please put it in your PATH, set ORC_RT_LLVM_LIT_EXECUTABLE to its full path, or point ORC_RT_LLVM_TOOLS_DIR to its directory.")
    set(ORC_RT_LLVM_TOOLS_AVAILABLE FALSE)
  endif()
endif()

if (TARGET FileCheck)
  list(APPEND ORC_RT_TEST_DEPS FileCheck)
else()
  find_program(ORC_RT_FILECHECK_EXECUTABLE
    NAMES FileCheck
    PATHS ${ORC_RT_LLVM_TOOLS_DIR})
  if (NOT ORC_RT_FILECHECK_EXECUTABLE)
    message(STATUS "Cannot find FileCheck. Please put it in your PATH, set ORC_RT_FILECHECK_EXECUTABLE to its full path, or point ORC_RT_LLVM_TOOLS_DIR to its directory.")
    set(ORC_RT_LLVM_TOOLS_AVAILABLE FALSE)
  endif()
endif()

if (TARGET not)
  list(APPEND ORC_RT_TEST_DEPS not)
else()
  find_program(ORC_RT_NOT_EXECUTABLE
    NAMES not
    PATHS ${ORC_RT_LLVM_TOOLS_DIR})
  if (NOT ORC_RT_NOT_EXECUTABLE)
    message(STATUS "Cannot find 'not'. Please put it in your PATH, set ORC_RT_NOT_EXECUTABLE to its full path, or point ORC_RT_LLVM_TOOLS_DIR to its directory.")
    set(ORC_RT_LLVM_TOOLS_AVAILABLE FALSE)
  endif()
endif()
