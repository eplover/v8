// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler-dispatcher/compiler-dispatcher.h"

#include "include/v8-platform.h"
#include "src/api.h"
#include "src/base/platform/semaphore.h"
#include "src/compiler-dispatcher/compiler-dispatcher-job.h"
#include "src/compiler-dispatcher/compiler-dispatcher-tracer.h"
#include "src/compiler.h"
#include "src/flags.h"
#include "src/handles.h"
#include "src/objects-inl.h"
#include "src/parsing/parse-info.h"
#include "src/parsing/parsing.h"
#include "src/v8.h"
#include "test/unittests/compiler-dispatcher/compiler-dispatcher-helper.h"
#include "test/unittests/test-utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace v8 {
namespace internal {

class CompilerDispatcherTestFlags {
 public:
  static void SetFlagsForTest() {
    CHECK_NULL(save_flags_);
    save_flags_ = new SaveFlags();
    FLAG_single_threaded = true;
    FLAG_ignition = true;
    FlagList::EnforceFlagImplications();
    FLAG_compiler_dispatcher = true;
  }

  static void RestoreFlags() {
    CHECK_NOT_NULL(save_flags_);
    delete save_flags_;
    save_flags_ = nullptr;
  }

 private:
  static SaveFlags* save_flags_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(CompilerDispatcherTestFlags);
};

SaveFlags* CompilerDispatcherTestFlags::save_flags_ = nullptr;

class CompilerDispatcherTest : public TestWithContext {
 public:
  CompilerDispatcherTest() = default;
  ~CompilerDispatcherTest() override = default;

  static void SetUpTestCase() {
    CompilerDispatcherTestFlags::SetFlagsForTest();
    TestWithContext::SetUpTestCase();
  }

  static void TearDownTestCase() {
    TestWithContext::TearDownTestCase();
    CompilerDispatcherTestFlags::RestoreFlags();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(CompilerDispatcherTest);
};

class CompilerDispatcherTestWithoutContext : public v8::TestWithIsolate {
 public:
  CompilerDispatcherTestWithoutContext() = default;
  ~CompilerDispatcherTestWithoutContext() override = default;

  static void SetUpTestCase() {
    CompilerDispatcherTestFlags::SetFlagsForTest();
    TestWithContext::SetUpTestCase();
  }

  static void TearDownTestCase() {
    TestWithContext::TearDownTestCase();
    CompilerDispatcherTestFlags::RestoreFlags();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(CompilerDispatcherTestWithoutContext);
};

namespace {

class MockPlatform : public v8::Platform {
 public:
  MockPlatform() : time_(0.0), time_step_(0.0), idle_task_(nullptr), sem_(0) {}
  ~MockPlatform() override {
    base::LockGuard<base::Mutex> lock(&mutex_);
    EXPECT_TRUE(foreground_tasks_.empty());
    EXPECT_TRUE(background_tasks_.empty());
    EXPECT_TRUE(idle_task_ == nullptr);
  }

  size_t NumberOfAvailableBackgroundThreads() override { return 1; }

  void CallOnBackgroundThread(Task* task,
                              ExpectedRuntime expected_runtime) override {
    base::LockGuard<base::Mutex> lock(&mutex_);
    background_tasks_.push_back(task);
  }

  void CallOnForegroundThread(v8::Isolate* isolate, Task* task) override {
    base::LockGuard<base::Mutex> lock(&mutex_);
    foreground_tasks_.push_back(task);
  }

  void CallDelayedOnForegroundThread(v8::Isolate* isolate, Task* task,
                                     double delay_in_seconds) override {
    UNREACHABLE();
  }

  void CallIdleOnForegroundThread(v8::Isolate* isolate,
                                  IdleTask* task) override {
    base::LockGuard<base::Mutex> lock(&mutex_);
    ASSERT_TRUE(idle_task_ == nullptr);
    idle_task_ = task;
  }

  bool IdleTasksEnabled(v8::Isolate* isolate) override { return true; }

  double MonotonicallyIncreasingTime() override {
    time_ += time_step_;
    return time_;
  }

  void RunIdleTask(double deadline_in_seconds, double time_step) {
    time_step_ = time_step;
    IdleTask* task;
    {
      base::LockGuard<base::Mutex> lock(&mutex_);
      task = idle_task_;
      ASSERT_TRUE(idle_task_ != nullptr);
      idle_task_ = nullptr;
    }
    task->Run(deadline_in_seconds);
    delete task;
  }

  bool IdleTaskPending() {
    base::LockGuard<base::Mutex> lock(&mutex_);
    return idle_task_;
  }

  bool BackgroundTasksPending() {
    base::LockGuard<base::Mutex> lock(&mutex_);
    return !background_tasks_.empty();
  }

  bool ForegroundTasksPending() {
    base::LockGuard<base::Mutex> lock(&mutex_);
    return !foreground_tasks_.empty();
  }

