/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/hlo_scheduling.h"

#include <map>
#include <utility>
#include <vector>

#include "tensorflow/compiler/xla/service/heap_simulator.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/tuple_points_to_analysis.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/stringprintf.h"
#include "tensorflow/core/platform/logging.h"

using ::tensorflow::strings::HumanReadableNumBytes;

namespace xla {

StatusOr<int64> MinimumMemoryForSequence(
    const SequentialHloOrdering::HloModuleSequence& module_sequence,
    const LogicalBuffer::SizeFunction& size_function) {
  if (module_sequence.empty()) {
    return 0;
  }

  const HloModule* module = module_sequence.begin()->first->parent();
  TF_ASSIGN_OR_RETURN(std::unique_ptr<TuplePointsToAnalysis> points_to_analysis,
                      TuplePointsToAnalysis::Run(module));

  // The absolute minimum memory required for a given sequence of instructions
  // is determined by the sequence of Alloc and Free calls on a simulated heap,
  // ignoring fragmentation. We run the heap simulation on the whole module,
  // rather than summing each computation, since it gives us a better lower
  // bound, by minimizing the liveness of sub-computations.
  TF_ASSIGN_OR_RETURN(
      HeapSimulator::Result result,
      HeapSimulator::Run(MakeUnique<NoFragmentationStatsHeap>(), *module,
                         module_sequence, *points_to_analysis, size_function));
  return result.heap_size;
}

namespace {

// Class implementing a list scheduler of HLO instructions which produces a
// sequence which minimizes memory usage by preferring to schedule the node that
// frees bigger buffer and defines smaller outputs.
//
// Note that list scheduler is a greedy algorithm which cannot guarantee a
// global optimal solution. As a counterexample, considering the following
// graph:
//
//      +--> B ===> C -------+
// A -> |                    |
//      |                    v
//      +--> D ---> F=======>G
//      |           ^
//      |           |
//      +--> E -----+
//
//  --> : Buffer with size 1
//  ==> : Buffer with size 2
//
// The list scheduler will always try to defer scheduling B in a greedy way
// since its output buffer is bigger than input. The sequence it creates will
// be:
//   A D E F B C G
// , which has a maximum memory usage of 6 (B is alive while F is executing).
//
// An optimal way to shedule the previous graph is:
//   A B C D E F G
// , which has a maximum memory usage of 5 (when F is executing).
//
class ListScheduler {
 public:
  // Construct and return a memory-minimizing sequence of HLO instructions
  // containing the given HLO computation.
  static StatusOr<std::vector<const HloInstruction*>> Run(
      const HloComputation& computation,
      const TuplePointsToAnalysis& points_to_analysis,
      const LogicalBuffer::SizeFunction& size_function,
      const tensorflow::gtl::FlatMap<const HloComputation*, int64>&
          memory_by_computation) {
    ListScheduler scheduler(computation, points_to_analysis, size_function,
                            memory_by_computation);
    return scheduler.CreateSchedule();
  }

  // Returns whether the memory used by the given HLO should be ignored by the
  // scheduling heuristic.
  static bool IgnoreInstruction(const HloInstruction& instruction) {
    return instruction.opcode() == HloOpcode::kParameter ||
           instruction.opcode() == HloOpcode::kConstant;
  }

 private:
  // The scheduling priority of an instruction is first the number of bytes
  // freed by scheduling the instruction, and second (tie-breaker) by the number
  // of users. This is represented as a std::pair containing these two values
  // (first element is the bytes freed). std::pair provides the necessary
  // comparison operators.
  using Priority = std::pair<int64, int64>;

