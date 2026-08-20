//===-- JITExecutionUnit.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Expression/JITExecutionUnit.h"

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
#include "lldb/Expression/ObjectFileJIT.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Target/Language.h"
#include "lldb/Target/LanguageRuntime.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/LLDBAssert.h"

#include <optional>

#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/ThreadPlan.h"
#include "lldb/Target/ThreadPlanCallUserExpression.h"
#include "lldb/Utility/ErrorMessages.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Policy.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/lldb-defines.h"

using namespace lldb_private;

JITExecutionUnit::~JITExecutionUnit() { m_execution_engine_up.reset(); }

lldb::ExpressionResults JITExecutionUnit::Run(
    llvm::ArrayRef<lldb::addr_t> args, ExecutionContext &exe_ctx,
    const EvaluateExpressionOptions &options,
    DiagnosticManager &diagnostic_manager,
    lldb::UserExpressionSP &expression_sp, lldb::addr_t &function_stack_bottom,
    lldb::addr_t &function_stack_top) {
  // The expression log is quite verbose, and if you're just tracking the
  // execution of the expression, it's quite convenient to have these logs come
  // out with the STEP log as well.
  Log *log(GetLog(LLDBLog::Expressions | LLDBLog::Step));

  if (!exe_ctx.HasThreadScope()) {
    diagnostic_manager.Printf(lldb::eSeverityError,
                              "%s called with no thread selected",
                              __FUNCTION__);
    return lldb::eExpressionSetupError;
  }

  // Store away the thread ID for error reporting, in case it exits
  // during execution:
  lldb::tid_t expr_thread_id = exe_ctx.GetThreadRef().GetID();

  Address wrapper_address(m_function_load_addr);

  lldb::ThreadPlanSP call_plan_sp(new ThreadPlanCallUserExpression(
      exe_ctx.GetThreadRef(), wrapper_address, args, options, expression_sp));

  StreamString ss;
  if (!call_plan_sp || !call_plan_sp->ValidatePlan(&ss)) {
    diagnostic_manager.PutString(lldb::eSeverityError, ss.GetString());
    return lldb::eExpressionSetupError;
  }

  ThreadPlanCallUserExpression *user_expression_plan =
      static_cast<ThreadPlanCallUserExpression *>(call_plan_sp.get());

  lldb::addr_t function_stack_pointer =
      user_expression_plan->GetFunctionStackPointer();

  function_stack_bottom = function_stack_pointer - HostInfo::GetPageSize();
  function_stack_top = function_stack_pointer;

  LLDB_LOGF(log,
            "-- [UserExpression::Execute] Execution of expression begins --");

  if (exe_ctx.GetProcessPtr())
    exe_ctx.GetProcessPtr()->SetRunningUserExpression(true);

  PolicyStack::Guard expr_policy_guard =
      PolicyStack::Get().PushPublicStateRunningExpression();

  lldb::ExpressionResults execution_result =
      exe_ctx.GetProcessRef().RunThreadPlan(exe_ctx, call_plan_sp, options,
                                            diagnostic_manager);

  if (exe_ctx.GetProcessPtr())
    exe_ctx.GetProcessPtr()->SetRunningUserExpression(false);

  LLDB_LOGF(log, "-- [UserExpression::Execute] Execution of expression "
                 "completed --");

  if (execution_result == lldb::eExpressionInterrupted ||
      execution_result == lldb::eExpressionHitBreakpoint) {
    const char *error_desc = nullptr;
    const char *explanation = execution_result == lldb::eExpressionInterrupted
                                  ? "was interrupted"
                                  : "hit a breakpoint";

    if (user_expression_plan) {
      if (auto real_stop_info_sp = user_expression_plan->GetRealStopInfo())
        error_desc = real_stop_info_sp->GetDescription();
    }

    if (error_desc)
      diagnostic_manager.Printf(lldb::eSeverityError,
                                "Expression execution %s: %s.", explanation,
                                error_desc);
    else
      diagnostic_manager.Printf(lldb::eSeverityError,
                                "Expression execution %s.", explanation);

    if ((execution_result == lldb::eExpressionInterrupted &&
         options.DoesUnwindOnError()) ||
        (execution_result == lldb::eExpressionHitBreakpoint &&
         options.DoesIgnoreBreakpoints()))
      diagnostic_manager.AppendMessageToDiagnostic(
          "The process has been returned to the state before expression "
          "evaluation.");
    else {
      if (execution_result == lldb::eExpressionHitBreakpoint)
        user_expression_plan->TransferExpressionOwnership();
      diagnostic_manager.AppendMessageToDiagnostic(
          "The process has been left at the point where it was "
          "interrupted, use \"thread return -x\" to return to the state "
          "before expression evaluation.");
    }

    return execution_result;
  }

  if (execution_result == lldb::eExpressionStoppedForDebug) {
    diagnostic_manager.PutString(
        lldb::eSeverityInfo,
        "Expression execution was halted at the first instruction of the "
        "expression function because \"debug\" was requested.\n"
        "Use \"thread return -x\" to return to the state before expression "
        "evaluation.");
    return execution_result;
  }

  if (execution_result == lldb::eExpressionThreadVanished) {
    diagnostic_manager.Printf(lldb::eSeverityError,
                              "Couldn't execute expression: the thread on "
                              "which the expression was being run (0x%" PRIx64
                              ") exited during its execution.",
                              expr_thread_id);
    return execution_result;
  }

  if (execution_result != lldb::eExpressionCompleted) {
    diagnostic_manager.Printf(lldb::eSeverityError,
                              "Couldn't execute expression: result was %s",
                              toString(execution_result).c_str());
    return execution_result;
  }

  return lldb::eExpressionCompleted;
}

