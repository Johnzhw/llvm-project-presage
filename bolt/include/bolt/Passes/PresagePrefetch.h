//===- bolt/Passes/PresagePrefetch.h -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Presage mechanism-transfer prefetch insertion (reproduction experiment).
//
// Inserts a fixed, branch-free, machine-verified lookahead-prefetch sequence
// at basic-block entry points specified by an external site list. The site
// list carries the register roles derived from a source/binary analysis
// (see docs/presage-reproduction.md):
//
//   function:hexoffset:dist:P,E,G1,G2,S1,S2
//
//     P  = streamed record pointer (16-byte records, e.g. packed_edge IJ[k])
//     E  = one-past-end pointer of the record array
//     G1,G2 = base registers of the two codependent gathered int64 arrays
//     S1,S2 = scratch registers, required dead (not live-in) at insertion
//
// Emitted sequence (D = dist, disp = 16*D):
//
//   lea   disp(P), S1        # &rec[k+D]
//   lea   (P),    S2         # &rec[k]   (in-bounds clamp target)
//   cmp   S1, E               # E - S1 ; CF|ZF <=> E <= S1 (out of range)
//   cmovbe S2, S1             # clamp to &rec[k] when lookahead is OOB
//   mov   (S1),    S2         # pi = rec[k+D].v0
//   mov   8(S1),   S1         # pj = rec[k+D].v1
//   prefetcht0 (G1,S2,8)      # gather1[pi]
//   prefetcht0 (G1,S1,8)      # gather1[pj]
//   prefetcht0 (G2,S2,8)      # gather2[pi]
//   prefetcht0 (G2,S1,8)      # gather2[pj]
//
// Guarantees: no new scratch (S1/S2 must be dead at insertion, verified by
// liveness), no architectural state change (flags dead at insertion, verified;
// all flag effects consumed inside the sequence), no branches/calls, no
// change to register allocation. The bound gate makes the lookahead loads
// always in-bounds; prefetches never fault regardless.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BOLT_PASSES_PRESAGEPREFETCH_H
#define LLVM_BOLT_PASSES_PRESAGEPREFETCH_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class PresagePrefetch : public BinaryFunctionPass {
  /// Per-site data, parsed from the site list.
  struct Site {
    std::string Func;
    uint64_t Offset;
    bool Absolute = false; // '@'-prefixed: absolute input address
    bool Chain = false;    // chain mode: insert after BB[Idx]
    unsigned Idx = 0;      // chain mode: instruction index inside BB
    std::string PSpec;     // chain mode: RPOS register or "m<disp>" stack slot
    unsigned Dist;
    unsigned P, E, G1, G2, S1, S2; // MCPhysReg ids
    std::string Raw;
  };

  std::vector<Site> Sites;
  unsigned Accepted = 0;
  unsigned Rejected = 0;
  std::vector<std::string> RejectReasons;

  void parseSiteList(BinaryContext &BC);
  bool runOnFunction(BinaryContext &BC, BinaryFunction &BF,
                     class RegAnalysis &RA);
  void scanSites(BinaryContext &BC, class RegAnalysis &RA);

public:
  explicit PresagePrefetch() : BinaryFunctionPass(false) {}

  const char *getName() const override { return "presage-prefetch"; }

  bool shouldPrint(const BinaryFunction &BF) const override { return false; }

  Error runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif
