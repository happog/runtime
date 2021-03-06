// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//===- host_context.cc - CPU thread and memory abstraction ----------------===//
//
// This file implements the generic interface for thread pool abstractions.
//
//===----------------------------------------------------------------------===//

#include "tfrt/host_context/host_context.h"

#include "llvm/Support/Error.h"
#include "tfrt/host_context/chain.h"
#include "tfrt/host_context/concurrent_work_queue.h"
#include "tfrt/host_context/function.h"
#include "tfrt/host_context/host_allocator.h"
#include "tfrt/host_context/location.h"
#include "tfrt/host_context/shared_context.h"
#include "tfrt/support/mutex.h"
#include "tfrt/support/string_util.h"

namespace tfrt {

void LocationHandler::VtableAnchor() {}

std::atomic<int> HostContext::num_shared_context_types_{0};
static std::atomic<int> next_host_context_index{0};
HostContext* HostContext::all_host_contexts_[HostContextPtr::kDummyIndex];

HostContext::HostContext(
    std::function<void(const DecodedDiagnostic&)> diag_handler,
    std::unique_ptr<HostAllocator> allocator,
    std::unique_ptr<ConcurrentWorkQueue> work_queue)
    : diag_handler_(std::move(diag_handler)),
      allocator_(std::move(allocator)),
      work_queue_(std::move(work_queue)),
      shared_context_mgr_(std::make_unique<SharedContextManager>(this)),
      instance_ptr_{next_host_context_index.fetch_add(1)} {
  assert(instance_index() < HostContextPtr::kDummyIndex &&
         "Created too many HostContext instances");
  all_host_contexts_[instance_index()] = this;
  ready_chain_ = MakeAvailableAsyncValueRef<Chain>();
}

HostContext::~HostContext() {
  // We need to free the ready chain AsyncValue first, as the destructor of the
  // AsyncValue calls the HostContext to free its memory.
  ready_chain_.reset();
  all_host_contexts_[instance_index()] = nullptr;
}

void Function::VtableAnchor() {}

// Construct an empty IndirectAsyncValue, not forwarding to anything.
RCReference<IndirectAsyncValue> HostContext::MakeIndirectAsyncValue() {
  return TakeRef(Construct<IndirectAsyncValue>(instance_ptr_));
}

//===----------------------------------------------------------------------===//
// Error Reporting
//===----------------------------------------------------------------------===//

// Emit an error for a specified decoded diagnostic, which gets funneled
// through a location handler.
void HostContext::EmitError(const DecodedDiagnostic& diagnostic) {
  // Emit the message to the global handler, guaranteeing that it will be seen
  // by the handler registered with the HostContext.
  diag_handler_(diagnostic);
}

// Create a ConcreteAsyncValue in error state for a specified decoded
// diagnostic.
RCReference<ErrorAsyncValue> HostContext::MakeErrorAsyncValueRef(
    DecodedDiagnostic&& diagnostic) {
  // Create an AsyncValue for this error condition.
  auto* error_value =
      Construct<ErrorAsyncValue>(instance_ptr_, std::move(diagnostic));

  return TakeRef(error_value);
}

// Create a ConcreteAsyncValue in error state for a specified error message.
RCReference<ErrorAsyncValue> HostContext::MakeErrorAsyncValueRef(
    string_view message) {
  return MakeErrorAsyncValueRef(DecodedDiagnostic(message));
}

void HostContext::CancelExecution(string_view msg) {
  // Create an AsyncValue in error state for cancel.
  auto* error_value = MakeErrorAsyncValueRef(msg).release();

  AsyncValue* expected_value = nullptr;
  // Use memory_order_release for the success case so that error_value is
  // visible to other threads when they load with memory_order_acquire. For the
  // failure case, we do not care about expected_value, so we can use
  // memory_order_relaxed.
  if (!cancel_value_.compare_exchange_strong(expected_value, error_value,
                                             std::memory_order_release,
                                             std::memory_order_relaxed)) {
    error_value->DropRef();
  }
}

void HostContext::Restart() {
  // Use memory_order_acq_rel so that previous writes on this thread are visible
  // to other threads and previous writes from other threads (e.g. the return
  // 'value') are visible to this thread.
  auto value = cancel_value_.exchange(nullptr, std::memory_order_acq_rel);
  if (value) {
    value->DropRef();
  }
}

//===----------------------------------------------------------------------===//
// Memory Management
//===----------------------------------------------------------------------===//

// Allocate the specified number of bytes at the specified alignment.
void* HostContext::AllocateBytes(size_t size, size_t alignment) {
  return allocator_->AllocateBytes(size, alignment);
}

// Deallocate the specified pointer, that had the specified size.
void HostContext::DeallocateBytes(void* ptr, size_t size) {
  allocator_->DeallocateBytes(ptr, size);
}

//===----------------------------------------------------------------------===//
// Concurrency
//===----------------------------------------------------------------------===//

void HostContext::Quiesce() { work_queue_->Quiesce(); }

void HostContext::Await(ArrayRef<RCReference<AsyncValue>> values) {
  work_queue_->Await(values);
}

// Add some work to the workqueue managed by this CPU device.
void HostContext::EnqueueWork(llvm::unique_function<void()> work) {
  work_queue_->AddTask(TaskFunction(std::move(work)));
}

// Add some work to the workqueue managed by this CPU device.
bool HostContext::EnqueueBlockingWork(llvm::unique_function<void()> work) {
  Optional<TaskFunction> task = work_queue_->AddBlockingTask(
      TaskFunction(std::move(work)), /*allow_queuing=*/true);
  return !task.hasValue();
}

int HostContext::GetNumWorkerThreads() const {
  return work_queue_->GetParallelismLevel();
}

// Run the specified function when the specified set of AsyncValue's are all
// resolved.  This is a set-version of "AndThen".
void HostContext::RunWhenReady(ArrayRef<AsyncValue*> values,
                               llvm::unique_function<void()>&& callee) {
  // Perform a quick scan of the arguments.  If they are all available, or if
  // any is already an error, then we can run the callee synchronously.
  SmallVector<AsyncValue*, 4> unavailable_values;
  for (auto i : values) {
    if (!i->IsAvailable()) unavailable_values.push_back(i);
  }

  // If we can synchronously call 'callee', then do it and we're done.
  if (unavailable_values.empty()) return callee();

  // If there is exactly one unavailable value, then we can just AndThen it.
  if (unavailable_values.size() == 1) {
    unavailable_values[0]->AndThen(
        [callee = std::move(callee)]() mutable { callee(); });
    return;
  }

  struct CounterAndCallee {
    std::atomic<size_t> counter;
    llvm::unique_function<void()> callee;
  };

  // Otherwise, we have multiple unavailable values.  Put a counter on the heap
  // and have each unavailable value decrement and test it.
  auto* data =
      new CounterAndCallee{{unavailable_values.size()}, std::move(callee)};

  for (auto* val : unavailable_values) {
    val->AndThen([data]() {
      // Decrement the counter unless we're the last to be here.
      if (data->counter.fetch_sub(1) != 1) return;

      // If we are the last one, then run the callee and free the data.
      data->callee();
      delete data;
    });
  }
}

namespace {

// If ParallelFor will choose to execute `compute` function asynchronously, it
// will move all the arguments into this context, and will keep it on the heap,
// until all submitted asynchronous work is completed.
class ParallelForExecutionContext {
 public:
  static ParallelForExecutionContext* Allocate(
      HostContext* host, size_t n, size_t block_size,
      llvm::unique_function<void(size_t, size_t)> compute,
      llvm::unique_function<void()> on_done) {
    return new ParallelForExecutionContext(
        host, n, block_size, std::move(compute), std::move(on_done));
  }

