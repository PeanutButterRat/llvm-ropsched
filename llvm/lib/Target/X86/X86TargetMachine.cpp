//===-- X86TargetMachine.cpp - Define TargetMachine for the X86 -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the X86 specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//

#include "X86TargetMachine.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "TargetInfo/X86TargetInfo.h"
#include "X86.h"
#include "X86MachineFunctionInfo.h"
#include "X86MacroFusion.h"
#include "X86Subtarget.h"
#include "X86TargetObjectFile.h"
#include "X86TargetTransformInfo.h"
#include "llvm-c/Visibility.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/ExecutionDomainFix.h"
#include "llvm/CodeGen/GlobalISel/CSEInfo.h"
#include "llvm/CodeGen/GlobalISel/CallLowering.h"
#include "llvm/CodeGen/GlobalISel/IRTranslator.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelect.h"
#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/RegBankSelect.h"
#include "llvm/CodeGen/MIRParser/MIParser.h"
#include "llvm/CodeGen/MIRYamlMapping.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Pass.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/CFGuard.h"
#include <memory>
#include <optional>
#include <string>

// Extra includes for instruction lowering.
#include "X86AsmPrinter.h"
#include "MCTargetDesc/X86MCAsmInfo.h"
#include "MCTargetDesc/X86EncodingOptimization.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/IR/Mangler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCCodeEmitter.h"

using namespace llvm;

// This flag is for selecting whether or not to use the CapstoneRopSchedStrategy during
// scheduling or the generic post-RA scheduler.
cl::opt<bool> EnableX86CapstoneRopSchedStrategy(
    "enable-x86-capstone-ropsched", cl::Hidden, cl::init(false),
    cl::desc("Toggles which post-RA machine scheduler should be used for x86 (PostGenericScheduler or X86CapstoneRopSchedStrategy)"));

static cl::opt<bool> EnableMachineCombinerPass("x86-machine-combiner",
                               cl::desc("Enable the machine combiner pass"),
                               cl::init(true), cl::Hidden);

static cl::opt<bool>
    EnableTileRAPass("x86-tile-ra",
                     cl::desc("Enable the tile register allocation pass"),
                     cl::init(true), cl::Hidden);

extern "C" LLVM_C_ABI void LLVMInitializeX86Target() {
  // Register the target.
  RegisterTargetMachine<X86TargetMachine> X(getTheX86_32Target());
  RegisterTargetMachine<X86TargetMachine> Y(getTheX86_64Target());

  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeX86LowerAMXIntrinsicsLegacyPassPass(PR);
  initializeX86LowerAMXTypeLegacyPassPass(PR);
  initializeX86PreTileConfigPass(PR);
  initializeGlobalISel(PR);
  initializeWinEHStatePassPass(PR);
  initializeFixupBWInstPassPass(PR);
  initializeCompressEVEXPassPass(PR);
  initializeFixupLEAPassPass(PR);
  initializeFPSPass(PR);
  initializeX86FixupSetCCPassPass(PR);
  initializeX86CallFrameOptimizationPass(PR);
  initializeX86CmovConverterPassPass(PR);
  initializeX86TileConfigPass(PR);
  initializeX86FastPreTileConfigPass(PR);
  initializeX86FastTileConfigPass(PR);
  initializeKCFIPass(PR);
  initializeX86LowerTileCopyPass(PR);
  initializeX86ExpandPseudoPass(PR);
  initializeX86ExecutionDomainFixPass(PR);
  initializeX86DomainReassignmentPass(PR);
  initializeX86AvoidSFBPassPass(PR);
  initializeX86AvoidTrailingCallPassPass(PR);
  initializeX86SpeculativeLoadHardeningPassPass(PR);
  initializeX86SpeculativeExecutionSideEffectSuppressionPass(PR);
  initializeX86FlagsCopyLoweringPassPass(PR);
  initializeX86LoadValueInjectionLoadHardeningPassPass(PR);
  initializeX86LoadValueInjectionRetHardeningPassPass(PR);
  initializeX86OptimizeLEAPassPass(PR);
  initializeX86PartialReductionPass(PR);
  initializePseudoProbeInserterPass(PR);
  initializeX86ReturnThunksPass(PR);
  initializeX86DAGToDAGISelLegacyPass(PR);
  initializeX86ArgumentStackSlotPassPass(PR);
  initializeX86AsmPrinterPass(PR);
  initializeX86FixupInstTuningPassPass(PR);
  initializeX86FixupVectorConstantsPassPass(PR);
  initializeX86DynAllocaExpanderPass(PR);
  initializeX86SuppressAPXForRelocationPassPass(PR);
  initializeX86WinEHUnwindV2Pass(PR);
}

static std::unique_ptr<TargetLoweringObjectFile> createTLOF(const Triple &TT) {
  if (TT.isOSBinFormatMachO()) {
    if (TT.getArch() == Triple::x86_64)
      return std::make_unique<X86_64MachoTargetObjectFile>();
    return std::make_unique<TargetLoweringObjectFileMachO>();
  }

  if (TT.isOSBinFormatCOFF())
    return std::make_unique<TargetLoweringObjectFileCOFF>();

  if (TT.getArch() == Triple::x86_64)
    return std::make_unique<X86_64ELFTargetObjectFile>();
  return std::make_unique<X86ELFTargetObjectFile>();
}

static std::string computeDataLayout(const Triple &TT) {
  // X86 is little endian
  std::string Ret = "e";

  Ret += DataLayout::getManglingComponent(TT);
  // X86 and x32 have 32 bit pointers.
  if (!TT.isArch64Bit() || TT.isX32() || TT.isOSNaCl())
    Ret += "-p:32:32";

  // Address spaces for 32 bit signed, 32 bit unsigned, and 64 bit pointers.
  Ret += "-p270:32:32-p271:32:32-p272:64:64";

  // Some ABIs align 64 bit integers and doubles to 64 bits, others to 32.
  // 128 bit integers are not specified in the 32-bit ABIs but are used
  // internally for lowering f128, so we match the alignment to that.
  if (TT.isArch64Bit() || TT.isOSWindows() || TT.isOSNaCl())
    Ret += "-i64:64-i128:128";
  else if (TT.isOSIAMCU())
    Ret += "-i64:32-f64:32";
  else
    Ret += "-i128:128-f64:32:64";

  // Some ABIs align long double to 128 bits, others to 32.
  if (TT.isOSNaCl() || TT.isOSIAMCU())
    ; // No f80
  else if (TT.isArch64Bit() || TT.isOSDarwin() || TT.isWindowsMSVCEnvironment())
    Ret += "-f80:128";
  else
    Ret += "-f80:32";

  if (TT.isOSIAMCU())
    Ret += "-f128:32";

  // The registers can hold 8, 16, 32 or, in x86-64, 64 bits.
  if (TT.isArch64Bit())
    Ret += "-n8:16:32:64";
  else
    Ret += "-n8:16:32";

  // The stack is aligned to 32 bits on some ABIs and 128 bits on others.
  if ((!TT.isArch64Bit() && TT.isOSWindows()) || TT.isOSIAMCU())
    Ret += "-a:0:32-S32";
  else
    Ret += "-S128";

  return Ret;
}

