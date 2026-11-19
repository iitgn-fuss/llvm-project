# SAVIO

SAVIO decides which groups of operations should stay in scalar code and which
groups should be combined into vector operations. It models the target's
execution ports, searches for a profitable scalar/vector split, and records the
decision used by the rewrite pass.

## Start here

| Question | Read |
| --- | --- |
| What data is available for a target? | `PortDescription.h/.cpp` |
| How are instructions classified and grouped? | `Partitioner.h`, `Partitioner.cpp` |
| How is a candidate assignment costed? | `RegionCost.h/.cpp`, `CostModel.cpp` |
| How is the best scalar/vector fraction selected? | `FractionSearch.cpp` |
| How are scalar and vector IR forms produced? | `Scalarize.cpp`, `SingletonVectorize.cpp`, `ExpandIntrinsics.cpp` |
| Where are the end-to-end decisions assembled? | `SAVIOIR.cpp` |

## Module map

| Module | Responsibility |
| --- | --- |
| `Availability` | Checks whether an instruction can participate in SAVIO. |
| `Partitioner` | Builds regions, discovers groups, classifies inputs, and forms the region graph. |
| `PartitionerUtils.h` | Private, small helpers shared by partitioning and costing. |
| `RegionCost` | Cost-node data structures and target-independent scheduling estimates. |
| `CostModel` | Builds the scalar/vector assignment graph with `NodeBuilder`. |
| `FractionSearch` | Searches per-class promotion fractions and applies force policies. |
| `PortDescription` | Loads and queries target execution-port descriptions. |
| `Scalarize` | Rewrites selected operations into scalar forms. |
| `SingletonVectorize` | Handles vectorization of singleton groups and live-outs. |
| `ExpandIntrinsics` | Expands operations that need explicit IR before rewriting. |
| `SAVIOIR` | Coordinates analysis, decisions, and IR changes. |

The implementation is intentionally split at behavior boundaries. `CostModel`
owns `NodeBuilder` and the exported `estimateAssignment` seam because those
pieces share the cost-node construction helpers. `FractionSearch` owns the
search state and calls that seam; it does not duplicate cost construction.

## Data flow

```text
LLVM IR
  |
  v
Availability -> Partitioner -> RegionGraph
                                  |
                                  v
                         CostModel / NodeBuilder
                                  |
                                  v
                         FractionSearch -> Decision
                                  |
                                  v
                         SAVIOIR rewrites IR
```

For each candidate region, partitioning creates groups and dependency edges.
The cost model builds a schedulable graph for a proposed assignment. The
fraction search evaluates evenly distributed promotions within each slice
class, respecting recurrence and target costs, then returns the best decision
that clears the configured margin.

## Public and private interfaces

The public interfaces are under
`llvm/include/llvm/Transforms/Vectorize/SAVIO/`. In particular,
`Partitioner.h`, `PortDescription.h`, `RegionCost.h`, and `SearchCore.h` are
consumed by the pass and tests.

`PartitionerUtils.h` is private implementation glue. Keep it beside the
implementation and do not add it to LLVM's public include tree. The
`estimateAssignment` declaration there is the only cross-file dependency
between assignment costing and fraction search.

## Adding a target port description

1. Add or update the target data under `src/port-info/<arch>/<cpu>/`.
2. Keep generated files generated: use the repository scripts for `ops.json`,
   `probes.json`, and `lane_transfers.json`.
3. Label every value as measured, estimated, or published and retain its
   evidence.
4. Add focused port-description and cost-model tests before changing expected
   decisions.
5. Run the port-info validation and SAVIO tests listed below.

## Testing

Build the implementation and unit-test binary:

```sh
ninja -C llvm-project/build LLVMVectorize VectorizeTests
```

Run all SAVIO unit tests:

```sh
llvm-project/build/unittests/Transforms/Vectorize/VectorizeTests \
  --gtest_filter='SAVIO*'
```

Run the focused LLVM lit suites:

```sh
llvm-project/build/bin/llvm-lit -q \
  llvm-project/llvm/test/Transforms/SAVIO \
  llvm-project/llvm/test/CodeGen/{AArch64,X86}/savio-*.ll
```

The unit tests are split by responsibility:

* `SAVIOGroupingTest.cpp` covers classification, grouping, and region
  formation.
* `SAVIOCostModelTest.cpp` covers assignment costs, fractions, scalarization,
  and bit-count handling.
* `SAVIOPortDescriptionTest.cpp` covers port data loading and queries.
* `SAVIORegionCostTest.cpp` covers target-independent cost scheduling.

## Debugging

Use LLVM's normal debug logging when investigating a decision:

```sh
llvm-project/build/bin/opt -debug-only=savio ...
```

Inspect the IR before and after the SAVIO pass, then compare the reported
region estimate with the target port description. A missing target cost should
remain visible as a diagnostic or an unavailable estimate; do not replace it
with a silent default.

## Invariants

* Preserve the public SAVIO headers and behavior when reorganizing files.
* Keep recurrence dependencies and live-out handling intact.
* A decision must be based on a complete assignment estimate or be rejected
  when required cost data is unavailable.
* Do not hand-edit generated port information.
* Add a focused test for every new behavior or target-specific rule.
