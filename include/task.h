#pragma once

#include <stdint.h>

#include <bitset>
#include <cassert>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "cov.h"

namespace rgd {

// JIT'ed function for each relational constraint
typedef void(*test_fn_type)(uint64_t*);

// the first two slots of the arguments for reseved for the left and right operands
static const int RET_OFFSET = 2;

struct Constraint {
  Constraint() = delete;
  Constraint(int ast_size): fn(nullptr), const_num(0) {
    ast = std::make_shared<AstNode>(ast_size);
  }
  Constraint(const Constraint&) = default; // XXX: okay to use default?
  const AstNode *get_root() const { return const_cast<const AstNode*>(ast.get()); }

  // JIT'ed function for a comparison expression
  test_fn_type fn;
  // the AST
  std::shared_ptr<AstNode> ast;

  // During constraint collection, (symbolic) input bytes are recorded
  // as offsets from the beginning of the input.  However, the JIT'ed
  // function consumes inputs as an input array.  So, when building the
  // function, we need to map the offset to the idx in input array,
  // which is stored in local_map.
  std::map<size_t, uint32_t> local_map;
  // if const {false, const value}, if symbolic {true, index in the inputs}
  // during local search, we use a single global array (to avoid memory
  // allocation and free) to prepare the inputs, so we need to know where
  // to load the input values into the input array.
  std::vector<std::pair<bool, uint64_t>> input_args;
  // map the offset to iv (initial value)
  std::unordered_map<uint32_t, uint8_t> inputs;
  // shape information about the input (e.g., 1, 2, 4, 8 bytes)
  std::unordered_map<uint32_t, uint32_t> shapes;
  // special infomation for atoi: offset -> (result_length, base, str_length)
  std::unordered_map<uint32_t, std::tuple<uint32_t, uint32_t, uint32_t>> atoi_info;
  // record the involved operations
  std::bitset<rgd::LastOp> ops;
  // number of constant in the input array
  uint32_t const_num;
  // recorded comparison operands
  uint64_t op1, op2;
};

struct ConsMeta {
  // per-constraint arg mapping, so we can share the constraints
  std::vector<std::pair<bool, uint64_t>> input_args;
  // per-constraint relational operator, so we can share the AST
  uint32_t comparison;
  // input2state inference related
  std::vector<std::pair<size_t, uint32_t>> i2s_candidates;
  uint64_t op1, op2;
};

struct SearchTask {
  SearchTask(): scratch_args(nullptr), max_const_num(0),
      stopped(false), attempts(0), solved(false), skip_next(false),
      base_task(nullptr) {}
  SearchTask(const SearchTask&) = delete;
  ~SearchTask() { if (scratch_args) free(scratch_args); }
  bool has_finalized() const { return scratch_args != nullptr; }

  uint32_t num_exprs;
  // constraints, could be shared, strictly read-only
  std::vector<std::shared_ptr<const Constraint>> constraints;
  // temporary storage for the comparison operation
  std::vector<uint32_t> comparisons;
  // per-constraint mutable metadata
  std::vector<std::unique_ptr<ConsMeta>> consmeta;

  // inputs as pairs of <offset (from the beginning of the input, and value>
  std::vector<std::pair<uint32_t, uint8_t>> inputs;
  // shape information at each offset
  std::unordered_map<uint32_t, uint32_t> shapes;
  // aggreated atoi info
  std::unordered_map<uint32_t, std::tuple<uint32_t, uint32_t, uint32_t>> atoi_info;
  // max number of constants in the input array
  uint32_t max_const_num;
  // record constraints that use a certain input byte
  std::unordered_map<uint32_t, std::vector<size_t>> cmap;
  // the input array used for all JIT'ed functions
  // all input bytes are extended to 64 bits
  uint64_t* scratch_args;

  // intermediate states for the search
  std::vector<uint64_t> min_distances; // current best
  std::vector<uint64_t> distances; // general scratch
  std::vector<uint64_t> plus_distances; // used in partial derivation
  std::vector<uint64_t> minus_distances; // used in partial derivation