Status JITExecutionUnit::DisassembleFunction(Stream &stream,
                                             lldb::ProcessSP &process_wp) {
  Log *log = GetLog(LLDBLog::Expressions);

  ExecutionContext exe_ctx(process_wp);

  Status ret;

  ret.Clear();

  lldb::addr_t func_local_addr = LLDB_INVALID_ADDRESS;
  lldb::addr_t func_remote_addr = LLDB_INVALID_ADDRESS;

  for (JittedFunction &function : m_jitted_functions) {
    if (function.m_name == m_name) {
      func_local_addr = function.m_local_addr;
      func_remote_addr = function.m_remote_addr;
    }
  }

  if (func_local_addr == LLDB_INVALID_ADDRESS) {
    ret = Status::FromErrorStringWithFormatv(
        "Couldn't find function {0} for disassembly", m_name);
    return ret;
  }

  LLDB_LOGF(log,
            "Found function, has local address 0x%" PRIx64
            " and remote address 0x%" PRIx64,
            (uint64_t)func_local_addr, (uint64_t)func_remote_addr);

  std::pair<lldb::addr_t, lldb::addr_t> func_range;

  func_range = GetRemoteRangeForLocal(func_local_addr);

  if (func_range.first == 0 && func_range.second == 0) {
    ret = Status::FromErrorStringWithFormatv(
        "Couldn't find code range for function {0}", m_name);
    return ret;
  }

  LLDB_LOGF(log, "Function's code range is [0x%" PRIx64 "+0x%" PRIx64 "]",
            func_range.first, func_range.second);

  Target *target = exe_ctx.GetTargetPtr();
  if (!target) {
    ret = Status::FromErrorString("Couldn't find the target");
    return ret;
  }

  lldb::WritableDataBufferSP buffer_sp(
      new DataBufferHeap(func_range.second, 0));

  Process *process = exe_ctx.GetProcessPtr();
  Status err;
  process->ReadMemory(func_remote_addr, buffer_sp->GetBytes(),
                      buffer_sp->GetByteSize(), err);

  if (!err.Success()) {
    ret = Status::FromErrorStringWithFormat("Couldn't read from process: %s",
                                            err.AsCString("unknown error"));
    return ret;
  }

  ArchSpec arch(target->GetArchitecture());

  const char *plugin_name = nullptr;
  const char *flavor_string = nullptr;
  const char *cpu_string = nullptr;
  const char *features_string = nullptr;
  lldb::DisassemblerSP disassembler_sp = Disassembler::FindPlugin(
      arch, flavor_string, cpu_string, features_string, plugin_name);

  if (!disassembler_sp) {
    ret = Status::FromErrorStringWithFormat(
        "Unable to find disassembler plug-in for %s architecture.",
        arch.GetArchitectureName());
    return ret;
  }

  if (!process) {
    ret = Status::FromErrorString("Couldn't find the process");
    return ret;
  }

  DataExtractor extractor(buffer_sp, process->GetByteOrder(),
                          target->GetArchitecture().GetAddressByteSize());

  if (log) {
    LLDB_LOGF(log, "Function data has contents:");
    extractor.PutToLog(log, 0, extractor.GetByteSize(), func_remote_addr, 16,
                       DataExtractor::TypeUInt8);
  }

  disassembler_sp->DecodeInstructions(Address(func_remote_addr), extractor, 0,
                                      UINT32_MAX, false, false);

  InstructionList &instruction_list = disassembler_sp->GetInstructionList();
  instruction_list.Dump(&stream, true, true, /*show_control_flow_kind=*/false,
                        &exe_ctx);

  return ret;
}

