/*******************************************************************************
 * BaseSignalGP<EXEC_STEPPER_T, CUSTOM_COMPONENT_T>
 * ..
 * EXEC_STEPPER_T - Execution Stepper
 *   - The execution stepper knows how to execute programs. What type of programs?
 *     That's entirely up to the particular implementation of the execution stepper.
 *     So long as the execution stepper provides an appropriate interface, SignalGP
 *     does not care about the particulars.
 *   - Execution stepper interface requirements:
 *     - Required types:
 *       - exec_state_t => execution state information
 *       - program_t    => what type of program does the execution stepper run?
 *       - tag_t        => what type of tag does the execution stepper use to reference modules?
 *       - ?module_t?
 *     - Required function signatures:
 *       - EXEC_STEPPER_T(const EXEC_STEPPER_T&)
 *         - Copy constructor.
 *       - program_t & GetProgram()
 *         - Returns the program currently loaded on the execution stepper.
 *       - void SetProgram(const program_t&)
 *         - Loads a new program on the execution stepper. Handles all appropriate
 *           cleanup and internal state resetting necessary when switching to running
 *           a new program.
 *       - void ResetProgram()
 *         - Clear out the old program (if any). Reset internal state as appropriate.
 *       - void ResetHardwareState()
 *         - Reset internal state of execution stepper without resetting the program.
 *       - vector<size_t> FindModuleMatch(const tag_t&, size_t N)
 *         - Return a vector (max size N) of module IDs that match with specified
 *           tag.
 *       - void InitThread(Thread &, size_t module_id)
 *         - Initialize given thread by calling the module specified by module_id.
 *       - void SingleExecutionStep(SignalGP<EXEC_STEPPER_T, CUSTOM_COMPONENT_T> &, exec_state_t&)
 *         - Advance a single execution stepper on the given execution state using
 *           the given SignalGP hardware state.
 *******************************************************************************/

#ifndef EMP_SIGNALGP_BASE_H
#define EMP_SIGNALGP_BASE_H

#include <iostream>
#include <utility>
#include <limits>
#include <optional>
#include <queue>
#include <tuple>

#include "base/Ptr.h"
#include "base/vector.h"
#include "tools/Random.h"

#include "EventLibrary.h"
#include "InstructionLibrary.h"

// TODO - allow a hardcap on TOTAL THREAD SPACE (pending + active) to be set!

// @discussion - how could I use concepts to clean this up?
// @discussion - where should I put configurable lambdas?

namespace emp { namespace signalgp {

  struct BaseEvent {
    size_t id;

    size_t GetID() const { return id; }

    void Print(std::ostream & os) const {
      os << "{id:" << GetID() << "}";
    }
  };

  /// Placeholder additional component type.
  struct DefaultCustomComponent { };

  // todo - move function implementations outside of class
  // todo - make signalgp hardware not awful (& safe) to make copies of
  // @discussion - template/organization structure
  // What about program_t?
  // TODO - rename to SignalGPBase.h
  template<typename DERIVED_T,
           typename EXEC_STATE_T,
           typename TAG_T,
           typename CUSTOM_COMPONENT_T=DefaultCustomComponent>  // @DISCUSSION - additional component here versus in derived? arg for: signaling to derived devs?
  class BaseSignalGP {
  public:
    struct Thread;

    /// Theoretical thread limit for hardware.
    /// Some function use max size_t to indicate no threads... TODO - internal thread_id struct
    static constexpr size_t THREAD_LIMIT = std::numeric_limits<size_t>::max() - 1;

    // Types that base signalgp functionality needs to know about.
    using hardware_t = DERIVED_T;
    using exec_state_t = EXEC_STATE_T;
    // using program_t = PROGRAM_T; @discussion - ??
    using tag_t = TAG_T;
    using custom_comp_t = CUSTOM_COMPONENT_T;

    using event_t = BaseEvent;
    using event_lib_t = EventLibrary<hardware_t>;

    using thread_t = Thread;

    using fun_print_hardware_state_t = std::function<void(const hardware_t&, std::ostream &)>;
    // using fun_print_program_t = std::function<void(const program_t&, const hardware_t&, std::ostream &)>;
    using fun_print_execution_state_t = std::function<void(const exec_state_t &, const hardware_t&, std::ostream &)>;
    using fun_print_event_t = std::function<void(const event_t &, const hardware_t&, std::ostream &)>;