static Reloc::Model getEffectiveRelocModel(const Triple &TT, bool JIT,
                                           std::optional<Reloc::Model> RM) {
  bool is64Bit = TT.getArch() == Triple::x86_64;
  if (!RM) {
    // JIT codegen should use static relocations by default, since it's
    // typically executed in process and not relocatable.
    if (JIT)
      return Reloc::Static;

    // Darwin defaults to PIC in 64 bit mode and dynamic-no-pic in 32 bit mode.
    // Win64 requires rip-rel addressing, thus we force it to PIC. Otherwise we
    // use static relocation model by default.
    if (TT.isOSDarwin()) {
      if (is64Bit)
        return Reloc::PIC_;
      return Reloc::DynamicNoPIC;
    }
    if (TT.isOSWindows() && is64Bit)
      return Reloc::PIC_;
    return Reloc::Static;
  }

  // ELF and X86-64 don't have a distinct DynamicNoPIC model.  DynamicNoPIC
  // is defined as a model for code which may be used in static or dynamic
  // executables but not necessarily a shared library. On X86-32 we just
  // compile in -static mode, in x86-64 we use PIC.
  if (*RM == Reloc::DynamicNoPIC) {
    if (is64Bit)
      return Reloc::PIC_;
    if (!TT.isOSDarwin())
      return Reloc::Static;
  }

  // If we are on Darwin, disallow static relocation model in X86-64 mode, since
  // the Mach-O file format doesn't support it.
  if (*RM == Reloc::Static && TT.isOSDarwin() && is64Bit)
    return Reloc::PIC_;

  return *RM;
}

static CodeModel::Model
getEffectiveX86CodeModel(const Triple &TT, std::optional<CodeModel::Model> CM,
                         bool JIT) {
  bool Is64Bit = TT.getArch() == Triple::x86_64;
  if (CM) {
    if (*CM == CodeModel::Tiny)
      reportFatalUsageError("target does not support the tiny CodeModel");
    return *CM;
  }
  if (JIT)
    return Is64Bit ? CodeModel::Large : CodeModel::Small;
  return CodeModel::Small;
}

/// Create an X86 target.
///
X86TargetMachine::X86TargetMachine(const Target &T, const Triple &TT,
                                   StringRef CPU, StringRef FS,
                                   const TargetOptions &Options,
                                   std::optional<Reloc::Model> RM,
                                   std::optional<CodeModel::Model> CM,
                                   CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, computeDataLayout(TT), TT, CPU, FS, Options,
                               getEffectiveRelocModel(TT, JIT, RM),
                               getEffectiveX86CodeModel(TT, CM, JIT), OL),
      TLOF(createTLOF(getTargetTriple())), IsJIT(JIT) {
  // On PS4/PS5, the "return address" of a 'noreturn' call must still be within
  // the calling function. Note that this also includes __stack_chk_fail,
  // so there was some target-specific logic in the instruction selectors
  // to handle that. That code has since been generalized, so the only thing
  // needed is to set TrapUnreachable here.
  if (TT.isPS() || TT.isOSBinFormatMachO()) {
    this->Options.TrapUnreachable = true;
    this->Options.NoTrapAfterNoreturn = TT.isOSBinFormatMachO();
  }

  setMachineOutliner(true);

  // x86 supports the debug entry values.
  setSupportsDebugEntryValues(true);

  initAsmInfo();
}

X86TargetMachine::~X86TargetMachine() = default;

const X86Subtarget *
X86TargetMachine::getSubtargetImpl(const Function &F) const {
  Attribute CPUAttr = F.getFnAttribute("target-cpu");
  Attribute TuneAttr = F.getFnAttribute("tune-cpu");
  Attribute FSAttr = F.getFnAttribute("target-features");

  StringRef CPU =
      CPUAttr.isValid() ? CPUAttr.getValueAsString() : (StringRef)TargetCPU;
  // "x86-64" is a default target setting for many front ends. In these cases,
  // they actually request for "generic" tuning unless the "tune-cpu" was
  // specified.
  StringRef TuneCPU = TuneAttr.isValid() ? TuneAttr.getValueAsString()
                      : CPU == "x86-64"  ? "generic"
                                         : (StringRef)CPU;
  StringRef FS =
      FSAttr.isValid() ? FSAttr.getValueAsString() : (StringRef)TargetFS;

  SmallString<512> Key;
  // The additions here are ordered so that the definitely short strings are
  // added first so we won't exceed the small size. We append the
  // much longer FS string at the end so that we only heap allocate at most
  // one time.

  // Extract prefer-vector-width attribute.
  unsigned PreferVectorWidthOverride = 0;
  Attribute PreferVecWidthAttr = F.getFnAttribute("prefer-vector-width");
  if (PreferVecWidthAttr.isValid()) {
    StringRef Val = PreferVecWidthAttr.getValueAsString();
    unsigned Width;
    if (!Val.getAsInteger(0, Width)) {
      Key += 'p';
      Key += Val;
      PreferVectorWidthOverride = Width;
    }
  }

  // Extract min-legal-vector-width attribute.
  unsigned RequiredVectorWidth = UINT32_MAX;
  Attribute MinLegalVecWidthAttr = F.getFnAttribute("min-legal-vector-width");
  if (MinLegalVecWidthAttr.isValid()) {
    StringRef Val = MinLegalVecWidthAttr.getValueAsString();
    unsigned Width;
    if (!Val.getAsInteger(0, Width)) {
      Key += 'm';
      Key += Val;
      RequiredVectorWidth = Width;
    }
  }

  // Add CPU to the Key.
  Key += CPU;

  // Add tune CPU to the Key.
  Key += TuneCPU;

  // Keep track of the start of the feature portion of the string.
  unsigned FSStart = Key.size();

  // FIXME: This is related to the code below to reset the target options,
  // we need to know whether or not the soft float flag is set on the
  // function before we can generate a subtarget. We also need to use
  // it as a key for the subtarget since that can be the only difference
  // between two functions.
  bool SoftFloat = F.getFnAttribute("use-soft-float").getValueAsBool();
  // If the soft float attribute is set on the function turn on the soft float
  // subtarget feature.
  if (SoftFloat)
    Key += FS.empty() ? "+soft-float" : "+soft-float,";

  Key += FS;

  // We may have added +soft-float to the features so move the StringRef to
  // point to the full string in the Key.
  FS = Key.substr(FSStart);

  auto &I = SubtargetMap[Key];
  if (!I) {
    // This needs to be done before we create a new subtarget since any
    // creation will depend on the TM and the code generation flags on the
    // function that reside in TargetOptions.
    resetTargetOptions(F);
    I = std::make_unique<X86Subtarget>(
        TargetTriple, CPU, TuneCPU, FS, *this,
        MaybeAlign(F.getParent()->getOverrideStackAlignment()),
        PreferVectorWidthOverride, RequiredVectorWidth);
  }
  return I.get();
}

yaml::MachineFunctionInfo *X86TargetMachine::createDefaultFuncInfoYAML() const {
  return new yaml::X86MachineFunctionInfo();
}

yaml::MachineFunctionInfo *
X86TargetMachine::convertFuncInfoToYAML(const MachineFunction &MF) const {
  const auto *MFI = MF.getInfo<X86MachineFunctionInfo>();
  return new yaml::X86MachineFunctionInfo(*MFI);
}

bool X86TargetMachine::parseMachineFunctionInfo(
    const yaml::MachineFunctionInfo &MFI, PerFunctionMIParsingState &PFS,
    SMDiagnostic &Error, SMRange &SourceRange) const {
  const auto &YamlMFI = static_cast<const yaml::X86MachineFunctionInfo &>(MFI);
  PFS.MF.getInfo<X86MachineFunctionInfo>()->initializeBaseYamlFields(YamlMFI);
  return false;
}

