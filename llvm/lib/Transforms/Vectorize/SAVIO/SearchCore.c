#include "llvm/Transforms/Vectorize/SAVIO/SearchCore.h"

#include <float.h>
#include <stddef.h>

static double estimate_or_infinite(savio_estimate_fn Estimate,
                                   void *Context,
                                   const unsigned *Moved,
                                   unsigned NumClasses) {
  if (!Estimate)
    return DBL_MAX;
  return Estimate(Context, Moved, NumClasses);
}

static unsigned effective_size(unsigned Size, const unsigned *Recurrence,
                                unsigned C) {
  return Recurrence && Recurrence[C] ? 1 : Size;
}

double savio_search_fraction(unsigned NumClasses,
                             const unsigned *Sizes,
                             const unsigned *Recurrence,
                             unsigned *MovedPerClass,
                             double InitialEstimate,
                             double Margin,
                             void *Context,
                             savio_estimate_fn Estimate,
                             unsigned RoundLimit) {
  if (!NumClasses || !Sizes || !MovedPerClass)
    return InitialEstimate;
  if (NumClasses > 64)
    return InitialEstimate;
  if (!Estimate)
    return InitialEstimate;

  unsigned Round;
  double After = InitialEstimate;
  for (Round = 0; Round < RoundLimit; ++Round) {
    int Changed = 0;
    unsigned C;
    for (C = 0; C < NumClasses; ++C) {
      unsigned U = effective_size(Sizes[C], Recurrence, C);
      unsigned Current = MovedPerClass[C];
      unsigned BestK = Current;
      double Best = After;
      unsigned K;
      for (K = 0; K <= U; ++K) {
        if (K == Current)
          continue;
        MovedPerClass[C] = K;
        double Candidate = estimate_or_infinite(Estimate, Context,
                                                MovedPerClass, NumClasses);
        if (Candidate < Best) {
          Best = Candidate;
          BestK = MovedPerClass[C];
        }
      }
      MovedPerClass[C] = Current;
      if (Best < After &&
          After - Best >= Margin * After) {
        MovedPerClass[C] = BestK;
        After = Best;
        Changed = 1;
      }
    }
    if (!Changed)
      break;
  }

  if (NumClasses <= 8) {
    unsigned A;
    for (A = 0; A < NumClasses; ++A) {
      if (Recurrence && Recurrence[A])
        continue;
      unsigned B;
      for (B = A + 1; B < NumClasses; ++B) {
        if (Recurrence && Recurrence[B])
          continue;

        unsigned CurrentA = MovedPerClass[A];
        unsigned CurrentB = MovedPerClass[B];
        unsigned BestA = CurrentA;
        unsigned BestB = CurrentB;
        double Best = After;
        unsigned KA;
        unsigned UA = Sizes[A];
        unsigned UB = Sizes[B];
        for (KA = 0; KA <= UA; ++KA) {
          unsigned KB;
          for (KB = 0; KB <= UB; ++KB) {
            if (KA == CurrentA && KB == CurrentB)
              continue;
            MovedPerClass[A] = KA;
            MovedPerClass[B] = KB;
            double Candidate = estimate_or_infinite(
                Estimate, Context, MovedPerClass, NumClasses);
            if (Candidate < Best) {
              Best = Candidate;
              BestA = KA;
              BestB = KB;
            }
          }
        }
        MovedPerClass[A] = CurrentA;
        MovedPerClass[B] = CurrentB;
        if (Best < After && After - Best >= Margin * After) {
          MovedPerClass[A] = BestA;
          MovedPerClass[B] = BestB;
          After = Best;
        }
      }
    }
  }

  return After;
}