    // QUESTION - Pros/cons of nesting Thread type in SignalGP class?
    enum class ThreadState { RUNNING, DEAD, PENDING };
    struct Thread {
      // comment => labels can exist inside execution state.
      exec_state_t exec_state;
      double priority;
      ThreadState run_state;

      Thread(const exec_state_t & _exec_state=exec_state_t(), double _priority=1.0)
        : exec_state(_exec_state),
          priority(_priority)
          run_state(ThreadState::DEAD) { ; }

      void Reset() {
        // @discussion - How do we want to handle this?
        exec_state.Clear(); // TODO - make this functionality more flexible! Currently assumes exec_state_t has a Clear function!
        run_state = ThreadState::DEAD;
        priority = 1.0;
      }

      exec_state_t & GetExecState() { return exec_state; }
      const exec_state_t & GetExecState() const { return exec_state; }

      void SetDead() { run_state = ThreadState::DEAD; }
      bool IsDead() const { return run_state == ThreadState::DEAD; }

      void SetPending() { run_state = ThreadState::PENDING; }
      bool IsPending() const { return run_state == ThreadState::PENDING; }

      void SetRunning() { run_state = ThreadState::RUNNING; }
      bool IsRunning() const { return run_state == ThreadState::RUNNING; }

      double GetPriority() const { return priority; }
      void SetPriority(double p) { priority = p; }
    };

  protected:
    Ptr<event_lib_t> event_lib;         ///< These are the events this hardware knows about.
    std::deque<event_t> event_queue;    ///< Queue of events to be processed every time step.

    // Thread management
    size_t max_active_threads=64;               ///< Maximum number of concurrently running threads.
    size_t max_thread_space=512;         ///
    emp::vector<thread_t> threads;       ///< All threads (each could be active/inactive/pending).
    // TODO - add a 'use_thread_priority' setting => default = true
    // @discussion - can't really track thread priorities separate from threads, as they can get updated on the fly
    emp::vector<size_t> thread_exec_order;  ///< Thread execution order.
    std::unordered_set<size_t> active_threads;        ///< Active thread ids.
    emp::vector<size_t> unused_threads;     ///< Unused thread ids.
    std::deque<size_t> pending_threads;     ///< Pending thread ids.

    size_t cur_thread_id=(size_t)-1;    ///< Currently executing thread.
    bool is_executing=false;            ///< Is this hardware unit currently executing (within a SingleProcess)?

    custom_comp_t custom_component;

    // Configurable print functions. @NOTE: should these emp_assert(false)?
    fun_print_hardware_state_t fun_print_hardware_state = [](const hardware_t& hw, std::ostream & os) { return; };
    // fun_print_program_t fun_print_program = [](const program_t& p, const hardware_t& hw, std::ostream & os) { return; };
    fun_print_execution_state_t fun_print_execution_state = [](const exec_state_t & e, const hardware_t& hw, std::ostream & os) { return; };
    fun_print_event_t fun_print_event = [](const event_t & e, const hardware_t& hw, std::ostream & os) { e.Print(os); };

    void ActivateThread(size_t thread_id) {
      emp_assert(thread_id < threads.size());
      // todo - guarantee that thread_id not already in thread_exec order
      active_threads.emplace(thread_id);
      thread_exec_order.emplace_back(thread_id);
      threads[thread_id].SetRunning();
    }

    /// notice - does not remove from execution order! (SingleProcess will clean that up)
    /// todo - Make public?
    void KillThread(size_t thread_id) {
      emp_assert(thread_id < threads.size());
      active_threads.erase(thread_id);
      threads[thread_id].SetDead();
    }

