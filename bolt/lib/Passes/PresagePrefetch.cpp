//===- bolt/Passes/PresagePrefetch.cpp -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception.
//
//===----------------------------------------------------------------------===//
//
// See bolt/Passes/PresagePrefetch.h for the mechanism and guarantees.
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/PresagePrefetch.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Core/BinaryFunctionCallGraph.h"
#include "bolt/Passes/DataflowInfoManager.h"
#include "bolt/Passes/RegAnalysis.h"
#include "bolt/Passes/LivenessAnalysis.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "MCTargetDesc/X86BaseInfo.h"

#include "llvm/MC/MCInst.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include <cstdlib>
#include <functional>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#undef DEBUG_TYPE
#define DEBUG_TYPE "presage"
using namespace llvm;
using namespace bolt;

namespace opts {

extern cl::OptionCategory BoltOptCategory;

static cl::opt<bool>
    PresagePrefetchFlag("presage-prefetch",
                        cl::desc("enable presage mechanism-transfer "
                                 "lookahead prefetch insertion"),
                        cl::init(false), cl::cat(BoltOptCategory),
                        cl::Hidden);

static cl::opt<std::string> PresageSiteList(
    "presage-prefetch-site-list",
    cl::desc("site list file: func:hexoff:dist:P,E,G1,G2,S1,S2 per line"),
    cl::init(""), cl::cat(BoltOptCategory), cl::Hidden);

static cl::opt<unsigned> PresageChainKind(
    "presage-prefetch-chain-kind",
    cl::desc("chain mode prefetch kind: 0=prefetcht0 (default), 1=prefetchnta"),
    cl::init(0), cl::cat(BoltOptCategory), cl::Hidden);

static cl::opt<unsigned> PresageDistance(
    "presage-prefetch-distance",
    cl::desc("default lookahead distance when the site omits one"),
    cl::init(8), cl::cat(BoltOptCategory), cl::Hidden);

static cl::opt<bool> PresageScanOnly(
    "presage-prefetch-scan-only",
    cl::desc("auto-discover prefetch sites (template matching on hot loops), "
             "write a site list, do not insert"),
    cl::init(false), cl::cat(BoltOptCategory), cl::Hidden);

static cl::opt<std::string> PresageScanOut(
    "presage-prefetch-scan-out",
    cl::desc("output file for -presage-prefetch-scan-only ('-' = stdout)"),
    cl::init("-"), cl::cat(BoltOptCategory), cl::Hidden);

} // namespace opts

namespace {

// Parse an x86-64 general purpose register given by name (rax..r15).
unsigned parseGPR64(const std::string &Name) {
  return StringSwitch<unsigned>(Name)
      .Case("rax", X86::RAX).Case("rcx", X86::RCX).Case("rdx", X86::RDX)
      .Case("rbx", X86::RBX).Case("rsp", X86::RSP).Case("rbp", X86::RBP)
      .Case("rsi", X86::RSI).Case("rdi", X86::RDI).Case("r8", X86::R8)
      .Case("r9", X86::R9).Case("r10", X86::R10).Case("r11", X86::R11)
      .Case("r12", X86::R12).Case("r13", X86::R13).Case("r14", X86::R14)
      .Case("r15", X86::R15)
      .Default(0);
}

// Accept 32-bit GPR names for RMATCH; return the 64-bit super register.
unsigned parseGPR64Alias(const std::string &N) {
  unsigned R = parseGPR64(N);
  if (R)
    return R;
  return StringSwitch<unsigned>(N)
      .Case("eax", X86::RAX).Case("ecx", X86::RCX).Case("edx", X86::RDX)
      .Case("ebx", X86::RBX).Case("esi", X86::RSI).Case("edi", X86::RDI)
      .Case("esp", X86::RSP).Case("ebp", X86::RBP).Case("r8d", X86::R8)
      .Case("r9d", X86::R9).Case("r10d", X86::R10).Case("r11d", X86::R11)
      .Case("r12d", X86::R12).Case("r13d", X86::R13).Case("r14d", X86::R14)
      .Case("r15d", X86::R15)
      .Default(0);
}

} // namespace

