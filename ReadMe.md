# **Co**operative **Pa**rallel **T**asking *(or)* **CoPaT**

This is a C++ 20 cooperative multi-tasking library. I used coroutines to achieve cooperative multi-task.
I developed this for and also use this as a task system in [Cranberry game engine].
I decided to make this library available publically under an MIT license. I will try and merge any improvements or changes I implement when using this in my game engine.

The best way to use this library is to copy the files in the *Source* directory and paste them into your code base. Then all you have to do is

```cpp
void mainTick(void* userData);

int main()
{
    copat::JobSystem js(copat::EThreadingConstraint::NoConstraints, COPAT_TCHAR("JsName"));
    /**
     * Initializes jobsystem with worker threads, sets up the TLS and setup singleton instance for future access
     * mainTick is call back that gets executed every loop of main thread(I do not create new thread for main thread)
     */
    js.initialize(&mainTick, nullptr);
    /**
     * Just like thread join. 
     * This join blocks and starts main thread loop. 
     * Main thread loop executes the mainTick and pumps the posted main thread jobs.
     * 
     * If your application just needs workers and will not queue any jobs to main queue, You can safely ignore joinMain().
     * Note that if you ignore joinMain() mainTick never gets called and main thread's queue will never get executed.
     */
    js.joinMain();
    /**
     * Must be called when you want to safely exit all threads. shutdown blocks
     */
    js.shutdown();
}
```

The above code is all you need to run the job system. You can exit from `joinMain()` by calling `JobSystem::exitMain()`.

## Future Goals

Right now the library is just a bunch of headers and TU files

- Convert to CMake library project
- Add unit test and cover all feature's basic use cases
- Integrate github's CI pipeline
- Other platforms support in the library
- Check if reusing nodes is worth it in FAAArrayQueues
- Of course bug fixes :wink:

## External libraries used