    void ActivatePendingThreads() {
      // NOTE: Assumes active threads is accurate!
      // NOTE: all pending threads + active threads should have unique ids

      // Are there pending threads to activate?
      if (pending_threads.empty()) return;

      // Spawn pending threads (in order of arrival) until no more room.
      while (pending_threads.size() && (active_threads.size() < max_active_threads)) {
        const size_t thread_id = pending_threads.front();
        emp_assert(thread_id < threads.size());
        emp_assert(threads[thread_id].IsPending());
        // threads[thread_id].SetRunning();
        // active_threads.emplace(pending_threads.front());
        ActivateThread(thread_id);
        pending_threads.pop_front();
      }

      // Have we finished handling pending threads?
      if (pending_threads.empty()) return;

      // If we're here, we hit max thread capacity. We'll need to kill active
      // threads to make room for pending threads.

      // Find max pending priority, use it to bound which active threads we consider
      // killing.
      const double max_pending_priority = pending_threads.front().GetPriority();
      for (size_t thread_id : pending_threads) {
        emp_assert(thread_id < threads.size());
        emp_assert(threads[thread_id].IsPending());
        const double priority = threads[thread_id].GetPriority();
        if (priority > max_pending_priority) max_pending_priority = priority;
      }

      // Create a MIN heap of the active threads (only include threads with priority < MAX_PENDING_PRIORITY)
      // - todo => make a member variable to avoid re-allocation every time?
      std::priority_queue<std::tuple<double, size_t>,
                          std::vector<std::tuple<double, size_t>>,
                          std::greater<std::tuple<double, size_t>>> active_priorities;
      for (size_t active_id : active_threads) {
        emp_assert(active_id < threads.size());
        thread_t & thread = threads[active_id];
        if (thread.GetPriority() < max_pending_priority) {
          active_priorities.emplace(std::make_tuple(thread.GetPriority(), active_id));
        }
      }

      // Reminder: we're considering active threads in order of least priority &
      //           pending threads in order of arrival.
      while(active_priorities.size() && pending_threads.size()) {
        const size_t pending_id = pending_threads.front();
        const size_t active_id = active_priorities.top().get<1>();
        if (threads[pending_id].GetPriority() > threads[active_id].GetPriority()) {
          active_priorities.pop();      // Remove active priority from heap.
          // Kill active thread.
          KillThread(active_id);
          // Activate pending thread.
          ActivateThread(pending_id);
          pending_threads.pop_front(); // Pop pending thread.
        } else {
          // This pending thread won't replace any active threads (not high enough priority).
          pending_threads.pop_front();
          threads[pending_id].SetDead(); // no longer pending
        }
      }

      // Any leftover pending threads do not have sufficient priority to kill
      // an active thread.
      while (pending_threads.size()) {
        const size_t pending_id = pending_threads.front();
        threads[pending_id].SetDead();  // no longer pending
        pending_threads.pop_front();
      }
    }

  public:
    BaseSignalGP(Ptr<event_lib_t> elib)
      : event_lib(elib),
        event_queue(),
        threads( (2*max_active_threads < max_thread_space) ? 2*max_active_threads : max_thread_space ),
        thread_exec_order(),
        active_threads(),
        unused_threads( threads.size() ),
        pending_threads()
    {
      // Set all threads to unused.
      for (size_t i = 0; i < unused_threads.size(); ++i) {
        unused_threads[i] = (unused_threads.size() - 1) - i;
      }
    }

    // Todo - test!
    /// Move constructor.
    BaseSignalGP(BaseSignalGP && in) = default;

    /// todo - test!
    /// Copy constructor.
    BaseSignalGP(const BaseSignalGP & in) = default;

    /// Destructor.
    // todo - test!
    ~BaseSignalGP() { }

    /// Full virtual hardware reset:
    /// Required
    virtual void Reset() = 0;

    /// Required
    virtual void SingleExecutionStep(DERIVED_T &, thread_t &) = 0;

    /// Required
    virtual vector<size_t> FindModuleMatch(const tag_t &, size_t) = 0;

    /// Required
    virtual void InitThread(thread_t &, size_t) = 0;

    /// HardwareState reset:
    /// - Reset execution stepper hardware state.
    /// - Clear event queue.
    /// - Reset all threads, move all to unused; clear pending.
    void BaseResetState() {
      emp_assert(!is_executing, "Cannot reset hardware while executing.");
      event_queue.clear();
      for (auto & thread : threads) {
        thread.Reset();
      }
      thread_exec_order.clear(); // No threads to execute.
      active_threads.clear();    // No active threads.
      pending_threads.clear();   // No pending threads.
      unused_threads.resize(threads.size());
      // unused_threads.resize(max); TODO - fix
      // Add all available threads to unused.
      for (size_t i = 0; i < unused_threads.size(); ++i) {
        unused_threads[i] = (unused_threads.size() - 1) - i;
      }
      cur_thread_id = (size_t)-1;
      is_executing = false;
    }

    /// Get event library associated with hardware.
    Ptr<const event_lib_t> GetEventLib() const { return event_lib; }

    /// Get reference to this hardware's execution stepper object.
    DERIVED_T & GetHardware() { return static_cast<DERIVED_T&>(*this); }
    const DERIVED_T & GetHardware() const { return static_cast<const DERIVED_T&>(*this); }

    /// Access hardware custom component.
    custom_comp_t & GetCustomComponent() { return custom_component; }
    const custom_comp_t & GetCustomComponent() const { return custom_component; }
    void SetCustomComponent(const custom_comp_t & val) { custom_component = val; }