void PresagePrefetch::parseSiteList(BinaryContext &BC) {
  Sites.clear();
  if (opts::PresageSiteList.empty())
    return;
  std::ifstream In(opts::PresageSiteList.getValue());
  if (!In) {
    BC.errs() << "BOLT-ERROR: presage-prefetch: cannot open site list "
              << opts::PresageSiteList << "\n";
    exit(1);
  }
  std::string Line;
  unsigned LineNo = 0;
  while (std::getline(In, Line)) {
    ++LineNo;
    // strip comments / blanks
    auto Hash = Line.find('#');
    if (Hash != std::string::npos)
      Line = Line.substr(0, Hash);
    if (Line.find_first_not_of(" \t") == std::string::npos)
      continue;
    // func:hexoff:dist:P,E,G1,G2,S1,S2   (hexoff may be '@'-prefixed absolute)
    std::istringstream LS(Line);
    Site S;
    S.Raw = Line;
    std::string Off, Dist, Regs;
    if (!std::getline(LS, S.Func, ':') || !std::getline(LS, Off, ':') ||
        !std::getline(LS, Dist, ':') || !std::getline(LS, Regs)) {
      BC.errs() << "BOLT-WARNING: presage-prefetch: malformed site line "
                << LineNo << ": " << Line << "\n";
      continue;
    }
    if (!Off.empty() && Off[0] == '@') {
      S.Absolute = true;
      Off = Off.substr(1);
    }
    if (Dist == "chain") {
      // chain site: func:BBoff+idx:chain:RPOS,RMATCH,RCUR
      //   BBoff+idx anchors the instruction AFTER which the 5-insn chain
      //   prefetch is inserted; RPOS may be 'm<disp>' = 32-bit slot at
      //   disp(%rsp). Anchor must be a MOV32rm loading RMATCH.
      S.Chain = true;
      auto Plus = Off.find('+');
      if (Plus == std::string::npos) {
        BC.errs() << "BOLT-WARNING: presage chain site needs BBoff+idx: "
                  << Line << "\n";
        continue;
      }
      S.Offset = std::strtoull(Off.substr(0, Plus).c_str(), nullptr, 0);
      S.Idx = std::strtoul(Off.substr(Plus + 1).c_str(), nullptr, 0);
      std::string RC[3];
      std::istringstream RS2(Regs);
      unsigned N2 = 0;
      while (N2 < 3 && std::getline(RS2, RC[N2], ','))
        ++N2;
      if (N2 < 3) {
        BC.errs() << "BOLT-WARNING: presage chain site needs "
                  << "RPOS,RMATCH,RCUR: " << Line << "\n";
        continue;
      }
      S.PSpec = RC[0];
      S.E = parseGPR64Alias(RC[1]);  // RMATCH: 32- or 64-bit name -> super reg
      S.G1 = parseGPR64(RC[2]);      // RCUR (64-bit buf pointer)
      if ((!RC[0].empty() && RC[0][0] != 'm' && !parseGPR64(RC[0])) ||
          !S.E || !S.G1) {
        BC.errs() << "BOLT-WARNING: presage chain site bad register: " << Line
                  << "\n";
        continue;
      }
      Sites.push_back(std::move(S));
      continue;
    }
    S.Offset = std::strtoull(Off.c_str(), nullptr, 0);
    S.Dist = std::strtoul(Dist.c_str(), nullptr, 0);
    if (S.Dist == 0)
      S.Dist = opts::PresageDistance.getValue();
    std::string RN[6];
    std::istringstream RS(Regs);
    unsigned NParsed = 0;
    while (NParsed < 6 && std::getline(RS, RN[NParsed], ','))
      ++NParsed;
    if (NParsed != 6) {
      BC.errs() << "BOLT-WARNING: presage-prefetch: need 6 registers "
                << "(P,E,G1,G2,S1,S2) at line " << LineNo << ": " << Line
                << "\n";
      continue;
    }
    S.P = parseGPR64(RN[0]);
    S.E = parseGPR64(RN[1]);
    S.G1 = parseGPR64(RN[2]);
    // G2 == "-" selects single-gather mode: 8-byte records (one int64 index
    // per record), one prefetched array. Otherwise 16-byte records (two
    // fields at +0/+8) and two codependent gather arrays.
    S.G2 = (RN[3] == "-") ? 0 : parseGPR64(RN[3]);
    S.S1 = parseGPR64(RN[4]);
    S.S2 = parseGPR64(RN[5]);
    if (!S.P || !S.E || !S.G1 || !S.S1 || !S.S2) {
      BC.errs() << "BOLT-WARNING: presage-prefetch: bad 64-bit GPR name at "
                << "line " << LineNo << ": " << Line << "\n";
      continue;
    }
    if (S.Dist > 32768) {
      BC.errs() << "BOLT-WARNING: presage-prefetch: dist too large at line "
                << LineNo << ": " << Line << "\n";
      continue;
    }
    Sites.push_back(std::move(S));
  }
  BC.errs() << "BOLT-INFO: presage-prefetch: parsed " << Sites.size()
            << " site(s) from " << opts::PresageSiteList << "\n";
}