namespace {
struct IRExecDiagnosticHandler : public llvm::DiagnosticHandler {
  Status *err;
  IRExecDiagnosticHandler(Status *err) : err(err) {}
  bool handleDiagnostics(const llvm::DiagnosticInfo &DI) override {
    if (DI.getSeverity() == llvm::DS_Error) {
      const auto &DISM = llvm::cast<llvm::DiagnosticInfoSrcMgr>(DI);
      if (err && err->Success()) {
        *err = Status::FromErrorStringWithFormat(
            "IRExecution error: %s",
            DISM.getSMDiag().getMessage().str().c_str());
      }
    }

    return true;
  }
};
} // namespace

void JITExecutionUnit::ReportSymbolLookupError(ConstString name) {
  m_failed_lookups.push_back(name);
}

llvm::Expected<std::shared_ptr<JITExecutionUnit>>
JITExecutionUnit::Create(std::unique_ptr<llvm::LLVMContext> &context_up,
                         std::unique_ptr<llvm::Module> &module_up,
                         ConstString &name, const lldb::TargetSP &target_sp,
                         ExpressionSymbolResolver symbol_resolver,
                         std::vector<std::string> &cpu_features) {
  auto unit_sp = std::make_shared<JITExecutionUnit>(
      context_up, module_up, name, target_sp, std::move(symbol_resolver),
      cpu_features);

  Status error;
  unit_sp->JITCompile(error);
  if (error.Fail())
    return error.ToError();

  return unit_sp;
}