    /// Get the maximum number of threads allowed to run simultaneously on this hardware
    /// object.
    size_t GetMaxActiveThreads() const { return max_active_threads; }

    /// Get maximum number of active + pending threads allowed to exist simultaneously
    /// on this hardware object.
    size_t GetMaxThreadSpace() const { return max_thread_space; }

    /// Get the number of currently running threads.
    size_t GetNumActiveThreads() const { return active_threads.size(); }

    /// Get number of threads being considered for activation.
    size_t GetNumPendingThreads() const { return pending_threads.size(); }

    /// Get number of unused threads. May be larger than max number of active threads.
    size_t GetNumUnusedThreads() const { return unused_threads.size(); }

    /// Get a reference to active threads.
    /// NOTE: use responsibly! No safety gloves here!
    emp::vector<thread_t> & GetThreads() { return threads; }

    /// Get a reference to a particular thread.
    thread_t & GetThread(size_t i) { emp_assert(i < threads.size()); return threads[i]; }
    const thread_t & GetThread(size_t i) const { emp_assert(i < threads.size()); return threads[i]; }

    /// Get const reference to vector of currently active threads active.
    const std::unordered_set<size_t> & GetActiveThreadIDs() const { return active_threads; }

    /// Get const reference to threads that are not currently active.
    const emp::vector<size_t> & GetUnusedThreadIDs() const { return unused_threads; }

    /// Get const reference to thread ids of pending threads.
    const std::deque<size_t> & GetPendingThreadIDs() const { return pending_threads; }

    /// Get const reference to thread execution order. Note, not all threads in exec
    /// order list guaranteed to be active.
    const emp::vector<size_t> & GetThreadExecOrder() const { return thread_exec_order; }

    /// Get the ID of the currently executing thread. If hardware is not in midst
    /// of an execution cycle, this will return (size_t)-1.
    size_t GetCurThreadID() { return cur_thread_id; }

    /// Get the currently executing thread. Only valid to call this while virtual
    /// hardware is executing. Otherwise, will error out.
    thread_t & GetCurThread() {
      emp_assert(is_executing, "Hardware is not executing! No current thread.");
      emp_assert(cur_thread_id < threads.size());
      return threads[cur_thread_id];
    }

    /// TODO - TEST => PULLED FROM ORIGINAL SIGNALGP
    // Warning: If you decrease max threads, you may kill actively running threads.
    // Warning: If you decrease max threads
    // Slow operation.
    // TODO - fix set thread limit function
    void SetThreadLimit(size_t n) {
      emp_assert(n, "Max thread count must be greater than 0.");
      emp_assert(n <= THREAD_LIMIT, "Max thread count must be less than or equal to", THREAD_LIMIT);
      emp_assert(!is_executing, "Cannot adjust SignalGP hardware max thread count while executing.");
      emp_assert(false, "TODO");
    }

    /// Spawn a number of threads (<= n). Use tag to select which modules to call.
    /// Return a vector of spawned thread IDs.
    ///  - If hardware is executing, these threads will be marked as pending.
    ///  - If hardware is not executing, each requested new thread will:
    ///    - Be initialized if # active threads < max_threads
    ///    - Be initialized if # active threads == max_threads && priority level
    ///      is greater than lowest priority level of active threads.
    ///    - Not be initialized if # active threads == max-threads && priority level
    ///      is less than lowest priority level of active threads.
    emp::vector<size_t> SpawnThreads(const tag_t & tag, size_t n, double priority=1.0) {
      emp_assert(false);
      emp::vector<size_t> matches(GetHardware().FindModuleMatch(tag, n));
      emp::vector<size_t> thread_ids;
      for (size_t match : matches) {
        // TODO!
      }
      return thread_ids;
    }

    size_t SpawnThreadWithTag(const tag_t & tag, double priority=1.0) {
      // TODO
      emp_assert(false);
    }