  void RunBackgroundTasksAndBlock(Platform* platform) {
    std::vector<Task*> tasks;
    {
      base::LockGuard<base::Mutex> lock(&mutex_);
      tasks.swap(background_tasks_);
    }
    platform->CallOnBackgroundThread(new TaskWrapper(this, tasks, true),
                                     kShortRunningTask);
    sem_.Wait();
  }

  void RunBackgroundTasks(Platform* platform) {
    std::vector<Task*> tasks;
    {
      base::LockGuard<base::Mutex> lock(&mutex_);
      tasks.swap(background_tasks_);
    }
    platform->CallOnBackgroundThread(new TaskWrapper(this, tasks, false),
                                     kShortRunningTask);
  }

  void RunForegroundTasks() {
    std::vector<Task*> tasks;
    {
      base::LockGuard<base::Mutex> lock(&mutex_);
      tasks.swap(foreground_tasks_);
    }
    for (auto& task : tasks) {
      task->Run();
      delete task;
    }
  }

  void ClearBackgroundTasks() {
    std::vector<Task*> tasks;
    {
      base::LockGuard<base::Mutex> lock(&mutex_);
      tasks.swap(background_tasks_);
    }
    for (auto& task : tasks) {
      delete task;
    }
  }

  void ClearForegroundTasks() {
    std::vector<Task*> tasks;
    {
      base::LockGuard<base::Mutex> lock(&mutex_);
      tasks.swap(foreground_tasks_);
    }
    for (auto& task : tasks) {
      delete task;
    }
  }

  void ClearIdleTask() {
    base::LockGuard<base::Mutex> lock(&mutex_);
    ASSERT_TRUE(idle_task_ != nullptr);
    delete idle_task_;
    idle_task_ = nullptr;
  }

 private:
  class TaskWrapper : public Task {
   public:
    TaskWrapper(MockPlatform* platform, const std::vector<Task*>& tasks,
                bool signal)
        : platform_(platform), tasks_(tasks), signal_(signal) {}
    ~TaskWrapper() = default;

    void Run() override {
      for (auto& task : tasks_) {
        task->Run();
        delete task;
      }
      if (signal_) platform_->sem_.Signal();
    }

   private:
    MockPlatform* platform_;
    std::vector<Task*> tasks_;
    bool signal_;

    DISALLOW_COPY_AND_ASSIGN(TaskWrapper);
  };

  double time_;
  double time_step_;

  // Protects all *_tasks_.
  base::Mutex mutex_;

  IdleTask* idle_task_;
  std::vector<Task*> background_tasks_;
  std::vector<Task*> foreground_tasks_;

  base::Semaphore sem_;

  DISALLOW_COPY_AND_ASSIGN(MockPlatform);
};

}  // namespace

TEST_F(CompilerDispatcherTest, Construct) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);
}

TEST_F(CompilerDispatcherTest, IsEnqueued) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script[] =
      "function g() { var y = 1; function f1(x) { return x * y }; return f1; } "
      "g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), script));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(dispatcher.Enqueue(shared));
  ASSERT_TRUE(dispatcher.IsEnqueued(shared));
  dispatcher.AbortAll(CompilerDispatcher::BlockingBehavior::kBlock);
  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(platform.IdleTaskPending());
  platform.ClearIdleTask();
}

TEST_F(CompilerDispatcherTest, FinishNow) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script[] =
      "function g() { var y = 1; function f2(x) { return x * y }; return f2; } "
      "g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), script));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_FALSE(shared->is_compiled());
  ASSERT_TRUE(dispatcher.Enqueue(shared));
  ASSERT_TRUE(dispatcher.FinishNow(shared));
  // Finishing removes the SFI from the queue.
  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(shared->is_compiled());
  ASSERT_TRUE(platform.IdleTaskPending());
  platform.ClearIdleTask();
}

TEST_F(CompilerDispatcherTest, IdleTask) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script[] =
      "function g() { var y = 1; function f3(x) { return x * y }; return f3; } "
      "g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), script));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(dispatcher.Enqueue(shared));
  ASSERT_TRUE(platform.IdleTaskPending());

  // Since time doesn't progress on the MockPlatform, this is enough idle time
  // to finish compiling the function.
  platform.RunIdleTask(1000.0, 0.0);

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(shared->is_compiled());
}

TEST_F(CompilerDispatcherTest, IdleTaskSmallIdleTime) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script[] =
      "function g() { var y = 1; function f4(x) { return x * y }; return f4; } "
      "g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), script));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(dispatcher.Enqueue(shared));
  ASSERT_TRUE(platform.IdleTaskPending());

  // The job should be scheduled for the main thread.
  ASSERT_EQ(dispatcher.jobs_.size(), 1u);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kInitial);

  // Only grant a little idle time and have time advance beyond it in one step.
  platform.RunIdleTask(2.0, 1.0);

  ASSERT_TRUE(dispatcher.IsEnqueued(shared));
  ASSERT_FALSE(shared->is_compiled());
  ASSERT_TRUE(platform.IdleTaskPending());

  // The job should be still scheduled for the main thread, but ready for
  // parsing.
  ASSERT_EQ(dispatcher.jobs_.size(), 1u);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kReadyToParse);

  // Now grant a lot of idle time and freeze time.
  platform.RunIdleTask(1000.0, 0.0);

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(shared->is_compiled());
  ASSERT_FALSE(platform.IdleTaskPending());
}

