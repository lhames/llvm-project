//===-- IRExecutionUnit.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DiagnosticHandler.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include "lldb/Core/Debugger.h"
#include "lldb/Core/Disassembler.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/Section.h"
#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Expression/Expression.h"
#include "lldb/Expression/IRExecutionUnit.h"
#include "lldb/Expression/IRInterpreter.h"
#include "lldb/Expression/ObjectFileJIT.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Target/ABI.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Language.h"
#include "lldb/Target/LanguageRuntime.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/ThreadPlan.h"
#include "lldb/Target/ThreadPlanCallUserExpression.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/ErrorMessages.h"
#include "lldb/Utility/LLDBAssert.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Policy.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/lldb-defines.h"

#include <optional>

using namespace lldb_private;

IRExecutionUnit::IRExecutionUnit(std::unique_ptr<llvm::LLVMContext> &context_up,
                                 std::unique_ptr<llvm::Module> &module_up,
                                 ConstString &name,
                                 const lldb::TargetSP &target_sp,
                                 ExpressionSymbolResolver symbol_resolver,
                                 std::vector<std::string> &cpu_features)
    : IRMemoryMap(target_sp), m_context_up(std::move(context_up)),
      m_module_up(std::move(module_up)), m_module(m_module_up.get()),
      m_cpu_features(cpu_features), m_name(name),
      m_symbol_resolver(std::move(symbol_resolver)) {}

IRExecutionUnit::~IRExecutionUnit() {
  m_module_up.reset();
  m_context_up.reset();
}

lldb::addr_t IRExecutionUnit::FindSymbol(lldb_private::ConstString name,
                                         bool &missing_weak) {
  return m_symbol_resolver.FindSymbol(name, missing_weak);
}