    /// Spawn a new thread with given ID.
    /// If no unused threads & already maxed out thread space, will not spawn new
    /// thread.
    /// Otherwise, mark thread as pending.
    std::optional<size_t> SpawnThreadWithID(size_t module_id, double priority=1.0) {
      size_t thread_id;
      // Is there an unused thread to commandeer?
      if (unused_threads.size()) {
        // Unused thread is available, use it.
        thread_id = unused_threads.back();
        unused_threads.pop_back();
      } else if (threads.size() < max_thread_space) {
        // No unused threads available, but we have space to make a new one.
        thread_id = threads.size();
        threads.emplace_back();
      } else {
        // No unused threads available, and no more space to make a new one.
        return std::nullopt;
      }
      // If we make it here, we have a valid thread_id to use.
      emp_assert(thread_id < threads.size());

      // We've identified a thread to commandeer. Reset it, initialize it, and
      // mark it appropriately.
      thread_t & thread = threads[thread_id];
      thread.Reset();
      thread.SetPriority(priority);

      // Let derived hardware initialize thread w/appropriate module.
      GetHardware().InitThread(thread, module_id);

      // Mark thread as pending.
      thread.SetPending();
      pending_threads.emplace_back(thread_id);

      return std::optional<size_t>{thread_id}; // this could mess with thread priority level!
    }

    /// Handle an event (on this hardware) now!.
    void HandleEvent(const event_t & event) { event_lib->HandleEvent(GetHardware(), event); }

    /// Trigger an event (from this hardware).
    void TriggerEvent(const event_t & event) { event_lib->TriggerEvent(GetHardware(), event); }

    /// Queue an event (to be handled by this hardware) next time this hardware
    /// unit is executed.
    void QueueEvent(const event_t & event) { event_queue.emplace_back(event); }

    /// Advance the hardware by a single step.
    void SingleProcess() {
      // Handle events (which may spawn threads)
      while (!event_queue.empty()) {
        HandleEvent(event_queue.front());
        event_queue.pop_front();
      }

      // Activate all pending threads. (which may kill currently active threads)
      ActivatePendingThreads();
      emp_assert(active_threads.size() <= max_active_threads);

      // Begin execution!
      is_executing = true;
      size_t active_thread_id = 0;
      size_t active_thread_cnt = active_threads.size();
      size_t adjust = 0;

      while (active_thread_id < thread_cnt) {
        // todo!
      }

      // todo
      emp_assert(false);
    }

    /// Advance hardware by some arbitrary number of steps.
    void Process(size_t num_steps) {
      for (size_t i = 0; i < num_steps; ++i) {
        SingleProcess();
      }
    }

    /// How does the hardware state get printed?
    void SetPrintHardwareStateFun(const fun_print_hardware_state_t & print_fun) {
      fun_print_hardware_state = print_fun;
    }

    /// How does a single execution state get printed?
    void SetPrintExecutionStateFun(const fun_print_execution_state_t & print_fun) {
      fun_print_execution_state = print_fun;
    }

    /// How do we print a single event?
    void SetPrintEventFun(const fun_print_event_t & print_fun) {
      fun_print_event = print_fun;
    }

    /// Print active threads.
    void PrintActiveThreadStates(std::ostream & os=std::cout) const {
      for (size_t i = 0; i < active_threads.size(); ++i) {
        size_t thread_id = active_threads[i];
        const thread_t & thread = threads[thread_id];
        os << "Thread " << i << " (ID="<< thread_id << "):\n";
        PrintExecutionState(thread.GetExecState(), GetHardware(), os);
        os << "\n";
      }
    }

    /// Print loaded program.
    // void PrintProgram(std::ostream & os=std::cout) const { fun_print_program(os); }

    /// Print overall state of hardware.
    void PrintHardwareState(std::ostream & os=std::cout) const { fun_print_hardware_state(GetHardware(), os); }

    /// Print thread usage status (active, unused, and pending thread ids).
    void PrintThreadUsage(std::ostream & os=std::cout) const {
      // Active threads
      os << "Active threads (" << active_threads.size() << "): [";
      for (size_t i = 0; i < active_threads.size(); ++i) {
        if (i) os << ", ";
        os << active_threads[i];
      }
      os << "]\n";
      // Unused threads
      os << "Unused threads (" << unused_threads.size() << "): [";
      for (size_t i = 0; i < unused_threads.size(); ++i) {
        if (i) os << ", ";
        os << unused_threads[i];
      }
      os << "]\n";
      // Pending threads
      os << "Pending threads (" << pending_threads.size() << "): [";
      for (size_t i = 0; i < pending_threads.size(); ++i) {
        if (i) os << ", ";
        os << pending_threads[i];
      }
      os << "]";
    }

    /// Print everything in the event queue.
    void PrintEventQueue(std::ostream & os=std::cout) const {
      os << "Event queue (" << event_queue.size() << "): [";
      for (size_t i = 0; i < event_queue.size(); ++i) {
        if (i) os << ", ";
        fun_print_event(event_queue[i], GetHardware(), os);
      }
      os << "]";
    }

  };

}}

#endif