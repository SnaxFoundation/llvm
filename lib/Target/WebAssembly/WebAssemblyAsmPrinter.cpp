//===-- WebAssemblyAsmPrinter.cpp - WebAssembly LLVM assembly writer ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains a printer that converts from our internal
/// representation of machine-dependent LLVM code to the WebAssembly assembly
/// language.
///
//===----------------------------------------------------------------------===//

#include "WebAssemblyAsmPrinter.h"
#include "InstPrinter/WebAssemblyInstPrinter.h"
#include "MCTargetDesc/WebAssemblyMCTargetDesc.h"
#include "MCTargetDesc/WebAssemblyTargetStreamer.h"
#include "WebAssembly.h"
#include "WebAssemblyMCInstLower.h"
#include "WebAssemblyMachineFunctionInfo.h"
#include "WebAssemblyRegisterInfo.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSectionWasm.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCSymbolELF.h"
#include "llvm/MC/MCSymbolWasm.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "asm-printer"

//===----------------------------------------------------------------------===//
// Helpers.
//===----------------------------------------------------------------------===//

MVT WebAssemblyAsmPrinter::getRegType(unsigned RegNo) const {
  const TargetRegisterInfo *TRI = Subtarget->getRegisterInfo();
  const TargetRegisterClass *TRC = MRI->getRegClass(RegNo);
  for (MVT T : {MVT::i32, MVT::i64, MVT::f32, MVT::f64, MVT::v16i8, MVT::v8i16,
                MVT::v4i32, MVT::v4f32})
    if (TRI->isTypeLegalForClass(*TRC, T))
      return T;
  LLVM_DEBUG(errs() << "Unknown type for register number: " << RegNo);
  llvm_unreachable("Unknown register type");
  return MVT::Other;
}

std::string WebAssemblyAsmPrinter::regToString(const MachineOperand &MO) {
  unsigned RegNo = MO.getReg();
  assert(TargetRegisterInfo::isVirtualRegister(RegNo) &&
         "Unlowered physical register encountered during assembly printing");
  assert(!MFI->isVRegStackified(RegNo));
  unsigned WAReg = MFI->getWAReg(RegNo);
  assert(WAReg != WebAssemblyFunctionInfo::UnusedReg);
  return '$' + utostr(WAReg);
}

WebAssemblyTargetStreamer *WebAssemblyAsmPrinter::getTargetStreamer() {
  MCTargetStreamer *TS = OutStreamer->getTargetStreamer();
  return static_cast<WebAssemblyTargetStreamer *>(TS);
}

//===----------------------------------------------------------------------===//
// WebAssemblyAsmPrinter Implementation.
//===----------------------------------------------------------------------===//
uint64_t char_to_symbol( char c ) {
   if( c >= 'a' && c <= 'z' )
      return (c - 'a') + 6;
   if( c >= '1' && c <= '5' )
      return (c - '1') + 1;
   return 0;
}

uint64_t string_to_name( const char* str ) {
   uint64_t name = 0;
   int i = 0;
   for ( ; str[i] && i < 12; ++i) {
       // NOTE: char_to_symbol() returns char type, and without this explicit
       // expansion to uint64 type, the compilation fails at the point of usage
       // of string_to_name(), where the usage requires constant (compile time) expression.
        name |= (char_to_symbol(str[i]) & 0x1f) << (64 - 5 * (i + 1));
    }

   // The for-loop encoded up to 60 high bits into uint64 'name' variable,
   // if (strlen(str) > 12) then encode str[12] into the low (remaining)
   // 4 bits of 'name'
   if (i == 12)
       name |= char_to_symbol(str[12]) & 0x0F;
   return name;
}

