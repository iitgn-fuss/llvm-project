# SAVIO

SAVIO partitions eligible instructions between scalar and vector execution
units. It analyzes candidate instructions, constructs regions and assignment
graphs, estimates alternatives, selects a partition, and rewrites the LLVM IR.

## Processing pipeline

1. **Candidate analysis** - validates instruction types, flags, addresses, and
   target-operation availability.
2. **Partitioning** - forms connected instruction groups and region graphs.
3. **Assignment costing** - constructs and estimates scalar/vector assignments.
4. **Fraction search** - evaluates group-class fractions and optional virtual
   unrolling.
5. **IR rewriting** - materializes the selected scalar and vector operations.

```text
IR
 |
+--> CandidateAnalysis.cpp --> Partitioner.cpp
                                  |
                                  +--> OperationCosts.cpp
                                  |       operation and transfer costs
                                  |
                                  +--> CostModel.cpp
                                          assignment graph and estimates
                                                  |
                                                  v
                                         FractionSearch.cpp
                                                  |
                                                  v
                                           SAVIOIR.cpp
                                      scalarize/vectorize rewrites
```

## Source map

| Responsibility | Primary source | Related source |
| --- | --- | --- |
| Candidate eligibility and target-key checks | `CandidateAnalysis.cpp` | `Availability.cpp`, `PartitionerUtils.h` |
| Region and group formation | `Partitioner.cpp` | `Partitioner.h` |
| Individual operation and transfer costs | `OperationCosts.cpp` | `PortDescription.h` |
| Assignment graph construction and estimation | `CostModel.cpp` | `RegionCost.cpp` |
| Group-fraction search policy | `FractionSearch.cpp` | `SearchCore.h` |
| Scalarization | `Scalarize.cpp` | `OperationCosts.cpp` |
| Singleton vectorization and vector divides | `SingletonVectorize.cpp` | `Availability.cpp` |
| Pass-level loop and function orchestration | `SAVIOIR.cpp` | `Partitioner.h` |
| CPU data loading and queries | `PortDescription.cpp` | `src/port-info/README.md` |
| Target-independent graph bounds | `RegionCost.cpp` | `RegionCost.h` |

## Cost model extension points

The costing implementation has three layers:

* **`OperationCosts.cpp`** maps an operation or transfer to an `OpCost`.
  Modify this file for instruction-price rules.
* **`CostModel.cpp`** builds the dependency graph for a proposed assignment.
  It handles inputs, live-outs, recurrence edges, virtual unrolling, and
  vector divides.
* **`RegionCost.cpp`** computes resource, recurrence, dispatch, and memory
  bounds from the completed graph.

Alternative cost models should preserve the existing `Region`, `RegionGraph`,
`Decision`, and `RegionEstimate` interfaces. Operation pricing can be replaced
or extended in `OperationCosts.cpp`; assignment-graph behavior belongs in
`CostModel.cpp`. Search-policy changes belong in `FractionSearch.cpp`.

Private implementation interfaces:

* `PartitionerUtils.h` - shared instruction, candidate, and partitioning
  helpers.
* `CostModelUtils.h` - operation-pricing helpers shared with the assignment
  builder.

Public SAVIO data structures and entry points are under
`llvm/include/llvm/Transforms/Vectorize/SAVIO/`.

## IR rewriting

The cost model produces a `Decision`; it does not modify LLVM IR.

* `SingletonVectorize.cpp` materializes scalar work in vector lane zero and
  handles vector-divide lane groups.
* `Scalarize.cpp` converts selected vector work into scalar operations.
* `ExpandIntrinsics.cpp` lowers SAVIO-specific intrinsics to target IR.

Changes to decision semantics must be compatible with the corresponding
rewriter.

## Tests

Build the relevant targets:

```sh
ninja -C llvm-project/build LLVMVectorize VectorizeTests
```

Run SAVIO unit tests:

```sh
llvm-project/build/unittests/Transforms/Vectorize/VectorizeTests \
  --gtest_filter='SAVIO*'
```

Run focused compiler tests:

```sh
llvm-project/build/bin/llvm-lit -q \
  llvm-project/llvm/test/Transforms/SAVIO \
  llvm-project/llvm/test/CodeGen/AArch64/savio-*.ll \
  llvm-project/llvm/test/CodeGen/X86/savio-*.ll
```

| Test source | Coverage |
| --- | --- |
| `SAVIOGroupingTest.cpp` | Candidates, groups, regions, and graph shape |
| `SAVIOCostModelTest.cpp` | Assignment costs, fractions, scalarization, and bit counts |
| `SAVIOPortDescriptionTest.cpp` | JSON loading and target queries |
| `SAVIORegionCostTest.cpp` | Graph scheduling and bounds |

Add behavior tests to the file covering the changed responsibility. Use the
focused lit tests for IR and code-generation behavior.

## Debugging

Enable SAVIO diagnostics with `-debug-only=savio`.

| Symptom | First locations to inspect |
| --- | --- |
| Candidate is rejected | `CandidateAnalysis.cpp`, target data under `src/port-info/<arch>/<cpu>/` |
| Assignment loses | Cost estimate in `CostModel.cpp`, then target port data |
| No partition is selected | `FractionSearch.cpp` and its margin/availability checks |
| Rewrite output is unexpected | `Partitioner.h` decision flow, `FractionSearch.cpp`, `SAVIOIR.cpp`, and the selected rewriter |

## Invariants

* Missing operation costs must not be silently invented.
* Recurrence edges and live-out transfers must remain represented.
* Port-information files must be regenerated by their scripts.
* Public headers should remain stable unless an API change is required.
* New decision or rewrite rules require focused tests.