  // statistics
  uint64_t start; //start time
  bool stopped;
  int attempts;

  // solutions
  bool solved;
  std::unordered_map<size_t, uint8_t> solution;

  // base task
  std::shared_ptr<SearchTask> base_task;
  bool skip_next; // FIXME: an ugly hack to skip the next task

  void finalize() {
    // aggregate the contraints, map each input byte to a constraint to
    // an index in the "global" input array (i.e., the scratch_args)
    std::unordered_map<uint32_t, uint32_t> sym_map;
    uint32_t gidx = 0;
    for (size_t i = 0; i < constraints.size(); i++) {
      std::unique_ptr<ConsMeta> cm = std::make_unique<ConsMeta>();
      cm->input_args = constraints[i]->input_args;
      cm->comparison = comparisons[i];
      uint32_t last_offset = -1;
      uint32_t size = 0;
      for (const auto& [offset, lidx] : constraints[i]->local_map) {
        auto gitr = sym_map.find(offset);
        if (gitr == sym_map.end()) {
          gidx = inputs.size();
          sym_map[offset] = gidx;
          inputs.push_back(std::make_pair(offset, constraints[i]->inputs.at(offset)));
          shapes[offset] = constraints[i]->shapes.at(offset);
        } else {
          gidx = gitr->second;
        }
        // record input to constraint mapping
        // skip memcmp constraints
        if (cm->comparison != rgd::Memcmp && cm->comparison != rgd::MemcmpN) {
          auto slot = cmap.find(gidx);
          if (slot != cmap.end()) {
            slot->second.push_back(i);
          } else {
            cmap.emplace(std::make_pair(gidx, std::vector<size_t>{i}));
          }
        }
        // save the mapping between the local index (i.e., where the JIT'ed
        // function is going to read the input from) and the global index
        // (i.e., where the current value corresponding to the input byte
        // is stored in MutInput)
        cm->input_args[lidx].second = gidx;

        // check if the input bytes are consecutive
        // using std::map ensures that the offsets (keys) are sorted
        if (last_offset != -1 && last_offset + 1 != offset) {
          // a new set of consecutive input bytes, save the info
          // and resset
          cm->i2s_candidates.push_back({last_offset + 1 - size, size});
          size = 0;
        }
        last_offset = offset;
        size++;
      }
      // save the last set of consecutive input bytes
      cm->i2s_candidates.push_back({last_offset + 1 - size, size});

      // process atoi
      for (const auto& [offset, info] : constraints[i]->atoi_info) {
        // check dependencies
        uint32_t length = std::get<2>(info);
        for (auto j = 0; j < length; ++j) {
          auto ditr = cmap.find(offset + j);
          if (ditr != cmap.end()) {
            fprintf(stderr, "atoi bytes (%d) used in other constraints\n", offset + j);
          }
        }
        auto itr = atoi_info.find(offset);
        if (itr != atoi_info.end()) {
          fprintf(stderr, "atoi bytes (%d) already exists\n", offset);
          assert(info == itr->second);
        }
        atoi_info[offset] = info;
      }

      // update the number of required constants in the input array
      if (max_const_num < constraints[i]->const_num)
        max_const_num = constraints[i]->const_num;

      // insert the constraint metadata
      consmeta.push_back(std::move(cm));
    }

    // allocate the input array, reserver 2 for comparison operands a,b
    scratch_args = (uint64_t*)aligned_alloc(sizeof(*scratch_args),
        (2 + inputs.size() + max_const_num + 1) * sizeof(*scratch_args));
    min_distances.resize(constraints.size(), 0);
    distances.resize(constraints.size(), 0);
    plus_distances.resize(constraints.size(), 0);
    minus_distances.resize(constraints.size(), 0);
  }

  void load_hint() { // load hint from base task
    if (!base_task || !base_task->solved) return;
    for (auto itr = inputs.begin(); itr != inputs.end(); itr++) {
      auto got = base_task->solution.find(itr->first);
      if (got != base_task->solution.end())
        itr->second = got->second;
    }
  }

};

using task_t = std::shared_ptr<rgd::SearchTask>;

}; // namespace rgd