TEST_F(CompilerDispatcherTest, IdleTaskException) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, 50);

  std::string script("function g() { function f5(x) { var a = ");
  for (int i = 0; i < 1000; i++) {
    script += "'x' + ";
  }
  script += " 'x'; }; return f5; } g();";
  Handle<JSFunction> f =
      Handle<JSFunction>::cast(RunJS(isolate(), script.c_str()));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(dispatcher.Enqueue(shared));
  ASSERT_TRUE(platform.IdleTaskPending());

  // Since time doesn't progress on the MockPlatform, this is enough idle time
  // to finish compiling the function.
  platform.RunIdleTask(1000.0, 0.0);

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_FALSE(shared->is_compiled());
  ASSERT_FALSE(i_isolate()->has_pending_exception());
}

TEST_F(CompilerDispatcherTest, CompileOnBackgroundThread) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script[] =
      "function g() { var y = 1; function f6(x) { return x * y }; return f6; } "
      "g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), script));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(dispatcher.Enqueue(shared));
  ASSERT_TRUE(platform.IdleTaskPending());

  ASSERT_EQ(dispatcher.jobs_.size(), 1u);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kInitial);

  // Make compiling super expensive, and advance job as much as possible on the
  // foreground thread.
  dispatcher.tracer_->RecordCompile(50000.0, 1);
  platform.RunIdleTask(10.0, 0.0);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kReadyToCompile);

  ASSERT_TRUE(dispatcher.IsEnqueued(shared));
  ASSERT_FALSE(shared->is_compiled());
  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(platform.BackgroundTasksPending());

  platform.RunBackgroundTasksAndBlock(V8::GetCurrentPlatform());

  ASSERT_TRUE(platform.IdleTaskPending());
  ASSERT_FALSE(platform.BackgroundTasksPending());
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kCompiled);

  // Now grant a lot of idle time and freeze time.
  platform.RunIdleTask(1000.0, 0.0);

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(shared->is_compiled());
  ASSERT_FALSE(platform.IdleTaskPending());
}

TEST_F(CompilerDispatcherTest, FinishNowWithBackgroundTask) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script[] =
      "function g() { var y = 1; function f7(x) { return x * y }; return f7; } "
      "g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), script));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(dispatcher.Enqueue(shared));
  ASSERT_TRUE(platform.IdleTaskPending());

  ASSERT_EQ(dispatcher.jobs_.size(), 1u);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kInitial);

  // Make compiling super expensive, and advance job as much as possible on the
  // foreground thread.
  dispatcher.tracer_->RecordCompile(50000.0, 1);
  platform.RunIdleTask(10.0, 0.0);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kReadyToCompile);

  ASSERT_TRUE(dispatcher.IsEnqueued(shared));
  ASSERT_FALSE(shared->is_compiled());
  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(platform.BackgroundTasksPending());

  // This does not block, but races with the FinishNow() call below.
  platform.RunBackgroundTasks(V8::GetCurrentPlatform());

  ASSERT_TRUE(dispatcher.FinishNow(shared));
  // Finishing removes the SFI from the queue.
  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(shared->is_compiled());
  if (platform.IdleTaskPending()) platform.ClearIdleTask();
  ASSERT_FALSE(platform.BackgroundTasksPending());
}

TEST_F(CompilerDispatcherTest, IdleTaskMultipleJobs) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script1[] =
      "function g() { var y = 1; function f8(x) { return x * y }; return f8; } "
      "g();";
  Handle<JSFunction> f1 = Handle<JSFunction>::cast(RunJS(isolate(), script1));
  Handle<SharedFunctionInfo> shared1(f1->shared(), i_isolate());

  const char script2[] =
      "function g() { var y = 1; function f9(x) { return x * y }; return f9; } "
      "g();";
  Handle<JSFunction> f2 = Handle<JSFunction>::cast(RunJS(isolate(), script2));
  Handle<SharedFunctionInfo> shared2(f2->shared(), i_isolate());

  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(dispatcher.Enqueue(shared1));
  ASSERT_TRUE(dispatcher.Enqueue(shared2));
  ASSERT_TRUE(platform.IdleTaskPending());

  // Since time doesn't progress on the MockPlatform, this is enough idle time
  // to finish compiling the function.
  platform.RunIdleTask(1000.0, 0.0);

  ASSERT_FALSE(dispatcher.IsEnqueued(shared1));
  ASSERT_FALSE(dispatcher.IsEnqueued(shared2));
  ASSERT_TRUE(shared1->is_compiled());
  ASSERT_TRUE(shared2->is_compiled());
}

