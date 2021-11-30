#ifndef PRUSALSICER_WORKER_HPP
#define PRUSALSICER_WORKER_HPP

#include <memory>

#include "Job.hpp"

namespace Slic3r { namespace GUI {

// An interface of a worker that runs jobs on a dedicated worker thread, one
// after the other. It is assumed that every method of this class is called
// from the same main thread.
class Worker {
public:
    // Queue up a new job after the current one. This call does not block.
    // Returns false if the job gets discarded.
    virtual bool start_next(std::unique_ptr<Job> job) = 0;

    // Returns true if no job is running and no job message is left to be processed.
    // This means that nothing is left to finalize or take care of in the main thread.
    virtual bool is_idle() const = 0;

    // Ask the current job gracefully to cancel. This call is not blocking and
    // the job may or may not cancel eventually, depending on its
    // implementation. Note that it is not trivial to kill a thread forcefully
    // and we don't need that.
    virtual void cancel() = 0;

    // This method will delete the queued jobs and cancel the current one.
    virtual void cancel_all() = 0;

    // Needs to be called continuously to process events (like status update or
    // finalizing of jobs) in the UI thread. This can be done e.g. in a wxIdle
    // handler.
    virtual void process_events() = 0;

    // The destructor shall properly close the worker thread.
    virtual ~Worker() = default;
};

template<class Fn> constexpr bool IsProcessFn = std::is_invocable_v<Fn, Job::Ctl&>;
template<class Fn> constexpr bool IsFinishFn  = std::is_invocable_v<Fn, bool, std::exception_ptr&>;

// Helper function to use the worker with arbitrary functors.
template<class ProcessFn, class FinishFn,
         class = std::enable_if_t<IsProcessFn<ProcessFn>>,
         class = std::enable_if_t<IsFinishFn<FinishFn>> >
bool queue_job(Worker &w, ProcessFn fn, FinishFn finishfn)
{
    struct LambdaJob: Job {
        ProcessFn fn;
        FinishFn  finishfn;

        LambdaJob(ProcessFn pfn, FinishFn ffn)
            : fn{std::move(pfn)}, finishfn{std::move(ffn)}
        {}

        void process(Ctl &ctl) override { fn(ctl); }
        void finalize(bool canceled, std::exception_ptr &eptr) override
        {
            finishfn(canceled, eptr);
        }
    };

    auto j = std::make_unique<LambdaJob>(std::move(fn), std::move(finishfn));
    return w.start_next(std::move(j));
}

template<class ProcessFn, class = std::enable_if_t<IsProcessFn<ProcessFn>>>
bool queue_job(Worker &w, ProcessFn fn)
{
    return queue_job(w, std::move(fn), [](bool, std::exception_ptr &) {});
}

inline bool queue_job(Worker &w, std::unique_ptr<Job> j)
{
    return w.start_next(std::move(j));
}

// Replace the current job queue with a new job. This cancels all jobs and
// will not wait. The new job will begin after the queue cancels properly.
// Note that this can be called from the UI thread and will not block it if the
// jobs take longer to cancel.
template<class...Args> bool replace_job(Worker &w, Args&& ...args)
{
    w.cancel_all();
    return queue_job(w, std::forward<Args>(args)...);
}

}} // namespace Slic3r::GUI

#endif // WORKER_HPP