void WebAssemblyAsmPrinter::EmitEndOfAsmFile(Module &M) {
  for (const auto &F : M) {
    // Emit function type info for all undefined functions
    if (F.isDeclarationForLinker() && !F.isIntrinsic()) {
      SmallVector<MVT, 4> Results;
      SmallVector<MVT, 4> Params;
      ComputeSignatureVTs(F, TM, Params, Results);
      MCSymbol *Sym = getSymbol(&F);
      getTargetStreamer()->emitIndirectFunctionType(Sym, Params, Results);

      if (TM.getTargetTriple().isOSBinFormatWasm() &&
          F.hasFnAttribute("wasm-import-module")) {
        MCSymbolWasm *WasmSym = cast<MCSymbolWasm>(Sym);
        StringRef Name = F.getFnAttribute("wasm-import-module")
                             .getValueAsString();
        getTargetStreamer()->emitImportModule(WasmSym, Name);
      }
    }
  }
  for (const auto &G : M.globals()) {
    if (!G.hasInitializer() && G.hasExternalLinkage()) {
      if (G.getValueType()->isSized()) {
        uint16_t Size = M.getDataLayout().getTypeAllocSize(G.getValueType());
        if (TM.getTargetTriple().isOSBinFormatELF())
          getTargetStreamer()->emitGlobalImport(G.getGlobalIdentifier());
        OutStreamer->emitELFSize(getSymbol(&G),
                                 MCConstantExpr::create(Size, OutContext));
      }
    }
  }

  if (const NamedMDNode *Named = M.getNamedMetadata("wasm.custom_sections")) {
    for (const Metadata *MD : Named->operands()) {
      const MDTuple *Tuple = dyn_cast<MDTuple>(MD);
      if (!Tuple || Tuple->getNumOperands() != 2)
        continue;
      const MDString *Name = dyn_cast<MDString>(Tuple->getOperand(0));
      const MDString *Contents = dyn_cast<MDString>(Tuple->getOperand(1));
      if (!Name || !Contents)
        continue;

      OutStreamer->PushSection();
      std::string SectionName = (".custom_section." + Name->getString()).str();
      MCSectionWasm *mySection =
          OutContext.getWasmSection(SectionName, SectionKind::getMetadata());
      OutStreamer->SwitchSection(mySection);
      OutStreamer->EmitBytes(Contents->getString());
      OutStreamer->PopSection();
    }
  }
   
  bool has_import = false;
  bool has_abi    = false;
  bool has_snax_action = false;
  bool has_snax_notify = false;
  for (const auto &F : M) {
     if (F.hasFnAttribute("snax_wasm_import"))
        has_import = true;
     if (F.hasFnAttribute("snax_wasm_abi"))
        has_abi = true;
     if (F.hasFnAttribute("snax_wasm_action"))
        has_snax_action = true;
     if (F.hasFnAttribute("snax_wasm_notify"))
        has_snax_notify = true;
  }

  if (has_import) {
     OutStreamer->PushSection();
     std::string SectionName = ".imports";
     MCSectionWasm *mySection =
         OutContext.getWasmSection(SectionName, SectionKind::getMetadata());
     OutStreamer->SwitchSection(mySection);
     for (const auto &F : M) {
        if (F.hasFnAttribute("snax_wasm_import")) {
           OutStreamer->EmitULEB128IntValue(F.getName().size());
           OutStreamer->EmitBytes(F.getName());
        }
     }
     OutStreamer->PopSection();
  }
  if (has_abi) {
     OutStreamer->PushSection(); 
     std::string SectionName = ".snax_abi";
     MCSectionWasm *mySection =
         OutContext.getWasmSection(SectionName, SectionKind::getMetadata());
     OutStreamer->SwitchSection(mySection);
     for (const auto &F : M) {
        if (F.hasFnAttribute("snax_wasm_abi")) {
           StringRef abi = F.getFnAttribute("snax_wasm_abi").getValueAsString();
           OutStreamer->EmitULEB128IntValue(abi.size());
           OutStreamer->EmitBytes(abi);
        }
     }
     OutStreamer->PopSection();
  }
  if (has_snax_action) {
     OutStreamer->PushSection(); 
     std::string SectionName = ".snax_actions";
     MCSectionWasm *mySection =
         OutContext.getWasmSection(SectionName, SectionKind::getMetadata());
     OutStreamer->SwitchSection(mySection);
     for (const auto &F : M) {
        if (F.hasFnAttribute("snax_wasm_action")) {
           StringRef action_name = F.getFnAttribute("snax_wasm_action").getValueAsString();
           OutStreamer->EmitULEB128IntValue(action_name.size());
           OutStreamer->EmitBytes(action_name);
        }
     }
     OutStreamer->PopSection();
  }
  if (has_snax_notify) {
     OutStreamer->PushSection(); 
     std::string SectionName = ".snax_notify";
     MCSectionWasm *mySection =
         OutContext.getWasmSection(SectionName, SectionKind::getMetadata());
     OutStreamer->SwitchSection(mySection);
     for (const auto &F : M) {
        if (F.hasFnAttribute("snax_wasm_notify")) {
           StringRef name = F.getFnAttribute("snax_wasm_notify").getValueAsString();
           OutStreamer->EmitULEB128IntValue(name.size());
           OutStreamer->EmitBytes(name);
        }
     }
     OutStreamer->PopSection();
  }

}

