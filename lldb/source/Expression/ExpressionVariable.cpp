//===-- ExpressionVariable.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Expression/ExpressionVariable.h"
#include "lldb/Expression/IRExecutionUnit.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include <optional>

using namespace lldb_private;

char ExpressionVariable::ID;

ExpressionVariable::ExpressionVariable() : m_flags(0) {}

uint8_t *ExpressionVariable::GetValueBytes() {
  lldb::ValueObjectSP valobj_sp = GetValueObject();
  std::optional<uint64_t> byte_size =
      llvm::expectedToOptional(valobj_sp->GetByteSize());
  if (byte_size && *byte_size) {
    if (valobj_sp->GetDataExtractor().GetByteSize() < *byte_size) {
      valobj_sp->GetValue().ResizeData(*byte_size);
      valobj_sp->GetValue().GetData(valobj_sp->GetDataExtractor());
    }
    return const_cast<uint8_t *>(valobj_sp->GetDataExtractor().GetDataStart());
  }
  return nullptr;
}

char PersistentExpressionState::ID;

PersistentExpressionState::PersistentExpressionState() = default;

void ExpressionVariable::TransferAddress(bool force) {
  if (!m_live_sp)
    return;

  if (!m_frozen_sp)
    return;

  if (force || (m_frozen_sp->GetLiveAddress() == LLDB_INVALID_ADDRESS)) {
    lldb::addr_t live_addr = m_live_sp->GetLiveAddress();
    m_frozen_sp->SetLiveAddress(live_addr);
    // One more detail, if there's an offset_to_top in the frozen_sp, then we
    // need to appy that offset by hand.  The live_sp can't compute this
    // itself as its type is the type of the contained object which confuses
    // the dynamic type calculation.  So we have to update the contents of the
    // m_live_sp with the dynamic value.
    // Note: We could get this right when we originally write the address, but
    // that happens in different ways for the various flavors of
    // Entity*::Materialize, but everything comes through here, and it's just
    // one extra memory write.

    // You can only have an "offset_to_top" with pointers or references:
    if (!m_frozen_sp->GetCompilerType().IsPointerOrReferenceType())
      return;

    lldb::ProcessSP process_sp = m_frozen_sp->GetProcessSP();
    // If there's no dynamic value, then there can't be an offset_to_top:
    if (!process_sp ||
        !process_sp->IsPossibleDynamicValue(*(m_frozen_sp.get())))
      return;

    lldb::ValueObjectSP dyn_sp = m_frozen_sp->GetDynamicValue(m_dyn_option);
    if (!dyn_sp)
      return;
    ValueObject::AddrAndType static_addr = m_frozen_sp->GetPointerValue();
    if (static_addr.type != eAddressTypeLoad)
      return;

    ValueObject::AddrAndType dynamic_addr = dyn_sp->GetPointerValue();
    if (dynamic_addr.type != eAddressTypeLoad ||
        static_addr.address == dynamic_addr.address)
      return;

    Status error;
    Log *log = GetLog(LLDBLog::Expressions);
    lldb::addr_t cur_value =
        process_sp->ReadPointerFromMemory(live_addr, error);
    if (error.Fail())
      return;

    if (cur_value != static_addr.address) {
      LLDB_LOG(log,
               "Stored value: {0} read from {1} doesn't "
               "match static addr: {2}",
               cur_value, live_addr, static_addr.address);
      return;
    }

    if (!process_sp->WritePointerToMemory(live_addr, dynamic_addr.address,
                                          error)) {
      LLDB_LOG(log, "Got error: {0} writing dynamic value: {1} to {2}", error,
               dynamic_addr.address, live_addr);
      return;
    }
  }
}

PersistentExpressionState::~PersistentExpressionState() = default;

lldb::addr_t PersistentExpressionState::LookupSymbol(ConstString name) {
  SymbolMap::iterator si = m_symbol_map.find(name.GetCString());

  if (si != m_symbol_map.end())
    return si->second;
  else
    return LLDB_INVALID_ADDRESS;
}

void PersistentExpressionState::RegisterExecutionUnit(
    lldb::IRExecutionUnitSP &execution_unit_sp) {
  // Publishing a symbol and keeping its execution unit alive are one
  // operation: the address is only valid for as long as the unit that emitted
  // the code lives, and m_execution_units is what keeps it living.
  m_execution_units.insert(execution_unit_sp);

  execution_unit_sp->GetExportedSymbols(
      [this](ConstString name, lldb::addr_t addr) {
        m_symbol_map[name.GetCString()] = addr;
      });
}
