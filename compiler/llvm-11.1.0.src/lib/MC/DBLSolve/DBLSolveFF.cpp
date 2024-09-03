//DBL
#include "llvm/MC/MCAssembler.h"
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

using namespace llvm;

//This file contains all logic thats "solves" the layout puzzle
//It determines a feasable binary layout where earch target bit is
//associated with one victim bit. Victim bits can only be used once and
//blocks cannot not overlap. Blocks without a target can be put anywhere.
//The offset within a range target is bound by the provided range size and the
//distance between linked destination targets is equal to
//2^bitflip_index_in_jmp_offset
//
//It does this heuristically:
//  - the search for a suitable victim frame to link with a target frame is
//    best fit ("best" means the least amount of padding in front of the bundle)
//  - the search for a spot in the virtual address space for a piece of
//    code is first fit
//
//This code merely gives a result of a feasable layout and does not actually
//modifies the program under compilation
//MCAssembler::layout does the actual binary modifications

//Bundles can cross the page end but they cannot cross the page top boundary,
//the fragment of interest is always the first in the bundle, thus the
//assignment of a bundle to a victim frame is only for the first page of the
//bundle
//TODO TODO TODO there are bugs when fragments are cut in half at page
//bounderies... (these situations are very rare)

typedef std::list<std::pair<uint64_t, uint64_t>> FreeListT;
typedef std::_List_iterator<std::pair<uint64_t, uint64_t>> FreeListItrT;

struct TargetVictimInfo {
  uint64_t TargetOffset; //offset of target in block
  //uint64_t ByteOffsetRange; //this is the SIZE of the range (always >= 1)
};

//This layout heuristic operates on Blocks (they represent the Bundles from
//MCAssembler with some other "solver" related info)
//A Block is thus a piece of code that needs to be positioned in the binary
struct Block {
  unsigned BundleIdx; //index in Bundles (in MCAssembler::layout)
  uint64_t BlockSize;
  //The next two fields are filled in by the "solver"

  //This represents the offset in the section at which this block starts
  //Unique for all blocks (bcs they cannot overlap)
  uint64_t SectionOffset;
  //Assignment of a matching victim (if the block has a target: None in
  //case of a destination)
  std::optional<TargetVictimInfo> TargetVictimInfo;
};

Block createBlock(std::vector<MCAssembler::Bundle>& Bundles, unsigned BundleIdx,
                  const MCAsmLayout& Layout, const MCAssembler& Asm) {
  std::optional<TargetVictimInfo> TVI;
  MCAssembler::Bundle& Bundle = Bundles[BundleIdx];
  assert(BundleIdx < Bundles.size());
  if(Bundle.Spec.isTarget()) {
    TVI = TargetVictimInfo {
      Bundle.Spec.getTargetOffsetInFragment(), //TargetOffset
      //Bundle.Spec.getTargetByteRange() //ByteOffsetRange
    };
  }

  return Block {
    BundleIdx,
    Bundle.getBundleSize(Layout, Asm), //BlockSize
    0,    //SectionOffset
    TVI   //TargetVictimInfo
  };
}

//// victim frame assignment

//To prevent the same victim from being used twice
//This keeps track of the used victim FRAMES! bcs the loader currently does not
//support multiple victims in the same frame
static std::set<uint64_t> Used;