bool PresagePrefetch::runOnFunction(BinaryContext &BC, BinaryFunction &BF,
                                    RegAnalysis &RA) {
  bool Changed = false;
  for (const Site &S : Sites) {
    if (BF.getOneName() != S.Func)
      continue;
    auto Reason = [&](const char *R) {
      ++Rejected;
      RejectReasons.push_back((S.Func + ":" + utohexstr(S.Offset) + ": " + R));
      BC.errs() << "BOLT-INFO: presage-prefetch: rejected "
                << S.Func << "+0x" << utohexstr(S.Offset) << ": " << R << "\n";
    };
    uint64_t Offset = S.Offset;
    if (S.Absolute) {
      if (S.Offset < BF.getAddress()) {
        Reason("absolute address below function start");
        continue;
      }
      Offset = S.Offset - BF.getAddress();
    }
    // ---- chain mode: insert AFTER instruction Idx of the BB at Offset ----
    if (S.Chain) {
      BinaryBasicBlock *BB = BF.getBasicBlockContainingOffset(S.Offset);
      if (!BB || BB->empty() || S.Idx >= BB->size() ||
          BB->getInputOffset() != S.Offset) {
        Reason("chain: no basic block at input offset");
        continue;
      }
      auto AI = BB->begin() + S.Idx;
      MCInst &Anchor = *AI;
      // machine check: anchor must be a 32-bit load into RMATCH
      unsigned M32 = X86::MOV32rm;
      unsigned E32 = BC.MRI->getSubReg(S.E, X86::sub_32bit);
      BitVector MA = BC.MIB->getAliases(Anchor.getOperand(0).getReg());
      if (Anchor.getOpcode() != M32 || !E32 || !MA.test(E32)) {
        Reason("chain: anchor is not a MOV32rm into RMATCH");
        continue;
      }
      DataflowInfoManager DIM(BF, &RA, nullptr);
      LivenessAnalysis &LA = DIM.getLivenessAnalysis();
      const BitVector &LiveOut = LA.getLiveOut(Anchor);
      auto RegLive = [&](unsigned R) {
        return LiveOut.anyCommon(BC.MIB->getAliases(R));
      };
      if (RegLive(X86::EFLAGS)) {
        Reason("chain: flags live after anchor");
        continue;
      }
      if (!RegLive(S.E) || !RegLive(S.G1)) {
        Reason("chain: RMATCH/RCUR not live after anchor");
        continue;
      }
      unsigned RposReg = 0;
      int64_t RposDisp = 0;
      bool RposMem = !S.PSpec.empty() && S.PSpec[0] == 'm';
      if (RposMem)
        RposDisp = std::strtoll(S.PSpec.c_str() + 1, nullptr, 0);
      else {
        RposReg = parseGPR64(S.PSpec);
        if (RposReg && !RegLive(RposReg)) {
          Reason("chain: RPOS not live after anchor");
          continue;
        }
      }
      // scavenge ONE dead GPR (single-scratch sequence: tight loops rarely
      // have two dead regs)
      unsigned S1 = 0;
      for (unsigned R : {X86::RAX, X86::RCX, X86::RDX, X86::RBX, X86::RSI,
                         X86::RDI, X86::R8, X86::R9, X86::R10, X86::R11,
                         X86::R12, X86::R13, X86::R14, X86::R15}) {
        if (R == RposReg || R == S.E || R == S.G1 || RegLive(R))
          continue;
        S1 = R;
        break;
      }
      if (!S1) {
        Reason("chain: no dead scratch GPR");
        continue;
      }
      // 4-insn sequence, one scratch:
      //   [pos -> S1]; subl RMATCH32, S1d (delta); negq S1 (-delta);
      //   prefetcht0 (RCUR, S1, 1)   == cur + (-delta) == cur - delta
      unsigned S1d = BC.MRI->getSubReg(S1, X86::sub_32bit);
      std::vector<MCInst> Seq;
      if (RposMem) { // movl disp(%rsp), S1d
        MCInst I;
        I.setOpcode(X86::MOV32rm);
        I.addOperand(MCOperand::createReg(S1d));
        I.addOperand(MCOperand::createReg(X86::RSP));
        I.addOperand(MCOperand::createImm(1));
        I.addOperand(MCOperand::createReg(0));
        I.addOperand(MCOperand::createImm(RposDisp));
        I.addOperand(MCOperand::createReg(0));
        Seq.push_back(std::move(I));
      } else { // mov Rpos, S1
        MCInst I;
        I.setOpcode(X86::MOV64rr);
        I.addOperand(MCOperand::createReg(S1));
        I.addOperand(MCOperand::createReg(RposReg));
        Seq.push_back(std::move(I));
      }
      { // subl RMATCH32, S1d  (32-bit wraparound like the source)
        // LLVM 23 models flag-setting rr ops with a trailing imm8 EFLAGS
        // operand (same layout as CMP64rr)
        MCInst I;
        I.setOpcode(X86::SUB32rr);
        // canonical two-address form (llvm-mc --show-inst): (dst, dst, src)
        I.addOperand(MCOperand::createReg(S1d));
        I.addOperand(MCOperand::createReg(S1d));
        I.addOperand(MCOperand::createReg(BC.MRI->getSubReg(S.E, X86::sub_32bit)));
        Seq.push_back(std::move(I));
      }
      { // negq S1  -> -delta  (EFLAGS verified dead; trailing imm8 EFLAGS op)
        MCInst I;
        I.setOpcode(X86::NEG64r);
        // canonical two-address form: (reg, reg)
        I.addOperand(MCOperand::createReg(S1));
        I.addOperand(MCOperand::createReg(S1));
        Seq.push_back(std::move(I));
      }
      { // prefetcht0 (RCUR + S1*1)  == cur - delta
        MCInst I;
        I.setOpcode(opts::PresageChainKind == 1 ? X86::PREFETCHNTA
                                                : X86::PREFETCHT0);
        I.addOperand(MCOperand::createReg(S.G1));
        I.addOperand(MCOperand::createImm(1));
        I.addOperand(MCOperand::createReg(S1));
        I.addOperand(MCOperand::createImm(0));
        I.addOperand(MCOperand::createReg(0));
        Seq.push_back(std::move(I));
      }
      auto It = BB->begin() + S.Idx + 1;
      for (auto SI = Seq.rbegin(); SI != Seq.rend(); ++SI)
        It = BB->insertInstruction(It, std::move(*SI));
      ++Accepted;
      BC.errs() << "BOLT-INFO: presage-prefetch: accepted(chain) "
                << S.Func << "+0x" << utohexstr(S.Offset) << "+" << S.Idx
                << " RPOS=" << (RposMem ? S.PSpec : StringRef(BC.MRI->getName(RposReg)).str())
                << " RMATCH=" << BC.MRI->getName(S.E)
                << " RCUR=" << BC.MRI->getName(S.G1)
                << " scratch=" << BC.MRI->getName(S1) << "\n";
      Changed = true;
      continue;
    }
    // Instruction-level input offsets are only annotated on branches/calls,
    // but basic blocks keep their input offsets, and this pass always inserts
    // at a BB entry anyway. Anchor: the BB containing the input offset
    // (early position: current layout == input layout, so the lookup is
    // against input offsets).
    BinaryBasicBlock *BB = BF.getBasicBlockContainingOffset(Offset);
    if (!BB || BB->empty()) {
      Reason("no basic block covers input offset");
      continue;
    }
    MCInst *Inst = &*BB->begin();
    if (BB->getInputOffset() != Offset) {
      BC.errs() << "BOLT-WARNING: presage-prefetch: site 0x" << utohexstr(Offset)
                << " anchors at BB entry 0x" << utohexstr(BB->getInputOffset())
                << " (" << S.Func << ")\n";
    }

    // Liveness checks at BB entry.
    DataflowInfoManager DIM(BF, &RA, /*CCA=*/nullptr);
    LivenessAnalysis &LA = DIM.getLivenessAnalysis();
    const BitVector &LiveIn = LA.getLiveIn(*Inst);
    auto RegLive = [&](unsigned R) {
      return LiveIn.anyCommon(BC.MIB->getAliases(R));
    };
    if (RegLive(S.S1) || RegLive(S.S2)) {
      Reason("scratch register live at insertion point");
      continue;
    }
    if (RegLive(X86::EFLAGS)) {
      Reason("flags live at insertion point");
      continue;
    }
    if (!RegLive(S.P) || !RegLive(S.E) || !RegLive(S.G1) ||
        (S.G2 && !RegLive(S.G2))) {
      Reason("P/E/G1/G2 register not live at insertion point (bad site?)");
      continue;
    }

    // Build the fixed sequence. Record stride: 16 bytes (packed_edge) in
    // dual-gather mode, 8 bytes (single int64 index) in single-gather mode.
    const bool Single = (S.G2 == 0);
    const int64_t Stride = Single ? 8 : 16;
    const int64_t Disp = Stride * (int64_t)S.Dist;
    std::vector<MCInst> Seq;
    auto Lea = [&](unsigned Dst, unsigned Base, int64_t D) {
      MCInst I;
      I.setOpcode(X86::LEA64r);
      I.addOperand(MCOperand::createReg(Dst));
      I.addOperand(MCOperand::createReg(Base));
      I.addOperand(MCOperand::createImm(1));
      I.addOperand(MCOperand::createReg(0));
      I.addOperand(MCOperand::createImm(D));
      I.addOperand(MCOperand::createReg(0));
      Seq.push_back(std::move(I));
    };
    Lea(S.S1, S.P, Disp);  // S1 = &rec[k+D]
    Lea(S.S2, S.P, 0);     // S2 = &rec[k] (safe clamp target)

    { // cmp S1, E  (AT&T: cmp %S1, %E -> operands E, S1)
      MCInst I;
      I.setOpcode(X86::CMP64rr);
      I.addOperand(MCOperand::createReg(S.E));
      I.addOperand(MCOperand::createReg(S.S1));
      I.addOperand(MCOperand::createImm(0));
      Seq.push_back(std::move(I));
    }
    { // cmovbe S2 -> S1 when E <= S1 (lookahead OOB); AT&T: cmovbeq %S2, %S1
      MCInst I;
      I.setOpcode(X86::CMOV64rr);
      I.addOperand(MCOperand::createReg(S.S1)); // dst (= src1)
      I.addOperand(MCOperand::createReg(S.S1)); // src1
      I.addOperand(MCOperand::createReg(S.S2)); // src2
      I.addOperand(MCOperand::createImm(X86::COND_BE));
      Seq.push_back(std::move(I));
    }
    auto Mov = [&](unsigned Dst, unsigned Base, int64_t D) {
      MCInst I;
      I.setOpcode(X86::MOV64rm);
      I.addOperand(MCOperand::createReg(Dst));
      I.addOperand(MCOperand::createReg(Base));
      I.addOperand(MCOperand::createImm(1));
      I.addOperand(MCOperand::createReg(0));
      I.addOperand(MCOperand::createImm(D));
      I.addOperand(MCOperand::createReg(0));
      Seq.push_back(std::move(I));
    };
    auto Pf = [&](unsigned Base, unsigned Index) {
      MCInst I;
      I.setOpcode(X86::PREFETCHT0);
      I.addOperand(MCOperand::createReg(Base));
      I.addOperand(MCOperand::createImm(8));
      I.addOperand(MCOperand::createReg(Index));
      I.addOperand(MCOperand::createImm(0));
      I.addOperand(MCOperand::createReg(0));
      Seq.push_back(std::move(I));
    };
    if (Single) {
      Mov(S.S2, S.S1, 0); // pj = rec[k+D]
      Pf(S.G1, S.S2);     // gather1[pj]
    } else {
      Mov(S.S2, S.S1, 0); // pi = rec[k+D].v0
      Mov(S.S1, S.S1, 8); // pj = rec[k+D].v1
      Pf(S.G1, S.S2);     // gather1[pi]
      Pf(S.G1, S.S1);     // gather1[pj]
      Pf(S.G2, S.S2);     // gather2[pi]
      Pf(S.G2, S.S1);     // gather2[pj]
    }

    for (auto It = Seq.rbegin(); It != Seq.rend(); ++It)
      BB->insertInstruction(BB->begin(), std::move(*It));

    ++Accepted;
    BC.errs() << "BOLT-INFO: presage-prefetch: accepted " << S.Func << "+0x"
              << utohexstr(S.Offset) << " dist=" << S.Dist
              << " seq=" << Seq.size() << " insns mode="
              << (Single ? "single" : "dual") << " scratch="
              << BC.MRI->getName(S.S1) << "," << BC.MRI->getName(S.S2) << "\n";
    Changed = true;
  }
  return Changed;
}

