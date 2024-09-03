//===- lib/MC/MCAssembler.cpp - Assembler Backend Implementation ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCAssembler.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAsmLayout.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCCodeView.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCFixupKindInfo.h"
#include "llvm/MC/MCFragment.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <utility>

//DBL
#include "toml/toml.hpp"
#include "llvm/DBLCLIArgs.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include <fstream>

using namespace llvm;

#define DEBUG_TYPE "assembler"

namespace {
namespace stats {

STATISTIC(EmittedFragments, "Number of emitted assembler fragments - total");
STATISTIC(EmittedRelaxableFragments,
          "Number of emitted assembler fragments - relaxable");
STATISTIC(EmittedDataFragments,
          "Number of emitted assembler fragments - data");
STATISTIC(EmittedCompactEncodedInstFragments,
          "Number of emitted assembler fragments - compact encoded inst");
STATISTIC(EmittedAlignFragments,
          "Number of emitted assembler fragments - align");
STATISTIC(EmittedFillFragments,
          "Number of emitted assembler fragments - fill");
STATISTIC(EmittedOrgFragments,
          "Number of emitted assembler fragments - org");
STATISTIC(evaluateFixup, "Number of evaluated fixups");
STATISTIC(FragmentLayouts, "Number of fragment layouts");
STATISTIC(ObjectBytes, "Number of emitted object file bytes");
STATISTIC(RelaxationSteps, "Number of assembler layout and relaxation steps");
STATISTIC(RelaxedInstructions, "Number of relaxed instructions");

} // end namespace stats
} // end anonymous namespace

// FIXME FIXME FIXME: There are number of places in this file where we convert
// what is a 64-bit assembler value used for computation into a value in the
// object file, which may truncate it. We should detect that truncation where
// invalid and report errors back.

/* *** */

MCAssembler::MCAssembler(MCContext &Context,
                         std::unique_ptr<MCAsmBackend> Backend,
                         std::unique_ptr<MCCodeEmitter> Emitter,
                         std::unique_ptr<MCObjectWriter> Writer)
    : Context(Context), Backend(std::move(Backend)),
      Emitter(std::move(Emitter)), Writer(std::move(Writer)),
      BundleAlignSize(0), RelaxAll(false), SubsectionsViaSymbols(false),
      IncrementalLinkerCompatible(false), ELFHeaderEFlags(0) {
  VersionInfo.Major = 0; // Major version == 0 for "none specified"
}

MCAssembler::~MCAssembler() = default;

void MCAssembler::reset() {
  Sections.clear();
  Symbols.clear();
  IndirectSymbols.clear();
  DataRegions.clear();
  LinkerOptions.clear();
  FileNames.clear();
  ThumbFuncs.clear();
  BundleAlignSize = 0;
  RelaxAll = false;
  SubsectionsViaSymbols = false;
  IncrementalLinkerCompatible = false;
  ELFHeaderEFlags = 0;
  LOHContainer.reset();
  VersionInfo.Major = 0;
  VersionInfo.SDKVersion = VersionTuple();

  // reset objects owned by us
  if (getBackendPtr())
    getBackendPtr()->reset();
  if (getEmitterPtr())
    getEmitterPtr()->reset();
  if (getWriterPtr())
    getWriterPtr()->reset();
  getLOHContainer().reset();
}

bool MCAssembler::registerSection(MCSection &Section) {
  if (Section.isRegistered())
    return false;
  Sections.push_back(&Section);
  Section.setIsRegistered(true);
  return true;
}

bool MCAssembler::isThumbFunc(const MCSymbol *Symbol) const {
  if (ThumbFuncs.count(Symbol))
    return true;

  if (!Symbol->isVariable())
    return false;

  const MCExpr *Expr = Symbol->getVariableValue();

  MCValue V;
  if (!Expr->evaluateAsRelocatable(V, nullptr, nullptr))
    return false;

  if (V.getSymB() || V.getRefKind() != MCSymbolRefExpr::VK_None)
    return false;

  const MCSymbolRefExpr *Ref = V.getSymA();
  if (!Ref)
    return false;

  if (Ref->getKind() != MCSymbolRefExpr::VK_None)
    return false;

  const MCSymbol &Sym = Ref->getSymbol();
  if (!isThumbFunc(&Sym))
    return false;

  ThumbFuncs.insert(Symbol); // Cache it.
  return true;
}

bool MCAssembler::isSymbolLinkerVisible(const MCSymbol &Symbol) const {
  // Non-temporary labels should always be visible to the linker.
  if (!Symbol.isTemporary())
    return true;

  if (Symbol.isUsedInReloc())
    return true;

  return false;
}

const MCSymbol *MCAssembler::getAtom(const MCSymbol &S) const {
  // Linker visible symbols define atoms.
  if (isSymbolLinkerVisible(S))
    return &S;

  // Absolute and undefined symbols have no defining atom.
  if (!S.isInSection())
    return nullptr;

  // Non-linker visible symbols in sections which can't be atomized have no
  // defining atom.
  if (!getContext().getAsmInfo()->isSectionAtomizableBySymbols(
          *S.getFragment()->getParent()))
    return nullptr;

  // Otherwise, return the atom for the containing fragment.
  return S.getFragment()->getAtom();
}

bool MCAssembler::evaluateFixup(const MCAsmLayout &Layout,
                                const MCFixup &Fixup, const MCFragment *DF,
                                MCValue &Target, uint64_t &Value,
                                bool &WasForced) const {
  ++stats::evaluateFixup;

  // FIXME: This code has some duplication with recordRelocation. We should
  // probably merge the two into a single callback that tries to evaluate a
  // fixup and records a relocation if one is needed.

  // On error claim to have completely evaluated the fixup, to prevent any
  // further processing from being done.
  const MCExpr *Expr = Fixup.getValue();
  MCContext &Ctx = getContext();
  Value = 0;
  WasForced = false;
  if (!Expr->evaluateAsRelocatable(Target, &Layout, &Fixup)) {
    Ctx.reportError(Fixup.getLoc(), "expected relocatable expression");
    return true;
  }
  if (const MCSymbolRefExpr *RefB = Target.getSymB()) {
    if (RefB->getKind() != MCSymbolRefExpr::VK_None) {
      Ctx.reportError(Fixup.getLoc(),
                      "unsupported subtraction of qualified symbol");
      return true;
    }
  }

  assert(getBackendPtr() && "Expected assembler backend");
  bool IsTarget = getBackendPtr()->getFixupKindInfo(Fixup.getKind()).Flags &
                  MCFixupKindInfo::FKF_IsTarget;

  if (IsTarget)
    return getBackend().evaluateTargetFixup(*this, Layout, Fixup, DF, Target,
                                            Value, WasForced);

  unsigned FixupFlags = getBackendPtr()->getFixupKindInfo(Fixup.getKind()).Flags;
  bool IsPCRel = getBackendPtr()->getFixupKindInfo(Fixup.getKind()).Flags &
                 MCFixupKindInfo::FKF_IsPCRel;

  bool IsResolved = false;
  if (IsPCRel) {
    if (Target.getSymB()) {
      IsResolved = false;
    } else if (!Target.getSymA()) {
      IsResolved = false;
    } else {
      const MCSymbolRefExpr *A = Target.getSymA();
      const MCSymbol &SA = A->getSymbol();
      if (A->getKind() != MCSymbolRefExpr::VK_None || SA.isUndefined()) {
        IsResolved = false;
      } else if (auto *Writer = getWriterPtr()) {
        IsResolved = (FixupFlags & MCFixupKindInfo::FKF_Constant) ||
                     Writer->isSymbolRefDifferenceFullyResolvedImpl(
                         *this, SA, *DF, false, true);
      }
    }
  } else {
    IsResolved = Target.isAbsolute();
  }

  Value = Target.getConstant();

  if (const MCSymbolRefExpr *A = Target.getSymA()) {
    const MCSymbol &Sym = A->getSymbol();
    if (Sym.isDefined())
      Value += Layout.getSymbolOffset(Sym);
  }
  if (const MCSymbolRefExpr *B = Target.getSymB()) {
    const MCSymbol &Sym = B->getSymbol();
    if (Sym.isDefined())
      Value -= Layout.getSymbolOffset(Sym);
  }

  bool ShouldAlignPC = getBackend().getFixupKindInfo(Fixup.getKind()).Flags &
                       MCFixupKindInfo::FKF_IsAlignedDownTo32Bits;
  assert((ShouldAlignPC ? IsPCRel : true) &&
    "FKF_IsAlignedDownTo32Bits is only allowed on PC-relative fixups!");

  if (IsPCRel) {
    uint32_t Offset = Layout.getFragmentOffset(DF) + Fixup.getOffset();

    // A number of ARM fixups in Thumb mode require that the effective PC
    // address be determined as the 32-bit aligned version of the actual offset.
    if (ShouldAlignPC) Offset &= ~0x3;
    Value -= Offset;
  }

  // Let the backend force a relocation if needed.
  if (IsResolved && getBackend().shouldForceRelocation(*this, Fixup, Target)) {
    IsResolved = false;
    WasForced = true;
  }

  return IsResolved;
}

