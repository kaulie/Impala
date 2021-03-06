// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef IMPALA_EXEC_EXEC_NODE_H
#define IMPALA_EXEC_EXEC_NODE_H

#include <vector>
#include <sstream>

#include "common/status.h"
#include "runtime/descriptors.h"  // for RowDescriptor
#include "util/runtime-profile.h"
#include "util/blocking-queue.h"
#include "gen-cpp/PlanNodes_types.h"

namespace impala {

class Expr;
class ObjectPool;
class Counters;
class RowBatch;
class RuntimeState;
class TPlan;
class TupleRow;
class DataSink;
class MemTracker;

// Superclass of all executor nodes.
// All subclasses need to make sure to check RuntimeState::is_cancelled()
// periodically in order to ensure timely termination after the cancellation
// flag gets set.
class ExecNode {
 public:
  // Init conjuncts.
  ExecNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs);

  virtual ~ExecNode();

  // Initializes this object from the thrift tnode desc. The subclass should
  // do any initialization that can fail in Init() rather than the ctor.
  // If overridden in subclass, must first call superclass's Init().
  virtual Status Init(const TPlanNode& tnode);

  // Sets up internal structures, etc., without doing any actual work.
  // Must be called prior to Open(). Will only be called once in this
  // node's lifetime.
  // All code generation (adding functions to the LlvmCodeGen object) must happen
  // in Prepare().  Retrieving the jit compiled function pointer must happen in
  // Open().
  // If overridden in subclass, must first call superclass's Prepare().
  virtual Status Prepare(RuntimeState* state);

  // Performs any preparatory work prior to calling GetNext().
  // Caller must not be holding any io buffers. This will cause deadlock.
  virtual Status Open(RuntimeState* state) = 0;

  // Retrieves rows and returns them via row_batch. Sets eos to true
  // if subsequent calls will not retrieve any more rows.
  // Data referenced by any tuples returned in row_batch must not be overwritten
  // by the callee until Close() is called. The memory holding that data
  // can be returned via row_batch's tuple_data_pool (in which case it may be deleted
  // by the caller) or held on to by the callee. The row_batch, including its
  // tuple_data_pool, will be destroyed by the caller at some point prior to the final
  // Close() call.
  // In other words, if the memory holding the tuple data will be referenced
  // by the callee in subsequent GetNext() calls, it must *not* be attached to the
  // row_batch's tuple_data_pool.
  // Caller must not be holding any io buffers. This will cause deadlock.
  // TODO: AggregationNode and HashJoinNode cannot be "re-opened" yet.
  virtual Status GetNext(RuntimeState* state, RowBatch* row_batch, bool* eos) = 0;

  // Close() will get called for every exec node, regardless of what else is called and
  // the status of these calls (i.e. Prepare() may never have been called, or
  // Prepare()/Open()/GetNext() returned with an error).
  // Close() releases all resources that were allocated in Open()/GetNext(), even if the
  // latter ended with an error. Close() can be called if the node has been prepared or
  // the node is closed.
  // The default implementation updates runtime profile counters and calls
  // Close() on the children. Subclasses should check if the node has already been
  // closed (is_closed()), then close themselves, then call the base Close().
  // Nodes that are using tuples returned by a child may call Close() on their children
  // before their own Close() if the child node has returned eos.
  virtual void Close(RuntimeState* state);

  // Creates exec node tree from list of nodes contained in plan via depth-first
  // traversal. All nodes are placed in pool.
  // Returns error if 'plan' is corrupted, otherwise success.
  static Status CreateTree(ObjectPool* pool, const TPlan& plan,
                           const DescriptorTbl& descs, ExecNode** root);

  // Set debug action for node with given id in 'tree'
  static void SetDebugOptions(int node_id, TExecNodePhase::type phase,
                              TDebugAction::type action, ExecNode* tree);

  // Collect all nodes of given 'node_type' that are part of this subtree, and return in
  // 'nodes'.
  void CollectNodes(TPlanNodeType::type node_type, std::vector<ExecNode*>* nodes);

  // Collect all scan node types.
  void CollectScanNodes(std::vector<ExecNode*>* nodes);

  // Evaluate exprs over row.  Returns true if all exprs return true.
  // TODO: This doesn't use the vector<Expr*> signature because I haven't figured
  // out how to deal with declaring a templated std:vector type in IR
  static bool EvalConjuncts(Expr* const* exprs, int num_exprs, TupleRow* row);

  // Codegen function to evaluate the conjuncts.  Returns NULL if codegen was
  // not supported for the conjunct exprs.
  // Codegen'd signature is bool EvalConjuncts(Expr** exprs, int num_exprs, TupleRow*);
  // The first two arguments are ignored (the Expr's are baked into the codegen)
  // but it is included so the signature can match EvalConjuncts.
  static llvm::Function* CodegenEvalConjuncts(LlvmCodeGen* codegen,
      const std::vector<Expr*>& conjuncts);

  // Returns a string representation in DFS order of the plan rooted at this.
  std::string DebugString() const;