//Selects an appropriate victim frame for the given block (best fit manner)
std::pair<uint64_t, uint64_t> findVictim(Block& Block,
    const MCAssembler& Asm, uint64_t Bit, bool Sign) {
  uint64_t TargetOffset = Block.TargetVictimInfo.value().TargetOffset;
  assert(TargetOffset < Asm.PageSize); //just use %pagesize ?

  int64_t Best = INT64_MAX;
  auto Ret = std::make_pair(UINT64_MAX, UINT64_MAX);
  for(unsigned I = 0; I < Asm.VictimInfos.size(); I++) {
    auto& VictimInfo = Asm.VictimInfos[I];
    for(unsigned J = 0; J < VictimInfo.size(); J++) {
      auto G = std::make_pair(I, J);
      auto& Victim = VictimInfo[J];
      uint64_t FrameOffset = Victim.VictimAddr % Asm.PageSize;
      int64_t E = FrameOffset - TargetOffset;
      if(E > 0 && E < Best && Victim.Bit == Bit && Victim.Sign == Sign &&
         !Used.count(Victim.VictimAddr / Asm.PageSize)) {
        Ret = G;
        Best = E;
      }
    }
  }

  //if this fails, you probably have too few victims in your
  //victim_addresses.txt file
  assert(Ret.first != UINT64_MAX &&
         "Do you have enough victims in victim_addresses.txt?");
  Used.insert(Asm.VictimInfos[Ret.first][Ret.second].VictimAddr / Asm.PageSize);
  return Ret;
}


//// virtual memory assignment

struct RollBackInfo {
  //range that is removed during rollback
  FreeListItrT Start;
  FreeListItrT End; //exclusive
  //value it is replaced with
  std::pair<uint64_t, uint64_t> Value;
};

//on return, Itr points to the second element of the split
void splitFreelistAt(FreeListT FreeList, FreeListItrT& Itr, uint64_t Offset) {
  auto New = std::make_pair(Itr->first, Offset);
  FreeList.insert(Itr, New);
  Itr->first += Offset;
  Itr->second -= Offset;
}

//The available virtual address space to put code in is represented as a
//free list
//When a block gets assigned to a spot in virtual memory, this spot is not
//available anymore for later assignments
//This function reserves a chunck in the virtual address space and removes
//it from the free list
//It returns rollback info for when this action has to be reversed
//TODO add support to merge with previous/next free block
RollBackInfo removeFromFreelist(FreeListT& FreeList,
    FreeListItrT Itr, uint64_t Offset, uint64_t Size) {
  auto S = FreeList.end(); //start of new parts
  auto T = FreeList.end(); //end of new parts
  std::pair<uint64_t, uint64_t> P; //list of old parts

  if(Offset == 0 && Itr->second == Size) {
    //whole free block is used
    P = *Itr;
    S = FreeList.erase(Itr);
    T = S; //exclusive
  } else if(Offset == 0) {
    //block is at the top of the freeblock
    P = *Itr;
    S = Itr;
    T = std::next(S);
    Itr->first += Size;
    Itr->second -= Size;
  } else if(Offset + Size == Itr->first + Itr->second) {
    //block is at the bottom of freeblock
    P = *Itr;
    S = Itr;
    T = std::next(Itr);
    Itr->second -= Size;
    assert(Itr->second == Offset);
  } else {
    //block is somewhere in between, split the freeblock
    P = *Itr;
    auto New = std::make_pair(Itr->first, Offset);
    FreeList.insert(Itr, New);
    S = std::prev(Itr);
    T = std::next(Itr);
    Itr->first += Offset + Size;
    Itr->second -= (Offset + Size);
  }

  return RollBackInfo{S, T, P};
}

//Maps a page to its assigned frame
//Used to make sure each page is assigned to at most one frame
static std::map<uint64_t, uint64_t> Page2Frame;

//The next `asignSpot##` functions select a spot in the virtual address space
//to put the block in (first fit manner)
//The different function verions adhere to different placement constrains

//Select a free spot in virtual memeory that is large enough for the block
//witout extra contraints
RollBackInfo assignSpot(
    FreeListItrT& FreeListItr, FreeListT& FreeList, Block& Block) {
  //choose first spot that is big enough for NormalDest
  while(FreeListItr != FreeList.end() &&
        FreeListItr->second < Block.BlockSize)
    FreeListItr++;
  //S is never end(), there is always a spot at the end
  assert(FreeListItr != FreeList.end());

  Block.SectionOffset = FreeListItr->first;
  return removeFromFreelist(FreeList, FreeListItr, 0, Block.BlockSize);
}

