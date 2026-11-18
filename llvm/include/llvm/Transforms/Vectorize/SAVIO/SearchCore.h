#ifndef LLVM_TRANSFORMS_VECTORIZE_SAVIO_SEARCHCORE_H
#define LLVM_TRANSFORMS_VECTORIZE_SAVIO_SEARCHCORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef double (*savio_estimate_fn)(void *Context,
                                    const unsigned *MovedPerClass,
                                    unsigned NumClasses);

/*
 * Pure C fraction search.
 *
 * Sizes are the number of movable instances in each class.  Recurrence is 1
 * for a class that must move as one instance and 0 for an ordinary slice.
 * MovedPerClass is initialized to zero by the caller and is updated with the
 * best assignment.  Margin is relative to the current estimate.
 */
double savio_search_fraction(unsigned NumClasses,
                             const unsigned *Sizes,
                             const unsigned *Recurrence,
                             unsigned *MovedPerClass,
                             double InitialEstimate,
                             double Margin,
                             void *Context,
                             savio_estimate_fn Estimate,
                             unsigned RoundLimit);

#ifdef __cplusplus
}
#endif

#endif