  // Recursive helper method for generating a string for DebugString().
  // Implementations should call DebugString(int, std::stringstream) on their children.
  // Input parameters:
  //   indentation_level: Current level in plan tree.
  // Output parameters:
  //   out: Stream to accumulate debug string.
  virtual void DebugString(int indentation_level, std::stringstream* out) const;

  const std::vector<Expr*>& conjuncts() const { return conjuncts_; }

  int id() const { return id_; }
  TPlanNodeType::type type() const { return type_; }
  const RowDescriptor& row_desc() const { return row_descriptor_; }
  int64_t rows_returned() const { return num_rows_returned_; }
  int64_t limit() const { return limit_; }
  bool ReachedLimit() { return limit_ != -1 && num_rows_returned_ >= limit_; }

  RuntimeProfile* runtime_profile() { return runtime_profile_.get(); }
  MemTracker* mem_tracker() { return mem_tracker_.get(); }

  // Extract node id from p->name().
  static int GetNodeIdFromProfile(RuntimeProfile* p);

  // Names of counters shared by all exec nodes
  static const std::string ROW_THROUGHPUT_COUNTER;

 protected:
  friend class DataSink;

  // Extends blocking queue for row batches. Row batches have a property that
  // they must be processed in the order they were produced, even in cancellation
  // paths. Preceding row batches can contain ptrs to memory in subsequent row batches
  // and we need to make sure those ptrs stay valid.
  // Row batches that are added after Shutdown() are queued in another queue, which can
  // be cleaned up during Close().
  // All functions are thread safe.
  class RowBatchQueue : public BlockingQueue<RowBatch*> {
   public:
    // max_batches is the maximum number of row batches that can be queued.
    // When the queue is full, producers will block.
    RowBatchQueue(int max_batches);
    ~RowBatchQueue();

    // Adds a batch to the queue. This is blocking if the queue is full.
    void AddBatch(RowBatch* batch);

    // Gets a row batch from the queue. Returns NULL if there are no more.
    // This function blocks.
    // Returns NULL after Shutdown().
    RowBatch* GetBatch();

    // Deletes all row batches in cleanup_queue_. Not valid to call AddBatch()
    // after this is called.
    // Returns the number of io buffers that were released (for debug tracking)
    int Cleanup();

   private:
    // Lock protecting cleanup_queue_
    SpinLock lock_;

    // Queue of orphaned row batches
    std::list<RowBatch*> cleanup_queue_;
  };

  int id_;  // unique w/in single plan tree
  TPlanNodeType::type type_;
  ObjectPool* pool_;
  std::vector<Expr*> conjuncts_;

  // True if the codegen'd function for 'conjuncts_' is thread safe.  If not, copies
  // of the conjuncts_ need to be made if the conjuncts will be evaluated by multiple
  // threads.
  // TODO: we need to make Expr's always thread safe and this can be removed.
  bool codegend_conjuncts_thread_safe_;

  std::vector<ExecNode*> children_;
  RowDescriptor row_descriptor_;

  // debug-only: if debug_action_ is not INVALID, node will perform action in
  // debug_phase_
  TExecNodePhase::type debug_phase_;
  TDebugAction::type debug_action_;

  int64_t limit_;  // -1: no limit
  int64_t num_rows_returned_;

  boost::scoped_ptr<RuntimeProfile> runtime_profile_;
  RuntimeProfile::Counter* rows_returned_counter_;
  RuntimeProfile::Counter* rows_returned_rate_;

  // Account for peak memory used by this node
  boost::scoped_ptr<MemTracker> mem_tracker_;

  // Execution options that are determined at runtime.  This is added to the
  // runtime profile at Close().  Examples for options logged here would be
  // "Codegen Enabled"
  boost::mutex exec_options_lock_;
  std::string runtime_exec_options_;

  ExecNode* child(int i) { return children_[i]; }
  bool is_closed() { return is_closed_; }

  // Create a single exec node derived from thrift node; place exec node in 'pool'.
  static Status CreateNode(ObjectPool* pool, const TPlanNode& tnode,
                           const DescriptorTbl& descs, ExecNode** node);

  static Status CreateTreeHelper(ObjectPool* pool, const std::vector<TPlanNode>& tnodes,
      const DescriptorTbl& descs, ExecNode* parent, int* node_idx, ExecNode** root);

  Status PrepareConjuncts(RuntimeState* state);

  virtual bool IsScanNode() const { return false; }

  void InitRuntimeProfile(const std::string& name);

  // Executes debug_action_ if phase matches debug_phase_.
  // 'phase' must not be INVALID.
  Status ExecDebugAction(TExecNodePhase::type phase, RuntimeState* state);

  // Appends option to 'runtime_exec_options_'
  void AddRuntimeExecOption(const std::string& option);

 private:
  // Set in ExecNode::Close(). Used to make Close() idempotent. This is not protected
  // by a lock, it assumes all calls to Close() are made by the same thread.
  bool is_closed_;
};

}
#endif