uint64_t MCAssembler::computeFragmentSize(const MCAsmLayout &Layout,
                                          const MCFragment &F) const {
  assert(getBackendPtr() && "Requires assembler backend");
  switch (F.getKind()) {
  case MCFragment::FT_Data:
    return cast<MCDataFragment>(F).getContents().size();
  case MCFragment::FT_Relaxable:
    return cast<MCRelaxableFragment>(F).getContents().size();
  case MCFragment::FT_CompactEncodedInst:
    return cast<MCCompactEncodedInstFragment>(F).getContents().size();
  case MCFragment::FT_Fill: {
    auto &FF = cast<MCFillFragment>(F);
    int64_t NumValues = 0;
    if (!FF.getNumValues().evaluateAsAbsolute(NumValues, Layout)) {
      getContext().reportError(FF.getLoc(),
                               "expected assembly-time absolute expression");
      return 0;
    }
    int64_t Size = NumValues * FF.getValueSize();
    if (Size < 0) {
      getContext().reportError(FF.getLoc(), "invalid number of bytes");
      return 0;
    }
    return Size;
  }

  case MCFragment::FT_LEB:
    return cast<MCLEBFragment>(F).getContents().size();

  case MCFragment::FT_BoundaryAlign:
    return cast<MCBoundaryAlignFragment>(F).getSize();

  case MCFragment::FT_SymbolId:
    return 4;

  case MCFragment::FT_Align: {
    //DBL MCAlignFragments in .dbl_text section have alignment 1, therefore, size is 0
    //if(DBLMode != Baseline &&
    //   F.getParent() == getContext().getObjectFileInfo()->getDBLTextSection() )
    //  return 0; //why did i do this? assert fails bcs layout is not final

    const MCAlignFragment &AF = cast<MCAlignFragment>(F);
    unsigned Offset = Layout.getFragmentOffset(&AF);
    unsigned Size = offsetToAlignment(Offset, Align(AF.getAlignment()));

    // Insert extra Nops for code alignment if the target define
    // shouldInsertExtraNopBytesForCodeAlign target hook.
    if (AF.getParent()->UseCodeAlign() && AF.hasEmitNops() &&
        getBackend().shouldInsertExtraNopBytesForCodeAlign(AF, Size))
      return Size;

    // If we are padding with nops, force the padding to be larger than the
    // minimum nop size.
    if (Size > 0 && AF.hasEmitNops()) {
      while (Size % getBackend().getMinimumNopSize())
        Size += AF.getAlignment();
    }
    if (Size > AF.getMaxBytesToEmit())
      return 0;
    return Size;
  }

  case MCFragment::FT_Org: {
    const MCOrgFragment &OF = cast<MCOrgFragment>(F);
    MCValue Value;
    if (!OF.getOffset().evaluateAsValue(Value, Layout)) {
      getContext().reportError(OF.getLoc(),
                               "expected assembly-time absolute expression");
        return 0;
    }

    uint64_t FragmentOffset = Layout.getFragmentOffset(&OF);
    int64_t TargetLocation = Value.getConstant();
    if (const MCSymbolRefExpr *A = Value.getSymA()) {
      uint64_t Val;
      if (!Layout.getSymbolOffset(A->getSymbol(), Val)) {
        getContext().reportError(OF.getLoc(), "expected absolute expression");
        return 0;
      }
      TargetLocation += Val;
    }
    int64_t Size = TargetLocation - FragmentOffset;
    if (Size < 0 || Size >= 0x40000000) {
      getContext().reportError(
          OF.getLoc(), "invalid .org offset '" + Twine(TargetLocation) +
                           "' (at offset '" + Twine(FragmentOffset) + "')");
      return 0;
    }
    return Size;
  }

  case MCFragment::FT_Dwarf:
    return cast<MCDwarfLineAddrFragment>(F).getContents().size();
  case MCFragment::FT_DwarfFrame:
    return cast<MCDwarfCallFrameFragment>(F).getContents().size();
  case MCFragment::FT_CVInlineLines:
    return cast<MCCVInlineLineTableFragment>(F).getContents().size();
  case MCFragment::FT_CVDefRange:
    return cast<MCCVDefRangeFragment>(F).getContents().size();
  case MCFragment::FT_Dummy:
    llvm_unreachable("Should not have been added");
  }

  llvm_unreachable("invalid fragment kind");
}

void MCAsmLayout::layoutFragment(MCFragment *F) {
  MCFragment *Prev = F->getPrevNode();

  // We should never try to recompute something which is valid.
  assert(!isFragmentValid(F) && "Attempt to recompute a valid fragment!");
  // We should never try to compute the fragment layout if its predecessor
  // isn't valid.
  assert((!Prev || isFragmentValid(Prev)) &&
         "Attempt to compute fragment before its predecessor!");

  assert(!F->IsBeingLaidOut && "Already being laid out!");
  F->IsBeingLaidOut = true;

  ++stats::FragmentLayouts;

  // Compute fragment offset and size.
  if (Prev)
    F->Offset = Prev->Offset + getAssembler().computeFragmentSize(*this, *Prev);
  else
    F->Offset = 0;
  F->IsBeingLaidOut = false;
  LastValidFragment[F->getParent()] = F;

  // If bundling is enabled and this fragment has instructions in it, it has to
  // obey the bundling restrictions. With padding, we'll have:
  //
  //
  //        BundlePadding
  //             |||
  // -------------------------------------
  //   Prev  |##########|       F        |
  // -------------------------------------
  //                    ^
  //                    |
  //                    F->Offset
  //
  // The fragment's offset will point to after the padding, and its computed
  // size won't include the padding.
  //
  // When the -mc-relax-all flag is used, we optimize bundling by writting the
  // padding directly into fragments when the instructions are emitted inside
  // the streamer. When the fragment is larger than the bundle size, we need to
  // ensure that it's bundle aligned. This means that if we end up with
  // multiple fragments, we must emit bundle padding between fragments.
  //
  // ".align N" is an example of a directive that introduces multiple
  // fragments. We could add a special case to handle ".align N" by emitting
  // within-fragment padding (which would produce less padding when N is less
  // than the bundle size), but for now we don't.
  //
  if (Assembler.isBundlingEnabled() && F->hasInstructions()) {
    //DBL
    assert(false && "Bundling not supported");

    assert(isa<MCEncodedFragment>(F) &&
           "Only MCEncodedFragment implementations have instructions");
    MCEncodedFragment *EF = cast<MCEncodedFragment>(F);
    uint64_t FSize = Assembler.computeFragmentSize(*this, *EF);

    if (!Assembler.getRelaxAll() && FSize > Assembler.getBundleAlignSize())
      report_fatal_error("Fragment can't be larger than a bundle size");

    uint64_t RequiredBundlePadding =
        computeBundlePadding(Assembler, EF, EF->Offset, FSize);
    if (RequiredBundlePadding > UINT8_MAX)
      report_fatal_error("Padding cannot exceed 255 bytes");
    EF->setBundlePadding(static_cast<uint8_t>(RequiredBundlePadding));
    EF->Offset += RequiredBundlePadding;
  }
}

void MCAssembler::registerSymbol(const MCSymbol &Symbol, bool *Created) {
  bool New = !Symbol.isRegistered();
  if (Created)
    *Created = New;
  if (New) {
    Symbol.setIsRegistered(true);
    Symbols.push_back(&Symbol);
  }
}

void MCAssembler::writeFragmentPadding(raw_ostream &OS,
                                       const MCEncodedFragment &EF,
                                       uint64_t FSize) const {
  assert(getBackendPtr() && "Expected assembler backend");
  // Should NOP padding be written out before this fragment?
  unsigned BundlePadding = EF.getBundlePadding();
  if (BundlePadding > 0) {
    assert(isBundlingEnabled() &&
           "Writing bundle padding with disabled bundling");
    assert(EF.hasInstructions() &&
           "Writing bundle padding for a fragment without instructions");

    unsigned TotalLength = BundlePadding + static_cast<unsigned>(FSize);
    if (EF.alignToBundleEnd() && TotalLength > getBundleAlignSize()) {
      // If the padding itself crosses a bundle boundary, it must be emitted
      // in 2 pieces, since even nop instructions must not cross boundaries.
      //             v--------------v   <- BundleAlignSize
      //        v---------v             <- BundlePadding
      // ----------------------------
      // | Prev |####|####|    F    |
      // ----------------------------
      //        ^-------------------^   <- TotalLength
      unsigned DistanceToBoundary = TotalLength - getBundleAlignSize();
      if (!getBackend().writeNopData(OS, DistanceToBoundary))
        report_fatal_error("unable to write NOP sequence of " +
                           Twine(DistanceToBoundary) + " bytes");
      BundlePadding -= DistanceToBoundary;
    }
    if (!getBackend().writeNopData(OS, BundlePadding))
      report_fatal_error("unable to write NOP sequence of " +
                         Twine(BundlePadding) + " bytes");
  }
}