void WebAssemblyAsmPrinter::EmitConstantPool() {
  assert(MF->getConstantPool()->getConstants().empty() &&
         "WebAssembly disables constant pools");
}

void WebAssemblyAsmPrinter::EmitJumpTableInfo() {
  // Nothing to do; jump tables are incorporated into the instruction stream.
}

void WebAssemblyAsmPrinter::EmitFunctionBodyStart() {
  getTargetStreamer()->emitParam(CurrentFnSym, MFI->getParams());

  SmallVector<MVT, 4> ResultVTs;
  const Function &F = MF->getFunction();

  // Emit the function index.
  if (MDNode *Idx = F.getMetadata("wasm.index")) {
    assert(Idx->getNumOperands() == 1);

    getTargetStreamer()->emitIndIdx(AsmPrinter::lowerConstant(
        cast<ConstantAsMetadata>(Idx->getOperand(0))->getValue()));
  }

  ComputeLegalValueVTs(F, TM, F.getReturnType(), ResultVTs);

  // If the return type needs to be legalized it will get converted into
  // passing a pointer.
  if (ResultVTs.size() == 1)
    getTargetStreamer()->emitResult(CurrentFnSym, ResultVTs);
  else
    getTargetStreamer()->emitResult(CurrentFnSym, ArrayRef<MVT>());

  if (TM.getTargetTriple().isOSBinFormatELF()) {
    assert(MFI->getLocals().empty());
    for (unsigned Idx = 0, IdxE = MRI->getNumVirtRegs(); Idx != IdxE; ++Idx) {
      unsigned VReg = TargetRegisterInfo::index2VirtReg(Idx);
      unsigned WAReg = MFI->getWAReg(VReg);
      // Don't declare unused registers.
      if (WAReg == WebAssemblyFunctionInfo::UnusedReg)
        continue;
      // Don't redeclare parameters.
      if (WAReg < MFI->getParams().size())
        continue;
      // Don't declare stackified registers.
      if (int(WAReg) < 0)
        continue;
      MFI->addLocal(getRegType(VReg));
    }
  }

  getTargetStreamer()->emitLocal(MFI->getLocals());

  AsmPrinter::EmitFunctionBodyStart();
}

void WebAssemblyAsmPrinter::EmitFunctionBodyEnd() {
  if (TM.getTargetTriple().isOSBinFormatELF())
    getTargetStreamer()->emitEndFunc();
}