  // EvalBlocks() recursively splits the assigned block range and enqueues work
  // to the HostContext. This improves latency, by removing a sequential step
  // from the caller thread. After enqueuing work to the host context, it
  // evaluates a single block in the caller thread.
  void EvalBlocks(size_t start_block, size_t end_block) {
    while (end_block - start_block > 1) {
      const size_t mid_block = start_block + (end_block - start_block) / 2;

      // Evaluate [mid_block, end_block) blocks.
      host_->EnqueueWork(
          [this, mid_block, end_block]() { EvalBlocks(mid_block, end_block); });

      // Current range becomes [start_block, mid_block).
      end_block = mid_block;
    }

    assert(end_block - start_block == 1);

    // Call `compute` for a single block.
    compute_(start_block * block_size_, std::min(n_, end_block * block_size_));

    // Delete this context if it was the last block.
    if (pending_blocks_.fetch_sub(1) == 1) delete this;
  }

  int PendingBlocks() { return pending_blocks_; }

 private:
  ParallelForExecutionContext(
      HostContext* host, size_t n, size_t block_size,
      llvm::unique_function<void(size_t, size_t)> compute,
      llvm::unique_function<void()> on_done)
      : host_(host),
        n_(n),
        block_size_(block_size),
        pending_blocks_(DivUp(n, block_size)),
        compute_(std::move(compute)),
        on_done_(std::move(on_done)) {}