/// Write the fragment \p F to the output file.
static void writeFragment(raw_ostream &OS, const MCAssembler &Asm,
                          const MCAsmLayout &Layout, const MCFragment &F,
                          uint64_t SecStart, std::string SecName) {
  // FIXME: Embed in fragments instead?
  uint64_t FragmentSize = Asm.computeFragmentSize(Layout, F);

  //DBL
  //For MCRelaxableFragment, the offset may not point to the exact byte but it
  //will after the instruction is relaxed
  const MCDBLFragment* Frag = dyn_cast<MCDBLFragment>(&F);
  auto TargetsToFindItr = Asm.TargetsToFindForSection.find(SecName.c_str());
  //will be empty in ROUND 2 bcs all targets are found by then
  if(Frag && DBLMode == DBLModeT::DBL &&
     TargetsToFindItr != Asm.TargetsToFindForSection.end() &&
     !TargetsToFindItr->second.empty()) {
    //find the fragment for each target offset and calc fragment offset
    //TargetsToFind is sorted ascendingly
    std::list<TargetSpec>& TargetsToFind = TargetsToFindItr->second;
    auto CurrentOffset = OS.tell() - SecStart;
    auto Target = TargetsToFind.begin();
    //look if this fragment contains the next target
    if(Target->isTarget() && Target->TargetOffsetInOutput >= CurrentOffset &&
       Target->TargetOffsetInOutput < CurrentOffset + FragmentSize) {
      uint64_t TargetOffsetInFragment =
        Target->TargetOffsetInOutput - CurrentOffset;
      DEBUG_WITH_TYPE("dbl_trace",
        errs() << format("Found flip target offset 0x%x in:",
                         Target->TargetOffsetInOutput);
        for(unsigned i = 0; i < Frag->getContents().size();) {
          if(i == TargetOffsetInFragment)
            errs().changeColor(raw_fd_ostream::Colors::RED, true);
          unsigned char C = Frag->getContents()[i++];
          errs() << format(" 0x%x", C);
          errs().resetColor();
        }
        if(isa<MCRelaxableFragment>(Frag))
          errs() << " (MCRelaxableFragment => content will change)";
        errs() << "\n"
      );

      Frag->TargetSpec = *Target; //copy
      Frag->TargetSpec->setTargetOffsetInFragment(TargetOffsetInFragment);
      TargetsToFind.erase(Target);
    } else if(std::get_if<TargetDestination>(&Target->Kind)) {
      if(CurrentOffset == Target->TargetOffsetInOutput) {
        Frag->TargetSpec = *Target; //copy
        TargetsToFind.erase(Target);
        DEBUG_WITH_TYPE("dbl_trace",
          errs() << format("Found destination fragment offset 0x%x in:",
                           Frag->TargetSpec->TargetOffsetInOutput);
          for(unsigned i = 0; i < Frag->getContents().size(); i++)
            errs() << format(" 0x%x", (unsigned char)Frag->getContents()[i]);
          errs() << "\n"
        );
      }
    } else if(std::get_if<TargetNone>(&Target->Kind)) {
      Frag->TargetSpec = *Target; //copy
      TargetsToFind.erase(Target);
      DEBUG_WITH_TYPE("dbl_trace",
          errs() << format("Found ignored fragment offset 0x%x\n",
                           Frag->TargetSpec->TargetOffsetInOutput);
      );
    }
  }

  support::endianness Endian = Asm.getBackend().Endian;

  if (const MCEncodedFragment *EF = dyn_cast<MCEncodedFragment>(&F))
    Asm.writeFragmentPadding(OS, *EF, FragmentSize);

  // This variable (and its dummy usage) is to participate in the assert at
  // the end of the function.
  uint64_t Start = OS.tell();
  (void) Start;

  ++stats::EmittedFragments;

  switch (F.getKind()) {
  case MCFragment::FT_Align: {
    ++stats::EmittedAlignFragments;
    const MCAlignFragment &AF = cast<MCAlignFragment>(F);
    assert(AF.getValueSize() && "Invalid virtual align in concrete fragment!");

    uint64_t Count = FragmentSize / AF.getValueSize();

    // FIXME: This error shouldn't actually occur (the front end should emit
    // multiple .align directives to enforce the semantics it wants), but is
    // severe enough that we want to report it. How to handle this?
    if (Count * AF.getValueSize() != FragmentSize)
      report_fatal_error("undefined .align directive, value size '" +
                        Twine(AF.getValueSize()) +
                        "' is not a divisor of padding size '" +
                        Twine(FragmentSize) + "'");

    // See if we are aligning with nops, and if so do that first to try to fill
    // the Count bytes.  Then if that did not fill any bytes or there are any
    // bytes left to fill use the Value and ValueSize to fill the rest.
    // If we are aligning with nops, ask that target to emit the right data.
    if (AF.hasEmitNops()) {
      if (!Asm.getBackend().writeNopData(OS, Count))
        report_fatal_error("unable to write nop sequence of " +
                          Twine(Count) + " bytes");
      break;
    }

    // Otherwise, write out in multiples of the value size.
    for (uint64_t i = 0; i != Count; ++i) {
      switch (AF.getValueSize()) {
      default: llvm_unreachable("Invalid size!");
      case 1: OS << char(AF.getValue()); break;
      case 2:
        support::endian::write<uint16_t>(OS, AF.getValue(), Endian);
        break;
      case 4:
        support::endian::write<uint32_t>(OS, AF.getValue(), Endian);
        break;
      case 8:
        support::endian::write<uint64_t>(OS, AF.getValue(), Endian);
        break;
      }
    }
    break;
  }

  case MCFragment::FT_Data:
    ++stats::EmittedDataFragments;
    OS << cast<MCDataFragment>(F).getContents();
    break;

  case MCFragment::FT_Relaxable:
    ++stats::EmittedRelaxableFragments;
    OS << cast<MCRelaxableFragment>(F).getContents();
    break;

  case MCFragment::FT_CompactEncodedInst:
    ++stats::EmittedCompactEncodedInstFragments;
    OS << cast<MCCompactEncodedInstFragment>(F).getContents();
    break;

  case MCFragment::FT_Fill: {
    ++stats::EmittedFillFragments;
    const MCFillFragment &FF = cast<MCFillFragment>(F);
    uint64_t V = FF.getValue();
    unsigned VSize = FF.getValueSize();
    const unsigned MaxChunkSize = 16;
    char Data[MaxChunkSize];
    assert(0 < VSize && VSize <= MaxChunkSize && "Illegal fragment fill size");
    // Duplicate V into Data as byte vector to reduce number of
    // writes done. As such, do endian conversion here.
    for (unsigned I = 0; I != VSize; ++I) {
      unsigned index = Endian == support::little ? I : (VSize - I - 1);
      Data[I] = uint8_t(V >> (index * 8));
    }
    for (unsigned I = VSize; I < MaxChunkSize; ++I)
      Data[I] = Data[I - VSize];

    // Set to largest multiple of VSize in Data.
    const unsigned NumPerChunk = MaxChunkSize / VSize;
    // Set ChunkSize to largest multiple of VSize in Data
    const unsigned ChunkSize = VSize * NumPerChunk;

    // Do copies by chunk.
    StringRef Ref(Data, ChunkSize);
    for (uint64_t I = 0, E = FragmentSize / ChunkSize; I != E; ++I)
      OS << Ref;

    // do remainder if needed.
    unsigned TrailingCount = FragmentSize % ChunkSize;
    if (TrailingCount)
      OS.write(Data, TrailingCount);
    break;
  }

  case MCFragment::FT_LEB: {
    const MCLEBFragment &LF = cast<MCLEBFragment>(F);
    OS << LF.getContents();
    break;
  }

  case MCFragment::FT_BoundaryAlign: {
    if (!Asm.getBackend().writeNopData(OS, FragmentSize))
      report_fatal_error("unable to write nop sequence of " +
                         Twine(FragmentSize) + " bytes");
    break;
  }

  case MCFragment::FT_SymbolId: {
    const MCSymbolIdFragment &SF = cast<MCSymbolIdFragment>(F);
    support::endian::write<uint32_t>(OS, SF.getSymbol()->getIndex(), Endian);
    break;
  }

  case MCFragment::FT_Org: {
    ++stats::EmittedOrgFragments;
    const MCOrgFragment &OF = cast<MCOrgFragment>(F);

    for (uint64_t i = 0, e = FragmentSize; i != e; ++i)
      OS << char(OF.getValue());

    break;
  }

  case MCFragment::FT_Dwarf: {
    const MCDwarfLineAddrFragment &OF = cast<MCDwarfLineAddrFragment>(F);
    OS << OF.getContents();
    break;
  }
  case MCFragment::FT_DwarfFrame: {
    const MCDwarfCallFrameFragment &CF = cast<MCDwarfCallFrameFragment>(F);
    OS << CF.getContents();
    break;
  }
  case MCFragment::FT_CVInlineLines: {
    const auto &OF = cast<MCCVInlineLineTableFragment>(F);
    OS << OF.getContents();
    break;
  }
  case MCFragment::FT_CVDefRange: {
    const auto &DRF = cast<MCCVDefRangeFragment>(F);
    OS << DRF.getContents();
    break;
  }
  case MCFragment::FT_Dummy:
    llvm_unreachable("Should not have been added");
  }

  assert(OS.tell() - Start == FragmentSize &&
         "The stream should advance by fragment size");
}