void WebAssemblyAsmPrinter::EmitInstruction(const MachineInstr *MI) {
  LLVM_DEBUG(dbgs() << "EmitInstruction: " << *MI << '\n');

  switch (MI->getOpcode()) {
  case WebAssembly::ARGUMENT_I32:
  case WebAssembly::ARGUMENT_I64:
  case WebAssembly::ARGUMENT_F32:
  case WebAssembly::ARGUMENT_F64:
  case WebAssembly::ARGUMENT_v16i8:
  case WebAssembly::ARGUMENT_v8i16:
  case WebAssembly::ARGUMENT_v4i32:
  case WebAssembly::ARGUMENT_v4f32:
    // These represent values which are live into the function entry, so there's
    // no instruction to emit.
    break;
  case WebAssembly::FALLTHROUGH_RETURN_I32:
  case WebAssembly::FALLTHROUGH_RETURN_I64:
  case WebAssembly::FALLTHROUGH_RETURN_F32:
  case WebAssembly::FALLTHROUGH_RETURN_F64:
  case WebAssembly::FALLTHROUGH_RETURN_v16i8:
  case WebAssembly::FALLTHROUGH_RETURN_v8i16:
  case WebAssembly::FALLTHROUGH_RETURN_v4i32:
  case WebAssembly::FALLTHROUGH_RETURN_v4f32: {
    // These instructions represent the implicit return at the end of a
    // function body. The operand is always a pop.
    assert(MFI->isVRegStackified(MI->getOperand(0).getReg()));

    if (isVerbose()) {
      OutStreamer->AddComment("fallthrough-return: $pop" +
                              Twine(MFI->getWARegStackId(
                                  MFI->getWAReg(MI->getOperand(0).getReg()))));
      OutStreamer->AddBlankLine();
    }
    break;
  }
  case WebAssembly::FALLTHROUGH_RETURN_VOID:
    // This instruction represents the implicit return at the end of a
    // function body with no return value.
    if (isVerbose()) {
      OutStreamer->AddComment("fallthrough-return");
      OutStreamer->AddBlankLine();
    }
    break;
  default: {
    WebAssemblyMCInstLower MCInstLowering(OutContext, *this);
    MCInst TmpInst;
    MCInstLowering.Lower(MI, TmpInst);
    EmitToStreamer(*OutStreamer, TmpInst);
    break;
  }
  }
}

const MCExpr *WebAssemblyAsmPrinter::lowerConstant(const Constant *CV) {
  if (const GlobalValue *GV = dyn_cast<GlobalValue>(CV))
    if (GV->getValueType()->isFunctionTy()) {
      return MCSymbolRefExpr::create(
          getSymbol(GV), MCSymbolRefExpr::VK_WebAssembly_FUNCTION, OutContext);
    }
  return AsmPrinter::lowerConstant(CV);
}

bool WebAssemblyAsmPrinter::PrintAsmOperand(const MachineInstr *MI,
                                            unsigned OpNo, unsigned AsmVariant,
                                            const char *ExtraCode,
                                            raw_ostream &OS) {
  if (AsmVariant != 0)
    report_fatal_error("There are no defined alternate asm variants");

  // First try the generic code, which knows about modifiers like 'c' and 'n'.
  if (!AsmPrinter::PrintAsmOperand(MI, OpNo, AsmVariant, ExtraCode, OS))
    return false;

  if (!ExtraCode) {
    const MachineOperand &MO = MI->getOperand(OpNo);
    switch (MO.getType()) {
    case MachineOperand::MO_Immediate:
      OS << MO.getImm();
      return false;
    case MachineOperand::MO_Register:
      OS << regToString(MO);
      return false;
    case MachineOperand::MO_GlobalAddress:
      getSymbol(MO.getGlobal())->print(OS, MAI);
      printOffset(MO.getOffset(), OS);
      return false;
    case MachineOperand::MO_ExternalSymbol:
      GetExternalSymbolSymbol(MO.getSymbolName())->print(OS, MAI);
      printOffset(MO.getOffset(), OS);
      return false;
    case MachineOperand::MO_MachineBasicBlock:
      MO.getMBB()->getSymbol()->print(OS, MAI);
      return false;
    default:
      break;
    }
  }

  return true;
}

bool WebAssemblyAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                                  unsigned OpNo,
                                                  unsigned AsmVariant,
                                                  const char *ExtraCode,
                                                  raw_ostream &OS) {
  if (AsmVariant != 0)
    report_fatal_error("There are no defined alternate asm variants");

  // The current approach to inline asm is that "r" constraints are expressed
  // as local indices, rather than values on the operand stack. This simplifies
  // using "r" as it eliminates the need to push and pop the values in a
  // particular order, however it also makes it impossible to have an "m"
  // constraint. So we don't support it.

  return AsmPrinter::PrintAsmMemoryOperand(MI, OpNo, AsmVariant, ExtraCode, OS);
}

// Force static initialization.
extern "C" void LLVMInitializeWebAssemblyAsmPrinter() {
  RegisterAsmPrinter<WebAssemblyAsmPrinter> X(getTheWebAssemblyTarget32());
  RegisterAsmPrinter<WebAssemblyAsmPrinter> Y(getTheWebAssemblyTarget64());
}