bool X86TargetMachine::isNoopAddrSpaceCast(unsigned SrcAS,
                                           unsigned DestAS) const {
  assert(SrcAS != DestAS && "Expected different address spaces!");
  if (getPointerSize(SrcAS) != getPointerSize(DestAS))
    return false;
  return SrcAS < 256 && DestAS < 256;
}

void X86TargetMachine::reset() { SubtargetMap.clear(); }

#undef DEBUG_TYPE
#define DEBUG_TYPE "ropsched"

// Replace TAILJMP opcodes with their equivalent opcodes that have encoding
// information.
static unsigned convertTailJumpOpcode(unsigned Opcode) {
  switch (Opcode) {
  case X86::TAILJMPr:
    Opcode = X86::JMP32r;
    break;
  case X86::TAILJMPm:
    Opcode = X86::JMP32m;
    break;
  case X86::TAILJMPr64:
    Opcode = X86::JMP64r;
    break;
  case X86::TAILJMPm64:
    Opcode = X86::JMP64m;
    break;
  case X86::TAILJMPr64_REX:
    Opcode = X86::JMP64r_REX;
    break;
  case X86::TAILJMPm64_REX:
    Opcode = X86::JMP64m_REX;
    break;
  case X86::TAILJMPd:
  case X86::TAILJMPd64:
    Opcode = X86::JMP_1;
    break;
  case X86::TAILJMPd_CC:
  case X86::TAILJMPd64_CC:
    Opcode = X86::JCC_1;
    break;
  }

  return Opcode;
}

class X86MCInstLowerCopy {
  MCContext &Ctx;
  const MachineFunction &MF;
  const TargetMachine &TM;
  const MCAsmInfo &MAI;
  X86AsmPrinter &AsmPrinter;

public:
  X86MCInstLowerCopy(const MachineFunction &MF, X86AsmPrinter &asmprinter);

  MCOperand LowerMachineOperand(const MachineInstr *MI,
                                const MachineOperand &MO) const;
  void Lower(const MachineInstr *MI, MCInst &OutMI) const;

  MCSymbol *GetSymbolFromOperand(const MachineOperand &MO) const;
  MCOperand LowerSymbolOperand(const MachineOperand &MO, MCSymbol *Sym) const;

private:
  MachineModuleInfoMachO &getMachOMMI() const;
};

X86MCInstLowerCopy::X86MCInstLowerCopy(const MachineFunction &mf, X86AsmPrinter &asmprinter)
    : Ctx(asmprinter.OutContext), MF(mf), TM(mf.getTarget()),
      MAI(*TM.getMCAsmInfo()), AsmPrinter(asmprinter) {}

MCOperand X86MCInstLowerCopy::LowerMachineOperand(const MachineInstr *MI, const MachineOperand &MO) const {
  switch (MO.getType()) {
  default:
    MI->print(errs());
    return MCOperand();  // Operands sometimes reach here for whatever reason during scheduling.
    llvm_unreachable("unknown operand type");
  case MachineOperand::MO_Register:
    // Ignore all implicit register operands.
    if (MO.isImplicit())
      return MCOperand();
    return MCOperand::createReg(MO.getReg());
  case MachineOperand::MO_Immediate:
    return MCOperand::createImm(MO.getImm());
  case MachineOperand::MO_MachineBasicBlock:
  case MachineOperand::MO_GlobalAddress:
  case MachineOperand::MO_ExternalSymbol:
    return LowerSymbolOperand(MO, GetSymbolFromOperand(MO));
  case MachineOperand::MO_MCSymbol:
    return LowerSymbolOperand(MO, MO.getMCSymbol());
  case MachineOperand::MO_JumpTableIndex:
    return MCOperand::createExpr(MCConstantExpr::create(0, Ctx));
  case MachineOperand::MO_ConstantPoolIndex:
    return MCOperand::createExpr(MCConstantExpr::create(0, Ctx));
  case MachineOperand::MO_BlockAddress:
    return MCOperand::createExpr(MCConstantExpr::create(0, Ctx));
  case MachineOperand::MO_RegisterMask:
    // Ignore call clobbers.
    return MCOperand();
  }
}

void X86MCInstLowerCopy::Lower(const MachineInstr *MI, MCInst &OutMI) const {
  OutMI.setOpcode(MI->getOpcode());

  for (const MachineOperand &MO : MI->operands())
    if (auto Op = LowerMachineOperand(MI, MO); Op.isValid())
      OutMI.addOperand(Op);

  bool In64BitMode = true;
  if (X86::optimizeInstFromVEX3ToVEX2(OutMI, MI->getDesc()) ||
      X86::optimizeShiftRotateWithImmediateOne(OutMI) ||
      X86::optimizeVPCMPWithImmediateOneOrSix(OutMI) ||
      X86::optimizeMOVSX(OutMI) || X86::optimizeINCDEC(OutMI, In64BitMode) ||
      X86::optimizeMOV(OutMI, In64BitMode) ||
      X86::optimizeToFixedRegisterOrShortImmediateForm(OutMI))
    return;

  // Handle a few special cases to eliminate operand modifiers.
  switch (OutMI.getOpcode()) {
  case X86::LEA64_32r:
  case X86::LEA64r:
  case X86::LEA16r:
  case X86::LEA32r:
    // LEA should have a segment register, but it must be empty.
    assert(OutMI.getNumOperands() == 1 + X86::AddrNumOperands &&
           "Unexpected # of LEA operands");
    assert(OutMI.getOperand(1 + X86::AddrSegmentReg).getReg() == 0 &&
           "LEA has segment specified!");
    break;
  case X86::MULX32Hrr:
  case X86::MULX32Hrm:
  case X86::MULX64Hrr:
  case X86::MULX64Hrm: {
    // Turn into regular MULX by duplicating the destination.
    unsigned NewOpc;
    switch (OutMI.getOpcode()) {
    default: llvm_unreachable("Invalid opcode");
    case X86::MULX32Hrr: NewOpc = X86::MULX32rr; break;
    case X86::MULX32Hrm: NewOpc = X86::MULX32rm; break;
    case X86::MULX64Hrr: NewOpc = X86::MULX64rr; break;
    case X86::MULX64Hrm: NewOpc = X86::MULX64rm; break;
    }
    OutMI.setOpcode(NewOpc);
    // Duplicate the destination.
    MCRegister DestReg = OutMI.getOperand(0).getReg();
    OutMI.insert(OutMI.begin(), MCOperand::createReg(DestReg));
    break;
  }
  // CALL64r, CALL64pcrel32 - These instructions used to have
  // register inputs modeled as normal uses instead of implicit uses.  As such,
  // they we used to truncate off all but the first operand (the callee). This
  // issue seems to have been fixed at some point. This assert verifies that.
  case X86::CALL64r:
  case X86::CALL64pcrel32:
    assert(OutMI.getNumOperands() == 1 && "Unexpected number of operands!");
    break;
  case X86::EH_RETURN:
  case X86::EH_RETURN64: {
    OutMI = MCInst();
    OutMI.setOpcode(X86::RET64);
    break;
  }
  case X86::CLEANUPRET: {
    // Replace CLEANUPRET with the appropriate RET.
    OutMI = MCInst();
    OutMI.setOpcode(X86::RET64);
    break;
  }
  case X86::CATCHRET: {
    // Replace CATCHRET with the appropriate RET.
    OutMI = MCInst();
    OutMI.setOpcode(X86::RET64);
    OutMI.addOperand(MCOperand::createReg(X86::RAX));
    break;
  }
  // TAILJMPd, TAILJMPd64, TailJMPd_cc - Lower to the correct jump
  // instruction.
  case X86::TAILJMPr:
  case X86::TAILJMPr64:
  case X86::TAILJMPr64_REX:
  case X86::TAILJMPd:
  case X86::TAILJMPd64:
    assert(OutMI.getNumOperands() == 1 && "Unexpected number of operands!");
    OutMI.setOpcode(convertTailJumpOpcode(OutMI.getOpcode()));
    break;
  case X86::TAILJMPd_CC:
  case X86::TAILJMPd64_CC:
    assert(OutMI.getNumOperands() == 2 && "Unexpected number of operands!");
    OutMI.setOpcode(convertTailJumpOpcode(OutMI.getOpcode()));
    break;
  case X86::TAILJMPm:
  case X86::TAILJMPm64:
  case X86::TAILJMPm64_REX:
    assert(OutMI.getNumOperands() == X86::AddrNumOperands &&
           "Unexpected number of operands!");
    OutMI.setOpcode(convertTailJumpOpcode(OutMI.getOpcode()));
    break;
  case X86::MASKMOVDQU:
  case X86::VMASKMOVDQU:
    if (In64BitMode)
      OutMI.setFlags(X86::IP_HAS_AD_SIZE);
    break;
  case X86::BSF16rm:
  case X86::BSF16rr:
  case X86::BSF32rm:
  case X86::BSF32rr:
  case X86::BSF64rm:
  case X86::BSF64rr: {
    // Add an REP prefix to BSF instructions so that new processors can
    // recognize as TZCNT, which has better performance than BSF.
    // BSF and TZCNT have different interpretations on ZF bit. So make sure
    // it won't be used later.
    const MachineOperand *FlagDef =
        MI->findRegisterDefOperand(X86::EFLAGS, /*TRI=*/nullptr);
    if (!MF.getFunction().hasOptSize() && FlagDef && FlagDef->isDead())
      OutMI.setFlags(X86::IP_HAS_REPEAT);
    break;
  }
  default:
    break;
  }
}