  ListScheduler(const HloComputation& computation,
                const TuplePointsToAnalysis& points_to_analysis,
                const LogicalBuffer::SizeFunction& size_function,
                const tensorflow::gtl::FlatMap<const HloComputation*, int64>&
                    memory_by_computation)
      : computation_(computation),
        points_to_analysis_(points_to_analysis),
        size_function_(size_function),
        memory_by_computation_(memory_by_computation) {
    // Create a map containing the LogicalBuffer uses for each HLO
    // instruction. An HLO instruction "uses" a LogicalBuffer if the
    // LogicalBuffer is in an operand of the instruction as indicated by
    // points-to analysis.
    for (auto* instruction : computation.instructions()) {
      tensorflow::gtl::FlatSet<const LogicalBuffer*> instr_uses;
      for (auto* operand : instruction->operands()) {
        points_to_analysis.GetPointsToSet(operand).ForEachElement(
            [&](const ShapeIndex& /*index*/,
                const PointsToSet::BufferList& buffers) {
              instr_uses.insert(buffers.begin(), buffers.end());
            });
      }
      buffer_uses_[instruction] = std::vector<const LogicalBuffer*>(
          instr_uses.begin(), instr_uses.end());
    }

    // Create map containing the number of unscheduled uses (hlo instructions)
    // of each logical buffer.
    for (auto* instruction : computation.instructions()) {
      for (auto* buffer :
           points_to_analysis.GetBuffersDefinedByInstruction(instruction)) {
        unscheduled_use_count_[buffer] = 0;
      }
    }
    for (auto* instruction : computation.instructions()) {
      for (const LogicalBuffer* buffer : buffer_uses_.at(instruction)) {
        ++unscheduled_use_count_[buffer];
      }
    }

    // Buffers live out of the computation have an implicit use at the end of
    // the computation.
    for (const LogicalBuffer* live_out_buffer :
         points_to_analysis.GetPointsToSet(computation.root_instruction())
             .CreateFlattenedSet()) {
      ++unscheduled_use_count_[live_out_buffer];
    }
  }

  // Returns whether the memory used by the given buffer should be ignored by
  // the scheduling heuristic.
  static bool IgnoreBuffer(const LogicalBuffer& buffer) {
    return IgnoreInstruction(*buffer.instruction());
  }

  // An entry in the worklist used by CreateSchedule.  Corresponds to one
  // HloInstruction, plus some cached metadata, saved for the purposes of making
  // BytesFreedIfScheduled fast.
  struct ReadyListEntry {
    const HloInstruction* instruction;

    // The total size of all buffers defined by this instruction.
    int64 bytes_defined;

    // For each buffer B used by this instruction, we keep a pair (B, U), where
    // U is the number of uses of B that have not yet been scheduled. This pair
    // is a pointer into the unscheduled_use_count_ map, so it gets updated for
    // free when we update counts in the map.
    std::vector<const std::pair<const LogicalBuffer* const, int64>*>
        used_buffer_unscheduled_use_counts;
  };

  // Creates a ReadyListEntry for the given instruction.
  ReadyListEntry MakeReadyListEntry(const HloInstruction* instruction) {
    ReadyListEntry entry;
    entry.instruction = instruction;

    entry.bytes_defined = 0;
    for (auto* buffer :
         points_to_analysis_.GetBuffersDefinedByInstruction(instruction)) {
      if (!IgnoreBuffer(*buffer)) {
        entry.bytes_defined += size_function_(*buffer);
      }
    }

    for (auto* buffer : buffer_uses_.at(instruction)) {
      if (IgnoreBuffer(*buffer)) {
        continue;
      }
      auto unscheduled_use_count_it = unscheduled_use_count_.find(buffer);
      CHECK(unscheduled_use_count_it != unscheduled_use_count_.end());
      entry.used_buffer_unscheduled_use_counts.push_back(
          &*unscheduled_use_count_it);
    }
    return entry;
  }