void JITExecutionUnit::JITCompile(Status &error) {
  lldb::ProcessSP process_sp(GetProcessWP().lock());

  if (!process_sp) {
    error =
        Status::FromErrorString("Couldn't write the JIT compiled code into the "
                                "process because the process is invalid");
    return;
  }

  Log *log = GetLog(LLDBLog::Expressions);

  std::string error_string;

  if (log) {
    std::string s;
    llvm::raw_string_ostream oss(s);

    m_module->print(oss, nullptr);

    LLDB_LOGF(log, "Module being sent to JIT: \n%s", s.c_str());
  }

  m_module_up->getContext().setDiagnosticHandler(
      std::make_unique<IRExecDiagnosticHandler>(&error));

  llvm::EngineBuilder builder(std::move(m_module_up));
  llvm::Triple triple(m_module->getTargetTriple());

  builder.setEngineKind(llvm::EngineKind::JIT)
      .setErrorStr(&error_string)
      .setRelocationModel(triple.isOSBinFormatMachO() ? llvm::Reloc::PIC_
                                                      : llvm::Reloc::Static)
      .setMCJITMemoryManager(std::make_unique<MemoryManager>(*this))
      .setOptLevel(llvm::CodeGenOptLevel::Less);

  // Resulted jitted code can be placed too far from the code in the binary
  // and thus can contain more than +-2GB jumps, that are not available
  // in RISC-V without large code model.
  if (triple.isRISCV64())
    builder.setCodeModel(llvm::CodeModel::Large);

  llvm::StringRef mArch;
  llvm::StringRef mCPU;
  llvm::SmallVector<std::string, 0> mAttrs;

  for (std::string &feature : m_cpu_features)
    mAttrs.push_back(feature);

  llvm::TargetMachine *target_machine =
      builder.selectTarget(triple, mArch, mCPU, mAttrs);

  m_execution_engine_up.reset(builder.create(target_machine));

  if (!m_execution_engine_up) {
    error = Status::FromErrorStringWithFormat("Couldn't JIT the function: %s",
                                              error_string.c_str());
    return;
  }

  class ObjectDumper : public llvm::ObjectCache {
  public:
    ObjectDumper(FileSpec output_dir) : m_out_dir(output_dir) {}
    void notifyObjectCompiled(const llvm::Module *module,
                              llvm::MemoryBufferRef object) override {
      int fd = 0;
      llvm::SmallVector<char, 256> result_path;
      std::string object_name_model =
          "jit-object-" + module->getModuleIdentifier() + "-%%%.o";
      FileSpec model_spec =
          m_out_dir.CopyByAppendingPathComponent(object_name_model);
      std::string model_path = model_spec.GetPath();

      std::error_code result =
          llvm::sys::fs::createUniqueFile(model_path, fd, result_path);
      if (!result) {
        llvm::raw_fd_ostream fds(fd, true);
        fds.write(object.getBufferStart(), object.getBufferSize());
      }
    }
    std::unique_ptr<llvm::MemoryBuffer>
    getObject(const llvm::Module *module) override {
      // Return nothing - we're just abusing the object-cache mechanism to dump
      // objects.
      return nullptr;
    }

  private:
    FileSpec m_out_dir;
  };

  FileSpec save_objects_dir = process_sp->GetTarget().GetSaveJITObjectsDir();
  if (save_objects_dir) {
    m_object_cache_up = std::make_unique<ObjectDumper>(save_objects_dir);
    m_execution_engine_up->setObjectCache(m_object_cache_up.get());
  }

  // Make sure we see all sections, including ones that don't have
  // relocations...
  m_execution_engine_up->setProcessAllSections(true);

  m_execution_engine_up->DisableLazyCompilation();

  for (llvm::Function &function : *m_module) {
    if (function.isDeclaration() || function.hasPrivateLinkage())
      continue;

    const bool external = !function.hasLocalLinkage();

    void *fun_ptr = m_execution_engine_up->getPointerToFunction(&function);

    if (!error.Success()) {
      // We got an error through our callback!
      return;
    }

    if (!fun_ptr) {
      error = Status::FromErrorStringWithFormat(
          "'%s' was in the JITted module but wasn't lowered",
          function.getName().str().c_str());
      return;
    }
    m_jitted_functions.push_back(
        JittedFunction(function.getName().str().c_str(), external,
                       reinterpret_cast<uintptr_t>(fun_ptr)));
  }

  CommitAllocations(process_sp);
  ReportAllocations(*m_execution_engine_up);

  // We have to do this after calling ReportAllocations because for the MCJIT,
  // getGlobalValueAddress will cause the JIT to perform all relocations.  That
  // can only be done once, and has to happen after we do the remapping from
  // local -> remote. That means we don't know the local address of the
  // Variables, but we don't need that for anything, so that's okay.

  std::function<void(llvm::GlobalValue &)> RegisterOneValue =
      [this](llvm::GlobalValue &val) {
        if (val.hasExternalLinkage() && !val.isDeclaration()) {
          uint64_t var_ptr_addr =
              m_execution_engine_up->getGlobalValueAddress(val.getName().str());

          lldb::addr_t remote_addr = GetRemoteAddressForLocal(var_ptr_addr);

          // This is a really unfortunae API that sometimes returns local
          // addresses and sometimes returns remote addresses, based on whether
          // the variable was relocated during ReportAllocations or not.

          if (remote_addr == LLDB_INVALID_ADDRESS) {
            remote_addr = var_ptr_addr;
          }

          if (var_ptr_addr != 0)
            m_jitted_global_variables.push_back(
                JittedGlobalVariable(val.getName().str().c_str(),
                                     LLDB_INVALID_ADDRESS, remote_addr));
        }
      };

  for (llvm::GlobalVariable &global_var : m_module->globals()) {
    RegisterOneValue(global_var);
  }

  for (llvm::GlobalAlias &global_alias : m_module->aliases()) {
    RegisterOneValue(global_alias);
  }

  WriteData(process_sp);

  if (m_failed_lookups.size()) {
    StreamString ss;

    ss.PutCString("Couldn't look up symbols:\n");

    bool emitNewLine = false;

    for (ConstString failed_lookup : m_failed_lookups) {
      if (emitNewLine)
        ss.PutCString("\n");
      emitNewLine = true;
      ss.PutCString("  ");
      ss.PutCString(Mangled(failed_lookup).GetDemangledName().GetStringRef());
    }

    m_failed_lookups.clear();
    ss.PutCString(
        "\nHint: The expression tried to call a function that is not present "
        "in the target, perhaps because it was optimized out by the compiler.");
    error = Status(ss.GetString().str());

    return;
  }

  m_function_load_addr = LLDB_INVALID_ADDRESS;
  m_function_end_load_addr = LLDB_INVALID_ADDRESS;

  for (JittedFunction &jitted_function : m_jitted_functions) {
    jitted_function.m_remote_addr =
        GetRemoteAddressForLocal(jitted_function.m_local_addr);

    if (!m_name.IsEmpty() && jitted_function.m_name == m_name) {
      AddrRange func_range =
          GetRemoteRangeForLocal(jitted_function.m_local_addr);
      m_function_end_load_addr = func_range.first + func_range.second;
      m_function_load_addr = jitted_function.m_remote_addr;
    }
  }

  if (log) {
    LLDB_LOGF(log, "Code can be run in the target.");

    StreamString disassembly_stream;

    Status err = DisassembleFunction(disassembly_stream, process_sp);

    if (!err.Success()) {
      LLDB_LOGF(log, "Couldn't disassemble function : %s",
                err.AsCString("unknown error"));
    } else {
      LLDB_LOGF(log, "Function disassembly:\n%s", disassembly_stream.GetData());
    }

    LLDB_LOGF(log, "Sections: ");
    for (AllocationRecord &record : m_records) {
      if (record.m_process_address != LLDB_INVALID_ADDRESS) {
        record.dump(log);

        DataBufferHeap my_buffer(record.m_size, 0);
        Status err;
        ReadMemory(my_buffer.GetBytes(), record.m_process_address,
                   record.m_size, err);

        if (err.Success()) {
          DataExtractor my_extractor(my_buffer.GetBytes(),
                                     my_buffer.GetByteSize(),
                                     lldb::eByteOrderBig, 8);
          my_extractor.PutToLog(log, 0, my_buffer.GetByteSize(),
                                record.m_process_address, 16,
                                DataExtractor::TypeUInt8);
        }
      } else {
        record.dump(log);

        DataExtractor my_extractor((const void *)record.m_host_address,
                                   record.m_size, lldb::eByteOrderBig, 8);
        my_extractor.PutToLog(log, 0, record.m_size, record.m_host_address, 16,
                              DataExtractor::TypeUInt8);
      }
    }
  }
}