/// GetSymbolFromOperand - Lower an MO_GlobalAddress or MO_ExternalSymbol
/// operand to an MCSymbol.
MCSymbol *X86MCInstLowerCopy::GetSymbolFromOperand(const MachineOperand &MO) const {
  const Triple &TT = TM.getTargetTriple();
  if (MO.isGlobal() && TT.isOSBinFormatELF())
    return AsmPrinter.getSymbolPreferLocal(*MO.getGlobal());

  const DataLayout &DL = MF.getDataLayout();
  assert((MO.isGlobal() || MO.isSymbol() || MO.isMBB()) &&
         "Isn't a symbol reference");

  MCSymbol *Sym = nullptr;
  SmallString<128> Name;
  StringRef Suffix;

  switch (MO.getTargetFlags()) {
  case X86II::MO_DLLIMPORT:
    // Handle dllimport linkage.
    Name += "__imp_";
    break;
  case X86II::MO_COFFSTUB:
    Name += ".refptr.";
    break;
  case X86II::MO_DARWIN_NONLAZY:
  case X86II::MO_DARWIN_NONLAZY_PIC_BASE:
    Suffix = "$non_lazy_ptr";
    break;
  }

  if (!Suffix.empty())
    Name += DL.getPrivateGlobalPrefix();

  if (MO.isGlobal()) {
    const GlobalValue *GV = MO.getGlobal();
    AsmPrinter.getNameWithPrefix(Name, GV);
  } else if (MO.isSymbol()) {
    Mangler::getNameWithPrefix(Name, MO.getSymbolName(), DL);
  } else if (MO.isMBB()) {
    assert(Suffix.empty());
    Sym = MO.getMBB()->getSymbol();
  }

  Name += Suffix;
  if (!Sym)
    Sym = Ctx.getOrCreateSymbol(Name);

  // If the target flags on the operand changes the name of the symbol, do that
  // before we return the symbol.
  switch (MO.getTargetFlags()) {
  default:
    break;
  case X86II::MO_COFFSTUB: {
    MachineModuleInfoCOFF &MMICOFF =
        AsmPrinter.MMI->getObjFileInfo<MachineModuleInfoCOFF>();
    MachineModuleInfoImpl::StubValueTy &StubSym = MMICOFF.getGVStubEntry(Sym);
    if (!StubSym.getPointer()) {
      assert(MO.isGlobal() && "Extern symbol not handled yet");
      StubSym = MachineModuleInfoImpl::StubValueTy(
          AsmPrinter.getSymbol(MO.getGlobal()), true);
    }
    break;
  }
  case X86II::MO_DARWIN_NONLAZY:
  case X86II::MO_DARWIN_NONLAZY_PIC_BASE: {
    MachineModuleInfoImpl::StubValueTy &StubSym =
        getMachOMMI().getGVStubEntry(Sym);
    if (!StubSym.getPointer()) {
      assert(MO.isGlobal() && "Extern symbol not handled yet");
      StubSym = MachineModuleInfoImpl::StubValueTy(
          AsmPrinter.getSymbol(MO.getGlobal()),
          !MO.getGlobal()->hasInternalLinkage());
    }
    break;
  }
  }

  return Sym;
}