void MCAssembler::writeSectionData(raw_ostream &OS, const MCSection *Sec,
                                   const MCAsmLayout &Layout) const {
  assert(getBackendPtr() && "Expected assembler backend");

  // Ignore virtual sections.
  if (Sec->isVirtualSection()) {
    assert(Layout.getSectionFileSize(Sec) == 0 && "Invalid size for section!");

    // Check that contents are only things legal inside a virtual section.
    for (const MCFragment &F : *Sec) {
      switch (F.getKind()) {
      default: llvm_unreachable("Invalid fragment in virtual section!");
      case MCFragment::FT_Data: {
        // Check that we aren't trying to write a non-zero contents (or fixups)
        // into a virtual section. This is to support clients which use standard
        // directives to fill the contents of virtual sections.
        const MCDataFragment &DF = cast<MCDataFragment>(F);
        if (DF.fixup_begin() != DF.fixup_end())
          getContext().reportError(SMLoc(), Sec->getVirtualSectionKind() +
                                                " section '" + Sec->getName() +
                                                "' cannot have fixups");
        for (unsigned i = 0, e = DF.getContents().size(); i != e; ++i)
          if (DF.getContents()[i]) {
            getContext().reportError(SMLoc(),
                                     Sec->getVirtualSectionKind() +
                                         " section '" + Sec->getName() +
                                         "' cannot have non-zero initializers");
            break;
          }
        break;
      }
      case MCFragment::FT_Align:
        // Check that we aren't trying to write a non-zero value into a virtual
        // section.
        assert((cast<MCAlignFragment>(F).getValueSize() == 0 ||
                cast<MCAlignFragment>(F).getValue() == 0) &&
               "Invalid align in virtual section!");
        break;
      case MCFragment::FT_Fill:
        assert((cast<MCFillFragment>(F).getValue() == 0) &&
               "Invalid fill in virtual section!");
        break;
      }
    }

    return;
  }

  uint64_t Start = OS.tell();
  (void)Start;

  //DBL
  uint64_t SecStart = OS.tell();
  StringRef SecName = Sec->getName();

  for (const MCFragment &F : *Sec) {
    writeFragment(OS, *this, Layout, F, SecStart, SecName.str());
  }

  assert(OS.tell() - Start == Layout.getSectionAddressSize(Sec));
}

std::tuple<MCValue, uint64_t, bool>
MCAssembler::handleFixup(const MCAsmLayout &Layout, MCFragment &F,
                         const MCFixup &Fixup) {
  // Evaluate the fixup.
  MCValue Target;
  uint64_t FixedValue;
  bool WasForced;
  bool IsResolved = evaluateFixup(Layout, Fixup, &F, Target, FixedValue,
                                  WasForced);
  if (!IsResolved) {
    // The fixup was unresolved, we need a relocation. Inform the object
    // writer of the relocation, and give it an opportunity to adjust the
    // fixup value if need be.
    if (Target.getSymA() && Target.getSymB() &&
        getBackend().requiresDiffExpressionRelocations()) {
      // The fixup represents the difference between two symbols, which the
      // backend has indicated must be resolved at link time. Split up the fixup
      // into two relocations, one for the add, and one for the sub, and emit
      // both of these. The constant will be associated with the add half of the
      // expression.
      MCFixup FixupAdd = MCFixup::createAddFor(Fixup);
      MCValue TargetAdd =
          MCValue::get(Target.getSymA(), nullptr, Target.getConstant());
      getWriter().recordRelocation(*this, Layout, &F, FixupAdd, TargetAdd,
                                   FixedValue);
      MCFixup FixupSub = MCFixup::createSubFor(Fixup);
      MCValue TargetSub = MCValue::get(Target.getSymB());
      getWriter().recordRelocation(*this, Layout, &F, FixupSub, TargetSub,
                                   FixedValue);
    } else {
      getWriter().recordRelocation(*this, Layout, &F, Fixup, Target,
                                   FixedValue);
    }
  }
  return std::make_tuple(Target, FixedValue, IsResolved);
}

//DBL
void MCAssembler::printSectionLayout(MCAsmLayout& Layout, MCSection& Section,
                          std::vector<Bundle>& Bundles,
                          bool PrintFragmentOffsets = false) {
  //printFragmentOffsets default false: during reordering,
  //layoutOrders/fragmentOffsets/... are not correct yet so running
  //Layout.getFragmentOffset (which calls ensureValid and layoutFragment)
  //would fail on some asserts
  //set to true when layouting is done
  errs() << "## SECTION " << Section.getName()
         << ": nr of fragments: " << Section.getFragmentList().size()
         << ", nr of bundles of interest: " << Bundles.size() << "\n";
  //temporary check for overlapping bundles (fragment in more than one bundle)
  //very inefficient, takes a long time for big targets!
  /*for(auto F = Section.begin(); F != Section.end(); F++) {
    int Found = -1;
    for(unsigned Idx = 0; Idx < Bundles.size(); Idx++) {
      auto B = Bundles[Idx];
      auto End = B.BundleEnd; End++; //exclusive
      auto I = B.BundleBegin;
      for(; I != End; I++) {
        if(I == F) {
          if(Found != -1) {
            errs() << "Fragment was found in Bundles " << Found
                   << " and " << Idx << "\n";
            MCDataFragment* DF = dyn_cast<MCDataFragment>(F);
            if(DF) {
              for(unsigned q = 0; q < DF->getContents().size(); q++) {
                unsigned char C = DF->getContents()[q];
                errs() << format("0x%x ", C);
              } errs() << "\n";
            }
          }
          assert(Found == -1);
          Found = Idx;
        }
      }
    }
  }*/

  //TODO remove this ugly thing when first bundle stuff is fixed
  //use MCFragment::dump
  //update: first bundle stuff is fixed
  for(auto FragItr = Section.begin(); FragItr != Section.end(); FragItr++) {
    bool Bundle = false;
    for(unsigned J = 0; J < Bundles.size(); J++) {
      auto T = Bundles[J];
      auto Itr = T.BundleBegin;
      auto BundleEnd = T.BundleEnd; BundleEnd++;
      if(Itr == FragItr) {
        uint64_t Size = 0;
        Bundle = true;
        DEBUG_WITH_TYPE("dbl_trace",
            errs() << "==== Bundle " << J << " ====\n");
        int I = 0;
        for(; Itr != BundleEnd; Itr++, FragItr++, I++) {
          assert(Itr == FragItr && "Bundle is not contiguous");
          if(isa<MCAlignFragment>(&*Itr)) continue;
          MCDBLFragment* F = dyn_cast<MCDBLFragment>(FragItr);
          assert(F && "Not a MCDBLFragment");
          DEBUG_WITH_TYPE("dbl_trace",
              errs() << "    --- Fragment " << I << " " << F;
              if(PrintFragmentOffsets)
                errs() << " FragmentOffset: " << Layout.getFragmentOffset(F);
              errs() << " ---\n\t";
              for(unsigned q = 0; q < F->getContents().size(); q++) {
                unsigned char C = F->getContents()[q];
                if(F->TargetSpec && F->TargetSpec.value().isTarget() &&
                   q == F->TargetSpec.value().getTargetOffsetInFragment())
                  errs().changeColor(raw_fd_ostream::Colors::RED, true);
                errs() << format(" 0x%x", C);
                errs().resetColor();
              }
              errs() << "\n";
          );
          Size += computeFragmentSize(Layout, *FragItr);
        }
        FragItr--;
        errs() << "  Bundle " << J << " with " << I << " fragment(s), size: "
               << Size << " bytes\n";
        break;
      }
    }
    if(!Bundle) {
      MCFragment::FragmentType Type = FragItr->getKind();
      if(Type == MCFragment::FragmentType::FT_Fill) {
        DEBUG_WITH_TYPE("dbl_trace", errs() << "==== ");
        errs() << "Fragment (not in bundle) type: " << "FT_Fill" /*Type*/
               << ", Size: " << computeFragmentSize(Layout, *FragItr)
               << " bytes";

        if(PrintFragmentOffsets)
          errs() << ", FragmentOffset: " << Layout.getFragmentOffset(&*FragItr);
        DEBUG_WITH_TYPE("dbl_trace", errs() << " " << &*FragItr);
        errs() << "\n";

        if(MCDBLFragment* F = dyn_cast<MCDBLFragment>(FragItr)) {
          for(unsigned q = 0; q < F->getContents().size(); q++) {
            unsigned char C = F->getContents()[q];
            errs() << format(" 0x%x", C);
          }
        }
      }
    }
  }
}