TEST_F(CompilerDispatcherTest, FinishNowException) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, 50);

  std::string script("function g() { function f10(x) { var a = ");
  for (int i = 0; i < 1000; i++) {
    script += "'x' + ";
  }
  script += " 'x'; }; return f10; } g();";
  Handle<JSFunction> f =
      Handle<JSFunction>::cast(RunJS(isolate(), script.c_str()));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(dispatcher.Enqueue(shared));
  ASSERT_TRUE(platform.IdleTaskPending());

  ASSERT_FALSE(dispatcher.FinishNow(shared));

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_FALSE(shared->is_compiled());
  ASSERT_TRUE(i_isolate()->has_pending_exception());

  i_isolate()->clear_pending_exception();
  platform.ClearIdleTask();
}

TEST_F(CompilerDispatcherTest, AsyncAbortAllPendingBackgroundTask) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script[] =
      "function g() { var y = 1; function f11(x) { return x * y }; return f11; "
      "} g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), script));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(dispatcher.Enqueue(shared));
  ASSERT_TRUE(platform.IdleTaskPending());

  ASSERT_EQ(dispatcher.jobs_.size(), 1u);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kInitial);

  // Make compiling super expensive, and advance job as much as possible on the
  // foreground thread.
  dispatcher.tracer_->RecordCompile(50000.0, 1);
  platform.RunIdleTask(10.0, 0.0);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kReadyToCompile);

  ASSERT_TRUE(dispatcher.IsEnqueued(shared));
  ASSERT_FALSE(shared->is_compiled());
  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(platform.BackgroundTasksPending());

  // The background task hasn't yet started, so we can just cancel it.
  dispatcher.AbortAll(CompilerDispatcher::BlockingBehavior::kDontBlock);
  ASSERT_FALSE(platform.ForegroundTasksPending());

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_FALSE(shared->is_compiled());

  platform.RunBackgroundTasksAndBlock(V8::GetCurrentPlatform());

  if (platform.IdleTaskPending()) platform.ClearIdleTask();
  ASSERT_FALSE(platform.BackgroundTasksPending());
  ASSERT_FALSE(platform.ForegroundTasksPending());
}

TEST_F(CompilerDispatcherTest, AsyncAbortAllRunningBackgroundTask) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script1[] =
      "function g() { var y = 1; function f11(x) { return x * y }; return f11; "
      "} g();";
  Handle<JSFunction> f1 = Handle<JSFunction>::cast(RunJS(isolate(), script1));
  Handle<SharedFunctionInfo> shared1(f1->shared(), i_isolate());

  const char script2[] =
      "function g() { var y = 1; function f12(x) { return x * y }; return f12; "
      "} g();";
  Handle<JSFunction> f2 = Handle<JSFunction>::cast(RunJS(isolate(), script2));
  Handle<SharedFunctionInfo> shared2(f2->shared(), i_isolate());

  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(dispatcher.Enqueue(shared1));
  ASSERT_TRUE(platform.IdleTaskPending());

  ASSERT_EQ(dispatcher.jobs_.size(), 1u);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kInitial);

  // Make compiling super expensive, and advance job as much as possible on the
  // foreground thread.
  dispatcher.tracer_->RecordCompile(50000.0, 1);
  platform.RunIdleTask(10.0, 0.0);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kReadyToCompile);

  ASSERT_TRUE(dispatcher.IsEnqueued(shared1));
  ASSERT_FALSE(shared1->is_compiled());
  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(platform.BackgroundTasksPending());

  // Kick off background tasks and freeze them.
  dispatcher.block_for_testing_.SetValue(true);
  platform.RunBackgroundTasks(V8::GetCurrentPlatform());

  // Busy loop until the background task started running.
  while (dispatcher.block_for_testing_.Value()) {
  }
  dispatcher.AbortAll(CompilerDispatcher::BlockingBehavior::kDontBlock);
  ASSERT_TRUE(platform.ForegroundTasksPending());

  // We can't schedule new tasks while we're aborting.
  ASSERT_FALSE(dispatcher.Enqueue(shared2));

  // Run the first AbortTask. Since the background job is still pending, it
  // can't do anything.
  platform.RunForegroundTasks();
  {
    base::LockGuard<base::Mutex> lock(&dispatcher.mutex_);
    ASSERT_TRUE(dispatcher.abort_);
  }

  // Release background task.
  dispatcher.semaphore_for_testing_.Signal();

  // Busy loop until the background task scheduled another AbortTask task.
  while (!platform.ForegroundTasksPending()) {
  }

  platform.RunForegroundTasks();
  ASSERT_TRUE(dispatcher.jobs_.empty());
  {
    base::LockGuard<base::Mutex> lock(&dispatcher.mutex_);
    ASSERT_FALSE(dispatcher.abort_);
  }

  ASSERT_TRUE(platform.IdleTaskPending());
  platform.RunIdleTask(5.0, 1.0);
  ASSERT_FALSE(platform.BackgroundTasksPending());
  ASSERT_FALSE(platform.ForegroundTasksPending());

  // Now it's possible to enqueue new functions again.
  ASSERT_TRUE(dispatcher.Enqueue(shared2));
  ASSERT_TRUE(platform.IdleTaskPending());
  ASSERT_FALSE(platform.BackgroundTasksPending());
  ASSERT_FALSE(platform.ForegroundTasksPending());
  platform.ClearIdleTask();
}