MCOperand X86MCInstLowerCopy::LowerSymbolOperand(const MachineOperand &MO,
                                             MCSymbol *Sym) const {
  // FIXME: We would like an efficient form for this, so we don't have to do a
  // lot of extra uniquing.
  const MCExpr *Expr = nullptr;
  uint16_t Specifier = X86::S_None;

  switch (MO.getTargetFlags()) {
  default:
    llvm_unreachable("Unknown target flag on GV operand");
  case X86II::MO_NO_FLAG: // No flag.
  // These affect the name of the symbol, not any suffix.
  case X86II::MO_DARWIN_NONLAZY:
  case X86II::MO_DLLIMPORT:
  case X86II::MO_COFFSTUB:
    break;

  case X86II::MO_TLVP:
    Specifier = X86::S_TLVP;
    break;
  case X86II::MO_TLVP_PIC_BASE:
    Expr = MCSymbolRefExpr::create(Sym, X86::S_TLVP, Ctx);
    // Subtract the pic base.
    Expr = MCBinaryExpr::createSub(
        Expr, MCSymbolRefExpr::create(MF.getPICBaseSymbol(), Ctx), Ctx);
    break;
  case X86II::MO_SECREL:
    Specifier = uint16_t(X86::S_COFF_SECREL);
    break;
  case X86II::MO_TLSGD:
    Specifier = X86::S_TLSGD;
    break;
  case X86II::MO_TLSLD:
    Specifier = X86::S_TLSLD;
    break;
  case X86II::MO_TLSLDM:
    Specifier = X86::S_TLSLDM;
    break;
  case X86II::MO_GOTTPOFF:
    Specifier = X86::S_GOTTPOFF;
    break;
  case X86II::MO_INDNTPOFF:
    Specifier = X86::S_INDNTPOFF;
    break;
  case X86II::MO_TPOFF:
    Specifier = X86::S_TPOFF;
    break;
  case X86II::MO_DTPOFF:
    Specifier = X86::S_DTPOFF;
    break;
  case X86II::MO_NTPOFF:
    Specifier = X86::S_NTPOFF;
    break;
  case X86II::MO_GOTNTPOFF:
    Specifier = X86::S_GOTNTPOFF;
    break;
  case X86II::MO_GOTPCREL:
    Specifier = X86::S_GOTPCREL;
    break;
  case X86II::MO_GOTPCREL_NORELAX:
    Specifier = X86::S_GOTPCREL_NORELAX;
    break;
  case X86II::MO_GOT:
    Specifier = X86::S_GOT;
    break;
  case X86II::MO_GOTOFF:
    Specifier = X86::S_GOTOFF;
    break;
  case X86II::MO_PLT:
    Specifier = X86::S_PLT;
    break;
  case X86II::MO_ABS8:
    Specifier = X86::S_ABS8;
    break;
  case X86II::MO_PIC_BASE_OFFSET:
  case X86II::MO_DARWIN_NONLAZY_PIC_BASE:
    Expr = MCSymbolRefExpr::create(Sym, Ctx);
    // Subtract the pic base.
    Expr = MCBinaryExpr::createSub(
        Expr, MCSymbolRefExpr::create(MF.getPICBaseSymbol(), Ctx), Ctx);
    if (MO.isJTI()) {
      assert(MAI.doesSetDirectiveSuppressReloc());
      // If .set directive is supported, use it to reduce the number of
      // relocations the assembler will generate for differences between
      // local labels. This is only safe when the symbols are in the same
      // section so we are restricting it to jumptable references.
      MCSymbol *Label = Ctx.createTempSymbol();
      AsmPrinter.OutStreamer->emitAssignment(Label, Expr);
      Expr = MCSymbolRefExpr::create(Label, Ctx);
    }
    break;
  }

  if (!Expr)
    Expr = MCSymbolRefExpr::create(Sym, Specifier, Ctx);

  if (!MO.isJTI() && !MO.isMBB() && MO.getOffset())
    Expr = MCBinaryExpr::createAdd(
        Expr, MCConstantExpr::create(MO.getOffset(), Ctx), Ctx);
  return MCOperand::createExpr(Expr);
}

MachineModuleInfoMachO &X86MCInstLowerCopy::getMachOMMI() const {
  return AsmPrinter.MMI->getObjFileInfo<MachineModuleInfoMachO>();
}


#define DEBUG_TYPE "ropsched"

class X86CapstoneRopSchedStrategy : public MachineSchedStrategy {
  std::vector<SUnit *> Ready;
  ScheduleDAGMI *DAG;
  MCSubtargetInfo *MSTI;
  
  std::unique_ptr<MCContext> Context;
  std::unique_ptr<MCCodeEmitter> Emitter;
  std::unique_ptr<X86AsmPrinter> Printer;
  std::unique_ptr<X86MCInstLowerCopy> Lowerer;

  std::map<SUnit *, SmallVector<char, 16>> InstructionEncodings;
  std::vector<uint8_t> Schedule;
  Capstone CS;
  size_t LastReturnInstr;
  size_t LastJumpInstr;
  size_t LastCallInstr;

public:
  explicit X86CapstoneRopSchedStrategy(const MachineSchedContext *C) 
    : Ready(), DAG(nullptr), MSTI(nullptr), Emitter(nullptr), Lowerer(nullptr), InstructionEncodings(),
      CS(CS_ARCH_X86, CS_MODE_64), LastReturnInstr(0), LastJumpInstr(0), LastCallInstr(0) { }

  void initialize(ScheduleDAGMI *DAG) override {
    this->DAG = DAG;
    Schedule.clear();
    LastReturnInstr = 0;
    LastJumpInstr = 0;
    LastCallInstr = 0;

    const MachineFunction &MF = DAG->MF;
    const TargetMachine &TM = MF.getTarget();
    const Target &T = TM.getTarget();
    const MCAsmInfo *MAI = TM.getMCAsmInfo();
    const MCRegisterInfo *MRI = TM.getMCRegisterInfo();
    const MCSubtargetInfo *MSTI = TM.getMCSubtargetInfo();
    const MCInstrInfo *MCII = TM.getMCInstrInfo();

    Context = std::make_unique<MCContext>(TM.getTargetTriple(), MAI, MRI, MSTI);
    std::unique_ptr<MCStreamer> Streamer{T.createNullStreamer(*Context)};

    Printer = std::make_unique<X86AsmPrinter>(const_cast<TargetMachine &>(TM), std::move(Streamer));
    Emitter = std::unique_ptr<MCCodeEmitter>(T.createMCCodeEmitter(*MCII, *Context));
    Lowerer = std::make_unique<X86MCInstLowerCopy>(MF, *Printer);
  }

  void enterMBB(MachineBasicBlock *MBB) override {}

  SUnit *pickNode(bool &IsTopNode) override {
    int MinimumNumberOfGadgets = std::numeric_limits<int>::max();
    SUnit *Next = nullptr;

    for (SUnit *SU : Ready) {
      int NumberOfGadgets = countGadgets(SU);

      if (NumberOfGadgets < MinimumNumberOfGadgets) {
        Next = SU;
        MinimumNumberOfGadgets = NumberOfGadgets;
      }
    }

    if (Next) {
      Ready.erase(std::find(Ready.begin(), Ready.end(), Next));
      IsTopNode = false;
    }

    return Next;
  }

  int countGadgets(SUnit *Candidate) {
    const SmallVector<char, 16> &Encoding = InstructionEncodings[Candidate];
    const size_t CandidateSize = Encoding.size();

    if (CandidateSize == 0) {
      return std::numeric_limits<int>::max() - 1;
    }

    Schedule.insert(Schedule.begin(), Encoding.begin(), Encoding.end());

    int GadgetCount = countGadgetsFromIndex(LastJumpInstr + CandidateSize);
    GadgetCount += countGadgetsFromIndex(LastJumpInstr + CandidateSize);
    GadgetCount += countGadgetsFromIndex(LastCallInstr + CandidateSize);

    Schedule.erase(Schedule.begin(), Schedule.begin() + CandidateSize);

    return GadgetCount;
  }

  void schedNode(SUnit *SU, bool IsTopNode) override {
    const SmallVector<char, 16> &Encoding = InstructionEncodings[SU];

    Schedule.insert(Schedule.begin(), Encoding.begin(), Encoding.end());

    LastReturnInstr = findNextReturnInstruction();
    LastJumpInstr = findNextJumpInstruction();
    LastCallInstr = findNextCallInstruction();
  }

  size_t findNextReturnInstruction() {
    for (size_t i = 0; i < Schedule.size(); i++) {
      size_t AvailableBytesLeft = Schedule.size() - i;

      if (Schedule[i] == 0xC3 || Schedule[i] == 0xCB 
        || (AvailableBytesLeft >= 3 && Schedule[i] == 0xC2)
        || (AvailableBytesLeft >= 3 && Schedule[i] == 0xCA)) {
          return i;
      }
    }

    return Schedule.size();
  }