//DBL
//read target_offsets.toml and victim_addresses.txt
void MCAssembler::readConfig() {
  //Read Target Offsets
  auto TargetOffsets = toml::parse_file(TargetOffsetsPath);
  assert(TargetOffsets && "Could not open or parse Target Offsets file");
  auto TargetOffsetsTable = TargetOffsets.table();

  auto* T = TargetOffsetsTable["sections"].as_array();
  T->for_each([this](toml::table& El) {
    std::string SecName = El["name"].value<std::string>().value();
    El["values"].as_array()->for_each([SecName, this](toml::table& E){
        std::string S = E["type"].value<std::string>().value();
      if(S == "none") {
        uint64_t Offset = E["offset"].as_integer()->get();
        TargetSpec T {Offset, TargetNone{}};
        TargetsToFindForSection[SecName].push_back(T);
      } else if(S == "fixed") {
        uint64_t TargetOffset = E["offset"].as_integer()->get();
        int Bit = E["bit"].as_integer()->get();
        bool Sign = E["sign"].value<std::string>().value() != "-";
        TargetSpec T {TargetOffset, TargetFixed{Bit, Sign}};
        TargetsToFindForSection[SecName].push_back(T);
      } else if(S == "range") {
        uint64_t TargetStartOffset = E["start_offset"].as_integer()->get();
        unsigned DestAddrRange = E["range"].as_integer()->get();
        //for now, i only support instructions with 32 bit relative offsets,
        //others are converted to this type (see fixupNeedsRelaxation)
        assert(DestAddrRange == 4);
        uint64_t OffsetNormalDest = E["normal_dest"].as_integer()->get();
        uint64_t OffsetFlippedDest = E["flipped_dest"].as_integer()->get();
        TargetSpec T1{TargetStartOffset,
          TargetRange{DestAddrRange, OffsetNormalDest, OffsetFlippedDest}};
        TargetSpec T2{OffsetNormalDest, TargetDestination{}};
        TargetSpec T3{OffsetFlippedDest, TargetDestination{}};
        TargetsToFindForSection[SecName].push_back(T1);
        TargetsToFindForSection[SecName].push_back(T2);
        TargetsToFindForSection[SecName].push_back(T3);
      }
    });
  });

  //sort the targets to find
  for(auto& TargetsToFind: TargetsToFindForSection)
    TargetsToFind.second.sort([](TargetSpec& A, TargetSpec& B) {
      return A.TargetOffsetInOutput < B.TargetOffsetInOutput;
    });

  //Read Victims
  std::string Str;
  std::ifstream Victims;
  Victims.open(VictimAddressesPath);
  assert(Victims.is_open() && "Could not open Victim Addresses file");
  std::map<uint64_t, std::vector<VictimInfo>> Tmp;
  while(std::getline(Victims, Str)) {
    if(!Str.empty() && Str[0] != '#') {
      SmallVector<StringRef, 5> Parts;
      StringRef StrRef = Str;
      StrRef.split(Parts, " ");

      uint64_t Addr = std::stol(Parts[0].str(), nullptr, 16);
      uint64_t Bit = std::stol(Parts[1].str(), nullptr);
      bool Sign = Parts[2][0] != '-';
      std::vector<uint64_t> Aggrs;
      SmallVector<StringRef, 2> T;
      Parts[3].split(T, ",");
      for(StringRef S: T) Aggrs.push_back(std::stol(S.str(), nullptr, 16));
      uint64_t AggrInit = std::stol(Parts[4].str(), nullptr, 16);

      VictimInfo V {Addr, Bit, Sign, Aggrs, AggrInit};
      Tmp[Addr & PageAddrMask].push_back(V);
    }
  }

  for(auto& T: Tmp)
    VictimInfos.push_back(T.second);
}

