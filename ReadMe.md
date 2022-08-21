# **Co**operative **Pa**rallel **T**asking *(or)* **CoPaT**
This is a C++ 20 cooperative multi tasking library. I used coroutines to achieve cooperative multi task.
I developed this for and also using this as task system in [Cranberry game engine].
I decided to make this library available publically under MIT license. I will try and merge any improvements or changes I implement when using this in my game engine.

Best way to use this library is just copy the files in *Source* directory and paste it in your code base. Then all you have to do is
```cpp
void mainTick(void* userData);

int main()
{
    copat::JobSystem js;
    /**
     * Initializes jobsystem with worker threads, sets up the TLS and setup singleton instance for future access
     * mainTick is call back that gets executed every loop of main thread(I do not create new thread for main thread)
     */
    js.initialize(&mainTick, nullptr);
    /**
     * Just like thread join. This join blocks and starts main thread loop. Main thread loop executes the mainTick and pumps the posted main 
     * thread jobs.
     * If your application just needs workers and will not queue any jobs to main queue, You can safely ignore joinMain(). Note that if you
     * ignore joinMain() mainTick never gets called.
     */
    js.joinMain();
    /**
     * Must be called when you want to safely exit all threads. shutdown blocks
     */
    js.shutdown();
}
```
Above code is all you need to run the job system. You can exit from `joinMain()` by calling `JobSystem::exitMain()`.

## Future Goals
Right now the library is just a bunch of headers and TU files
- Convert to CMake library project
- Add unit test and cover all feature's basic use cases
- Integrate github's CI pipeline
- Other platforms support in library
- Check if reusing nodes is worth it in FAAArrayQueues
- Ofcourse bug fixes :wink:

## External libraries used
- [ConcurrencyFreaks] - Using modified version of [FAAArrayQueue.hpp](https://github.com/pramalhe/ConcurrencyFreaks/blob/master/CPP/queues/array/FAAArrayQueue.hpp) for MPMC(Multi Producer Multi Consumer) and MPSC(MP Single Consumer) queues used for job system. The algorithms are modified to suit my needs and embedded into this library's code. [License](https://github.com/pramalhe/ConcurrencyFreaks/blob/master/LICENSE)

## Compiler requirement
Any C++ 20 standard compliant compiler must be able to compile and run this library successfully.

## Platform support
Right now due to some platform specific codes, Supported platform is limited to **Windows**. However I have exposed `PlatformThreadingFuncs` type overrideable so that you can hook your application's platform specific code to be used in this library. You just have to add `OVERRIDE_PLATFORMTHREADINGFUNCTIONS` define to your platform functions class/wrapper in `CoPatConfig.h` and define few necessary functions(same as in `GenericThreadingFunctions`)
```cpp
/**
 * Override PlatformThreadingFunctions.
 */
#define OVERRIDE_PLATFORMTHREADINGFUNCTIONS YourPlatformFunctionsType
```

## Platform architecture support
I have run a few test cases in `x64` arch. However It should be fine in `x86` as well. I am not sure about `arm`
(I do not have much knowledge in that architecture)

# Usage
```cpp
template <typename RetType, typename BasePromiseType, bool EnqAtInitialSuspend, EJobThreadType EnqueueInThread>
class JobSystemTaskType;
```
Above *Awaitable* type is the main coroutine return type for this job system.

- `RetType` - What type will `co_await` on this object will return to awaiter
- `BasePromiseType` - can take `JobSystemPromiseBase` or `JobSystemPromiseBaseMC`. Once this coroutine job reaches `final_suspend` all the awaiters awaiting on this job will be resumed.
    * `JobSystemPromiseBase` for jobs which can be awaited in only one other coroutine
    * `JobSystemPromiseBaseMC` for jobs which can be awaited by more than one coroutines
- `EnqAtInitialSuspend` - As the name suggests. Jobs with this `true` will be enqueued to job system automatically at `initial_suspend`, The thread to which it will be enqueued depends on next enum `EnqueueInThread`. while `false` means this job will start executing in current thread synchronously until it is manually switched in the middle.
- `EnqueueInThread` - This value must be a valid enum of thread type to which this just must be queued to.

### Example
```cpp
// Do not queues the task to job system automatically
copat::JobSystemTaskType<..., /*EnqAtInitialSuspend*/ false, /*EnqueueInThread*/ EJobThreadType::WorkerThreads> noEnqJob();
// Enqueues to worker thread automatically
copat::JobSystemTaskType<..., /*EnqAtInitialSuspend*/ true, /*EnqueueInThread*/ EJobThreadType::WorkerThreads> workerJob();
// Enqueues to main thread automatically
copat::JobSystemTaskType<..., /*EnqAtInitialSuspend*/ true, /*EnqueueInThread*/ EJobThreadType::MainThread> mainThreadJob();
```

## Switching job thread
You can switch between threads in the middle of job manually using `template <EJobThreadType SwitchToThread> struct SwitchJobThreadAwaiter`
```cpp
// copat::JobSystemEnqTask<EJobThreadType::WorkerThreads> is just specialization of copat::JobSystemTaskType with boolean to decide which thread to enqueue this task
copat::JobSystemEnqTask<EJobThreadType::WorkerThreads> testManualSwitch(u32 counter)
{
    std::cout << copat::PlatformThreadingFuncs::getCurrentThreadName() << " is the executor, Counter : " << counter << std::endl;
    // Switching to main thread
    co_await copat::SwitchJobThreadAwaiter<EJobThreadType::MainThread>{};
    std::cout << copat::PlatformThreadingFuncs::getCurrentThreadName() << " is it main thread, Counter : " << counter << std::endl;
}
```

## Thread wait on a job
You can lock wait on a single job/awaitable using `copat::waitOnAwaitable(awaitable)`. Be aware that if you wait on a job that is already queued in same thread it will lead to dead-lock.
The thread that calls this will wait until the job is finished.
```cpp
copat::JobSystemReturnableTask<u32&, true, EJobThreadType::WorkerThreads> testCoroWait();
copat::JobSystemEnqTask<EJobThreadType::WorkerThreads> testCoroWaitNoRet();

auto retJob = testCoroWait();
auto noretJob = testCoroWaitNoRet();
// Waits until testCoroWait is done
u32& retVal = copat::waitOnAwaitable(retVal);
// Then waits until testCoroWaitNoRet is done
copat::waitOnAwaitable(noretJob);
```

## Job wait until all awaitables
This is non locking alternative for `copat::waitOnAwaitable(awaitable)`. So what this basically does is just suspend the waiting job and waits until awaiting job is done. This can be done in two ways

- Job to Job await - Awaitable returned from a job can be `co_await`ed on another job. This suspends second job until first job is completed.
Once first job is finished second job resumes in same thread of awaited job
- Multiple job to job await - Awaitable returned from several jobs can be piped through `copat::awaitAllTasks(...)` to create new awaitable which then can be awaited in another job to wait until all the jobs that it awaits on is finished.

### Example
```cpp
copat::JobSystemReturnableTask<u32&, true, EJobThreadType::WorkerThreads> testCoroWait();
copat::JobSystemEnqTask<EJobThreadType::WorkerThreads> testCoroWaitNoRet();

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


copat::JobSystemReturnableTask<u32, false, EJobThreadType::WorkerThreads> testRetCoro();
copat::JobSystemReturnableTask<u32&, false, EJobThreadType::WorkerThreads> testRetCoroRef();

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
`dispatch()` function can be used to dispatch a job on `n` number of data.

### Example
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

## User defined threads
Along with default main and worker threads, User can add their own special threads by defining `USER_DEFINED_THREADS()` with list of comma separated special threads enum values terminated with comma as well. First user thread enum must have value `EJobThreadType::MainThread + 1` and Last user thread enum must have value `EJobThreadType::WorkerThreads - 1`. Every enum must be in sequential order.

```cpp
/**
 * Thread types that are added by user
 */
//#define USER_DEFINED_THREADS() Thread1 = 1, Thread2, ... , ThreadN = WorkerThreads - 1
#define USER_DEFINED_THREADS() RenderThread, AudioThread, PhysicsThread,
```

## Enqueueing Coroutine Task to different Job system
Now you can have any number of job instance and have tasks be enqueued to any of those job system.
All you have to do is pass the JobSystem reference(`JobSystem &`) to the coroutine and call that coroutine with job system you want it to be enqueued to.

> Note that `JobSystem::get()` still exists and Coroutine jobs that do not have `JobSystem & or JobSystem *` as function's first parameter will use it to enqueue the job.
The job system that gets initialized the very first time will be stored in JobSystem singleton. This decision is to allow a main job system(Will be stored in the singleton) and some sub job system that will be useable inside different subsystem.

### Example
```cpp
copat::JobSystemEnqTask<copat::EJobThreadType::WorkerThreads> testThreadedTask(copat::JobSystem& jobSystem, u32 counter);

copat::JobSystem jsA;
copat::JobSystem jsB;

// This task gets enqueued to jsA's EJobThreadType::WorkerThreads
auto t1 = testThreadedTask(jsA, counter);
// This task gets enqueued to jsB's EJobThreadType::WorkerThreads
auto t2 = testThreadedTask(jsB, counter);
```

## References
- [cppcoro] - Wonderful library containing several useful codes for references
- [1024cores.net] - Explains the lock-free programming very well also covers how false sharing(cache line collisions) between thread impacts performance.
- [ConcurrencyFreaks] - Repository with some understandable but carefully written concurrent data structures

[//]: # (Below are link reference definitions)
[Cranberry game engine]: https://jeslaspravin.com/workDetails/projects/13
[cppcoro]: https://github.com/lewissbaker/cppcoro
[1024cores.net]: https://www.1024cores.net/home/lock-free-algorithms/introduction
[ConcurrencyFreaks]: https://github.com/pramalhe/ConcurrencyFreaks