// ---------------------------------------------------------------------------
// Automatic site discovery: -presage-prefetch-scan-only
//
// Template recognized per natural loop (no source needed):
//   1. hot loop (header has profile) with a latch whose tail contains
//        addq/leaq  P += 8|16        (const-stride record cursor)
//        cmpq       P, E             (E not written inside the loop)
//        jcc        -> header
//   2. loop-head region loads record fields: movq (P+0)/(P+8) -> f0/f1
//   3. gathers of the field values into loop-invariant base arrays:
//        movq (G, f*, 8)  or  leaq (G, f*, 8) -> t; movq (t)
//      with 1 (single mode, stride 8) or 2 (dual mode, stride 16) bases
//   4. two GPRs + EFLAGS dead at header live-in (liveness-verified scratch)
// Emits site lines; loops failing any check get a reject reason. Workloads
// without matching loops produce an empty site list by construction.
// ---------------------------------------------------------------------------
void PresagePrefetch::scanSites(BinaryContext &BC, RegAnalysis &RA) {
  struct Cand {
    std::string Line, Detail;
  };
  std::vector<Cand> Cands;
  unsigned LoopsSeen = 0, LoopsRejected = 0;

  for (auto &It : BC.getBinaryFunctions()) {
    BinaryFunction &BF = It.second;
    if (!BF.isSimple() || BF.empty() || !BF.hasValidProfile())
      continue;
    BF.calculateLoopInfo();
    std::vector<const BinaryLoop *> AllLoops;
    std::function<void(const BinaryLoop *)> Collect =
        [&](const BinaryLoop *L) {
          AllLoops.push_back(L);
          for (const BinaryLoop *Sub : L->getSubLoops())
            Collect(Sub);
        };
    for (const BinaryLoop *Top : BF.getLoopInfo())
      Collect(Top);
    for (const BinaryLoop *CLoop : AllLoops) {
      BinaryLoop *Loop = const_cast<BinaryLoop *>(CLoop);
      ++LoopsSeen;
      auto Reject = [&](const Twine &R) {
        ++LoopsRejected;
        BC.errs() << "BOLT-INFO: presage-scan: reject loop " << BF.getOneName()
                  << "+0x" << utohexstr(Loop->getHeader()->getInputOffset())
                  << ": " << R << "\n";
      };
      BinaryBasicBlock *Header = Loop->getHeader();
      if (Header->empty())
        continue;
      if (!Header->getKnownExecutionCount())
        continue; // cold loop: not interesting, not counted as reject

      // --- collect loop blocks, defs, invariant check set ---
      SmallVector<BinaryBasicBlock *, 4> Latches;
      Loop->getLoopLatches(Latches);
      std::set<const BinaryBasicBlock *> InLoop;
      std::vector<BinaryBasicBlock *> Blocks;
      for (BinaryBasicBlock *BB : Loop->blocks()) {
        InLoop.insert(BB);
        Blocks.push_back(BB);
      }
      auto defsInLoop = [&](MCPhysReg R) {
        BitVector Aliases = BC.MIB->getAliases(R);
        for (BinaryBasicBlock *BB : Blocks)
          for (MCInst &Inst : *BB) {
            const MCInstrDesc &D = BC.MII->get(Inst.getOpcode());
            for (unsigned I = 0; I < D.getNumDefs(); ++I) {
              if (Inst.getOperand(I).isReg() && Inst.getOperand(I).getReg() &&
                  Aliases.test(Inst.getOperand(I).getReg()))
                return true;
            }
          }
        return false;
      };

      // --- 1. latch tail: addq $stride,P ; cmpq P,E ; jcc header ---
      unsigned P = 0, E = 0;
      int64_t Stride = 0;
      bool LatchOK = false;
      for (BinaryBasicBlock *Latch : Latches) {
        std::vector<MCInst *> LI;
        for (MCInst &Mi : *Latch)
          LI.push_back(&Mi);
        const size_t N = LI.size();
        const size_t W = N < 5 ? N : 5;
        for (size_t I = N - W; I + 2 < N; ++I) {
          MCInst &A = *LI[I];
          MCInst &C = *LI[I + 1];
          MCInst &J = *LI[I + 2];
          if (A.getOpcode() != X86::ADD64ri8 && A.getOpcode() != X86::ADD64ri32)
            continue;
          unsigned Ar = A.getOperand(0).getReg();
          int64_t Imm = 0;
          for (unsigned K = 1, KE = A.getNumOperands(); K < KE; ++K)
            if (A.getOperand(K).isImm()) {
              Imm = A.getOperand(K).getImm();
              break;
            }
          if ((Imm != 8 && Imm != 16) || !Ar)
            continue;
          if (C.getOpcode() != X86::CMP64rr)
            continue;
          unsigned C0 = C.getOperand(0).getReg(), C1 = C.getOperand(1).getReg();
          if (!C0 || !C1)
            continue;
          unsigned Other = 0;
          if (BC.MIB->getAliases(Ar).test(C0) && !BC.MIB->getAliases(Ar).test(C1))
            Other = C1;
          else if (BC.MIB->getAliases(Ar).test(C1) &&
                   !BC.MIB->getAliases(Ar).test(C0))
            Other = C0;
          if (!Other || !BC.MIB->isConditionalBranch(J))
            continue;
          // back edge may be the branch target OR the fall-through edge
          // (gcc loop rotation: cmp; jcc exit; fall-through to header)
          bool ToHeader = false;
          for (BinaryBasicBlock *Succ : Latch->successors())
            if (Succ == Header) {
              ToHeader = true;
              break;
            }
          if (!ToHeader)
            continue;
          P = Ar;
          E = Other;
          Stride = Imm;
          LatchOK = true;
          break;
        }
        if (LatchOK)
          break;
      }
      if (!LatchOK) {
        Reject("no const-stride(8/16) cursor latch pattern");
        continue;
      }
      if (defsInLoop(E)) {
        Reject("bound register written in loop");
        continue;
      }

      // --- 2/3. record field loads + gathers over invariant bases ---
      // multi-pass: block order is not layout order, so collect fields
      // first, then lea aliases, then gather uses.
      unsigned F0 = 0, F1 = 0;
      std::map<unsigned, unsigned> AddrAlias; // lea dst -> gather base
      std::set<unsigned> Gathers;
      auto memOps = [&](MCInst &Inst, unsigned O, unsigned &Base,
                        int64_t &Scale, unsigned &Index, int64_t &Disp) {
        // rm forms: (reg, base, scale, index, disp, seg) -> O=1
        // mi forms: (base, scale, index, disp, seg, imm) -> O=0
        if (Inst.getNumOperands() < O + 5 || !Inst.getOperand(O).isReg())
          return false;
        Base = Inst.getOperand(O).getReg();
        Scale = Inst.getOperand(O + 1).isImm() ? Inst.getOperand(O + 1).getImm() : 1;
        Index = (Inst.getOperand(O + 2).isReg() && Inst.getOperand(O + 2).getReg())
                    ? (unsigned)Inst.getOperand(O + 2).getReg()
                    : 0;
        Disp = Inst.getOperand(O + 3).isImm() ? Inst.getOperand(O + 3).getImm() : 0;
        return true;
      };
      // pass 1: field loads movq (P+0)/(P+8)
      for (BinaryBasicBlock *BB : Blocks)
        for (MCInst &Inst : *BB) {
          if (Inst.getOpcode() != X86::MOV64rm ||
              Inst.getNumOperands() < 6 || !Inst.getOperand(0).isReg())
            continue;
          unsigned Dst = Inst.getOperand(0).getReg(), Base, Index;
          int64_t Scale, Disp;
          if (!memOps(Inst, 1, Base, Scale, Index, Disp))
            continue;
          if (Base == P && Index == 0 && Disp == 0)
            F0 = Dst;
          else if (Base == P && Index == 0 && Disp == 8)
            F1 = Dst;
        }
      // pass 2: leaq (G, f, 8) -> alias
      for (BinaryBasicBlock *BB : Blocks)
        for (MCInst &Inst : *BB) {
          if (Inst.getOpcode() != X86::LEA64r ||
              Inst.getNumOperands() < 6 || !Inst.getOperand(0).isReg())
            continue;
          unsigned Dst = Inst.getOperand(0).getReg(), Base, Index;
          int64_t Scale, Disp;
          if (!memOps(Inst, 1, Base, Scale, Index, Disp))
            continue;
          if (Scale == 8 && Index && (Index == F0 || Index == F1) &&
              Base != P && Base && !defsInLoop(Base))
            AddrAlias[Dst] = Base;
        }
      // pass 3: gather uses
      for (BinaryBasicBlock *BB : Blocks)
        for (MCInst &Inst : *BB) {
          unsigned Op = Inst.getOpcode();
          unsigned Base = 0, Index = 0;
          int64_t Scale = 1, Disp = 0;
          if (Op == X86::MOV64rm) {
            if (!Inst.getOperand(0).isReg() ||
                !memOps(Inst, 1, Base, Scale, Index, Disp))
              continue;
          } else if (Op == X86::CMP64rm || Op == X86::CMP32rm) {
            if (!memOps(Inst, 1, Base, Scale, Index, Disp))
              continue;
          } else if (Op == X86::CMP64mi8 || Op == X86::CMP64mi32) {
            if (!memOps(Inst, 0, Base, Scale, Index, Disp))
              continue;
          } else {
            continue;
          }
          if (Scale == 8 && Index && (Index == F0 || Index == F1) &&
              Base != P && Base && !defsInLoop(Base))
            Gathers.insert(Base);
          else if (Index == 0 && Disp <= 8) {
            auto It2 = AddrAlias.find(Base);
            if (It2 != AddrAlias.end())
              Gathers.insert(It2->second);
          }
        }
      if (!F0 || Gathers.empty()) {
        Reject("no record-field load or no fixed-base gather");
        continue;
      }
      // drop gathers that alias each other (same array in different regs)
      std::vector<unsigned> Gs(Gathers.begin(), Gathers.end());
      std::vector<unsigned> Unique;
      for (unsigned G : Gs) {
        bool Dup = false;
        for (unsigned U : Unique)
          if (BC.MIB->getAliases(G).test(U) || BC.MIB->getAliases(U).test(G))
            Dup = true;
        if (!Dup)
          Unique.push_back(G);
      }
      bool Dual = (Stride == 16 && F0 && F1);
      if (Dual && Unique.size() < 2) {
        Reject("dual mode needs two gather bases");
        continue;
      }
      if (!Dual && Unique.size() < 1) {
        Reject("no gather base");
        continue;
      }
      unsigned G1 = Unique[0];
      unsigned G2 = Dual ? Unique[1] : 0u;
      if (Dual && Unique.size() > 2) {
        Reject("more than two distinct gather bases (alias unknown)");
        continue;
      }
      if (!Dual && Unique.size() > 1) {
        Reject("single mode but multiple distinct gather bases");
        continue;
      }

      // --- 4. liveness: two dead GPRs + dead flags at header entry ---
      DataflowInfoManager DIM(BF, &RA, nullptr);
      LivenessAnalysis &LA = DIM.getLivenessAnalysis();
      const BitVector &LiveIn = LA.getLiveIn(*Header->begin());
      auto RegLive = [&](MCPhysReg R) {
        return LiveIn.anyCommon(BC.MIB->getAliases(R));
      };
      if (RegLive(X86::EFLAGS)) {
        Reject("flags live at header");
        continue;
      }
      unsigned S1 = 0, S2 = 0;
      for (unsigned R : {X86::RAX, X86::RCX, X86::RDX, X86::RBX, X86::RSI,
                          X86::RDI, X86::R8, X86::R9, X86::R10, X86::R11,
                          X86::R12, X86::R13, X86::R14, X86::R15}) {
        if (R == P || R == E || R == G1 || R == G2 || RegLive(R))
          continue;
        if (!S1)
          S1 = R;
        else {
          S2 = R;
          break;
        }
      }
      if (!S1 || !S2) {
        Reject("no two dead scratch GPRs at header");
        continue;
      }
      if (!RegLive(P) || !RegLive(E) || !RegLive(G1) ||
          (G2 && !RegLive(G2))) {
        Reject("P/E/G not live at header (unexpected)");
        continue;
      }
      std::string Mode = Dual ? "dual" : "single";
      auto LC = [&](unsigned R) { return StringRef(BC.MRI->getName(R)).lower(); };
      std::string Line = BF.getOneName().str() + ":0x" +
                         utohexstr(Header->getInputOffset()) + ":" +
                         std::to_string(opts::PresageDistance.getValue()) + ":" +
                         LC(P) + "," + LC(E) + "," + LC(G1) + "," +
                         (G2 ? LC(G2) : std::string("-")) + "," + LC(S1) + "," + LC(S2);
      BC.errs() << "BOLT-INFO: presage-scan: CANDIDATE " << BF.getOneName()
                << "+0x" << utohexstr(Header->getInputOffset()) << " mode="
                << Mode << " stride=" << Stride
                << " P=" << BC.MRI->getName(P) << " E=" << BC.MRI->getName(E)
                << " G1=" << BC.MRI->getName(G1)
                << (G2 ? (" G2=" + StringRef(BC.MRI->getName(G2)).str()) : "")
                << " S=" << BC.MRI->getName(S1) << "," << BC.MRI->getName(S2)
                << "\n";
      Cands.push_back({Line, Mode});
    }
  }

  std::error_code EC;
  raw_fd_ostream Out(opts::PresageScanOut.empty()
                         ? "-"
                         : opts::PresageScanOut.getValue(),
                     EC);
  Out << "# auto-generated by -presage-prefetch-scan-only\n";
  for (const Cand &C : Cands)
    Out << C.Line << "\n";
  BC.errs() << "BOLT-INFO: presage-scan: loops_seen=" << LoopsSeen
            << " rejected=" << LoopsRejected << " candidates=" << Cands.size()
            << " -> " << opts::PresageScanOut << "\n";
}