TEST_F(CompilerDispatcherTest, FinishNowDuringAbortAll) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script[] =
      "function g() { var y = 1; function f13(x) { return x * y }; return f13; "
      "} g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), script));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(dispatcher.Enqueue(shared));
  ASSERT_TRUE(platform.IdleTaskPending());

  ASSERT_EQ(dispatcher.jobs_.size(), 1u);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kInitial);

  // Make compiling super expensive, and advance job as much as possible on the
  // foreground thread.
  dispatcher.tracer_->RecordCompile(50000.0, 1);
  platform.RunIdleTask(10.0, 0.0);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kReadyToCompile);

  ASSERT_TRUE(dispatcher.IsEnqueued(shared));
  ASSERT_FALSE(shared->is_compiled());
  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(platform.BackgroundTasksPending());

  // Kick off background tasks and freeze them.
  dispatcher.block_for_testing_.SetValue(true);
  platform.RunBackgroundTasks(V8::GetCurrentPlatform());

  // Busy loop until the background task started running.
  while (dispatcher.block_for_testing_.Value()) {
  }
  dispatcher.AbortAll(CompilerDispatcher::BlockingBehavior::kDontBlock);
  ASSERT_TRUE(platform.ForegroundTasksPending());

  // Run the first AbortTask. Since the background job is still pending, it
  // can't do anything.
  platform.RunForegroundTasks();
  {
    base::LockGuard<base::Mutex> lock(&dispatcher.mutex_);
    ASSERT_TRUE(dispatcher.abort_);
  }

  // While the background thread holds on to a job, it is still enqueud.
  ASSERT_TRUE(dispatcher.IsEnqueued(shared));

  // Release background task.
  dispatcher.semaphore_for_testing_.Signal();

  // Force the compilation to finish, even while aborting.
  ASSERT_TRUE(dispatcher.FinishNow(shared));
  ASSERT_TRUE(dispatcher.jobs_.empty());

  // Busy wait for the background task to finish.
  for (;;) {
    base::LockGuard<base::Mutex> lock(&dispatcher.mutex_);
    if (dispatcher.num_background_tasks_ == 0) {
      break;
    }
  }

  ASSERT_TRUE(platform.ForegroundTasksPending());
  ASSERT_TRUE(platform.IdleTaskPending());
  ASSERT_FALSE(platform.BackgroundTasksPending());

  platform.RunForegroundTasks();
  {
    base::LockGuard<base::Mutex> lock(&dispatcher.mutex_);
    ASSERT_FALSE(dispatcher.abort_);
  }

  platform.ClearForegroundTasks();
  platform.ClearIdleTask();
}

TEST_F(CompilerDispatcherTest, MemoryPressure) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script[] =
      "function g() { var y = 1; function f14(x) { return x * y }; return f14; "
      "} g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), script));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  // Can't enqueue tasks under memory pressure.
  dispatcher.MemoryPressureNotification(v8::MemoryPressureLevel::kCritical,
                                        true);
  ASSERT_FALSE(dispatcher.Enqueue(shared));

  dispatcher.MemoryPressureNotification(v8::MemoryPressureLevel::kNone, true);
  ASSERT_TRUE(dispatcher.Enqueue(shared));

  // Memory pressure cancels current jobs.
  dispatcher.MemoryPressureNotification(v8::MemoryPressureLevel::kCritical,
                                        true);
  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  platform.ClearIdleTask();
}

namespace {

class PressureNotificationTask : public CancelableTask {
 public:
  PressureNotificationTask(Isolate* isolate, CompilerDispatcher* dispatcher,
                           base::Semaphore* sem)
      : CancelableTask(isolate), dispatcher_(dispatcher), sem_(sem) {}
  ~PressureNotificationTask() override {}

  void RunInternal() override {
    dispatcher_->MemoryPressureNotification(v8::MemoryPressureLevel::kCritical,
                                            false);
    sem_->Signal();
  }

 private:
  CompilerDispatcher* dispatcher_;
  base::Semaphore* sem_;

  DISALLOW_COPY_AND_ASSIGN(PressureNotificationTask);
};

}  // namespace

