//===- MSP430InstrInfo.h - MSP430 Instruction Information -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the MSP430 implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TARGET_MSP430INSTRINFO_H
#define LLVM_TARGET_MSP430INSTRINFO_H

#include "llvm/Target/TargetInstrInfo.h"
#include "MSP430RegisterInfo.h"

namespace llvm {

class MSP430TargetMachine;

class MSP430InstrInfo : public TargetInstrInfoImpl {
  const MSP430RegisterInfo RI;
  MSP430TargetMachine &TM;
public:
  explicit MSP430InstrInfo(MSP430TargetMachine &TM);

  /// getRegisterInfo - TargetInstrInfo is a superset of MRegister info.  As
  /// such, whenever a client has an instance of instruction info, it should
  /// always be able to get register info as well (through this method).
  ///
  virtual const TargetRegisterInfo &getRegisterInfo() const { return RI; }
};

}

#endif