void MCAssembler::layout(MCAsmLayout &Layout, bool round2) {
  assert(getBackendPtr() && "Expected assembler backend");
  DEBUG_WITH_TYPE("mc-dump", {
      errs() << "assembler backend - pre-layout\n--\n";
      dump(); });

  // Create dummy fragments and assign section ordinals.
  unsigned SectionIndex = 0;
  for (MCSection &Sec : *this) {
    // Create dummy fragments to eliminate any empty sections, this simplifies
    // layout.
    if (Sec.getFragmentList().empty())
      new MCDataFragment(&Sec);

    Sec.setOrdinal(SectionIndex++);

    //DBL remove all MCAlignFragments bcs they can change the layout
    //for now, this only applies to the .dbl_text section
    //edit: removing the fragment gives problems when applying fixups for
    //the .debug_loc section (fixup uses symbol to (align?) fragment that was
    //freed before, or something?)
    //so instead, set the alignment to 1 which removes the "align effect"
    if(DBLMode != Baseline &&
       &Sec == getContext().getObjectFileInfo()->getDBLTextSection()) {
      for(auto Itr = Sec.begin(); Itr != Sec.end();) {
        if((*Itr).getKind() == MCFragment::FT_Align) {
          //Itr = Sec.getFragmentList().erase(Itr);
          auto* F = dyn_cast<MCAlignFragment>(*&Itr);
          F->setAlignment(1); Itr++;
        }
        else Itr++;
      }
    }
  }


  // Assign layout order indices to sections and fragments.
  for (unsigned i = 0, e = Layout.getSectionOrder().size(); i != e; ++i) {
    MCSection *Sec = Layout.getSectionOrder()[i];
    Sec->setLayoutOrder(i);

    unsigned FragmentIndex = 0;
    for (MCFragment &Frag : *Sec)
      Frag.setLayoutOrder(FragmentIndex++);
  }

  // Layout until everything fits.
  while (layoutOnce(Layout)) {
    if (getContext().hadError())
      return;
    // Size of fragments in one section can depend on the size of fragments in
    // another. If any fragment has changed size, we have to re-layout (and
    // as a result possibly further relax) all.
    for (MCSection &Sec : *this)
      Layout.invalidateFragmentsFrom(&*Sec.begin());
  }

  DEBUG_WITH_TYPE("mc-dump", {
      errs() << "assembler backend - post-relaxation\n--\n";
      dump(); });

  // Finalize the layout, including fragment lowering.
  //DBL
  //this can be put after DBL layout, relaxation only consumes the space of
  //the boundaryAlign fragment after the relaxation fragment so all
  //instructions keep the same start offset
  finishLayout(Layout);

  DEBUG_WITH_TYPE("mc-dump", {
      errs() << "assembler backend - final-layout\n--\n";
      dump(); });

  //DBL
  //ROUND 2: create the required layout (before fixups get resolved)
  if(round2) {
    MCSection* TextSec = Context.getObjectFileInfo()->getDBLTextSection();
    Layout.invalidateFragmentsFrom(&*TextSec->begin());

    //assumption: section starts at page boundary
    //Fragments are bundled together so that the resulting bundle either starts
    //with a destination fragment, or contains at most one target fragment (in
    //the current implementation this target fragment will also be the first
    //fragment in the bundle, this could yield suboptimal puzzle results)
    //bundles are the puzzle pieces that get moved around during the
    //relayouting
    //offsets in fixups should not break after moving the bundles since offsets
    //to code are normally created using symbol refs

    //alternative is to only pad inside the pages and use the linker for

    std::vector<Bundle> Bundles;
    //TargetOffsetInOutput -> index in Bundles
    std::map<uint64_t, uint64_t> BundleMap;
    //map to find the bundle in which each each destination fragment resides
    std::map<const MCFragment*, unsigned> DestFragmentToBundleIdxMap;

    //iterate from back to front and cut to create bundles, the fragment of
    //interest will be the first one of each bundle
    auto& FragList = TextSec->getFragmentList();
    auto BundleEnd = FragList.rbegin();
    for(auto Itr = BundleEnd; Itr != FragList.rend(); Itr++) {
      if(isa<MCAlignFragment>(&*Itr)) continue;
      MCDBLFragment* Frag = dyn_cast<MCDBLFragment>(&*Itr);
      assert(Frag && "Not a MCDBLFragment");

      //cut on fragments with a TargetSpec
      if (Frag->TargetSpec) {
        auto& Spec = Frag->TargetSpec.value();
        Bundle Bundle {Spec /*copy*/,
                       Itr.getReverse(), BundleEnd.getReverse() /*inclusive*/};
        Bundles.push_back(Bundle);
        assert(BundleMap.count(Spec.TargetOffsetInOutput) == 0);
        BundleMap[Spec.TargetOffsetInOutput] = Bundles.size();
        BundleEnd = std::next(Itr);
      }
    }

    std::reverse(Bundles.begin(), Bundles.end());

    //find destinations
    //TODO use cleaner solution
    //converts offset to index in Bundles
    for(auto& Bundle: Bundles) {
      if(auto* P = std::get_if<TargetRange>(&Bundle.Spec.Kind)) {
        uint64_t NormalDest = P->NormalDest;
        uint64_t FlipDest = P->FlipDest;
        //if one of these fails, your target offsets are probably overlapping
        //(to many flips compared to the size of the binary)
        if(!BundleMap.count(NormalDest)) {
          errs() << format("NormalDest not found: 0x%x\n", NormalDest);
          assert(false);
        }
        if(!BundleMap.count(FlipDest)) {
          errs() << format("FlipDest not found: 0x%x\n", FlipDest);
          assert(false);
        }
        P->NormalDest = Bundles.size() - BundleMap[NormalDest];
        P->FlipDest = Bundles.size() - BundleMap[FlipDest];
      }
    }

    //important: stored Bundle indices are now invalid!
    BundleMap.clear();

    printSectionLayout(Layout, *TextSec, Bundles);

    assert(Bundles.front().BundleBegin == FragList.begin() &&
           "Not all fragments at the start are part of a Bundle");

    //add labels and jumps in the bundles to preserve control flow when they
    //get moved in the binary
    //TODO actually, it preserves the bundle order, so the jumps are not
    //necessary if the bundle ends with jmp/ret
    errs() << "Adding jmp instruction (5 bytes) in every bundle\n";
    MCSymbol* PrevLabel = nullptr;
    for(auto Itr = Bundles.rbegin(); Itr != Bundles.rend(); Itr++) {
      //search for the last MCDBLFragment
      MCDBLFragment* EndFrag = dyn_cast<MCDBLFragment>(Itr->BundleEnd);
      while(!EndFrag) {
        assert(Itr->BundleEnd->getKind() == MCFragment::FragmentType::FT_Align);
        auto I = Itr->BundleEnd; I--;
        EndFrag = dyn_cast<MCDBLFragment>(I);
      }

      //emit jump to Label in next block
      if(PrevLabel)
        EndFrag->addJmp(PrevLabel, Context, *Emitter.get());

      //emit label to the beginning of the bundle
      auto BeginFrag = Itr->BundleBegin;
      assert(!isa<MCFillFragment>(BeginFrag));
      MCSymbol* Label = getContext().createTempSymbol();
      registerSymbol(*Label);
      Label->setFragment(&*BeginFrag);
      Label->setOffset(0);

      PrevLabel = Label;
    }

    //solve
    //[section offset -> Result]
    //section offsets in ascending order
    std::map<uint64_t, Result> ResultMap;
    solveFF(Bundles, Layout, ResultMap);
    //all bundles should be in the ResultMap (not the first bundle)

    //reorder, padd, move bundles
    errs() << "########## REORDERING / PADDING / MOVING ##########\n";
    //page offset in section -> [Results for that page]
    auto SwapSpot = FragList.begin();
    uint64_t LastEnd = 0;

    //iterated in acsending BundleSectionOffset
    for(auto& Pair: ResultMap) {
      auto& BundleSectionOffset = Pair.first;
      Result& Result = Pair.second;

      DEBUG_WITH_TYPE("dbl_trace",
          printSectionLayout(Layout, *TextSec, Bundles));
      unsigned BundleIdx = Result.BundleIdx;
      errs() << "Processing bundle " <<  BundleIdx << "\n";
      auto TmpItrBegin = Bundles[BundleIdx].BundleBegin;
      auto TmpItrEnd = std::next(Bundles[BundleIdx].BundleEnd); //exclusive

      //add the padding before the bundle
      int64_t Fill = BundleSectionOffset - LastEnd;
      errs() << format("Fill: 0x%x bytes\n", Fill);
      assert(Fill >= 0 && "New fragments overlaps with previous one");
      //assert((uint64_t)Fill <= 2 * PageSize &&
      //       "Inserting page full of padding");
      //pages full of padding are possible bcs of the linked bundles, this could
      //be avoided by splitting the code in multiple section that are loaded at
      //a different address
      const MCConstantExpr* E = MCConstantExpr::create(Fill, getContext());
      //0x90 = 1 byte NOP
      //0xcc = INT3
      MCFillFragment* FillFrag = new MCFillFragment(0xcc, 1, *E, SMLoc());
      FillFrag->setParent(TextSec);
      FragList.insert(SwapSpot, FillFrag); //insert before
      LastEnd += Fill;
      LastEnd += Bundles[BundleIdx].getBundleSize(Layout, *this);
      errs() << format("  Inserted %lu (0x%x) bytes padding\n", Fill, Fill);

      std::string Name = "padding_before_bundle" + std::to_string(BundleIdx);
      MCSymbol* Label = getContext().getOrCreateSymbol(Name);
      MCSymbolELF* LabelELF = dyn_cast<MCSymbolELF>(Label);
      assert(LabelELF);
      //give the padding a GLOBAL, FUNC symbol, just for fun
      //LabelELF->setBinding(ELF::STB_GLOBAL);
      //LabelELF->setType(ELF::STT_FUNC);
      //LabelELF->setSize(E);
      registerSymbol(*Label);
      Label->setFragment(&*FillFrag);
      Label->setOffset(0);

      //move the bundle forward if it is not the next Bundle in the FragList
      std::vector<MCFragment*> ToReplace; //fragments of the bundle we move
      if(SwapSpot != TmpItrBegin) {
        errs() << "  Bundle is not the next in the original fragment list"
               << ", moving it forward\n";
        while(TmpItrBegin != TmpItrEnd) {
          auto T = TmpItrBegin; T++;
          ToReplace.push_back(FragList.remove(TmpItrBegin));
          TmpItrBegin = T;
        }
      } else SwapSpot = TmpItrEnd;

      for(auto* F: ToReplace)
        FragList.insert(SwapSpot, F); //insert before

      errs() << "  Inserted bundle " <<  BundleIdx << "\n";
    }

    //the .dbl_text section's symbol should point to the first fragment in the
    //section, the linker uses this symbol to calculate inter-section offsets
    //the end symbol (if any) usually doesn't cause problems
    TextSec->getBeginSymbol()->setFragment(&*FragList.begin());

    printSectionLayout(Layout, *TextSec, Bundles);

    //print final layout and physical mapping info to file
    Layout.invalidateFragmentsFrom(&*TextSec->begin());
    //be careful with intermediate getFragmentOffset!!
    unsigned FragmentIndex = 0;
    for (MCFragment &Frag : *TextSec)
      Frag.setLayoutOrder(FragmentIndex++);
    Layout.getFragmentOffset(&*FragList.rbegin()); //to validate all fragments
    errs() << "****** FINAL LAYOUT ******\n";
    printSectionLayout(Layout, *TextSec, Bundles, true);

    std::ofstream OutPutFile;
    std::ofstream ValidatorFile;
    OutPutFile.open(
      CompilerOutputPath + "/compiler_output_" + CompilationID + ".txt");
    ValidatorFile.open(
      CompilerOutputPath + "/page_allocation_" + CompilationID + ".txt");
    OutPutFile << "[General]\n\n";
    OutPutFile << "[Layout]\n";

    //TODO print per frame
    errs() << "########## PHYSICAL MEMORY MAP #############\n";
    for(auto T: ResultMap) {
      auto& Result = T.second;
      auto BundleIdx = Result.BundleIdx;
      errs() << Bundles[BundleIdx].str(BundleIdx, Layout, *this) << "\n";

      if(Result.VictimFrame) {
        errs() << format("  has victim at section offset: 0x%x\n",
                         Result.VictimPageOffset.value());
        auto& VictimFrame = Result.VictimFrame.value();
        auto& VictimFrameIdx = Result.VictimFrameIdx.value();
        auto& VictimInfo = VictimInfos[VictimFrame][VictimFrameIdx];
        auto& VictimAddr = VictimInfo.VictimAddr;
        auto FrameAddr = VictimAddr & PageAddrMask;
        int Bit = VictimInfo.Bit;
        const char* Sign = VictimInfo.Sign? "+" : "-";
        std::vector<uint64_t>& Aggrs = VictimInfo.Aggrs;
        auto AggrInit = VictimInfo.AggrInit;

        errs() << format("  frame info: addr 0x%x", FrameAddr)
               << format(", victim offset 0x%x",
                         VictimAddr & PageOffsetMask) << ", bit "
               << Bit << *Sign << ", aggressors:\n";
        OutPutFile << ".dbl_text " << std::hex << "0x"
                   << Result.VictimPageOffset.value()
                   << " 0x" << VictimAddr << " " << Bit << *Sign;
        ValidatorFile << "0,";
        std::string Sep = " ";
        for(uint64_t Aggr: Aggrs) {
          OutPutFile << std::hex << Sep << "0x" << Aggr;
          Sep = ",";
          errs() << format("    0x%x", Aggr) << ", init: "
                 << format("0x%x\n", AggrInit);
          ValidatorFile << std::hex << "0x" << Aggr << ",";
        }
        OutPutFile << std::hex << " 0x" << AggrInit << "\n";
        ValidatorFile << VictimAddr << "\n";
      } else {
        errs() << format(" at section offset 0x%x has no victim assigned\n",
                         T.first);
        //no file emit bcs no phys constraint
      }
    }
    OutPutFile.close();
    ValidatorFile.close();
  }

  // Allow the object writer a chance to perform post-layout binding (for
  // example, to set the index fields in the symbol data).
  getWriter().executePostLayoutBinding(*this, Layout);

  // Evaluate and apply the fixups, generating relocation entries as necessary.
  for (MCSection &Sec : *this) {
    for (MCFragment &Frag : Sec) {
      ArrayRef<MCFixup> Fixups;
      MutableArrayRef<char> Contents;
      const MCSubtargetInfo *STI = nullptr;

      // Process MCAlignFragment and MCEncodedFragmentWithFixups here.
      switch (Frag.getKind()) {
      default:
        continue;
      case MCFragment::FT_Align: {
        MCAlignFragment &AF = cast<MCAlignFragment>(Frag);
        // Insert fixup type for code alignment if the target define
        // shouldInsertFixupForCodeAlign target hook.
        if (Sec.UseCodeAlign() && AF.hasEmitNops())
          getBackend().shouldInsertFixupForCodeAlign(*this, Layout, AF);
        continue;
      }
      case MCFragment::FT_Data: {
        MCDataFragment &DF = cast<MCDataFragment>(Frag);
        Fixups = DF.getFixups();
        Contents = DF.getContents();
        STI = DF.getSubtargetInfo();
        assert(!DF.hasInstructions() || STI != nullptr);
        break;
      }
      case MCFragment::FT_Relaxable: {
        MCRelaxableFragment &RF = cast<MCRelaxableFragment>(Frag);
        Fixups = RF.getFixups();
        Contents = RF.getContents();
        STI = RF.getSubtargetInfo();
        assert(!RF.hasInstructions() || STI != nullptr);
        break;
      }
      case MCFragment::FT_CVDefRange: {
        MCCVDefRangeFragment &CF = cast<MCCVDefRangeFragment>(Frag);
        Fixups = CF.getFixups();
        Contents = CF.getContents();
        break;
      }
      case MCFragment::FT_Dwarf: {
        MCDwarfLineAddrFragment &DF = cast<MCDwarfLineAddrFragment>(Frag);
        Fixups = DF.getFixups();
        Contents = DF.getContents();
        break;
      }
      case MCFragment::FT_DwarfFrame: {
        MCDwarfCallFrameFragment &DF = cast<MCDwarfCallFrameFragment>(Frag);
        Fixups = DF.getFixups();
        Contents = DF.getContents();
        break;
      }
      }

      //DBL
      DEBUG_WITH_TYPE("dbl_trace",
        if(Fixups.size())
          errs() << "Applying fixups for fragment " << &Frag << "\n"
      );

      for (const MCFixup &Fixup : Fixups) {
        uint64_t FixedValue;
        bool IsResolved;
        MCValue Target;
        std::tie(Target, FixedValue, IsResolved) =
            handleFixup(Layout, Frag, Fixup);
        getBackend().applyFixup(*this, Fixup, Target, Contents, FixedValue,
                                IsResolved, STI);
      }
    }
  }
}