TEST_F(CompilerDispatcherTest, MemoryPressureFromBackground) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script[] =
      "function g() { var y = 1; function f15(x) { return x * y }; return f15; "
      "} g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), script));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_TRUE(dispatcher.Enqueue(shared));
  base::Semaphore sem(0);
  V8::GetCurrentPlatform()->CallOnBackgroundThread(
      new PressureNotificationTask(i_isolate(), &dispatcher, &sem),
      v8::Platform::kShortRunningTask);

  sem.Wait();

  // A memory pressure task is pending, and running it will cancel the job.
  ASSERT_TRUE(platform.ForegroundTasksPending());
  ASSERT_TRUE(dispatcher.IsEnqueued(shared));
  platform.RunForegroundTasks();
  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_FALSE(shared->is_compiled());

  // Since the AbortAll() call is made from a task, AbortAll thinks that there
  // is at least one task running, and fires of an AbortTask to be safe.
  ASSERT_TRUE(platform.ForegroundTasksPending());
  platform.RunForegroundTasks();
  ASSERT_FALSE(platform.ForegroundTasksPending());

  platform.ClearIdleTask();
}

TEST_F(CompilerDispatcherTest, EnqueueAndStep) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script[] =
      "function g() { var y = 1; function f16(x) { return x * y }; return f16; "
      "} g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), script));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(dispatcher.EnqueueAndStep(shared));
  ASSERT_TRUE(dispatcher.IsEnqueued(shared));

  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kReadyToParse);

  ASSERT_TRUE(platform.IdleTaskPending());
  platform.ClearIdleTask();
  ASSERT_TRUE(platform.BackgroundTasksPending());
  platform.ClearBackgroundTasks();
}

TEST_F(CompilerDispatcherTest, EnqueueParsed) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char source[] =
      "function g() { var y = 1; function f17(x) { return x * y }; return f17; "
      "} g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), source));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());
  Handle<Script> script(Script::cast(shared->script()), i_isolate());

  ParseInfo parse_info(shared);
  ASSERT_TRUE(Compiler::ParseAndAnalyze(&parse_info, i_isolate()));
  std::shared_ptr<DeferredHandles> handles;

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(dispatcher.Enqueue(script, shared, parse_info.literal(),
                                 parse_info.zone_shared(), handles, handles));
  ASSERT_TRUE(dispatcher.IsEnqueued(shared));

  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kAnalyzed);

  ASSERT_TRUE(platform.IdleTaskPending());
  platform.ClearIdleTask();
  ASSERT_FALSE(platform.BackgroundTasksPending());
}

TEST_F(CompilerDispatcherTest, EnqueueAndStepParsed) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char source[] =
      "function g() { var y = 1; function f18(x) { return x * y }; return f18; "
      "} g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), source));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());
  Handle<Script> script(Script::cast(shared->script()), i_isolate());

  ParseInfo parse_info(shared);
  ASSERT_TRUE(Compiler::ParseAndAnalyze(&parse_info, i_isolate()));
  std::shared_ptr<DeferredHandles> handles;

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(dispatcher.EnqueueAndStep(script, shared, parse_info.literal(),
                                        parse_info.zone_shared(), handles,
                                        handles));
  ASSERT_TRUE(dispatcher.IsEnqueued(shared));

  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kReadyToCompile);

  ASSERT_TRUE(platform.IdleTaskPending());
  ASSERT_TRUE(platform.BackgroundTasksPending());
  platform.ClearIdleTask();
  platform.ClearBackgroundTasks();
}

TEST_F(CompilerDispatcherTest, CompileParsedOutOfScope) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char source[] =
      "function g() { var y = 1; function f20(x) { return x + y }; return f20; "
      "} g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), source));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());
  Handle<Script> script(Script::cast(shared->script()), i_isolate());

  {
    HandleScope scope(i_isolate());  // Create handles scope for parsing.

    ASSERT_FALSE(shared->is_compiled());
    ParseInfo parse_info(shared);

    ASSERT_TRUE(parsing::ParseAny(&parse_info, i_isolate()));
    DeferredHandleScope handles_scope(i_isolate());
    { ASSERT_TRUE(Compiler::Analyze(&parse_info, i_isolate())); }
    std::shared_ptr<DeferredHandles> compilation_handles(
        handles_scope.Detach());

    ASSERT_FALSE(platform.IdleTaskPending());
    ASSERT_TRUE(dispatcher.Enqueue(
        script, shared, parse_info.literal(), parse_info.zone_shared(),
        parse_info.deferred_handles(), compilation_handles));
    ASSERT_TRUE(platform.IdleTaskPending());
  }
  // Exit the handles scope and destroy ParseInfo before running the idle task.

  // Since time doesn't progress on the MockPlatform, this is enough idle time
  // to finish compiling the function.
  platform.RunIdleTask(1000.0, 0.0);

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(shared->is_compiled());
}