  size_t findNextJumpInstruction() {
    for (size_t i = 0; i < Schedule.size(); i++) {
      size_t AvailableBytesLeft = Schedule.size() - i;

      if (((AvailableBytesLeft >= 2) && (Schedule[i] == 0xFF) && (inRange(Schedule[i + 1], 0xE0, 0xE7)))
        || ((AvailableBytesLeft >= 2) && (Schedule[i] == 0xFF) && (inRange(Schedule[i + 1], 0x20, 0x23) || inRange(Schedule[i + 1], 0x26, 0x27)))
        || ((AvailableBytesLeft >= 3) && (Schedule[i] == 0xFF) && (Schedule[i + 1] == 0x24) && (Schedule[i + 2] == 0x24))
        || ((AvailableBytesLeft >= 3) && (Schedule[i] == 0xFF) && (inRange(Schedule[i + 1], 0x60, 0x63) || inRange(Schedule[i + 1], 0x65, 0x67)))
        || ((AvailableBytesLeft >= 4) && (Schedule[i] == 0xFF) && (Schedule[i + 1] == 0x64) && (Schedule[i + 2] == 0x24))
        || ((AvailableBytesLeft >= 6) && (Schedule[i] == 0xFF) && (inRange(Schedule[i + 1], 0xA0, 0xA3) || inRange(Schedule[i + 1], 0xA5, 0xA7)))
        || ((AvailableBytesLeft >= 7) && (Schedule[i] == 0xFF) && (Schedule[i + 1] == 0xA4) && (Schedule[i + 2] == 0x24))

        // See https://github.com/JonathanSalwan/ROPgadget/blob/4e5d4da5a92a723f823ee0dc00dc0cfcfabe19f1/ropgadget/gadgets.py#L249 for an explanation for these patterns.
        || ((AvailableBytesLeft >= 3) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (inRange(Schedule[i + 2], 0xE0, 0xE7)))
        || ((AvailableBytesLeft >= 3) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (inRange(Schedule[i + 2], 0x20, 0x23) || inRange(Schedule[i + 2], 0x26, 0x27)))
        || ((AvailableBytesLeft >= 4) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (Schedule[i + 2] == 0x24) && (Schedule[i + 3] == 0x24))
        || ((AvailableBytesLeft >= 4) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (inRange(Schedule[i + 2], 0x60, 0x63) || inRange(Schedule[i + 2], 0x65, 0x67)))
        || ((AvailableBytesLeft >= 5) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (Schedule[i + 2] == 0x64) && (Schedule[i + 3] == 0x24))
        || ((AvailableBytesLeft >= 7) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (inRange(Schedule[i + 2], 0xA0, 0xA3) || inRange(Schedule[i + 2], 0xA5, 0xA7)))
        || ((AvailableBytesLeft >= 8) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (Schedule[i + 2] == 0xA4) && (Schedule[i + 3] == 0x24))

        || ((AvailableBytesLeft >= 2) && (Schedule[i] == 0xEB))
        || ((AvailableBytesLeft >= 5) && (Schedule[i] == 0xE9))) {
          return i;
      }
    }

    return Schedule.size();
  }

  size_t findNextCallInstruction() {
    for (size_t i = 0; i < Schedule.size(); i++) {
      size_t AvailableBytesLeft = Schedule.size() - i;

      if (((AvailableBytesLeft >= 2) && (Schedule[i] == 0xFF) && inRange(Schedule[i + 1], 0xD0, 0xD7))
        || ((AvailableBytesLeft >= 2) && (Schedule[i] == 0xFF) && (inRange(Schedule[i + 1], 0x10, 0x13) || inRange(Schedule[i + 1], 0x16, 0x17)))
        || ((AvailableBytesLeft >= 3) && (Schedule[i] == 0xFF) && (Schedule[i + 1] == 0x14) && (Schedule[i + 2] == 0x24))
        || ((AvailableBytesLeft >= 3) && (Schedule[i] == 0xFF) && (inRange(Schedule[i + 1], 0x50, 0x53) || inRange(Schedule[i + 1], 0x55, 0x57)))
        || ((AvailableBytesLeft >= 4) && (Schedule[i] == 0xFF) && (Schedule[i + 1] == 0x54) && Schedule[i + 2] == 0x24)
        || ((AvailableBytesLeft >= 6) && (Schedule[i] == 0xFF) && (inRange(Schedule[i + 1], 0x90, 0x93) || inRange(Schedule[i + 1], 0x95, 0x97)))
        || ((AvailableBytesLeft >= 7) && (Schedule[i] == 0xFF) && (Schedule[i + 1] == 0x94) && Schedule[i + 2] == 0x24)

        || ((AvailableBytesLeft >= 3) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (inRange(Schedule[i + 2], 0xD0, 0xD7)))
        || ((AvailableBytesLeft >= 3) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (inRange(Schedule[i + 2], 0x10, 0x13) || inRange(Schedule[i + 2], 0x16, 0x17)))
        || ((AvailableBytesLeft >= 4) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (Schedule[i + 2] == 0x14) && (Schedule[i + 3] == 0x24))
        || ((AvailableBytesLeft >= 4) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (inRange(Schedule[i + 2], 0x50, 0x53) || inRange(Schedule[i + 2], 0x55, 0x57)))
        || ((AvailableBytesLeft >= 5) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (Schedule[i + 2] == 0x54) && (Schedule[i + 3] == 0x24))
        || ((AvailableBytesLeft >= 7) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (inRange(Schedule[i + 2], 0x90, 0x93) || inRange(Schedule[i + 2], 0x95, 0x97)))
        || ((AvailableBytesLeft >= 8) && (Schedule[i] == 0x41) && (Schedule[i + 1] == 0xFF) && (Schedule[i + 2] == 0x94) && (Schedule[i + 3] == 0x24))) {
          return i;
      }
    }

    return Schedule.size();
  }

  bool inRange(uint8_t Byte, uint8_t Lower, uint8_t Higher) {
    return Lower <= Byte && Byte <= Higher;
  }

  SmallVector<char, 16> lowerInstruction(MachineInstr *MI) {
    MCInst MCI{};
    SmallVector<MCFixup, 4> Fixups{};
    SmallVector<char, 16> Bytes{};
    Lowerer->Lower(MI, MCI);
    Emitter->encodeInstruction(MCI, Bytes, Fixups, *DAG->MF.getTarget().getMCSubtargetInfo());
    return Bytes;
  }

  int countGadgetsFromIndex(size_t Index) {
    int GadgetCount = 0;
  
    for (size_t Depth = 1; Depth <= 10 && Depth <= Index; ++Depth) {
        size_t Start = Index - Depth;
        ArrayRef<uint8_t> Window{Schedule.data() + Start, Depth};
        auto Disassembled = CS.disassemble(Window);

        // It also might be worth checking if the disassembled bytes are the same length as the window
        // size (if it isn't, technically it wouldn't be a gadget). This is what RopGadget does, but this
        // results in worse performance overall.
        if (!Disassembled.empty()) {
            GadgetCount++;
        }
      }

      return GadgetCount;
  }

  void releaseTopNode(SUnit *SU) override {}

  void releaseBottomNode(SUnit *SU) override {
    auto Bytes = lowerInstruction(SU->getInstr());
    InstructionEncodings[SU] = Bytes;
    Ready.push_back(SU);
  }
};

// This is another example of the ExtendedScoreRopSchedStrategy which also doesn't perform that
// well. Theoretically, AArch64 should perform better because it doesn't suffer from misaligned gadgets.
struct X86ExtendedScoreRopSchedStrategy : public ExtendedScoreRopSchedStrategy {
  explicit X86ExtendedScoreRopSchedStrategy(const MachineSchedContext *C) : ExtendedScoreRopSchedStrategy(C) { }