void MCAssembler::Finish() {
  //DBL
  //no bundling support
  assert(!isBundlingEnabled() && "DBL: bundling is not supported");
  std::map<std::string, unsigned> SizeCache;
  if(DBLMode == DBLModeT::DBL) {
    //check that if target_offsets, victim_addresses and compiler_output path
    //are provided
    assert(TargetOffsetsPath != "" && VictimAddressesPath != "" &&
           CompilerOutputPath != "" && "The target offsets path, victim "
           "addresses path or the compuler output path was not provided");

    readConfig();
    for(auto T: TargetsToFindForSection)
      SizeCache[T.first] = T.second.size();
  }

  //ROUND 1: perform normal binary emittion, emit one inst per fragment
  //and identify the fragments containing targets and their fragment offset
  MCAsmLayout Layout(*this);
  layout(Layout);
  uint64_t Seek = getWriter().tell();
  stats::ObjectBytes += getWriter().writeObject(*this, Layout);

  //ROUND 2: perform DBL binary emittion with custom layout
  if(DBLMode == DBLModeT::DBL) {
    uint64_t OldSize = getWriter().tell() - Seek;
    //check if all offsets were found
    for(auto Pair: TargetsToFindForSection) {
      auto SecName = Pair.first;
      auto TargetOffsets = Pair.second;
      unsigned TargetNr = SizeCache[SecName];
      errs() << "Section " << SecName << ": found "
             << TargetNr - TargetOffsets.size() << " of "
             << TargetNr << " target offsets\n";
      for(auto& T: TargetOffsets)
        errs() << format("target offset 0x%x was not found in the fragments "
                         "for this section\n", T.TargetOffsetInOutput);
      assert(TargetOffsets.empty());
    }

    //perform the actual layouting
    //clear object file
    getWriter().reset();
    //getWriter().clear(); //not really necessary
    Seek = getWriter().tell();

    MCAsmLayout LayoutDBL(*this);
    layout(LayoutDBL, true);
    stats::ObjectBytes += getWriter().writeObject(*this, LayoutDBL);
    uint64_t NewSize = getWriter().tell() - Seek;
    errs() << "Replaced " << OldSize << " bytes old binary code with "
           << NewSize << " bytes of new binary code, fraction: "
           << (double)NewSize / (double)OldSize << "\n";
    //DBL it could be smaller! by "removing" MCAlignFragments
    //assert(NewSize > OldSize &&
    //       "New binary did not completely overwrite the old binary");
  }
}

bool MCAssembler::fixupNeedsRelaxation(const MCFixup &Fixup,
                                       const MCRelaxableFragment *DF,
                                       const MCAsmLayout &Layout) const {
  assert(getBackendPtr() && "Expected assembler backend");
  MCValue Target;

  if (Target.getSymA() &&
      Target.getSymA()->getKind() == MCSymbolRefExpr::VK_X86_ABS8 &&
      Fixup.getKind() == FK_Data_1)
    return false;

  //DBL
  //for now we assume every fixup needs relaxation
  //this fn is used if the instruction is a JMP_1/JCC_1 (or arith) and changes
  //it to a JMP_4/JCC_4
  //we assume that every jump can be cross bundles so we need enough space
  //(more than one byte) to encode the target
  //TODO could be optimized by checking if jump is to a different bundle or not
  if(DBLMode == Baseline) {
    uint64_t Value;
    bool WasForced;
    bool Resolved = evaluateFixup(Layout, Fixup, DF, Target, Value, WasForced);
    return getBackend().fixupNeedsRelaxationAdvanced(Fixup, Resolved, Value, DF,
                                                     Layout, WasForced);
  }
  return true; //DBL the same as setting Resolved to false
}

bool MCAssembler::fragmentNeedsRelaxation(const MCRelaxableFragment *F,
                                          const MCAsmLayout &Layout) const {
  assert(getBackendPtr() && "Expected assembler backend");
  // If this inst doesn't ever need relaxation, ignore it. This occurs when we
  // are intentionally pushing out inst fragments, or because we relaxed a
  // previous instruction to one that doesn't need relaxation.
  if (!getBackend().mayNeedRelaxation(F->getInst(), *F->getSubtargetInfo()))
    return false;

  for (const MCFixup &Fixup : F->getFixups())
    if (fixupNeedsRelaxation(Fixup, F, Layout))
      return true;

  return false;
}

bool MCAssembler::relaxInstruction(MCAsmLayout &Layout,
                                   MCRelaxableFragment &F) {
  assert(getEmitterPtr() &&
         "Expected CodeEmitter defined for relaxInstruction");
  if (!fragmentNeedsRelaxation(&F, Layout))
    return false;

  ++stats::RelaxedInstructions;

  // FIXME-PERF: We could immediately lower out instructions if we can tell
  // they are fully resolved, to avoid retesting on later passes.

  // Relax the fragment.

  MCInst Relaxed = F.getInst();
  getBackend().relaxInstruction(Relaxed, *F.getSubtargetInfo());

  // Encode the new instruction.
  //
  // FIXME-PERF: If it matters, we could let the target do this. It can
  // probably do so more efficiently in many cases.
  SmallVector<MCFixup, 4> Fixups;
  SmallString<256> Code;
  raw_svector_ostream VecOS(Code);
  getEmitter().encodeInstruction(Relaxed, VecOS, Fixups, *F.getSubtargetInfo());

  // Update the fragment.
  F.setInst(Relaxed);
  F.getContents() = Code;
  F.getFixups() = Fixups;

  return true;
}

bool MCAssembler::relaxLEB(MCAsmLayout &Layout, MCLEBFragment &LF) {
  uint64_t OldSize = LF.getContents().size();
  int64_t Value;
  bool Abs = LF.getValue().evaluateKnownAbsolute(Value, Layout);
  if (!Abs)
    report_fatal_error("sleb128 and uleb128 expressions must be absolute");
  SmallString<8> &Data = LF.getContents();
  Data.clear();
  raw_svector_ostream OSE(Data);
  // The compiler can generate EH table assembly that is impossible to assemble
  // without either adding padding to an LEB fragment or adding extra padding
  // to a later alignment fragment. To accommodate such tables, relaxation can
  // only increase an LEB fragment size here, not decrease it. See PR35809.
  if (LF.isSigned())
    encodeSLEB128(Value, OSE, OldSize);
  else
    encodeULEB128(Value, OSE, OldSize);
  return OldSize != LF.getContents().size();
}

/// Check if the branch crosses the boundary.
///
/// \param StartAddr start address of the fused/unfused branch.
/// \param Size size of the fused/unfused branch.
/// \param BoundaryAlignment alignment requirement of the branch.
/// \returns true if the branch cross the boundary.
static bool mayCrossBoundary(uint64_t StartAddr, uint64_t Size,
                             Align BoundaryAlignment) {
  uint64_t EndAddr = StartAddr + Size;
  return (StartAddr >> Log2(BoundaryAlignment)) !=
         ((EndAddr - 1) >> Log2(BoundaryAlignment));
}