namespace {

const char kExtensionSource[] = "native function Dummy();";

class MockNativeFunctionExtension : public Extension {
 public:
  MockNativeFunctionExtension()
      : Extension("mock-extension", kExtensionSource), function_(&Dummy) {}

  virtual v8::Local<v8::FunctionTemplate> GetNativeFunctionTemplate(
      v8::Isolate* isolate, v8::Local<v8::String> name) {
    return v8::FunctionTemplate::New(isolate, function_);
  }

  static void Dummy(const v8::FunctionCallbackInfo<v8::Value>& args) { return; }

 private:
  v8::FunctionCallback function_;

  DISALLOW_COPY_AND_ASSIGN(MockNativeFunctionExtension);
};

}  // namespace

TEST_F(CompilerDispatcherTestWithoutContext, CompileExtensionWithoutContext) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);
  Local<v8::Context> context = v8::Context::New(isolate());

  MockNativeFunctionExtension extension;
  Handle<String> script_str =
      i_isolate()
          ->factory()
          ->NewStringFromUtf8(CStrVector(kExtensionSource))
          .ToHandleChecked();
  Handle<Script> script = i_isolate()->factory()->NewScript(script_str);
  script->set_type(Script::TYPE_EXTENSION);

  Handle<SharedFunctionInfo> shared;
  {
    v8::Context::Scope scope(context);

    ParseInfo parse_info(script);
    parse_info.set_extension(&extension);

    ASSERT_TRUE(parsing::ParseAny(&parse_info, i_isolate()));
    Handle<FixedArray> shared_infos_array(i_isolate()->factory()->NewFixedArray(
        parse_info.max_function_literal_id() + 1));
    parse_info.script()->set_shared_function_infos(*shared_infos_array);
    DeferredHandleScope handles_scope(i_isolate());
    { ASSERT_TRUE(Compiler::Analyze(&parse_info, i_isolate())); }
    std::shared_ptr<DeferredHandles> compilation_handles(
        handles_scope.Detach());

    shared = i_isolate()->factory()->NewSharedFunctionInfoForLiteral(
        parse_info.literal(), script);
    parse_info.set_shared_info(shared);

    ASSERT_FALSE(platform.IdleTaskPending());
    ASSERT_TRUE(dispatcher.Enqueue(
        script, shared, parse_info.literal(), parse_info.zone_shared(),
        parse_info.deferred_handles(), compilation_handles));
    ASSERT_TRUE(platform.IdleTaskPending());
  }
  // Exit the context scope before running the idle task.

  // Since time doesn't progress on the MockPlatform, this is enough idle time
  // to finish compiling the function.
  platform.RunIdleTask(1000.0, 0.0);

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(shared->is_compiled());
}

TEST_F(CompilerDispatcherTest, CompileLazyFinishesDispatcherJob) {
  // Use the real dispatcher so that CompileLazy checks the same one for
  // enqueued functions.
  CompilerDispatcher* dispatcher = i_isolate()->compiler_dispatcher();

  const char source[] =
      "function g() { var y = 1; function f16(x) { return x * y }; return f16; "
      "} g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), source));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());

  ASSERT_FALSE(shared->is_compiled());
  ASSERT_FALSE(dispatcher->IsEnqueued(shared));
  ASSERT_TRUE(dispatcher->Enqueue(shared));
  ASSERT_TRUE(dispatcher->IsEnqueued(shared));

  // Now force the function to run and ensure CompileLazy finished and dequeues
  // it from the dispatcher.
  RunJS(isolate(), "g()();");
  ASSERT_TRUE(shared->is_compiled());
  ASSERT_FALSE(dispatcher->IsEnqueued(shared));
}

TEST_F(CompilerDispatcherTest, CompileLazy2FinishesDispatcherJob) {
  // Use the real dispatcher so that CompileLazy checks the same one for
  // enqueued functions.
  CompilerDispatcher* dispatcher = i_isolate()->compiler_dispatcher();

  const char source2[] = "function lazy2() { return 42; }; lazy2;";
  Handle<JSFunction> lazy2 =
      Handle<JSFunction>::cast(RunJS(isolate(), source2));
  Handle<SharedFunctionInfo> shared2(lazy2->shared(), i_isolate());
  ASSERT_FALSE(shared2->is_compiled());

  const char source1[] = "function lazy1() { return lazy2(); }; lazy1;";
  Handle<JSFunction> lazy1 =
      Handle<JSFunction>::cast(RunJS(isolate(), source1));
  Handle<SharedFunctionInfo> shared1(lazy1->shared(), i_isolate());
  ASSERT_FALSE(shared1->is_compiled());

  ASSERT_TRUE(dispatcher->Enqueue(shared1));
  ASSERT_TRUE(dispatcher->Enqueue(shared2));

  RunJS(isolate(), "lazy1();");
  ASSERT_TRUE(shared1->is_compiled());
  ASSERT_TRUE(shared2->is_compiled());
  ASSERT_FALSE(dispatcher->IsEnqueued(shared1));
  ASSERT_FALSE(dispatcher->IsEnqueued(shared2));
}