//Selects a free spot in virtual memory
//The block will be positioned so the block's target (page offset given
//by Block.TargetOffset) resides in the victim (page offset given
//by VictimOffsetInPage)
void assignSpotVictim(FreeListT& FreeList, Block& Block, uint64_t PageSize,
    MCAssembler::VictimInfo& VI) {
  auto S = FreeList.begin();
  int64_t TO = Block.TargetVictimInfo.value().TargetOffset;
  int64_t VictimOffsetInPage = VI.VictimAddr % PageSize;
  while(S != FreeList.end() && (
        //if the beginning doesn't fit (assumes TO + FreeBlockStart < PageSize)
        TO + (int64_t)(S->first % PageSize) > VictimOffsetInPage ||
        //if the end doesn't fit
        (Block.BlockSize - TO) + VictimOffsetInPage >
        S->first % PageSize + S->second ||
        //if the page is already assigned to a different frame
        (Page2Frame.count(S->first / PageSize) &&
         Page2Frame[S->first / PageSize] != VI.VictimAddr / PageSize))) S++;
  //S is never end(), there is always a spot at the end
  //dirty hack for when all TO+FreeBlockStart > VOP: take the last free block,
  //select one page further (to avoid problems with frame assignment)
  if(S == FreeList.end()) {
    S--;
    splitFreelistAt(FreeList, S, PageSize - S->first % PageSize);
    //S now points to the last free block
  }

  uint64_t BlockOffset =
    ((VictimOffsetInPage - TO - S->first) % PageSize + PageSize) % PageSize;
  Block.SectionOffset = S->first + BlockOffset;

  unsigned PageNr = S->first / PageSize;
  assert(!Page2Frame.count(PageNr));
  Page2Frame[PageNr] = VI.VictimAddr / PageSize;
  removeFromFreelist(FreeList, S, BlockOffset, Block.BlockSize);

  //keep the last element in FreeList free of frame assignments
  auto I = --FreeList.end();
  if(I->first / PageSize == PageNr)
    splitFreelistAt(FreeList, I, PageSize - (I->first % PageSize));
}

//Select two free spots in virtual memory for arg:NormalDest and arg:FlipDest
//that are big enough, and for which the distance between them is arg:Dist
//TODO(1) for now, the whole free block is skipped if the free block at
//dist is not usable, better would be to look if an offset inside the free
//block is usable
void assignSpotAtDist(FreeListT& FreeList,
    Block& NormalDest, Block& FlipDest, uint64_t Dist) {
  auto Start = FreeList.begin();
  auto End = FreeList.end();
  while(End == FreeList.end()) {
    auto Next = std::prev(Start); //undef behaviour? is `prev(begin())++ == begin()`??
    RollBackInfo RBI = assignSpot(Start, FreeList, NormalDest);
    auto T = RBI.Start;
    //check if position at Dist is free and big enough
    uint64_t F = NormalDest.SectionOffset + Dist;
    //FreeList is ordered
    while(T != FreeList.end() && F > T->first) T++;
    T--;
    if(T->first + T->second >= F + FlipDest.BlockSize)
      End = T;
    else {
      //rollback
      FreeList.insert(RBI.Start, RBI.Value); //insert before
      FreeList.erase(RBI.Start, RBI.End);
      Start = ++++Next;
    }
  }

  //if there is no usable gap in between bundles, the pair will be put at the
  //end, so there is always a match

  FlipDest.SectionOffset = NormalDest.SectionOffset + Dist;
  removeFromFreelist(FreeList, End,
      FlipDest.SectionOffset - End->first, FlipDest.BlockSize);
}