JITExecutionUnit::MemoryManager::MemoryManager(JITExecutionUnit &parent)
    : m_default_mm_up(new llvm::SectionMemoryManager()), m_parent(parent) {}

JITExecutionUnit::MemoryManager::~MemoryManager() = default;

lldb::SectionType JITExecutionUnit::GetSectionTypeFromSectionName(
    const llvm::StringRef &name, JITExecutionUnit::AllocationKind alloc_kind) {
  lldb::SectionType sect_type = lldb::eSectionTypeCode;
  switch (alloc_kind) {
  case AllocationKind::Stub:
    sect_type = lldb::eSectionTypeCode;
    break;
  case AllocationKind::Code:
    sect_type = lldb::eSectionTypeCode;
    break;
  case AllocationKind::Data:
    sect_type = lldb::eSectionTypeData;
    break;
  case AllocationKind::Global:
    sect_type = lldb::eSectionTypeData;
    break;
  case AllocationKind::Bytes:
    sect_type = lldb::eSectionTypeOther;
    break;
  }

  if (!name.empty()) {
    if (name == "__text" || name == ".text")
      sect_type = lldb::eSectionTypeCode;
    else if (name == "__data" || name == ".data")
      sect_type = lldb::eSectionTypeCode;
    else if (name.starts_with("__debug_") || name.starts_with(".debug_")) {
      const uint32_t name_idx = name[0] == '_' ? 8 : 7;
      llvm::StringRef dwarf_name(name.substr(name_idx));
      sect_type = ObjectFile::GetDWARFSectionTypeFromName(dwarf_name);
    } else if (name.starts_with("__apple_") || name.starts_with(".apple_"))
      sect_type = lldb::eSectionTypeInvalid;
    else if (name == "__objc_imageinfo")
      sect_type = lldb::eSectionTypeOther;
  }
  return sect_type;
}

uint8_t *JITExecutionUnit::MemoryManager::allocateCodeSection(
    uintptr_t Size, unsigned Alignment, unsigned SectionID,
    llvm::StringRef SectionName) {
  Log *log = GetLog(LLDBLog::Expressions);

  uint8_t *return_value = m_default_mm_up->allocateCodeSection(
      Size, Alignment, SectionID, SectionName);

  m_parent.m_records.push_back(AllocationRecord(
      (uintptr_t)return_value,
      lldb::ePermissionsReadable | lldb::ePermissionsExecutable,
      GetSectionTypeFromSectionName(SectionName, AllocationKind::Code), Size,
      Alignment, SectionID, SectionName.str().c_str()));

  LLDB_LOGF(log,
            "JITExecutionUnit::allocateCodeSection(Size=0x%" PRIx64
            ", Alignment=%u, SectionID=%u) = %p",
            (uint64_t)Size, Alignment, SectionID, (void *)return_value);

  if (m_parent.m_reported_allocations) {
    Status err;
    lldb::ProcessSP process_sp =
        m_parent.GetBestExecutionContextScope()->CalculateProcess();

    m_parent.CommitOneAllocation(process_sp, err, m_parent.m_records.back());
  }

  return return_value;
}