/// Check if the branch is against the boundary.
///
/// \param StartAddr start address of the fused/unfused branch.
/// \param Size size of the fused/unfused branch.
/// \param BoundaryAlignment alignment requirement of the branch.
/// \returns true if the branch is against the boundary.
static bool isAgainstBoundary(uint64_t StartAddr, uint64_t Size,
                              Align BoundaryAlignment) {
  uint64_t EndAddr = StartAddr + Size;
  return (EndAddr & (BoundaryAlignment.value() - 1)) == 0;
}

/// Check if the branch needs padding.
///
/// \param StartAddr start address of the fused/unfused branch.
/// \param Size size of the fused/unfused branch.
/// \param BoundaryAlignment alignment requirement of the branch.
/// \returns true if the branch needs padding.
static bool needPadding(uint64_t StartAddr, uint64_t Size,
                        Align BoundaryAlignment) {
  return mayCrossBoundary(StartAddr, Size, BoundaryAlignment) ||
         isAgainstBoundary(StartAddr, Size, BoundaryAlignment);
}

bool MCAssembler::relaxBoundaryAlign(MCAsmLayout &Layout,
                                     MCBoundaryAlignFragment &BF) {
  // BoundaryAlignFragment that doesn't need to align any fragment should not be
  // relaxed.
  if (!BF.getLastFragment())
    return false;

  uint64_t AlignedOffset = Layout.getFragmentOffset(&BF);
  uint64_t AlignedSize = 0;
  for (const MCFragment *F = BF.getLastFragment(); F != &BF;
       F = F->getPrevNode())
    AlignedSize += computeFragmentSize(Layout, *F);

  Align BoundaryAlignment = BF.getAlignment();
  uint64_t NewSize = needPadding(AlignedOffset, AlignedSize, BoundaryAlignment)
                         ? offsetToAlignment(AlignedOffset, BoundaryAlignment)
                         : 0U;
  if (NewSize == BF.getSize())
    return false;
  BF.setSize(NewSize);
  Layout.invalidateFragmentsFrom(&BF);
  return true;
}

bool MCAssembler::relaxDwarfLineAddr(MCAsmLayout &Layout,
                                     MCDwarfLineAddrFragment &DF) {
  MCContext &Context = Layout.getAssembler().getContext();
  uint64_t OldSize = DF.getContents().size();
  int64_t AddrDelta;
  bool Abs = DF.getAddrDelta().evaluateKnownAbsolute(AddrDelta, Layout);
  assert(Abs && "We created a line delta with an invalid expression");
  (void)Abs;
  int64_t LineDelta;
  LineDelta = DF.getLineDelta();
  SmallVectorImpl<char> &Data = DF.getContents();
  Data.clear();
  raw_svector_ostream OSE(Data);
  DF.getFixups().clear();

  if (!getBackend().requiresDiffExpressionRelocations()) {
    MCDwarfLineAddr::Encode(Context, getDWARFLinetableParams(), LineDelta,
                            AddrDelta, OSE);
  } else {
    uint32_t Offset;
    uint32_t Size;
    bool SetDelta = MCDwarfLineAddr::FixedEncode(Context,
                                                 getDWARFLinetableParams(),
                                                 LineDelta, AddrDelta,
                                                 OSE, &Offset, &Size);
    // Add Fixups for address delta or new address.
    const MCExpr *FixupExpr;
    if (SetDelta) {
      FixupExpr = &DF.getAddrDelta();
    } else {
      const MCBinaryExpr *ABE = cast<MCBinaryExpr>(&DF.getAddrDelta());
      FixupExpr = ABE->getLHS();
    }
    DF.getFixups().push_back(
        MCFixup::create(Offset, FixupExpr,
                        MCFixup::getKindForSize(Size, false /*isPCRel*/)));
  }

  return OldSize != Data.size();
}

bool MCAssembler::relaxDwarfCallFrameFragment(MCAsmLayout &Layout,
                                              MCDwarfCallFrameFragment &DF) {
  MCContext &Context = Layout.getAssembler().getContext();
  uint64_t OldSize = DF.getContents().size();
  int64_t AddrDelta;
  bool Abs = DF.getAddrDelta().evaluateKnownAbsolute(AddrDelta, Layout);
  assert(Abs && "We created call frame with an invalid expression");
  (void) Abs;
  SmallVectorImpl<char> &Data = DF.getContents();
  Data.clear();
  raw_svector_ostream OSE(Data);
  DF.getFixups().clear();

  if (getBackend().requiresDiffExpressionRelocations()) {
    uint32_t Offset;
    uint32_t Size;
    MCDwarfFrameEmitter::EncodeAdvanceLoc(Context, AddrDelta, OSE, &Offset,
                                          &Size);
    if (Size) {
      DF.getFixups().push_back(MCFixup::create(
          Offset, &DF.getAddrDelta(),
          MCFixup::getKindForSizeInBits(Size /*In bits.*/, false /*isPCRel*/)));
    }
  } else {
    MCDwarfFrameEmitter::EncodeAdvanceLoc(Context, AddrDelta, OSE);
  }

  return OldSize != Data.size();
}

bool MCAssembler::relaxCVInlineLineTable(MCAsmLayout &Layout,
                                         MCCVInlineLineTableFragment &F) {
  unsigned OldSize = F.getContents().size();
  getContext().getCVContext().encodeInlineLineTable(Layout, F);
  return OldSize != F.getContents().size();
}

bool MCAssembler::relaxCVDefRange(MCAsmLayout &Layout,
                                  MCCVDefRangeFragment &F) {
  unsigned OldSize = F.getContents().size();
  getContext().getCVContext().encodeDefRange(Layout, F);
  return OldSize != F.getContents().size();
}

bool MCAssembler::relaxFragment(MCAsmLayout &Layout, MCFragment &F) {
  switch(F.getKind()) {
  default:
    return false;
  case MCFragment::FT_Relaxable:
    assert(!getRelaxAll() &&
           "Did not expect a MCRelaxableFragment in RelaxAll mode");
    return relaxInstruction(Layout, cast<MCRelaxableFragment>(F));
  case MCFragment::FT_Dwarf:
    return relaxDwarfLineAddr(Layout, cast<MCDwarfLineAddrFragment>(F));
  case MCFragment::FT_DwarfFrame:
    return relaxDwarfCallFrameFragment(Layout,
                                       cast<MCDwarfCallFrameFragment>(F));
  case MCFragment::FT_LEB:
    return relaxLEB(Layout, cast<MCLEBFragment>(F));
  case MCFragment::FT_BoundaryAlign:
    return relaxBoundaryAlign(Layout, cast<MCBoundaryAlignFragment>(F));
  case MCFragment::FT_CVInlineLines:
    return relaxCVInlineLineTable(Layout, cast<MCCVInlineLineTableFragment>(F));
  case MCFragment::FT_CVDefRange:
    return relaxCVDefRange(Layout, cast<MCCVDefRangeFragment>(F));
  }
}

bool MCAssembler::layoutSectionOnce(MCAsmLayout &Layout, MCSection &Sec) {
  // Holds the first fragment which needed relaxing during this layout. It will
  // remain NULL if none were relaxed.
  // When a fragment is relaxed, all the fragments following it should get
  // invalidated because their offset is going to change.
  MCFragment *FirstRelaxedFragment = nullptr;

  // Attempt to relax all the fragments in the section.
  for (MCFragment &Frag : Sec) {
    // Check if this is a fragment that needs relaxation.
    bool RelaxedFrag = relaxFragment(Layout, Frag);
    if (RelaxedFrag && !FirstRelaxedFragment)
      FirstRelaxedFragment = &Frag;
  }
  if (FirstRelaxedFragment) {
    Layout.invalidateFragmentsFrom(FirstRelaxedFragment);
    return true;
  }
  return false;
}

bool MCAssembler::layoutOnce(MCAsmLayout &Layout) {
  ++stats::RelaxationSteps;

  bool WasRelaxed = false;
  for (MCSection &Sec : *this) {
    while (layoutSectionOnce(Layout, Sec))
      WasRelaxed = true;
  }

  return WasRelaxed;
}

void MCAssembler::finishLayout(MCAsmLayout &Layout) {
  assert(getBackendPtr() && "Expected assembler backend");
  // The layout is done. Mark every fragment as valid.
  for (unsigned int i = 0, n = Layout.getSectionOrder().size(); i != n; ++i) {
    MCSection &Section = *Layout.getSectionOrder()[i];
    Layout.getFragmentOffset(&*Section.getFragmentList().rbegin());
    computeFragmentSize(Layout, *Section.getFragmentList().rbegin());
  }
  getBackend().finishLayout(*this, Layout);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void MCAssembler::dump() const{
  raw_ostream &OS = errs();

  OS << "<MCAssembler\n";
  OS << "  Sections:[\n    ";
  for (const_iterator it = begin(), ie = end(); it != ie; ++it) {
    if (it != begin()) OS << ",\n    ";
    it->dump();
  }
  OS << "],\n";
  OS << "  Symbols:[";

  for (const_symbol_iterator it = symbol_begin(), ie = symbol_end(); it != ie; ++it) {
    if (it != symbol_begin()) OS << ",\n           ";
    OS << "(";
    it->dump();
    OS << ", Index:" << it->getIndex() << ", ";
    OS << ")";
  }
  OS << "]>\n";
}
#endif