  bool isConditionalDataMove(const MachineInstr &MI) override {
    switch (const auto Opcode = MI.getOpcode(); Opcode) {
      case X86::CMOV16rm:
      case X86::CMOV16rm_ND:
      case X86::CMOV16rr:
      case X86::CMOV16rr_ND:
      case X86::CMOV32rm:
      case X86::CMOV32rm_ND:
      case X86::CMOV32rr:
      case X86::CMOV32rr_ND:
      case X86::CMOV64rm:
      case X86::CMOV64rm_ND:
      case X86::CMOV64rr:
      case X86::CMOV64rr_ND:
      case X86::CMPXCHG16B:
      case X86::CMPXCHG16rm:
      case X86::CMPXCHG16rr:
      case X86::CMPXCHG32rm:
      case X86::CMPXCHG32rr:
      case X86::CMPXCHG64rm:
      case X86::CMPXCHG64rr:
      case X86::CMPXCHG8B:
      case X86::CMPXCHG8rm:
      case X86::CMPXCHG8rr:
        return true;
      default:
        return false;
    }
  }

  bool isConditionalSet(const MachineInstr &MI) override {
    switch (const auto Opcode = MI.getOpcode(); Opcode) {
      case X86::SETB_C32r:
      case X86::SETB_C64r:
      case X86::SETCCm:
      case X86::SETCCm_EVEX:
      case X86::SETCCr:
      case X86::SETCCr_EVEX:
      case X86::SETSSBSY:
      case X86::SETZUCCm:
      case X86::SETZUCCr:
        return true;
      default:
        return false;
    }
  }

  bool isShiftOrRotate(const MachineInstr &MI) override {
    const unsigned Opcode = MI.getOpcode();
    StringRef InstructionName = DAG->TII->getName(Opcode);
    static const std::vector<StringLiteral> Prefixes{ "SHL", "SHR", "SAR", "SAL", "ROR", "ROL", "RCR", "RCL" };

    for (auto Prefix : Prefixes) {
      if (InstructionName.starts_with(Prefix)) {
        return true;
      }
    }

    return false;
  }
};

ScheduleDAGInstrs *
X86TargetMachine::createMachineScheduler(MachineSchedContext *C) const {
  ScheduleDAGMILive *DAG = createSchedLive(C);
  DAG->addMutation(createX86MacroFusionDAGMutation());
  return DAG;
}

ScheduleDAGInstrs *
X86TargetMachine::createPostMachineScheduler(MachineSchedContext *C) const {
  ScheduleDAGMI *DAG = (EnableX86CapstoneRopSchedStrategy) ? createSchedPostRA<X86CapstoneRopSchedStrategy>(C) : createSchedPostRA(C);
  DAG->addMutation(createX86MacroFusionDAGMutation());
  return DAG;
}

#undef DEBUG_TYPE


//===----------------------------------------------------------------------===//
// X86 TTI query.
//===----------------------------------------------------------------------===//

TargetTransformInfo
X86TargetMachine::getTargetTransformInfo(const Function &F) const {
  return TargetTransformInfo(std::make_unique<X86TTIImpl>(this, F));
}

//===----------------------------------------------------------------------===//
// Pass Pipeline Configuration
//===----------------------------------------------------------------------===//

namespace {

/// X86 Code Generator Pass Configuration Options.
class X86PassConfig : public TargetPassConfig {
public:
  X86PassConfig(X86TargetMachine &TM, PassManagerBase &PM)
    : TargetPassConfig(TM, PM) {}

  X86TargetMachine &getX86TargetMachine() const {
    return getTM<X86TargetMachine>();
  }

  void addIRPasses() override;
  bool addInstSelector() override;
  bool addIRTranslator() override;
  bool addLegalizeMachineIR() override;
  bool addRegBankSelect() override;
  bool addGlobalInstructionSelect() override;
  bool addILPOpts() override;
  bool addPreISel() override;
  void addMachineSSAOptimization() override;
  void addPreRegAlloc() override;
  bool addPostFastRegAllocRewrite() override;
  void addPostRegAlloc() override;
  void addPreEmitPass() override;
  void addPreEmitPass2() override;
  void addPreSched2() override;
  bool addRegAssignAndRewriteOptimized() override;

  std::unique_ptr<CSEConfigBase> getCSEConfig() const override;
};

class X86ExecutionDomainFix : public ExecutionDomainFix {
public:
  static char ID;
  X86ExecutionDomainFix() : ExecutionDomainFix(ID, X86::VR128XRegClass) {}
  StringRef getPassName() const override {
    return "X86 Execution Dependency Fix";
  }
};
char X86ExecutionDomainFix::ID;

} // end anonymous namespace

INITIALIZE_PASS_BEGIN(X86ExecutionDomainFix, "x86-execution-domain-fix",
  "X86 Execution Domain Fix", false, false)
INITIALIZE_PASS_DEPENDENCY(ReachingDefAnalysis)
INITIALIZE_PASS_END(X86ExecutionDomainFix, "x86-execution-domain-fix",
  "X86 Execution Domain Fix", false, false)

TargetPassConfig *X86TargetMachine::createPassConfig(PassManagerBase &PM) {
  return new X86PassConfig(*this, PM);
}

MachineFunctionInfo *X86TargetMachine::createMachineFunctionInfo(
    BumpPtrAllocator &Allocator, const Function &F,
    const TargetSubtargetInfo *STI) const {
  return X86MachineFunctionInfo::create<X86MachineFunctionInfo>(Allocator, F,
                                                                STI);
}

void X86PassConfig::addIRPasses() {
  addPass(createAtomicExpandLegacyPass());

  // We add both pass anyway and when these two passes run, we skip the pass
  // based on the option level and option attribute.
  addPass(createX86LowerAMXIntrinsicsPass());
  addPass(createX86LowerAMXTypePass());

  TargetPassConfig::addIRPasses();

  if (TM->getOptLevel() != CodeGenOptLevel::None) {
    addPass(createInterleavedAccessPass());
    addPass(createX86PartialReductionPass());
  }

  // Add passes that handle indirect branch removal and insertion of a retpoline
  // thunk. These will be a no-op unless a function subtarget has the retpoline
  // feature enabled.
  addPass(createIndirectBrExpandPass());

  // Add Control Flow Guard checks.
  const Triple &TT = TM->getTargetTriple();
  if (TT.isOSWindows()) {
    if (TT.getArch() == Triple::x86_64) {
      addPass(createCFGuardDispatchPass());
    } else {
      addPass(createCFGuardCheckPass());
    }
  }

  if (TM->Options.JMCInstrument)
    addPass(createJMCInstrumenterPass());
}

bool X86PassConfig::addInstSelector() {
  // Install an instruction selector.
  addPass(createX86ISelDag(getX86TargetMachine(), getOptLevel()));

  // For ELF, cleanup any local-dynamic TLS accesses.
  if (TM->getTargetTriple().isOSBinFormatELF() &&
      getOptLevel() != CodeGenOptLevel::None)
    addPass(createCleanupLocalDynamicTLSPass());

  addPass(createX86GlobalBaseRegPass());
  addPass(createX86ArgumentStackSlotPass());
  return false;
}

bool X86PassConfig::addIRTranslator() {
  addPass(new IRTranslator(getOptLevel()));
  return false;
}

bool X86PassConfig::addLegalizeMachineIR() {
  addPass(new Legalizer());
  return false;
}