  ~ParallelForExecutionContext() { on_done_(); }

  static size_t DivUp(const size_t x, const size_t y) {
    assert(y > 0);
    return (x + y - 1) / y;
  }

  HostContext* host_;

  size_t n_;
  size_t block_size_;
  std::atomic<size_t> pending_blocks_;

  llvm::unique_function<void(size_t, size_t)> compute_;
  llvm::unique_function<void()> on_done_;
};

}  // namespace

void HostContext::ParallelFor(
    size_t n, llvm::unique_function<void(size_t, size_t)> compute,
    llvm::unique_function<void()> on_done, size_t min_block_size) {
  // Do not create too many small blocks.
  static constexpr size_t kMaxOversharding = 4;
  const size_t block_size =
      std::max(min_block_size, n / (kMaxOversharding * GetNumWorkerThreads()));

  assert(min_block_size >= 1 && "Illegal min block size");
  assert(block_size >= 1 && "Illegal block size");

  // Execute single block in the caller thread.
  if (n <= block_size) {
    compute(0, n);
    on_done();
    return;
  }

  // Allocate parallel for execution context on the heap.
  ParallelForExecutionContext* ctx = ParallelForExecutionContext::Allocate(
      this, n, block_size, std::move(compute), std::move(on_done));
  ctx->EvalBlocks(0, ctx->PendingBlocks());
}

//===----------------------------------------------------------------------===//
// SharedContext management
//===----------------------------------------------------------------------===//
//
class HostContext::SharedContextManager {
 public:
  explicit SharedContextManager(HostContext* host) : host_{host} {}
  // Returns the shared context instance with the given shared_context_id.
  // Create one if the requested shared context instance does not exist yet.
  SharedContext& GetOrCreateSharedContext(int shared_context_id,
                                          SharedContextFactory factory) {
    assert(shared_context_id < shared_context_instances_.size() &&
           "The requested SharedContext ID exceeds the maximum allowed");

    auto& item = shared_context_instances_[shared_context_id];

    std::call_once(item.second, [&] {
      assert(!item.first);
      item.first = factory(host_);
    });

    return *item.first;
  }

 private:
  HostContext* const host_;
  // We allow up to 256 ShareContext instances.
  std::array<std::pair<std::unique_ptr<SharedContext>, std::once_flag>, 256>
      shared_context_instances_{};
};

SharedContext& HostContext::GetOrCreateSharedContext(
    int shared_context_id, SharedContextFactory factory) {
  return shared_context_mgr_->GetOrCreateSharedContext(shared_context_id,
                                                       factory);
}

}  // namespace tfrt