uint8_t *JITExecutionUnit::MemoryManager::allocateDataSection(
    uintptr_t Size, unsigned Alignment, unsigned SectionID,
    llvm::StringRef SectionName, bool IsReadOnly) {
  Log *log = GetLog(LLDBLog::Expressions);

  uint8_t *return_value = m_default_mm_up->allocateDataSection(
      Size, Alignment, SectionID, SectionName, IsReadOnly);

  uint32_t permissions = lldb::ePermissionsReadable;
  if (!IsReadOnly)
    permissions |= lldb::ePermissionsWritable;
  m_parent.m_records.push_back(AllocationRecord(
      (uintptr_t)return_value, permissions,
      GetSectionTypeFromSectionName(SectionName, AllocationKind::Data), Size,
      Alignment, SectionID, SectionName.str().c_str()));
  LLDB_LOGF(log,
            "JITExecutionUnit::allocateDataSection(Size=0x%" PRIx64
            ", Alignment=%u, SectionID=%u) = %p",
            (uint64_t)Size, Alignment, SectionID, (void *)return_value);

  if (m_parent.m_reported_allocations) {
    Status err;
    lldb::ProcessSP process_sp =
        m_parent.GetBestExecutionContextScope()->CalculateProcess();

    m_parent.CommitOneAllocation(process_sp, err, m_parent.m_records.back());
  }

  return return_value;
}

void JITExecutionUnit::GetExportedSymbols(
    llvm::function_ref<void(ConstString, lldb::addr_t)> callback) {
  Log *log = GetLog(LLDBLog::Expressions);

  LLDB_LOGF(log, "Registering JITted Functions:\n");

  for (const JittedFunction &jitted_function : m_jitted_functions) {
    if (jitted_function.m_external && jitted_function.m_name != m_name &&
        jitted_function.m_remote_addr != LLDB_INVALID_ADDRESS) {
      callback(jitted_function.m_name, jitted_function.m_remote_addr);
      LLDB_LOGF(log, "  Function: %s at 0x%" PRIx64 ".",
                jitted_function.m_name.GetCString(),
                jitted_function.m_remote_addr);
    }
  }

  LLDB_LOGF(log, "Registering JIIted Symbols:\n");

  for (const JittedGlobalVariable &global_var : m_jitted_global_variables) {
    if (global_var.m_remote_addr != LLDB_INVALID_ADDRESS) {
      // Demangle the name before inserting it, so that lookups by the ConstStr
      // of the demangled name will find the mangled one (needed for looking up
      // metadata pointers.)
      Mangled mangler(global_var.m_name);
      mangler.GetDemangledName();
      callback(global_var.m_name, global_var.m_remote_addr);
      LLDB_LOGF(log, "  Symbol: %s at 0x%" PRIx64 ".",
                global_var.m_name.GetCString(), global_var.m_remote_addr);
    }
  }
}

void JITExecutionUnit::GetStaticInitializers(
    std::vector<lldb::addr_t> &static_initializers) {
  Log *log = GetLog(LLDBLog::Expressions);

  llvm::GlobalVariable *global_ctors =
      m_module->getNamedGlobal("llvm.global_ctors");
  if (!global_ctors) {
    LLDB_LOG(log, "Couldn't find llvm.global_ctors.");
    return;
  }
  auto *ctor_array =
      llvm::dyn_cast<llvm::ConstantArray>(global_ctors->getInitializer());
  if (!ctor_array) {
    LLDB_LOG(log, "llvm.global_ctors not a ConstantArray.");
    return;
  }

  for (llvm::Use &ctor_use : ctor_array->operands()) {
    auto *ctor_struct = llvm::dyn_cast<llvm::ConstantStruct>(ctor_use);
    if (!ctor_struct)
      continue;
    // this is standardized
    lldbassert(ctor_struct->getNumOperands() == 3);
    auto *ctor_function =
        llvm::dyn_cast<llvm::Function>(ctor_struct->getOperand(1));
    if (!ctor_function) {
      LLDB_LOG(log, "global_ctor doesn't contain an llvm::Function");
      continue;
    }

    ConstString ctor_function_name(ctor_function->getName().str());
    LLDB_LOG(log, "Looking for callable jitted function with name {0}.",
             ctor_function_name);

    for (JittedFunction &jitted_function : m_jitted_functions) {
      if (ctor_function_name != jitted_function.m_name)
        continue;
      if (jitted_function.m_remote_addr == LLDB_INVALID_ADDRESS) {
        LLDB_LOG(log, "Found jitted function with invalid address.");
        continue;
      }
      static_initializers.push_back(jitted_function.m_remote_addr);
      LLDB_LOG(log, "Calling function at address {0:x}.",
               jitted_function.m_remote_addr);
      break;
    }
  }
}

llvm::JITSymbol
JITExecutionUnit::MemoryManager::findSymbol(const std::string &Name) {
  bool missing_weak = false;
  uint64_t addr = GetSymbolAddressAndPresence(Name, missing_weak);
  // This is a weak symbol:
  if (missing_weak)
    return llvm::JITSymbol(addr, llvm::JITSymbolFlags::Exported |
                                     llvm::JITSymbolFlags::Weak);
  else
    return llvm::JITSymbol(addr, llvm::JITSymbolFlags::Exported);
}