- [ConcurrencyFreaks] - Using modified version of [FAAArrayQueue.hpp](https://github.com/pramalhe/ConcurrencyFreaks/blob/master/CPP/queues/array/FAAArrayQueue.hpp) for MPMC(Multi Producer Multi Consumer) and MPSC(MP Single Consumer) queues used for job system. The algorithms are modified to suit my needs and embedded into this library's code. [License](https://github.com/pramalhe/ConcurrencyFreaks/blob/master/LICENSE)

## Compiler requirement

Any C++ 20 standard-compliant compiler must be able to compile and run this library successfully.

## Platform support

Due to some platform-specific codes, the Supported platform is limited to **Windows**. However, I have exposed the `PlatformThreadingFuncs` type overrideable so that you can hook your application's platform-specific code to be used in this library. You have to add `OVERRIDE_PLATFORMTHREADINGFUNCTIONS` define to your platform functions class/wrapper in `CoPatConfig.h` and define a few necessary functions(same as in `GenericThreadingFunctions`)

```cpp
/**
 * Override PlatformThreadingFunctions.
 */
#define OVERRIDE_PLATFORMTHREADINGFUNCTIONS YourPlatformFunctionsType
```

## Platform architecture support

I have run a few test cases in the `x64` arch. However, It should be fine in `x86` as well. I am not sure about the `arm`
(I do not have much knowledge of that architecture)

## Usage

```cpp
template <typename RetType, typename BasePromiseType, bool EnqAtInitialSuspend, EJobThreadType EnqueueInThread, EJobPriority Priority>
class JobSystemTaskType;
```

The above *Awaitable* type is the main coroutine return type for this job system.

- `RetType` - What type will `co_await` on this object will return to awaiter
- `BasePromiseType` - can take `JobSystemPromiseBase` or `JobSystemPromiseBaseMC`. Once this coroutine job reaches `final_suspend` all the awaiters awaiting this job will be resumed.
  - `JobSystemPromiseBase` for jobs that can be awaited in only one other coroutine
  - `JobSystemPromiseBaseMC` for jobs which can be awaited by more than one coroutines
- `EnqAtInitialSuspend` - As the name suggests. Jobs with this `true` will be enqueued to the job system automatically at `initial_suspend`, The thread to which it will be enqueued depends on the next enum `EnqueueInThread`. while `false` means this job will start executing in the current thread synchronously until it is manually switched in the middle.
- `EnqueueInThread` - This value must be a valid enum of thread type to which the job must queue.
- `Priority` - Specifies the priority of the job. Possible values are `Critical`, `Normal`, and `Low`

### Usage Example

```cpp
// Do not queues the task to job system automatically
copat::JobSystemTaskType<..., /*EnqAtInitialSuspend*/ false, /*EnqueueInThread*/ EJobThreadType::WorkerThreads, /*Priority*/ Priority_Normal> noEnqJob();
// Enqueues to worker thread automatically
copat::JobSystemTaskType<..., /*EnqAtInitialSuspend*/ true, /*EnqueueInThread*/ EJobThreadType::WorkerThreads, /*Priority*/ Priority_Critical> workerJob();
// Enqueues to main thread automatically
copat::JobSystemTaskType<..., /*EnqAtInitialSuspend*/ true, /*EnqueueInThread*/ EJobThreadType::MainThread, /*Priority*/ Priority_Low> mainThreadJob();
```

> *Note that calling this coroutine will always enqueue to the back of the queue even if the current thread is the same as the coroutine's enqueue thread! This could be used to defer some jobs in the same thread.*

## Switching job thread

You can switch between threads in the middle of job manually using `template <EJobThreadType SwitchToThread> struct SwitchJobThreadAwaiter`

> *Note that using `SwitchJobThreadAwaiter` will enqueue to the back of the queue even if the current thread is the same as switching to the thread! This could be used to defer some jobs in the same thread.*

```cpp
// copat::JobSystemEnqTask<EJobThreadType::WorkerThreads, Priority_Normal> is just specialization of copat::JobSystemTaskType with boolean to decide which thread to enqueue this task
copat::JobSystemEnqTask<EJobThreadType::WorkerThreads, Priority_Normal> testManualSwitch(u32 counter)
{
    std::cout << copat::PlatformThreadingFuncs::getCurrentThreadName() << " is the executor, Counter : " << counter << std::endl;
    // Switching to main thread
    co_await copat::SwitchJobThreadAwaiter<EJobThreadType::MainThread>{};
    std::cout << copat::PlatformThreadingFuncs::getCurrentThreadName() << " is it main thread, Counter : " << counter << std::endl;
}
```

If you want to delay the execution of a job within the same thread but you are not aware of the currently running thread, you can use YieldAwaiter to defer this job and allow other jobs to run.

## Thread wait on a job

You can lock wait on a single job/awaitable using `copat::waitOnAwaitable(awaitable)`. Be aware that if you wait on a job that is already queued in the same thread it will lead to dead-lock.
The thread that calls this will wait until the job is finished.

```cpp
copat::JobSystemReturnableTask<u32&, true, EJobThreadType::WorkerThreads, Priority_Normal> testCoroWait();
copat::JobSystemEnqTask<EJobThreadType::WorkerThreads, Priority_Normal> testCoroWaitNoRet();

auto retJob = testCoroWait();
auto noretJob = testCoroWaitNoRet();
// Waits until testCoroWait is done
u32& retVal = copat::waitOnAwaitable(retVal);
// Then waits until testCoroWaitNoRet is done
copat::waitOnAwaitable(noretJob);
```

## Job wait until all awaitables

This is a non-locking alternative for `copat::waitOnAwaitable(awaitable)`. So what this basically does is just suspend the waiting job and waits until awaiting job is done. This can be done in two ways

- Job to Job await - Awaitable returned from a job can be `co_await`ed on another job. This suspends the second job until the first job is completed.
Once the first job is finished the second job resumes in the same thread as awaited job
- Multiple jobs to job await - Awaitable returned from several jobs can be piped through `copat::awaitAllTasks(...)` to create new awaitable which then can be awaited in another job to wait until all the jobs that it awaits on are finished.

### Job wait Example

```cpp
copat::JobSystemReturnableTask<u32&, true, EJobThreadType::WorkerThreads, Priority_Normal> testCoroWait();
copat::JobSystemEnqTask<EJobThreadType::WorkerThreads, Priority_Normal> testCoroWaitNoRet();

/**
 * This job awaits until all the sub tasks this starts and then switches to main thread and executes few lines of code
 */
copat::NormalFuncAwaiter testawaitAll()
{
    auto ret = testCoroWait();
    auto noret = testCoroWaitNoRet();
    co_await copat::awaitAllTasks(ret, noret);

    std::cout << copat::PlatformThreadingFuncs::getCurrentThreadName() << " is the executor after awaitAllTasks" << std::endl;
    co_await copat::SwitchJobThreadAwaiter<EJobThreadType::MainThread>{};
    std::cout << copat::PlatformThreadingFuncs::getCurrentThreadName() << " is it main thread after awaitAllTasks" << std::endl;
}


copat::JobSystemReturnableTask<u32, false, EJobThreadType::WorkerThreads, Priority_Normal> testRetCoro();
copat::JobSystemReturnableTask<u32&, false, EJobThreadType::WorkerThreads, Priority_Normal> testRetCoroRef();

/**
 * This job awaits sub tasks at different stages and finally returns
 */
copat::NormalFuncAwaiter testRetCoroCall()
{
    u32 val = 0;
    while (val < 4)
    {
        u32& retRef = co_await testRetCoroRef();
        std::cout << "Ref Returned val " << retRef << std::endl;
        val = retRef;
    }

    val = 0;
    while (val < 4)
    {
        val = co_await testRetCoro();
        std::cout << "Value Returned val " << val << std::endl;
    }
}
```

## Dispatching parallel tasks to workers

The `dispatch()` function can be used to dispatch a job on an `n` number of data.

### Dispatch Example

```cpp
copat::NormalFuncAwaiter testDispatch()
{
    copat::u32 tasksCount = 100;
    co_await copat::dispatch(js, [](copat::u32 jobIdx)
        {
            std::cout << "Dispatched job idx " + std::to_string(jobIdx) + "\n";
        }, tasksCount);
}
```

`diverge()` and `converge()` functions can be used to dispatch a returnable job on an `n` number of data. The `converge` collects all the returned values and returns them.

### Diverge Example

```cpp
    auto loadAssetsAsync = [](u32 idx) -> AssetBase *
    {
        // Do task
    };
    auto allAwaits = copat::diverge(
        copat::JobSystem::get(), 
        loadAssetsAsync,
        foundAssets.size()
    );
    std::vector<AssetBase *> loadedAssetsPerFile = copat::converge(std::move(allAwaits));
```

If you are going to use `dispatch()` or `diverge()` followed by a `waitOnAwaitable()` or `converge()`, Then it is best to use `parallelFor()` or `parallelForReturn()` respectively.

#### Diverge inside diverge

This can be done by returning an awaitable from the outer diverge. The outer diverge could be a `copat::JobSystemReturnableTask<RetType, true, copat::EJobThreadType::WorkerThreads, copat::EJobPriority::Priority_Normal>`.

Example:
```cpp
auto innerDiverge = [&](u32 idx)
{
    ...
    return Something{};
};

auto outerAwaitables = copat::diverge(
    &js,
    [&](u32 idx) -> copat::JobSystemReturnableTask<std::vector<Something>, true, copat::EJobThreadType::WorkerThreads, copat::EJobPriority::Priority_Normal>
        {
           u32 n;
           ...
           co_return co_await copat::awaitConverge(copat::diverge(&js, innerDiverge, n));
        }
    ),
    static_cast<u32>(oN)
);

/* vector need to be moved as JobSystemTasks cannot be copied */
std::vector<std::vector<Something>> allData = copat::waitOnAwaitable(std::move(copat::converge(std::move(shaderAwaitables))));
```

Practical example can be found at 
<script src="https://gist.github.com/jeslaspravin/1b306f6b4281c6c6eb17379a91ba7a86.js"></script>

### Parallel for Example

```cpp
    auto loadAssetsAsync = [](u32 idx) -> AssetBase *
    {
        // Do task
    };
    std::vector<AssetBase *> loadedAssetsPerFile = copat::parallelForReturn(
        copat::JobSystem::get(), 
        loadAssetsAsync,
        foundAssets.size()
    );
```

## User defined threads

Along with default main and worker threads, User can also add their own special threads by defining `FOR_EACH_UDTHREAD_TYPES_UNIQUE_FIRST_LAST(FirstMacroName, MacroName, LastMacroName)` with a list of special thread names enclosed inside `FirstMacroName()`, `MacroName()` and `LastMacroName()` depending on the position of the special thread in the list.

```cpp
/**
 * Thread types that are added by user
 */
// #define FOR_EACH_THREAD_TYPES_UNIQUE_FIRST_LAST(FirstMacroName, MacroName, LastMacroName)    \
//      FirstMacroName(Thread1)                                                                 \
//      MacroName(Thread2)                                                                      \
//      ...                                                                                     \
//      LastMacroName(ThreadN) 
// 
// #define FOR_EACH_THREAD_TYPES_UNIQUE_FIRST_LAST(FirstMacroName, MacroName, LastMacroName) FirstMacroName(RenderThread)
#define FOR_EACH_UDTHREAD_TYPES_UNIQUE_FIRST_LAST(FirstMacroName, MacroName, LastMacroName)     \
    FirstMacroName(RenderThread)                                                                \
    MacroName(PhysicsThread)                                                                    \
    LastMacroName(AudioThread)
```

## Enqueueing Coroutine Task to different Job system

Now you can have any number of job instances and have tasks be enqueued to any of those job systems.
All you have to do is pass the JobSystem reference(`JobSystem &`) to the coroutine and call that coroutine with the job system you want it to be enqueued to.

> Note that `JobSystem::get()` still exists and Coroutine jobs that do not have `JobSystem & or JobSystem *` as the function's first parameter will use it to enqueue the job.
The job system that gets initialized the very first time will be stored in JobSystem singleton. This decision is to allow a main job system(Which will be stored in the singleton) and some sub-job systems that will be used inside different subsystems.

### Enqueue to other job system Example

```cpp
copat::copat::JobSystemWorkerThreadTask testThreadedTask(copat::JobSystem& jobSystem, u32 counter);

testThreadedTask(copat::JobSystem& jobSystem, u32 counter);

copat::JobSystem jsA;
copat::JobSystem jsB;

// This task gets enqueued to jsA's EJobThreadType::WorkerThreads
auto t1 = testThreadedTask(jsA, counter);
// This task gets enqueued to jsB's EJobThreadType::WorkerThreads
auto t2 = testThreadedTask(jsB, counter);
```

## Overriding JobPriority specified at coroutine return type

CoPaT supports priority queues now. It is a simple implementation now, however, it is still useful.

Similar to enqueuing jobs to different job systems, any coroutine's priority can be overridden by passing the priority as the `1st` or `2nd parameter` of the coroutine.
Following coroutine signatures can be used to override priorities at runtime.

```cpp
copat::JobSystemWorkerThreadTask testThreadedTask(copat::JobSystem& jobSystem, EJobPriority jobPriority, u32 counter);
copat::JobSystemWorkerThreadTask testThreadedTask(EJobPriority jobPriority, u32 counter);

copat::JobSystem jsA;
copat::JobSystem jsB;

// This task gets enqueued to jsA's EJobThreadType::WorkerThreads with Normal priority
auto t1 = testThreadedTask(jsA, copat::Priority_Normal, counter);
// This task gets enqueued to jsB's EJobThreadType::WorkerThreads with critical priority
auto t2 = testThreadedTask(jsB, copat::Priority_Critical, counter);
// This task gets enqueued to default JobSystem's EJobThreadType::WorkerThreads with critical priority
auto t3 = testThreadedTask(copat::Priority_Critical, counter);

```

## Controlling threading using ThreadConstraints

JobSystem's threading model can be controlled coarsely using the `EThreadingConstraint` enum and passing it in the constructor of the JobSystem `JobSystem(u32 constraints)`

> `EThreadingConstraint::SingleThreaded` will make the entire job system run in a single thread. Users should take extra precautions to avoid deadlocks when using `copat::waitOnAwaitable(Awaitable)`

`EThreadingConstraint` enum acts as both value and flag. Any enum entry after `EThreadingConstraint::BitMasksStart` will be used to create a flag bit for that entry. Example `EThreadingConstraint::NoWorkerAffinity` entry must be used as a bit flag, The bit for this flag can be obtained using `THREADCONSTRAINT_ENUM_TO_FLAGBIT(NoWorkerAffinity)` macro.

*Let us assume we have two special threads Render and Audio. If you want to combine the render thread into the main thread but still keep the audio thread as a separate thread.
The user should pass the following as constraints to JobSystem's constructor*

```cpp
JobSystem js(EThreadingConstraint::NoConstraint | (EThreadingConstraint::BitMasksStart << (EThreadingConstraint::NoRender - EThreadingConstraint::BitMasksStart)), COPAT_TCHAR("JsName"));
```

or simply

```cpp
JobSystem js(EThreadingConstraint::BitMasksStart << (EThreadingConstraint::NoRender - EThreadingConstraint::BitMasksStart), COPAT_TCHAR("JsName"));
```

or use macro

```cpp
JobSystem js(NOSPECIALTHREAD_ENUM_TO_FLAGBIT(Render), COPAT_TCHAR("JsName"));
```

## References

- [cppcoro] - Wonderful library containing several useful codes for references
- [1024cores.net] - Explains the lock-free programming very well also covers how false sharing(cache line collisions) between thread impacts performance.
- [ConcurrencyFreaks] - Repository with some understandable but carefully written concurrent data structures

[//]: # (Below are link reference definitions)
[Cranberry game engine]: https://jeslaspravin.com/workDetails/projects/1001
[cppcoro]: https://github.com/lewissbaker/cppcoro
[1024cores.net]: https://www.1024cores.net/home/lock-free-algorithms/introduction
[ConcurrencyFreaks]: https://github.com/pramalhe/ConcurrencyFreaks