TEST_F(CompilerDispatcherTest, EnqueueAndStepTwice) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char source[] =
      "function g() { var y = 1; function f18(x) { return x * y }; return f18; "
      "} g();";
  Handle<JSFunction> f = Handle<JSFunction>::cast(RunJS(isolate(), source));
  Handle<SharedFunctionInfo> shared(f->shared(), i_isolate());
  Handle<Script> script(Script::cast(shared->script()), i_isolate());

  ParseInfo parse_info(shared);
  ASSERT_TRUE(Compiler::ParseAndAnalyze(&parse_info, i_isolate()));
  std::shared_ptr<DeferredHandles> handles;

  ASSERT_FALSE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(dispatcher.EnqueueAndStep(script, shared, parse_info.literal(),
                                        parse_info.zone_shared(), handles,
                                        handles));
  ASSERT_TRUE(dispatcher.IsEnqueued(shared));

  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kReadyToCompile);

  // EnqueueAndStep of the same function again (either already parsed or for
  // compile and parse) shouldn't step the job.
  ASSERT_TRUE(dispatcher.EnqueueAndStep(script, shared, parse_info.literal(),
                                        parse_info.zone_shared(), handles,
                                        handles));
  ASSERT_TRUE(dispatcher.IsEnqueued(shared));
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kReadyToCompile);
  ASSERT_TRUE(dispatcher.EnqueueAndStep(shared));
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kReadyToCompile);

  ASSERT_TRUE(platform.IdleTaskPending());
  ASSERT_TRUE(platform.BackgroundTasksPending());
  platform.ClearIdleTask();
  platform.ClearBackgroundTasks();
}

TEST_F(CompilerDispatcherTest, CompileMultipleOnBackgroundThread) {
  MockPlatform platform;
  CompilerDispatcher dispatcher(i_isolate(), &platform, FLAG_stack_size);

  const char script1[] =
      "function g() { var y = 1; function f19(x) { return x * y }; "
      "return f19; } g();";
  Handle<JSFunction> f1 = Handle<JSFunction>::cast(RunJS(isolate(), script1));
  Handle<SharedFunctionInfo> shared1(f1->shared(), i_isolate());
  const char script2[] =
      "function g() { var y = 1; function f20(x) { return x * y }; "
      "return f20; } g();";
  Handle<JSFunction> f2 = Handle<JSFunction>::cast(RunJS(isolate(), script2));
  Handle<SharedFunctionInfo> shared2(f2->shared(), i_isolate());

  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(dispatcher.Enqueue(shared1));
  ASSERT_TRUE(dispatcher.Enqueue(shared2));
  ASSERT_TRUE(platform.IdleTaskPending());

  ASSERT_EQ(dispatcher.jobs_.size(), 2u);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kInitial);
  ASSERT_TRUE((++dispatcher.jobs_.begin())->second->status() ==
              CompileJobStatus::kInitial);

  // Make compiling super expensive, and advance job as much as possible on the
  // foreground thread.
  dispatcher.tracer_->RecordCompile(50000.0, 1);
  platform.RunIdleTask(10.0, 0.0);
  ASSERT_EQ(dispatcher.jobs_.size(), 2u);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kReadyToCompile);
  ASSERT_TRUE((++dispatcher.jobs_.begin())->second->status() ==
              CompileJobStatus::kReadyToCompile);

  ASSERT_TRUE(dispatcher.IsEnqueued(shared1));
  ASSERT_TRUE(dispatcher.IsEnqueued(shared2));
  ASSERT_FALSE(shared1->is_compiled());
  ASSERT_FALSE(shared2->is_compiled());
  ASSERT_FALSE(platform.IdleTaskPending());
  ASSERT_TRUE(platform.BackgroundTasksPending());

  platform.RunBackgroundTasksAndBlock(V8::GetCurrentPlatform());

  ASSERT_TRUE(platform.IdleTaskPending());
  ASSERT_FALSE(platform.BackgroundTasksPending());
  ASSERT_EQ(dispatcher.jobs_.size(), 2u);
  ASSERT_TRUE(dispatcher.jobs_.begin()->second->status() ==
              CompileJobStatus::kCompiled);
  ASSERT_TRUE((++dispatcher.jobs_.begin())->second->status() ==
              CompileJobStatus::kCompiled);

  // Now grant a lot of idle time and freeze time.
  platform.RunIdleTask(1000.0, 0.0);

  ASSERT_FALSE(dispatcher.IsEnqueued(shared1));
  ASSERT_FALSE(dispatcher.IsEnqueued(shared2));
  ASSERT_TRUE(shared1->is_compiled());
  ASSERT_TRUE(shared2->is_compiled());
  ASSERT_FALSE(platform.IdleTaskPending());
}

}  // namespace internal
}  // namespace v8
