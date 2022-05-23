# **Co**operative **Pa**rallel **T**asking *(or)* **CoPaT**
This is an experimental cooperative multi tasking library. I used coroutines to achieve cooperative multi task.
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
- Ofcourse bug fixes :wink:

## External libraries used
- [ConcurrencyFreaks] - Using modified version of [FAAArrayQueue.hpp](https://github.com/pramalhe/ConcurrencyFreaks/blob/master/CPP/queues/array/FAAArrayQueue.hpp) for MPMC(Multi Producer Multi Consumer) and MPSC(MP Single Consumer) queues used for job system. The algorithms are modified to suit my needs and embedded into this libraries code. [License](https://github.com/pramalhe/ConcurrencyFreaks/blob/master/LICENSE)

## Compiler requirement
Any C++ 20 standard compliant compiler must be able to compile and run this library successfully.

## Platform support
Right now due to some platform specific codes. Supported platform is limited to **Windows**. However I have exposed `PlatformThreadingFuncs` type overrideable so that you can hook your application's platform specific code to be used in this library. You just have to add `OVERRIDE_PLATFORMTHREADINGFUNCTIONS` define to your platform functions class/wrapper in `CoPatConfig.h` and define few necessary functions(same as in `GenericThreadingFunctions`)
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
template <typename RetType, typename BasePromiseType, bool EnqAtInitialSuspend, bool InMainThread>
class JobSystemTaskType;
```
Above *Awaitable* type is the main coroutine return type for this job system.

- `RetType` - What type will `co_await` on this object will return to awaiter
- `BasePromiseType` - can take `JobSystemPromiseBase` or `JobSystemPromiseBaseMC`. Once this coroutine job reaches `final_suspend` all the awaiters awaiting on this job will be resumed.
    * `JobSystemPromiseBase` for jobs which can be awaited in only one other coroutine
    * `JobSystemPromiseBaseMC` for jobs which can be awaited by more than one coroutines
- `EnqAtInitialSuspend` - As the name suggests. Jobs with this `true` will be enqueued to job system automatically at `initial_suspend`, The thread to which it will be enqueued depends on next boolean `InMainThread`. while `false` means this job will start executing in current thread synchronously until it is manually switched in the middle.
- `InMainThread` - If previous boolean is `true`, The value in this constant will used to determine whether job must be queued to main thread or one of worker thread.

### Example
```cpp
// Do not queues the task to job system automatically
copat::JobSystemTaskType<..., /*EnqAtInitialSuspend*/ false, /*InMainThread*/ false> noEnqJob();
// Enqueues to worker thread automatically
copat::JobSystemTaskType<..., /*EnqAtInitialSuspend*/ true, /*InMainThread*/ false> workerJob();
// Enqueues to main thread automatically
copat::JobSystemTaskType<..., /*EnqAtInitialSuspend*/ true, /*InMainThread*/ true> mainThreadJob();
```

## Switching job thread
You can switch between threads in the middle of job manually using `template <bool SwitchToMain> struct SwitchJobThreadAwaiter`
```cpp
// copat::JobSystemEnqTask<false> is just specialization of copat::JobSystemTaskType with boolean to decide which thread to enqueue this task
// False means worker thread here
copat::JobSystemEnqTask<false> testManualSwitch(u32 counter)
{
    std::cout << copat::PlatformThreadingFuncs::getCurrentThreadName() << " is the executor, Counter : " << counter << std::endl;
    // Switching to main thread
    co_await copat::SwitchJobThreadAwaiter<true>{};
    std::cout << copat::PlatformThreadingFuncs::getCurrentThreadName() << " is it main thread, Counter : " << counter << std::endl;
}
```

## Thread wait on a job
You can lock wait on a single job/awaitable using `copat::waitOnAwaitable(awaitable)`. Be aware that if you wait on a job that is already queued in same thread it will lead to dead-lock.
The thread that calls this will wait until the job is finished.
```cpp
copat::JobSystemReturnableTask<u32&, true, false> testCoroWait();
copat::JobSystemEnqTask<false> testCoroWaitNoRet();

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
copat::JobSystemReturnableTask<u32&, true, false> testCoroWait();
copat::JobSystemEnqTask<false> testCoroWaitNoRet();

/**
 * This job awaits until all the sub tasks this starts and then switches to main thread and executes few lines of code
 */
copat::NormalFuncAwaiter testawaitAll()
{
    auto ret = testCoroWait();
    auto noret = testCoroWaitNoRet();
    co_await copat::awaitAllTasks(ret, noret);

    std::cout << copat::PlatformThreadingFuncs::getCurrentThreadName() << " is the executor after awaitAllTasks" << std::endl;
    co_await copat::SwitchJobThreadAwaiter<true>{};
    std::cout << copat::PlatformThreadingFuncs::getCurrentThreadName() << " is it main thread after awaitAllTasks" << std::endl;
}


copat::JobSystemReturnableTask<u32, false, false> testRetCoro();
copat::JobSystemReturnableTask<u32&, false, false> testRetCoroRef();

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

## References
- [cppcoro] - Wonderful library containing several useful codes for references
- [1024cores.net] - Explains the lock-free programming very well also covers how false sharing(cache line collisions) between thread impacts performance.
- [ConcurrencyFreaks] - Repository with some understandable but carefully written concurrent data structures

[//]: # (Below are link reference definitions)
[Cranberry game engine]: https://jeslaspravin.com/workDetails/projects/13
[cppcoro]: https://github.com/lewissbaker/cppcoro
[1024cores.net]: https://www.1024cores.net/home/lock-free-algorithms/introduction
[ConcurrencyFreaks]: https://github.com/pramalhe/ConcurrencyFreaks