  // Returns the number of bytes freed if the HLO instruction is scheduled.
  // If the instruction calls subcomputations, we count the memory used by the
  // subcomputations as memory "defined" by the instruction. This is not
  // entirely accurate, because subcomputation memory will be freed after the
  // instruction finishes. But it is more accurate than not taking
  // subcomputations into account at all. In the future, we may improve
  // accounting for subcomputation memory (b/65409243).
  int64 BytesFreedIfScheduled(const ReadyListEntry& entry) {
    int64 freed_bytes = 0;
    for (const auto& kv : entry.used_buffer_unscheduled_use_counts) {
      auto buffer = kv->first;
      auto use_count = kv->second;
      if (use_count == 1) {
        freed_bytes += size_function_(*buffer);
      }
    }
    // We only count the memory usage of the largest subcomputation, instead of
    // adding them all, because subcomputations won't execute in parallel.
    int64 max_subcomputation_bytes = 0;
    for (const auto* c : entry.instruction->called_computations()) {
      auto it = memory_by_computation_.find(c);
      if (it != memory_by_computation_.end()) {
        int64 subcomputation_bytes = it->second;
        if (subcomputation_bytes > max_subcomputation_bytes) {
          max_subcomputation_bytes = subcomputation_bytes;
        }
      }
    }
    return freed_bytes - entry.bytes_defined - max_subcomputation_bytes;
  }

  // Constructs the scheduling priority of the given instruction.
  Priority GetPriority(const ReadyListEntry& entry) {
    return {BytesFreedIfScheduled(entry), entry.instruction->user_count()};
  }

  std::vector<const HloInstruction*> CreateSchedule() {
    std::vector<const HloInstruction*> schedule;

    // Populate the ready list with instructions which have no operands or
    // control predecessors.
    tensorflow::gtl::FlatMap<const HloInstruction*, int64>
        unscheduled_pred_count;
    for (auto* instruction : computation_.instructions()) {
      // TODO(b/34466113): Replace this and above with successors() or
      // predecessors() when these methods are added to HloInstruction.
      for (const HloInstruction* user : instruction->users()) {
        unscheduled_pred_count[user]++;
      }
      for (const HloInstruction* succ : instruction->control_successors()) {
        unscheduled_pred_count[succ]++;
      }
    }

    // Use a multimap to sort ReadyListEntry according to their priority.
    std::multimap<Priority, ReadyListEntry> ready_queue;

    // Map of ready instructions to their iterators in ready_queue.
    tensorflow::gtl::FlatMap<const HloInstruction*,
                             std::multimap<Priority, ReadyListEntry>::iterator>
        ready_instructions;

    auto add_to_ready_queue = [&](HloInstruction* inst) {
      auto entry = MakeReadyListEntry(inst);
      auto it = ready_queue.emplace(GetPriority(entry), std::move(entry));
      ready_instructions[inst] = it;
    };

    for (auto* instruction : computation_.instructions()) {
      // Instruction with no operands or control predecessors will
      // not be in the map.
      if (unscheduled_pred_count.count(instruction) == 0) {
        add_to_ready_queue(instruction);
      }
    }

    while (!ready_queue.empty()) {
      // Remove the selected instruction from the ready list and add it to the
      // schedule.
      auto best_it = ready_queue.end();
      --best_it;
      const HloInstruction* best = best_it->second.instruction;
      ready_queue.erase(best_it);
      ready_instructions.erase(best);
      schedule.push_back(best);
      scheduled_instructions_.insert(best);

      bool adjust_ready_queue = false;
      // Update the unscheduled uses of the logical buffers.
      for (const LogicalBuffer* buffer : buffer_uses_.at(best)) {
        int64& count = unscheduled_use_count_[buffer];
        CHECK_GT(count, 0);
        --count;
        if (count == 1) {
          adjust_ready_queue = true;
        }
      }

      // Add new instructions to ready list.
      auto update_pred_count = [&](HloInstruction* inst) {
        int64 pred_count = --unscheduled_pred_count.at(inst);
        CHECK_GE(pred_count, 0);
        if (pred_count == 0) {
          add_to_ready_queue(inst);
        }
      };
      // TODO(b/34466113): Replace this and above with successors() or
      // predecessors() when these methods are added to HloInstruction.
      for (HloInstruction* user : best->users()) {
        update_pred_count(user);
      }
      for (HloInstruction* succ : best->control_successors()) {
        update_pred_count(succ);
      }
      // The unscheduled use count for a buffer has changed to 1, so the
      // priorities of some ready instructions may go up. We update them in the
      // ready queue, so that they can appear earlier.
      if (adjust_ready_queue) {
        for (HloInstruction* operand : best->operands()) {
          for (HloInstruction* operand_user : operand->users()) {
            auto ready_instructions_it = ready_instructions.find(operand_user);
            if (ready_instructions_it == ready_instructions.end()) {
              continue;
            }
            auto ready_queue_it = ready_instructions_it->second;
            auto& entry = ready_queue_it->second;
            Priority new_priority = GetPriority(entry);
            if (new_priority == ready_queue_it->first) {
              continue;
            }
            // Create a new entry in ready_queue, then update
            // ready_instructions[operand_user] to refer to the new entry.
            ready_instructions_it->second =
                ready_queue.emplace(new_priority, std::move(entry));
            // Remove the old entry in ready_queue.
            ready_queue.erase(ready_queue_it);
          }
        }
      }
    }
    CHECK_EQ(schedule.size(), computation_.instruction_count());
    CHECK_EQ(scheduled_instructions_.size(), computation_.instruction_count());

    return schedule;
  }