Error PresagePrefetch::runOnFunctions(BinaryContext &BC) {
  if (!BC.isX86()) {
    BC.errs() << "BOLT-ERROR: presage-prefetch is x86-only\n";
    exit(1);
  }
  if (opts::PresageScanOnly) {
    std::unique_ptr<BinaryFunctionCallGraph> CG =
        std::make_unique<BinaryFunctionCallGraph>(buildCallGraph(BC));
    RegAnalysis RA(BC, &BC.getBinaryFunctions(), &*CG);
    scanSites(BC, RA);
    return Error::success();
  }
  if (!opts::PresagePrefetchFlag) {
    BC.errs() << "BOLT-INFO: presage-prefetch: disabled\n";
    return Error::success();
  }
  parseSiteList(BC);
  if (Sites.empty()) {
    BC.errs() << "BOLT-INFO: presage-prefetch: no sites, nothing to do\n";
    return Error::success();
  }
  std::unique_ptr<BinaryFunctionCallGraph> CG =
      std::make_unique<BinaryFunctionCallGraph>(buildCallGraph(BC));
  RegAnalysis RA(BC, &BC.getBinaryFunctions(), &*CG);
  for (auto &It : BC.getBinaryFunctions()) {
    BinaryFunction &BF = It.second;
    if (!shouldOptimize(BF))
      continue;
    runOnFunction(BC, BF, RA);
  }
  std::string Reasons;
  for (const std::string &R : RejectReasons)
    Reasons += "  " + R + "\n";
  BC.errs() << "BOLT-INFO: presage-prefetch:\naccepted=" << Accepted
            << "\nrejected=" << Rejected << "\n";
  if (!Reasons.empty())
    BC.errs() << "reason:\n" << Reasons;
  else
    BC.errs() << "reason= (none)\n";
  return Error::success();
}
