# SAVIO: a human guide

SAVIO looks at a small part of a function and decides whether some work
should run on the scalar units or the vector units.

You normally do **not** need to understand the whole pass. Find the task you
want to change below and open the listed file.

## The five steps

SAVIO follows this path:

1. **Find usable instructions.** Candidate analysis rejects operations whose
   types, flags, addresses, or target costs are not safe.
2. **Make groups.** The partitioner joins connected scalar or vector
   instructions into groups and makes one region graph.
3. **Price one choice.** The cost model asks, “what would this region cost if
   these groups moved?”
4. **Try different choices.** The search tries different fractions of each
   group class and optional virtual unrolling.
5. **Rewrite the IR.** The selected decision is changed into scalar or vector
   instructions.

```text
IR
 |
 +--> CandidateAnalysis.cpp --> Partitioner.cpp
                                  |
                                  +--> OperationCosts.cpp
                                  |       operation and transfer prices
                                  |
                                  +--> CostModel.cpp
                                          assignment graph and estimates
                                                  |
                                                  v
                                         FractionSearch.cpp
                                                  |
                                                  v
                                           SAVIOIR.cpp
                                      Scalarize / Vectorize rewrites
```

## Which file should I edit?

| I want to... | Start here | Also read |
| --- | --- | --- |
| Allow or reject an instruction | `CandidateAnalysis.cpp` | `Availability.cpp`, `PartitionerUtils.h` |
| Change how instructions become groups | `Partitioner.cpp` | `Partitioner.h` |
| Change the price of an add, shift, load, or transfer | `OperationCosts.cpp` | `PortDescription.h` |
| Change how a complete assignment is built and priced | `CostModel.cpp` | `RegionCost.cpp` |
| Change how many groups are tried | `FractionSearch.cpp` | `SearchCore.h` |
| Change scalarization | `Scalarize.cpp` | `OperationCosts.cpp` |
| Change singleton vectorization or vector divides | `SingletonVectorize.cpp` | `Availability.cpp` |
| Change SAVIO's pass-level loop | `SAVIOIR.cpp` | `Partitioner.h` |
| Add or validate CPU data | `PortDescription.cpp` | `src/port-info/README.md` |
| Understand the target-independent timing estimate | `RegionCost.cpp` | `RegionCost.h` |

## Cost-model changes

There are three separate layers:

* **`OperationCosts.cpp`** converts one operation or transfer into an
  `OpCost`. This is the best place to try a different instruction-price rule.
* **`CostModel.cpp`** builds the dependency graph for a proposed scalar/vector
  assignment. It handles inputs, live-outs, recurrence edges, virtual
  unrolling, and vector divides.
* **`RegionCost.cpp`** turns the finished graph into resource, recurrence,
  dispatch, and memory bounds.

To experiment with a new cost model, keep the `Region`, `RegionGraph`,
`Decision`, and `RegionEstimate` interfaces unchanged. Replace or extend the
operation pricing in `OperationCosts.cpp`, or add a new assignment builder
beside `CostModel.cpp`. Keep `FractionSearch.cpp` unchanged unless the search
policy itself is changing.

The private headers have deliberately small jobs:

* `PartitionerUtils.h`: instruction and candidate helpers shared by analysis,
  partitioning, and costing.
* `CostModelUtils.h`: operation-pricing helpers shared by the assignment
  builder and operation-cost module.

They are implementation headers. The public data structures and entry points
are in `llvm/include/llvm/Transforms/Vectorize/SAVIO/`.

## Rewrites

The cost model only chooses a `Decision`; it does not change LLVM IR.

* `SingletonVectorize.cpp` moves scalar work into vector lane 0 and handles
  vector divide lane groups.
* `Scalarize.cpp` turns selected vector work into scalar operations.
* `ExpandIntrinsics.cpp` lowers SAVIO-specific intrinsics to ordinary target
  IR.

When changing a decision, check the matching rewrite. A cost-model choice must
describe an operation that the rewrite can actually produce.

## Tests

Build:

```sh
ninja -C llvm-project/build LLVMVectorize VectorizeTests
```

Run all SAVIO unit tests:

```sh
llvm-project/build/unittests/Transforms/Vectorize/VectorizeTests \
  --gtest_filter='SAVIO*'
```

Run compiler tests:

```sh
llvm-project/build/bin/llvm-lit -q \
  llvm-project/llvm/test/Transforms/SAVIO \
  llvm-project/llvm/test/CodeGen/AArch64/savio-*.ll \
  llvm-project/llvm/test/CodeGen/X86/savio-*.ll
```

Test files follow the same responsibilities:

* `SAVIOGroupingTest.cpp`: candidates, groups, regions, and graph shape.
* `SAVIOCostModelTest.cpp`: assignment costs, fractions, scalarization, and
  bit counts.
* `SAVIOPortDescriptionTest.cpp`: JSON loading and target queries.
* `SAVIORegionCostTest.cpp`: graph scheduling and bounds.

When adding behavior, add the smallest test in the matching file. Run the
unit test before and after the change; the focused lit tests catch IR and
code-generation changes.

## Common debugging questions

**Why was an instruction not selected?**
Use `-debug-only=savio`. Candidate analysis reports missing target keys and
the reason an operation is unavailable.

**Why did a choice lose?**
Inspect the estimate printed by the cost model, then compare it with the
target data under `src/port-info/<arch>/<cpu>/`.

**Why did the output not change?**
Check `FractionSearch.cpp` first. The search requires a margin, and an
unavailable cost rejects a move rather than silently guessing.

**Why is a test about a different file failing?**
Decisions cross module boundaries. Check the `Decision` in `Partitioner.h`,
then follow it from `FractionSearch.cpp` to `SAVIOIR.cpp` and the rewrite.

## Rules to keep

* Do not silently invent a missing operation cost.
* Keep recurrence edges and live-out transfers intact.
* Keep generated port-information files generated.
* Preserve public headers unless the behavior change requires an API change.
* Record a focused test for every new decision or rewrite rule.