uint64_t
JITExecutionUnit::MemoryManager::getSymbolAddress(const std::string &Name) {
  bool missing_weak = false;
  return GetSymbolAddressAndPresence(Name, missing_weak);
}

uint64_t JITExecutionUnit::MemoryManager::GetSymbolAddressAndPresence(
    const std::string &Name, bool &missing_weak) {
  Log *log = GetLog(LLDBLog::Expressions);

  ConstString name_cs(Name);

  lldb::addr_t ret =
      m_parent.GetExternalSymbolResolver().FindSymbol(name_cs, missing_weak);

  if (ret == LLDB_INVALID_ADDRESS) {
    LLDB_LOGF(log,
              "JITExecutionUnit::getSymbolAddress(Name=\"%s\") = <not found>",
              Name.c_str());

    m_parent.ReportSymbolLookupError(name_cs);
    return 0;
  } else {
    LLDB_LOGF(log, "JITExecutionUnit::getSymbolAddress(Name=\"%s\") = %" PRIx64,
              Name.c_str(), ret);
    return ret;
  }
}

void *JITExecutionUnit::MemoryManager::getPointerToNamedFunction(
    const std::string &Name, bool AbortOnFailure) {
  return (void *)getSymbolAddress(Name);
}

lldb::addr_t
JITExecutionUnit::GetRemoteAddressForLocal(lldb::addr_t local_address) {
  Log *log = GetLog(LLDBLog::Expressions);

  for (AllocationRecord &record : m_records) {
    if (local_address >= record.m_host_address &&
        local_address < record.m_host_address + record.m_size) {
      if (record.m_process_address == LLDB_INVALID_ADDRESS)
        return LLDB_INVALID_ADDRESS;

      lldb::addr_t ret =
          record.m_process_address + (local_address - record.m_host_address);

      LLDB_LOGF(log,
                "JITExecutionUnit::GetRemoteAddressForLocal() found 0x%" PRIx64
                " in [0x%" PRIx64 "..0x%" PRIx64 "], and returned 0x%" PRIx64
                " from [0x%" PRIx64 "..0x%" PRIx64 "].",
                local_address, (uint64_t)record.m_host_address,
                (uint64_t)record.m_host_address + (uint64_t)record.m_size, ret,
                record.m_process_address,
                record.m_process_address + record.m_size);

      return ret;
    }
  }

  return LLDB_INVALID_ADDRESS;
}

JITExecutionUnit::AddrRange
JITExecutionUnit::GetRemoteRangeForLocal(lldb::addr_t local_address) {
  for (AllocationRecord &record : m_records) {
    if (local_address >= record.m_host_address &&
        local_address < record.m_host_address + record.m_size) {
      if (record.m_process_address == LLDB_INVALID_ADDRESS)
        return AddrRange(0, 0);

      return AddrRange(record.m_process_address, record.m_size);
    }
  }

  return AddrRange(0, 0);
}

bool JITExecutionUnit::CommitOneAllocation(lldb::ProcessSP &process_sp,
                                           Status &error,
                                           AllocationRecord &record) {
  if (record.m_process_address != LLDB_INVALID_ADDRESS) {
    return true;
  }

  switch (record.m_sect_type) {
  case lldb::eSectionTypeInvalid:
  case lldb::eSectionTypeDWARFDebugAbbrev:
  case lldb::eSectionTypeDWARFDebugAddr:
  case lldb::eSectionTypeDWARFDebugAranges:
  case lldb::eSectionTypeDWARFDebugCuIndex:
  case lldb::eSectionTypeDWARFDebugFrame:
  case lldb::eSectionTypeDWARFDebugInfo:
  case lldb::eSectionTypeDWARFDebugLine:
  case lldb::eSectionTypeDWARFDebugLoc:
  case lldb::eSectionTypeDWARFDebugLocLists:
  case lldb::eSectionTypeDWARFDebugMacInfo:
  case lldb::eSectionTypeDWARFDebugPubNames:
  case lldb::eSectionTypeDWARFDebugPubTypes:
  case lldb::eSectionTypeDWARFDebugRanges:
  case lldb::eSectionTypeDWARFDebugStr:
  case lldb::eSectionTypeDWARFDebugStrOffsets:
  case lldb::eSectionTypeDWARFAppleNames:
  case lldb::eSectionTypeDWARFAppleTypes:
  case lldb::eSectionTypeDWARFAppleNamespaces:
  case lldb::eSectionTypeDWARFAppleObjC:
  case lldb::eSectionTypeDWARFGNUDebugAltLink:
    error.Clear();
    break;
  default:
    const bool zero_memory = false;
    if (auto address_or_error =
            Malloc(record.m_size, record.m_alignment, record.m_permissions,
                   eAllocationPolicyProcessOnly, zero_memory))
      record.m_process_address = *address_or_error;
    else
      error = Status::FromError(address_or_error.takeError());
    break;
  }

  return error.Success();
}