//The solve function
//Results: section offset -> [Result]
void MCAssembler::solveFF(std::vector<Bundle>& Bundles,
    const MCAsmLayout &Layout, std::map<uint64_t, Result>& Results) {

  //split in types
  uint64_t AvgSize = 0; //max size bcs of TODO(1)
  std::vector<Block> Targets;
  std::vector<std::pair<Block, Block>> TargetDests;
  uint64_t MaxNormalDestSize = 0;

  for (unsigned I = 0; I < Bundles.size(); I++) {
    if(!std::get_if<TargetDestination>(&Bundles[I].Spec.Kind)) {
      Targets.push_back(createBlock(Bundles, I, Layout, *this));
      //AvgSize += Bundles[I].getBundleSize(Layout, *this); TODO(1)
      AvgSize = std::max(AvgSize, Bundles[I].getBundleSize(Layout, *this));
      if(auto* TargetSpec = std::get_if<TargetRange>(&Bundles[I].Spec.Kind)) {
        TargetDests.push_back(std::make_pair(
            createBlock(Bundles, TargetSpec->NormalDest, Layout, *this),
            createBlock(Bundles, TargetSpec->FlipDest, Layout, *this)));
        uint64_t NormalDestSize =
          Bundles[TargetSpec->NormalDest].getBundleSize(Layout, *this);
        if(NormalDestSize > MaxNormalDestSize)
          MaxNormalDestSize = NormalDestSize;
      }
    } else {
      //Also counts in AvgSize Dist is at least the NormalDest size TODO(1)
      AvgSize = std::max(AvgSize, Bundles[I].getBundleSize(Layout, *this));
    }
  }

  //choose one fixed distance between all Normal- and FlipBlock pairs based on
  //average bundle size
  double J = std::log2(AvgSize /* * 1.5 / Targets.size() */);
  unsigned B = std::ceil(J);
  assert(B < 32);
  uint64_t Dist = std::pow(2, B);
  unsigned RangeByteOffset = B / 8;
  unsigned RangeBit = B % 8;
  bool RangeSign = true;

  //[(SectionOffset, Size)]
  FreeListT FreeList;
  FreeList.push_back(std::make_pair(0, UINT64_MAX / 2)); //to prevent overflow

  //1. position dest blocks: per pair with Dist in between, keep empty space
  //between bundles in FreeList
  for(auto& Pair: TargetDests) {
    Block& NormalDest = Pair.first;
    Block& FlipDest = Pair.second;
    assignSpotAtDist(FreeList, NormalDest, FlipDest, Dist);

    //create Results
    //Dest blocks should not have targets
    Result Res;
    Res.BundleIdx = NormalDest.BundleIdx;
    assert(Results.count(NormalDest.SectionOffset) == 0);
    Results[NormalDest.SectionOffset] = Res;

    Result Res2;
    Res2.BundleIdx = FlipDest.BundleIdx;
    assert(Results.count(FlipDest.SectionOffset) == 0);
    Results[FlipDest.SectionOffset] = Res2;
  }

  //2. position fixed flip blocks and range blocks
  for(Block& Block: Targets) {
    Result Res;
    Res.BundleIdx = Block.BundleIdx;

    if(Block.TargetVictimInfo.has_value()) {
      //assign phys frame with victim
      auto& TVI = Block.TargetVictimInfo.value();

      unsigned Bit = RangeBit;
      bool Sign = RangeSign;
      auto* T = std::get_if<TargetFixed>(&Bundles[Block.BundleIdx].Spec.Kind);
      if(T) {
        Bit = T->Bit;
        Sign = T->Sign;
      } else {
        assert(std::get_if<TargetRange>(&Bundles[Block.BundleIdx].Spec.Kind));
        TVI.TargetOffset += RangeByteOffset;
      }

      auto R = findVictim(Block, *this, Bit, Sign);
      auto& VictimInfo = VictimInfos[R.first][R.second];
      //spot in virtual mem
      assignSpotVictim(FreeList, Block, PageSize, VictimInfo);
      //index in VictimInfos
      Res.VictimFrame = R.first;
      //index into item in VictimInfos
      Res.VictimFrameIdx = R.second;
      //the offset in de section at which the victim page starts, this is not
      //the start of the bundle for bundles with size > pagesize
      Res.VictimPageOffset = TVI.TargetOffset + Block.SectionOffset;
    } else {
      //spot in virtual mem
      FreeListItrT FreeListItr = FreeList.begin();
      assignSpot(FreeListItr, FreeList, Block);
    }
    assert(Results.count(Block.SectionOffset) == 0);
    Results[Block.SectionOffset] = Res;
  }
}