  const HloComputation& computation_;
  const TuplePointsToAnalysis& points_to_analysis_;
  const LogicalBuffer::SizeFunction& size_function_;
  // Computations are analyzed in post-order. When scheduling an instruction
  // that includes subcomputations, such as a while loop, we use this map to
  // look up the memory needed by subcomputations.
  const tensorflow::gtl::FlatMap<const HloComputation*, int64>&
      memory_by_computation_;

  // A map containing the LogicalBuffers that each instruction uses.
  tensorflow::gtl::FlatMap<const HloInstruction*,
                           std::vector<const LogicalBuffer*>>
      buffer_uses_;

  // A map containing the count of unscheduled HLOs which using a particular
  // LogicalBuffer.  We rely on iterator stability in this map, and that the map
  // entries are std::pair's.
  std::unordered_map<const LogicalBuffer*, int64> unscheduled_use_count_;

  // Set of instructions which have been scheduled.
  tensorflow::gtl::FlatSet<const HloInstruction*> scheduled_instructions_;
};

int64 SumLogicalBufferSizes(
    const TuplePointsToAnalysis::BufferDefinitionVector& buffers,
    const LogicalBuffer::SizeFunction& size_function) {
  int64 size = 0;
  for (const LogicalBuffer* buffer : buffers) {
    size += size_function(*buffer);
  }
  return size;
}

StatusOr<std::vector<const HloInstruction*>> CreateMemoryMinimizingSequence(
    const HloComputation& computation,
    const TuplePointsToAnalysis& points_to_analysis,
    const LogicalBuffer::SizeFunction& size_function,
    const MemorySchedulerAlgorithm& algorithm,
    const tensorflow::gtl::FlatMap<const HloComputation*, int64>&
        memory_by_computation) {
  VLOG(2) << "Computation: " << computation.name();
  if (algorithm) {
    return algorithm(computation, points_to_analysis, size_function,
                     memory_by_computation);
  }
  return DefaultMemoryScheduler(computation, points_to_analysis, size_function,
                                memory_by_computation);
}

}  // namespace

StatusOr<int64> MinimumMemoryForComputation(
    const HloComputation& computation,
    const std::vector<const HloInstruction*>& sequence,
    const TuplePointsToAnalysis& points_to_analysis,
    const LogicalBuffer::SizeFunction& size_function) {
  TF_ASSIGN_OR_RETURN(
      HeapSimulator::Result result,
      HeapSimulator::Run(MakeUnique<NoFragmentationStatsHeap>(), computation,
                         sequence, points_to_analysis, size_function));
  return result.heap_size;
}

StatusOr<std::vector<const HloInstruction*>> DFSMemoryScheduler(
    const HloComputation& computation,
    const TuplePointsToAnalysis& points_to_analysis,
    const LogicalBuffer::SizeFunction& size_function,
    const tensorflow::gtl::FlatMap<const HloComputation*, int64>&
        memory_by_computation) {
  // This ordering is based on DFS post-order, with a heuristic to decide which
  // operand to visit first.  The heuristic is based on 'extra_users', which is
  // simply users-1 for each instruction.  By subtracting 1, we're saying that
  // instructions with no users or a single user don't count; instructions with
  // lots of fan-out will be visited earlier.
  int64 cumulative_total_size = 0;
  tensorflow::gtl::FlatMap<const HloInstruction*, int64> extra_users;
  tensorflow::gtl::FlatMap<const HloInstruction*, int64> total_sizes;
  for (const HloInstruction* hlo : computation.MakeInstructionPostOrder()) {
    if (ListScheduler::IgnoreInstruction(*hlo)) {
      extra_users[hlo] = 0;
      total_sizes[hlo] = 0;
      continue;
    }
    extra_users[hlo] = hlo->users().empty() ? 0 : hlo->users().size() - 1;
    int64 logical_buffer_size = SumLogicalBufferSizes(
        points_to_analysis.GetBuffersDefinedByInstruction(hlo), size_function);
    total_sizes[hlo] = logical_buffer_size;
    cumulative_total_size += logical_buffer_size;
    tensorflow::gtl::FlatSet<const HloInstruction*> unique_operands(
        hlo->operands().begin(), hlo->operands().end());
    for (const HloInstruction* operand : unique_operands) {
      extra_users[hlo] += extra_users[operand];
      total_sizes[hlo] += total_sizes[operand];
    }
    total_sizes[hlo] = std::min(total_sizes[hlo], cumulative_total_size);
  }
  CHECK_EQ(extra_users.size(), computation.instruction_count());
  CHECK_EQ(total_sizes.size(), computation.instruction_count());

  // Construct a total order based on DFS post-order, visiting operands in
  // decreasing cumulative extra user order, and next by cumulative size, with a
  // tiebreaker by name for determinism.
  std::vector<const HloInstruction*> sequence;
  FunctionVisitor visitor([&sequence](HloInstruction* hlo) {
    sequence.push_back(hlo);
    return Status::OK();
  });
  TF_RETURN_IF_ERROR(computation.AcceptWithOperandOrder(
      &visitor, [&extra_users, &total_sizes](const HloInstruction* a,
                                             const HloInstruction* b) {
        if (extra_users[a] != extra_users[b]) {
          return extra_users[a] > extra_users[b];
        }
        if (total_sizes[a] != total_sizes[b]) {
          return total_sizes[a] > total_sizes[b];
        }
        return a->name() < b->name();
      }));
  CHECK_EQ(sequence.size(), computation.instruction_count());
  return sequence;
}  // namespace xla

StatusOr<std::vector<const HloInstruction*>> ListMemoryScheduler(
    const HloComputation& computation,
    const TuplePointsToAnalysis& points_to_analysis,
    const LogicalBuffer::SizeFunction& size_function,
    const tensorflow::gtl::FlatMap<const HloComputation*, int64>&
        memory_by_computation) {
  return ListScheduler::Run(computation, points_to_analysis, size_function,
                            memory_by_computation);
}

StatusOr<std::vector<const HloInstruction*>> PostOrderMemoryScheduler(
    const HloComputation& computation,
    const TuplePointsToAnalysis& points_to_analysis,
    const LogicalBuffer::SizeFunction& size_function,
    const tensorflow::gtl::FlatMap<const HloComputation*, int64>&
        memory_by_computation) {
  const auto& post_order = computation.MakeInstructionPostOrder();
  return std::vector<const HloInstruction*>{post_order.begin(),
                                            post_order.end()};
}

StatusOr<std::vector<const HloInstruction*>> DefaultMemoryScheduler(
    const HloComputation& computation,
    const TuplePointsToAnalysis& points_to_analysis,
    const LogicalBuffer::SizeFunction& size_function,
    const tensorflow::gtl::FlatMap<const HloComputation*, int64>&
        memory_by_computation) {
  // We try a few schedulers and choose whichever returns a lower min-memory,
  // not accounting for fragmentation.
  // - List is a scheduler that uses greedy heuristics.
  // - DFS visits HLOs in postorder, with a heuristic to decide the order of
  //   children.
  // - Postorder does not use any heuristics.
  // List wins for most of our benchmarks; postorder-based schedulers win for
  // some RNNs.
  TF_ASSIGN_OR_RETURN(
      std::vector<const HloInstruction*> list_sequence,
      ListMemoryScheduler(computation, points_to_analysis, size_function,
                          memory_by_computation));
  TF_ASSIGN_OR_RETURN(
      const int64 list_memory,
      MinimumMemoryForComputation(computation, list_sequence,
                                  points_to_analysis, size_function));
  VLOG(2) << "Min-memory list sequence: " << HumanReadableNumBytes(list_memory);

  TF_ASSIGN_OR_RETURN(std::vector<const HloInstruction*> dfs_sequence,
                      DFSMemoryScheduler(computation, points_to_analysis,
                                         size_function, memory_by_computation));
  TF_ASSIGN_OR_RETURN(
      const int64 dfs_memory,
      MinimumMemoryForComputation(computation, dfs_sequence, points_to_analysis,
                                  size_function));
  VLOG(2) << "Min-memory dfs sequence: " << HumanReadableNumBytes(dfs_memory);

  TF_ASSIGN_OR_RETURN(
      std::vector<const HloInstruction*> post_order_sequence,
      PostOrderMemoryScheduler(computation, points_to_analysis, size_function,
                               memory_by_computation));
  TF_ASSIGN_OR_RETURN(
      const int64 post_order_memory,
      MinimumMemoryForComputation(computation, post_order_sequence,
                                  points_to_analysis, size_function));
  VLOG(2) << "Min-memory post order sequence: "
          << HumanReadableNumBytes(post_order_memory);

  auto min_memory = std::min({dfs_memory, post_order_memory, list_memory});

  if (min_memory == list_memory) {
    VLOG(2) << "Chose min-memory list sequence: "
            << HumanReadableNumBytes(list_memory);
    return list_sequence;
  } else if (min_memory == dfs_memory) {
    VLOG(2) << "Chose min-memory dfs sequence: "
            << HumanReadableNumBytes(dfs_memory);
    return dfs_sequence;
  } else {
    VLOG(2) << "Chose min-memory post_order sequence: "
            << HumanReadableNumBytes(post_order_memory);
    return post_order_sequence;
  }
}

StatusOr<SequentialHloOrdering::HloModuleSequence>
CreateMemoryMinimizingSequence(const HloModule& module,
                               const LogicalBuffer::SizeFunction& size_function,
                               const MemorySchedulerAlgorithm& algorithm) {
  SequentialHloOrdering::HloModuleSequence sequence;
  TF_ASSIGN_OR_RETURN(std::unique_ptr<TuplePointsToAnalysis> points_to_analysis,
                      TuplePointsToAnalysis::Run(&module));
  tensorflow::gtl::FlatMap<const HloComputation*, int64> memory_by_computation;
  for (const auto* computation : module.MakeComputationPostOrder()) {
    if (!computation->IsFusionComputation()) {
      TF_ASSIGN_OR_RETURN(auto one_computation_sequence,
                          CreateMemoryMinimizingSequence(
                              *computation, *points_to_analysis, size_function,
                              algorithm, memory_by_computation));
      memory_by_computation[computation] =
          MinimumMemoryForComputation(*computation, one_computation_sequence,
                                      *points_to_analysis, size_function)
              .ValueOrDie();
      sequence[computation] = std::move(one_computation_sequence);
    }
  }
  return sequence;
}

StatusOr<std::vector<const HloInstruction*>> CreateMemoryMinimizingSequence(
    const HloComputation& computation,
    const LogicalBuffer::SizeFunction& size_function) {
  CHECK(!computation.IsFusionComputation());
  TF_ASSIGN_OR_RETURN(std::unique_ptr<TuplePointsToAnalysis> points_to_analysis,
                      TuplePointsToAnalysis::Run(computation.parent()));
  tensorflow::gtl::FlatMap<const HloComputation*, int64> empty_map;
  return CreateMemoryMinimizingSequence(computation, *points_to_analysis,
                                        size_function, nullptr, empty_map);
}

}  // namespace xla