bool JITExecutionUnit::CommitAllocations(lldb::ProcessSP &process_sp) {
  bool ret = true;

  lldb_private::Status err;

  for (AllocationRecord &record : m_records) {
    ret = CommitOneAllocation(process_sp, err, record);

    if (!ret) {
      break;
    }
  }

  if (!ret) {
    for (AllocationRecord &record : m_records) {
      if (record.m_process_address != LLDB_INVALID_ADDRESS) {
        Free(record.m_process_address, err);
        record.m_process_address = LLDB_INVALID_ADDRESS;
      }
    }
  }

  return ret;
}

void JITExecutionUnit::ReportAllocations(llvm::ExecutionEngine &engine) {
  m_reported_allocations = true;

  for (AllocationRecord &record : m_records) {
    if (record.m_process_address == LLDB_INVALID_ADDRESS)
      continue;

    if (record.m_section_id == eSectionIDInvalid)
      continue;

    engine.mapSectionAddress((void *)record.m_host_address,
                             record.m_process_address);
  }

  // Trigger re-application of relocations.
  engine.finalizeObject();
}

bool JITExecutionUnit::WriteData(lldb::ProcessSP &process_sp) {
  bool wrote_something = false;
  for (AllocationRecord &record : m_records) {
    if (record.m_process_address != LLDB_INVALID_ADDRESS) {
      lldb_private::Status err;
      WriteMemory(record.m_process_address, (uint8_t *)record.m_host_address,
                  record.m_size, err);
      if (err.Success())
        wrote_something = true;
    }
  }
  return wrote_something;
}

void JITExecutionUnit::AllocationRecord::dump(Log *log) {
  if (!log)
    return;

  LLDB_LOGF(log,
            "[0x%llx+0x%llx]->0x%llx (alignment %d, section ID %d, name %s)",
            (unsigned long long)m_host_address, (unsigned long long)m_size,
            (unsigned long long)m_process_address, (unsigned)m_alignment,
            (unsigned)m_section_id, m_name.c_str());
}

lldb::ByteOrder JITExecutionUnit::GetByteOrder() const {
  ExecutionContext exe_ctx(GetBestExecutionContextScope());
  return exe_ctx.GetByteOrder();
}

uint32_t JITExecutionUnit::GetAddressByteSize() const {
  ExecutionContext exe_ctx(GetBestExecutionContextScope());
  return exe_ctx.GetAddressByteSize();
}

void JITExecutionUnit::PopulateSymtab(lldb_private::ObjectFile *obj_file,
                                      lldb_private::Symtab &symtab) {
  // No symbols yet...
}

void JITExecutionUnit::PopulateSectionList(
    lldb_private::ObjectFile *obj_file,
    lldb_private::SectionList &section_list) {
  for (AllocationRecord &record : m_records) {
    if (record.m_size > 0) {
      lldb::SectionSP section_sp(new lldb_private::Section(
          obj_file->GetModule(), obj_file, record.m_section_id,
          ConstString(record.m_name), record.m_sect_type,
          record.m_process_address, record.m_size,
          record.m_host_address, // file_offset (which is the host address for
                                 // the data)
          record.m_size,         // file_size
          0,
          record.m_permissions)); // flags
      section_list.AddSection(section_sp);
    }
  }
}

ArchSpec JITExecutionUnit::GetArchitecture() {
  ExecutionContext exe_ctx(GetBestExecutionContextScope());
  if (Target *target = exe_ctx.GetTargetPtr())
    return target->GetArchitecture();
  return ArchSpec();
}

lldb::ModuleSP JITExecutionUnit::GetJITModule() {
  ExecutionContext exe_ctx(GetBestExecutionContextScope());
  Target *target = exe_ctx.GetTargetPtr();
  if (!target)
    return nullptr;

  auto Delegate = std::static_pointer_cast<lldb_private::ObjectFileJITDelegate>(
      std::static_pointer_cast<JITExecutionUnit>(shared_from_this()));

  lldb::ModuleSP jit_module_sp =
      lldb_private::Module::CreateModuleFromObjectFile<ObjectFileJIT>(Delegate);
  if (!jit_module_sp)
    return nullptr;

  bool changed = false;
  jit_module_sp->SetLoadAddress(*target, 0, true, changed);
  return jit_module_sp;
}