bool X86PassConfig::addRegBankSelect() {
  addPass(new RegBankSelect());
  return false;
}

bool X86PassConfig::addGlobalInstructionSelect() {
  addPass(new InstructionSelect(getOptLevel()));
  // Add GlobalBaseReg in case there is no SelectionDAG passes afterwards
  if (isGlobalISelAbortEnabled())
    addPass(createX86GlobalBaseRegPass());
  return false;
}

bool X86PassConfig::addILPOpts() {
  addPass(&EarlyIfConverterLegacyID);
  if (EnableMachineCombinerPass)
    addPass(&MachineCombinerID);
  addPass(createX86CmovConverterPass());
  return true;
}

bool X86PassConfig::addPreISel() {
  // Only add this pass for 32-bit x86 Windows.
  const Triple &TT = TM->getTargetTriple();
  if (TT.isOSWindows() && TT.getArch() == Triple::x86)
    addPass(createX86WinEHStatePass());
  return true;
}

void X86PassConfig::addPreRegAlloc() {
  if (getOptLevel() != CodeGenOptLevel::None) {
    addPass(&LiveRangeShrinkID);
    addPass(createX86FixupSetCC());
    addPass(createX86OptimizeLEAs());
    addPass(createX86CallFrameOptimization());
    addPass(createX86AvoidStoreForwardingBlocks());
  }

  addPass(createX86SuppressAPXForRelocationPass());

  addPass(createX86SpeculativeLoadHardeningPass());
  addPass(createX86FlagsCopyLoweringPass());
  addPass(createX86DynAllocaExpander());

  if (getOptLevel() != CodeGenOptLevel::None)
    addPass(createX86PreTileConfigPass());
  else
    addPass(createX86FastPreTileConfigPass());
}

void X86PassConfig::addMachineSSAOptimization() {
  addPass(createX86DomainReassignmentPass());
  TargetPassConfig::addMachineSSAOptimization();
}

void X86PassConfig::addPostRegAlloc() {
  addPass(createX86LowerTileCopyPass());
  addPass(createX86FloatingPointStackifierPass());
  // When -O0 is enabled, the Load Value Injection Hardening pass will fall back
  // to using the Speculative Execution Side Effect Suppression pass for
  // mitigation. This is to prevent slow downs due to
  // analyses needed by the LVIHardening pass when compiling at -O0.
  if (getOptLevel() != CodeGenOptLevel::None)
    addPass(createX86LoadValueInjectionLoadHardeningPass());
}

void X86PassConfig::addPreSched2() {
  addPass(createX86ExpandPseudoPass());
  addPass(createKCFIPass());
}

void X86PassConfig::addPreEmitPass() {
  if (getOptLevel() != CodeGenOptLevel::None) {
    addPass(new X86ExecutionDomainFix());
    addPass(createBreakFalseDeps());
  }

  addPass(createX86IndirectBranchTrackingPass());

  addPass(createX86IssueVZeroUpperPass());

  if (getOptLevel() != CodeGenOptLevel::None) {
    addPass(createX86FixupBWInsts());
    addPass(createX86PadShortFunctions());
    addPass(createX86FixupLEAs());
    addPass(createX86FixupInstTuning());
    addPass(createX86FixupVectorConstants());
  }
  addPass(createX86CompressEVEXPass());
  addPass(createX86DiscriminateMemOpsPass());
  addPass(createX86InsertPrefetchPass());
  addPass(createX86InsertX87waitPass());
}

void X86PassConfig::addPreEmitPass2() {
  const Triple &TT = TM->getTargetTriple();
  const MCAsmInfo *MAI = TM->getMCAsmInfo();

  // The X86 Speculative Execution Pass must run after all control
  // flow graph modifying passes. As a result it was listed to run right before
  // the X86 Retpoline Thunks pass. The reason it must run after control flow
  // graph modifications is that the model of LFENCE in LLVM has to be updated
  // (FIXME: https://bugs.llvm.org/show_bug.cgi?id=45167). Currently the
  // placement of this pass was hand checked to ensure that the subsequent
  // passes don't move the code around the LFENCEs in a way that will hurt the
  // correctness of this pass. This placement has been shown to work based on
  // hand inspection of the codegen output.
  addPass(createX86SpeculativeExecutionSideEffectSuppression());
  addPass(createX86IndirectThunksPass());
  addPass(createX86ReturnThunksPass());

  // Insert extra int3 instructions after trailing call instructions to avoid
  // issues in the unwinder.
  if (TT.isOSWindows() && TT.getArch() == Triple::x86_64)
    addPass(createX86AvoidTrailingCallPass());

  // Verify basic block incoming and outgoing cfa offset and register values and
  // correct CFA calculation rule where needed by inserting appropriate CFI
  // instructions.
  if (!TT.isOSDarwin() &&
      (!TT.isOSWindows() ||
       MAI->getExceptionHandlingType() == ExceptionHandling::DwarfCFI))
    addPass(createCFIInstrInserter());

  if (TT.isOSWindows()) {
    // Identify valid longjmp targets for Windows Control Flow Guard.
    addPass(createCFGuardLongjmpPass());
    // Identify valid eh continuation targets for Windows EHCont Guard.
    addPass(createEHContGuardTargetsPass());
  }
  addPass(createX86LoadValueInjectionRetHardeningPass());

  // Insert pseudo probe annotation for callsite profiling
  addPass(createPseudoProbeInserter());

  // KCFI indirect call checks are lowered to a bundle, and on Darwin platforms,
  // also CALL_RVMARKER.
  addPass(createUnpackMachineBundles([&TT](const MachineFunction &MF) {
    // Only run bundle expansion if the module uses kcfi, or there are relevant
    // ObjC runtime functions present in the module.
    const Function &F = MF.getFunction();
    const Module *M = F.getParent();
    return M->getModuleFlag("kcfi") ||
           (TT.isOSDarwin() &&
            (M->getFunction("objc_retainAutoreleasedReturnValue") ||
             M->getFunction("objc_unsafeClaimAutoreleasedReturnValue")));
  }));

  // Analyzes and emits pseudos to support Win x64 Unwind V2. This pass must run
  // after all real instructions have been added to the epilog.
  if (TT.isOSWindows() && (TT.getArch() == Triple::x86_64))
    addPass(createX86WinEHUnwindV2Pass());
}

bool X86PassConfig::addPostFastRegAllocRewrite() {
  addPass(createX86FastTileConfigPass());
  return true;
}

std::unique_ptr<CSEConfigBase> X86PassConfig::getCSEConfig() const {
  return getStandardCSEConfigForOpt(TM->getOptLevel());
}

static bool onlyAllocateTileRegisters(const TargetRegisterInfo &TRI,
                                      const MachineRegisterInfo &MRI,
                                      const Register Reg) {
  const TargetRegisterClass *RC = MRI.getRegClass(Reg);
  return static_cast<const X86RegisterInfo &>(TRI).isTileRegisterClass(RC);
}

bool X86PassConfig::addRegAssignAndRewriteOptimized() {
  // Don't support tile RA when RA is specified by command line "-regalloc".
  if (!isCustomizedRegAlloc() && EnableTileRAPass) {
    // Allocate tile register first.
    addPass(createGreedyRegisterAllocator(onlyAllocateTileRegisters));
    addPass(createX86TileConfigPass());
  }
  return TargetPassConfig::addRegAssignAndRewriteOptimized();
}
