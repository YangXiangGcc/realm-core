/*************************************************************************
 *
 * TIGHTDB CONFIDENTIAL
 * __________________
 *
 *  [2011] - [2012] TightDB Inc
 *  All Rights Reserved.
 *
 * NOTICE:  All information contained herein is, and remains
 * the property of TightDB Incorporated and its suppliers,
 * if any.  The intellectual and technical concepts contained
 * herein are proprietary to TightDB Incorporated
 * and its suppliers and may be covered by U.S. and Foreign Patents,
 * patents in process, and are protected by trade secret or copyright law.
 * Dissemination of this information or reproduction of this material
 * is strictly forbidden unless prior written permission is obtained
 * from TightDB Incorporated.
 *
 **************************************************************************/
#ifndef TIGHTDB_UTIL_THREAD_HPP
#define TIGHTDB_UTIL_THREAD_HPP

#include <exception>

#include <pthread.h>

// Use below line to enable a thread bug detection tool. Note: Will make program execution slower.
// #include <../test/pthread_test.hpp>

#include <cerrno>
#include <cstddef>
#include <string>

#include <tightdb/util/features.h>
#include <tightdb/util/assert.hpp>
#include <tightdb/util/terminate.hpp>
#include <tightdb/util/unique_ptr.hpp>
#include <tightdb/util/meta.hpp>

#ifdef TIGHTDB_HAVE_CXX11_ATOMIC
#  include <atomic>
#endif

// FIXME: enable this only on platforms where it might be needed
#ifdef __APPLE__
#define TIGHTDB_CONDVAR_EMULATION
#endif

#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>

namespace tightdb {
namespace util {


/// A separate thread of execution.
///
/// This class is a C++03 compatible reproduction of a subset of
/// std::thread from C++11 (when discounting Thread::start()).
class Thread {
public:
    Thread();
    ~Thread() TIGHTDB_NOEXCEPT;

    template<class F> explicit Thread(F func);

    /// This method is an extension of the API provided by
    /// std::thread. This method exists because proper move semantics
    /// is unavailable in C++03. If move semantics had been available,
    /// calling `start(func)` would have been equivalent to `*this =
    /// Thread(func)`. Please see std::thread::operator=() for
    /// details.
    template<class F> void start(F func);

    bool joinable() TIGHTDB_NOEXCEPT;

    void join();

private:
    pthread_t m_id;
    bool m_joinable;

    typedef void* (*entry_func_type)(void*);

    void start(entry_func_type, void* arg);

    template<class> static void* entry_point(void*) TIGHTDB_NOEXCEPT;

    TIGHTDB_NORETURN static void create_failed(int);
    TIGHTDB_NORETURN static void join_failed(int);
};


/// Low-level mutual exclusion device.
class Mutex {
public:
    Mutex();
    ~Mutex() TIGHTDB_NOEXCEPT;

    struct process_shared_tag {};

    /// Initialize this mutex for use across multiple processes. When
    /// constructed this way, the instance may be placed in memory
    /// shared by multiple processes, as well as in a memory mapped
    /// file. Such a mutex remains valid even after the constructing
    /// process terminates. Deleting the instance (freeing the memory
    /// or deleting the file) without first calling the destructor is
    /// legal and will not cause any system resources to be leaked.
    Mutex(process_shared_tag);

    friend class LockGuard;
    friend class UniqueLock;

protected:
    pthread_mutex_t m_impl;

    struct no_init_tag {};
    Mutex(no_init_tag) {}

    void init_as_regular();
    void init_as_process_shared(bool robust_if_available);

    void lock() TIGHTDB_NOEXCEPT;
    void unlock() TIGHTDB_NOEXCEPT;

    TIGHTDB_NORETURN static void init_failed(int);
    TIGHTDB_NORETURN static void attr_init_failed(int);
    TIGHTDB_NORETURN static void destroy_failed(int) TIGHTDB_NOEXCEPT;
    TIGHTDB_NORETURN static void lock_failed(int) TIGHTDB_NOEXCEPT;

    friend class CondVar;
    friend class CondVarEmulation;
};


/// A simple mutex ownership wrapper.
class LockGuard {
public:
    LockGuard(Mutex&) TIGHTDB_NOEXCEPT;
    ~LockGuard() TIGHTDB_NOEXCEPT;

private:
    Mutex& m_mutex;
    friend class CondVar;
    friend class CondVarEmulation;
};


/// See UniqueLock.
struct defer_lock_tag {};

/// A general-purpose mutex ownership wrapper supporting deferred
/// locking as well as repeated unlocking and relocking.
class UniqueLock {
public:
    UniqueLock(Mutex&) TIGHTDB_NOEXCEPT;
    UniqueLock(Mutex&, defer_lock_tag) TIGHTDB_NOEXCEPT;
    ~UniqueLock() TIGHTDB_NOEXCEPT;

    void lock() TIGHTDB_NOEXCEPT;
    void unlock() TIGHTDB_NOEXCEPT;

private:
    Mutex* m_mutex;
    bool m_is_locked;
};


/// A robust version of a process-shared mutex.
///
/// A robust mutex is one that detects whether a thread (or process)
/// has died while holding a lock on the mutex.
///
/// When the present platform does not offer support for robust
/// mutexes, this mutex class behaves as a regular process-shared
/// mutex, which means that if a thread dies while holding a lock, any
/// future attempt at locking will block indefinitely.
class RobustMutex: private Mutex {
public:
    RobustMutex();
    ~RobustMutex() TIGHTDB_NOEXCEPT;

    static bool is_robust_on_this_platform() TIGHTDB_NOEXCEPT;

    class NotRecoverable;

    /// \param recover_func If the present platform does not support
    /// robust mutexes, this function is never called. Otherwise it is
    /// called if, and only if a thread has died while holding a
    /// lock. The purpose of the function is to reestablish a
    /// consistent shared state. If it fails to do this by throwing an
    /// exception, the mutex enters the 'unrecoverable' state where
    /// any future attempt at locking it will fail and cause
    /// NotRecoverable to be thrown. This function is advised to throw
    /// NotRecoverable when it fails, but it may throw any exception.
    ///
    /// \throw NotRecoverable If thrown by the specified recover
    /// function, or if the mutex has entered the 'unrecoverable'
    /// state due to a different thread throwing from its recover
    /// function.
    template<class Func> void lock(Func recover_func);

    void unlock() TIGHTDB_NOEXCEPT;

    /// Low-level locking of robust mutex.
    ///
    /// If the present platform does not support robust mutexes, this
    /// function always returns true. Otherwise it returns false if,
    /// and only if a thread has died while holding a lock.
    ///
    /// \note Most application should never call this function
    /// directly. It is called automatically when using the ordinary
    /// lock() function.
    ///
    /// \throw NotRecoverable If this mutex has entered the "not
    /// recoverable" state. It enters this state if
    /// mark_as_consistent() is not called between a call to
    /// robust_lock() that returns false and the corresponding call to
    /// unlock().
    bool low_level_lock();

    /// Pull this mutex out of the 'inconsistent' state.
    ///
    /// Must be called only after low_level_lock() has returned false.
    ///
    /// \note Most application should never call this function
    /// directly. It is called automatically when using the ordinary
    /// lock() function.
    void mark_as_consistent() TIGHTDB_NOEXCEPT;

    /// Attempt to check if this mutex is a valid object.
    ///
    /// This attempts to trylock() the mutex, and if that fails returns false if
    /// the return value indicates that the low-level mutex is invalid (which is
    /// distinct from 'inconsistent'). Although pthread_mutex_trylock() may
    /// return EINVAL if the argument is not an initialized mutex object, merely
    /// attempting to check if an arbitrary blob of memory is a mutex object may
    /// involve undefined behavior, so it is only safe to assume that this
    /// function will run correctly when it is known that the mutex object is
    /// valid.
    bool is_valid() TIGHTDB_NOEXCEPT;

    friend class CondVar;
    friend class CondVarEmulation;
};

class RobustMutex::NotRecoverable: public std::exception {
public:
    const char* what() const TIGHTDB_NOEXCEPT_OR_NOTHROW TIGHTDB_OVERRIDE
    {
        return "Failed to recover consistent state of shared memory";
    }
};


/// A simple robust mutex ownership wrapper.
class RobustLockGuard {
public:
    /// \param recover_func See RobustMutex::lock().
    template<class TFunc>
    RobustLockGuard(RobustMutex&, TFunc func);
    ~RobustLockGuard() TIGHTDB_NOEXCEPT;

private:
    RobustMutex& m_mutex;
    friend class CondVar;
    friend class CondVarEmulation;
};



/// Condition variable for use in synchronization monitors.
/// This condition variable uses emulation based on semaphores
/// for the inter-process case, if enabled by TIGHDB_CONDVAR_EMULATION.
/// The emulation does not scale well to many databases, since it currently
/// uses a single shared semaphore. Compared to a good pthread implemenation,
/// the emulation carries an overhead of at most 2 task switches for
/// every waiter notified during notify() or notify_all().
///
/// It is a bit clumsy to have both process local and interprocess
/// condvars with and without emulation in the same class. We might want
/// to split it into PLocalCondVar and PSharedCondVar, possibly with a common
/// base class to express polymorphism.
class CondVar {
public:
    CondVar();
    ~CondVar() TIGHTDB_NOEXCEPT;

    struct process_shared_tag {};

    /// Initialize this condition variable for use across multiple
    /// processes. When constructed this way, you also must place a structure
    /// of type CondVar::SharedPart in memory shared by multiple processes
    /// or in a memory mapped file, and use set_shared_part() to associate
    /// the condition variable with it's shared part. You must initialize
    /// the shared part using CondVar::init_shared_part(), but only before
    /// first use and only when you have exclusive access to the shared part.
    CondVar(process_shared_tag);

#ifdef TIGHTDB_CONDVAR_EMULATION
    struct SharedPart {
        uint64_t signal_counter;
        uint32_t waiters;
    };
#else
    struct SharedPart {
        pthread_cond_t m_impl;
    };
#endif

    /// If you declare a process shared CondVar you need to
    /// bind the emulation to a SharedPart in shared/mmapped memory. The SharedPart
    /// is assumed to have been initialized (possibly by another process) earlier
    /// through a call to init_shared_part.
    void set_shared_part(SharedPart& shared_part, 
                         dev_t device, ino_t inode, std::size_t offset_of_condvar);

    /// Initialize the shared part of a (set of) condition variables.
    static void init_shared_part(SharedPart& shared_part);

    /// Wait for another thread to call notify() or notify_all().
    void wait(LockGuard& l) TIGHTDB_NOEXCEPT;

    // FIXME: we're not emulating wait with timeouts yet, so calling this one
    // with TIGHTDB_CONDVAR_EMULATION on, process sharing and tp != 0 will just assert
    template<class Func>
    void wait(RobustMutex& m, Func recover_func, const struct timespec* tp = 0);
    /// If any threads are wating for this condition, wake up at least
    /// one.
    void notify() TIGHTDB_NOEXCEPT;

    /// Wake up every thread that is currently waiting on this
    /// condition.
    void notify_all() TIGHTDB_NOEXCEPT;

    /// Cleanup and release system resources if possible
    void close() TIGHTDB_NOEXCEPT;
private:
    TIGHTDB_NORETURN static void init_failed(int);
    TIGHTDB_NORETURN static void attr_init_failed(int);
    TIGHTDB_NORETURN static void destroy_failed(int) TIGHTDB_NOEXCEPT;
    sem_t* get_semaphore();
    void handle_wait_error(int error);
    // non-zero if a shared part has been registered
    SharedPart* m_shared_part; 
    sem_t* m_sem; // non-zero if emulation is used
    pthread_cond_t* m_cond; // non-zero if process local
    bool is_process_shared() { return m_cond == 0; }
    static const char* m_name;
};




// Implementation:

inline Thread::Thread(): m_joinable(false)
{
}

template<class F> inline Thread::Thread(F func): m_joinable(true)
{
    UniquePtr<F> func2(new F(func)); // Throws
    start(&Thread::entry_point<F>, func2.get()); // Throws
    func2.release();
}

template<class F> inline void Thread::start(F func)
{
    if (m_joinable)
        std::terminate();
    UniquePtr<F> func2(new F(func)); // Throws
    start(&Thread::entry_point<F>, func2.get()); // Throws
    func2.release();
    m_joinable = true;
}

inline Thread::~Thread() TIGHTDB_NOEXCEPT
{
    if (m_joinable)
        std::terminate();
}

inline bool Thread::joinable() TIGHTDB_NOEXCEPT
{
    return m_joinable;
}

inline void Thread::start(entry_func_type entry_func, void* arg)
{
    const pthread_attr_t* attr = 0; // Use default thread attributes
    int r = pthread_create(&m_id, attr, entry_func, arg);
    if (TIGHTDB_UNLIKELY(r != 0))
        create_failed(r); // Throws
}

template<class F> inline void* Thread::entry_point(void* cookie) TIGHTDB_NOEXCEPT
{
    UniquePtr<F> func(static_cast<F*>(cookie));
    try {
        (*func)();
    }
    catch (...) {
        std::terminate();
    }
    return 0;
}


inline Mutex::Mutex()
{
    init_as_regular();
}

inline Mutex::Mutex(process_shared_tag)
{
    bool robust_if_available = false;
    init_as_process_shared(robust_if_available);
}

inline Mutex::~Mutex() TIGHTDB_NOEXCEPT
{
    int r = pthread_mutex_destroy(&m_impl);
    if (TIGHTDB_UNLIKELY(r != 0))
        destroy_failed(r);
}

inline void Mutex::init_as_regular()
{
    int r = pthread_mutex_init(&m_impl, 0);
    if (TIGHTDB_UNLIKELY(r != 0))
        init_failed(r);
}

inline void Mutex::lock() TIGHTDB_NOEXCEPT
{
    int r = pthread_mutex_lock(&m_impl);
    if (TIGHTDB_LIKELY(r == 0))
        return;
    lock_failed(r);
}

inline void Mutex::unlock() TIGHTDB_NOEXCEPT
{
    int r = pthread_mutex_unlock(&m_impl);
    TIGHTDB_ASSERT(r == 0);
    static_cast<void>(r);
}


inline LockGuard::LockGuard(Mutex& m) TIGHTDB_NOEXCEPT:
    m_mutex(m)
{
    m_mutex.lock();
}

inline LockGuard::~LockGuard() TIGHTDB_NOEXCEPT
{
    m_mutex.unlock();
}


inline UniqueLock::UniqueLock(Mutex& m) TIGHTDB_NOEXCEPT:
    m_mutex(&m)
{
    m_mutex->lock();
    m_is_locked = true;
}

inline UniqueLock::UniqueLock(Mutex& m, defer_lock_tag) TIGHTDB_NOEXCEPT:
    m_mutex(&m)
{
    m_is_locked = false;
}

inline UniqueLock::~UniqueLock() TIGHTDB_NOEXCEPT
{
    if (m_is_locked)
        m_mutex->unlock();
}

inline void UniqueLock::lock() TIGHTDB_NOEXCEPT
{
    m_mutex->lock();
    m_is_locked = true;
}

inline void UniqueLock::unlock() TIGHTDB_NOEXCEPT
{
    m_mutex->unlock();
    m_is_locked = false;
}

template<typename TFunc>
inline RobustLockGuard::RobustLockGuard(RobustMutex& m, TFunc func) :
    m_mutex(m)
{
    m_mutex.lock(func);
}

inline RobustLockGuard::~RobustLockGuard() TIGHTDB_NOEXCEPT
{
    m_mutex.unlock();
}



inline RobustMutex::RobustMutex():
    Mutex(no_init_tag())
{
    bool robust_if_available = true;
    init_as_process_shared(robust_if_available);
}

inline RobustMutex::~RobustMutex() TIGHTDB_NOEXCEPT
{
}

template<class Func> inline void RobustMutex::lock(Func recover_func)
{
    bool no_thread_has_died = low_level_lock(); // Throws
    if (TIGHTDB_LIKELY(no_thread_has_died))
        return;
    try {
        recover_func(); // Throws
        mark_as_consistent();
        // If we get this far, the protected memory has been
        // brought back into a consistent state, and the mutex has
        // been notified aboit this. This means that we can safely
        // enter the applications critical section.
    }
    catch (...) {
        // Unlocking without first calling mark_as_consistent()
        // means that the mutex enters the "not recoverable"
        // state, which will cause all future attempts at locking
        // to fail.
        unlock();
        throw;
    }
}

inline void RobustMutex::unlock() TIGHTDB_NOEXCEPT
{
    Mutex::unlock();
}


inline CondVar::CondVar()
{
    m_sem = 0;
    m_shared_part = 0;
    m_cond = new pthread_cond_t;
    int r = pthread_cond_init(m_cond, 0);
    if (TIGHTDB_UNLIKELY(r != 0))
        init_failed(r);
}

inline void CondVar::close() TIGHTDB_NOEXCEPT
{
    if (m_sem) { // true if emulating a process shared condvar
        sem_close(m_sem);
        m_sem = 0;
        return; // we don't need to clean up the SharedPart
    }
    if (m_cond) {  // == process local, we own the condition variable
        int r = pthread_cond_destroy(m_cond);
        if (TIGHTDB_UNLIKELY(r != 0))
            destroy_failed(r);
        delete m_cond;
        m_cond = 0;
    }
    // we don't do anything to the shared part, other CondVars may shared it
    m_shared_part = 0;
}


inline CondVar::~CondVar() TIGHTDB_NOEXCEPT
{
    close();
}



inline void CondVar::set_shared_part(SharedPart& shared_part, dev_t device, ino_t inode, std::size_t offset_of_condvar)
{
    TIGHTDB_ASSERT(m_shared_part == 0);
    TIGHTDB_ASSERT(is_process_shared());
    close();
    m_shared_part = &shared_part;
    static_cast<void>(device);
    static_cast<void>(inode);
    static_cast<void>(offset_of_condvar);
#ifdef TIGHTDB_CONDVAR_EMULATION
    m_sem = get_semaphore();
#endif
}

inline sem_t* CondVar::get_semaphore()
{
    TIGHTDB_ASSERT(m_name);
    TIGHTDB_ASSERT(m_shared_part);
    if (m_sem == 0) {
        m_sem = sem_open(m_name, O_CREAT, S_IRWXG | S_IRWXU, 0);
        // FIXME: error checking
    }
    return m_sem;
}

inline void CondVar::wait(LockGuard& l) TIGHTDB_NOEXCEPT
{
    pthread_cond_t* cond = m_cond;
#ifdef TIGHTDB_CONDVAR_EMULATION
    if (m_sem) {
        TIGHTDB_ASSERT(m_shared_part);
        m_shared_part->waiters++;
        uint64_t my_counter = m_shared_part->signal_counter;
        l.m_mutex.unlock();
        for (;;) {
            // FIXME: handle premature return due to signal
            sem_wait(m_sem);
            l.m_mutex.lock();
            if (m_shared_part->signal_counter != my_counter)
                break;
            sem_post(m_sem);
            sched_yield();
            l.m_mutex.unlock();
        }
        return;
    }
#else
    if (m_shared_part)
        cond = &m_shared_part->m_impl;
#endif
    // no emulation (whether local or process shared, same codepath)
    int r = pthread_cond_wait(cond, &l.m_mutex.m_impl);
    if (TIGHTDB_UNLIKELY(r != 0))
        TIGHTDB_TERMINATE("pthread_cond_wait() failed");
}







template<class Func>
inline void CondVar::wait(RobustMutex& m, Func recover_func, const struct timespec* tp)
{
    pthread_cond_t* cond = m_cond;
#ifdef TIGHTDB_CONDVAR_EMULATION
    if (m_sem) {
        TIGHTDB_ASSERT(m_shared_part);
        TIGHTDB_ASSERT(tp == 0);
        m_shared_part->waiters++;
        uint64_t my_counter = m_shared_part->signal_counter;
        m.unlock();
        for (;;) {
            // FIXME: handle premature return due to signal
            sem_wait(m_sem);
            m.lock(recover_func);
            if (m_shared_part->signal_counter != my_counter)
                break;
            sem_post(m_sem);
            sched_yield();
            m.unlock();
        }
        return;
    }
#else
    if (m_shared_part)
        cond = &m_shared_part->m_impl;
    static_cast<void>(m);
#endif
    int r;
    if (!tp) {
        r = pthread_cond_wait(cond, &m.m_impl);
    }
    else {
        r = pthread_cond_timedwait(cond, &m.m_impl, tp);
        if (r == ETIMEDOUT)
            return;
    }
    if (TIGHTDB_LIKELY(r == 0))
        return;
    handle_wait_error(r);
    try {
        recover_func(); // Throws
        m.mark_as_consistent();
        // If we get this far, the protected memory has been
        // brought back into a consistent state, and the mutex has
        // been notified aboit this. This means that we can safely
        // enter the applications critical section.
    }
    catch (...) {
        // Unlocking without first calling mark_as_consistent()
        // means that the mutex enters the "not recoverable"
        // state, which will cause all future attempts at locking
        // to fail.
        m.unlock();
        throw;
    }
}






inline void CondVar::notify() TIGHTDB_NOEXCEPT
{
    pthread_cond_t* cond = m_cond;
#ifdef TIGHTDB_CONDVAR_EMULATION
    if (m_sem) {
        TIGHTDB_ASSERT(m_shared_part);
        m_shared_part->signal_counter++;
        if (m_shared_part->waiters) {
            sem_post(m_sem);
            --m_shared_part->waiters;
        }
        return;
    }
#else
    if (m_shared_part)
        cond = &m_shared_part->m_impl;    
#endif
    int r = pthread_cond_signal(cond);
    TIGHTDB_ASSERT(r == 0);
    static_cast<void>(r);
}





inline void CondVar::notify_all() TIGHTDB_NOEXCEPT
{
    pthread_cond_t* cond = m_cond;
#ifdef TIGHTDB_CONDVAR_EMULATION
    if (m_sem) {
        TIGHTDB_ASSERT(m_shared_part);
        m_shared_part->signal_counter++;
        while (m_shared_part->waiters) {
            sem_post(m_sem);
            --m_shared_part->waiters;
        }
        return;
    }
#else
    if (m_shared_part)
        cond = &m_shared_part->m_impl;    
#endif
    int r = pthread_cond_broadcast(cond);
    TIGHTDB_ASSERT(r == 0);
    static_cast<void>(r);
}


// Support for simple atomic variables, inspired by C++11 atomics, but incomplete.
//
// The level of support provided is driven by the need of the tightdb library.
// It is not meant to provide full support for atomics, but it is meant to be
// the place where we put low level code related to atomic variables.
//
// Useful for non-blocking data structures.
//
// These primitives ensure that memory appears consistent around load/store
// of the variables, and ensures that the compiler will not optimize away
// relevant instructions.
//
// This template can only be used for types for which the underlying hardware
// guarantees atomic reads and writes. On almost any machine in production,
// this includes all types with the size of a machine word (or machine register) 
// or less, except bit fields.
// 
// FIXME: This leaves it to the user of the software to ascertain that the
// hardware lives up to the requirement. The long term goal should be to provide
// atomicity in a way which will cause compilation to fail if the underlying
// platform is not guaranteed to support the requirements given by the use of
// the primitives.
//
// FIXME: The current implementation provides the functionality required for the
// tightdb library, but *not* all the functionality often provided by atomics.
// (see C++11 atomics for an example). We'll add additional functionality as
// the need arises.
//
// Usage: For non blocking data structures, you need to wrap any synchronization
// variables using the Atomic template. Variables which are not used for
// synchronization need no special declaration. As long as signaling between threads
// is done using the store and load methods declared here, memory barriers will
// ensure a consistent view of the other variables.
//
// Prior to gcc 4.7 there was no portable ways of providing acquire/release semantics,
// so for earlier versions we fall back to sequential consistency.
// As some architectures, most notably x86, provide release and acquire semantics
// in hardware, this is somewhat annoying, because we will use a full memory barrier
// where no-one is needed.
//
// FIXME: introduce x86 specific optimization to avoid the memory
// barrier!
template<class T>
class Atomic
{
public:
    inline Atomic()
    {
        state = 0;
    }

    inline Atomic(T init_value)
    {
        state = init_value;
    }

    T load() const;
    T load_acquire() const;
    T load_relaxed() const;
    T fetch_sub_relaxed(T v);
    T fetch_sub_release(T v);
    T fetch_add_release(T v);
    T fetch_add_acquire(T v);
    T fetch_sub_acquire(T v);
    void store(T value);
    void store_release(T value);
    void store_relaxed(T value);
    bool compare_and_swap(T& oldvalue, T newvalue);
    T exchange_acquire(T newvalue);
private:
    // the following is not supported
    Atomic(Atomic<T>&);
    Atomic<T>& operator=(const Atomic<T>&);

    // Assumed to be naturally aligned - if not, hardware might not guarantee atomicity
#ifdef TIGHTDB_HAVE_CXX11_ATOMIC
    std::atomic<T> state;
#elif defined(_MSC_VER)
    volatile T state;
#elif defined(__GNUC__)
    T state;
#else
#error "Atomic is not support on this compiler"
#endif
};


#ifdef TIGHTDB_HAVE_CXX11_ATOMIC
template<typename T>
inline T Atomic<T>::load() const
{
    return state.load();
}

template<typename T>
inline T Atomic<T>::load_acquire() const
{
    return state.load(std::memory_order_acquire);
}

template<typename T>
inline T Atomic<T>::load_relaxed() const
{
    return state.load(std::memory_order_relaxed);
}

template<typename T>
inline T Atomic<T>::fetch_sub_relaxed(T v)
{
    return state.fetch_sub(v, std::memory_order_relaxed);
}

template<typename T>
inline T Atomic<T>::fetch_sub_release(T v)
{
    return state.fetch_sub(v, std::memory_order_release);
}

template<typename T>
inline T Atomic<T>::fetch_add_release(T v)
{
    return state.fetch_add(v, std::memory_order_release);
}

template<typename T>
inline T Atomic<T>::fetch_add_acquire(T v)
{
    return state.fetch_add(v, std::memory_order_acquire);
}

template<typename T>
inline T Atomic<T>::fetch_sub_acquire(T v)
{
    return state.fetch_sub(v, std::memory_order_acquire);
}

template<typename T>
inline void Atomic<T>::store(T value)
{
    state.store(value);
}

template<typename T>
inline void Atomic<T>::store_release(T value)
{
    state.store(value, std::memory_order_release);
}

template<typename T>
inline void Atomic<T>::store_relaxed(T value)
{
    state.store(value, std::memory_order_relaxed);
}

template<typename T>
inline bool Atomic<T>::compare_and_swap(T& oldvalue, T newvalue)
{
    return state.compare_exchange_weak(oldvalue, newvalue);
}

template<typename T>
inline T Atomic<T>::exchange_acquire(T value)
{
    return state.exchange(value, std::memory_order_acquire);
}

#elif defined(_MSC_VER)

template<typename T>
inline T Atomic<T>::load() const
{
    return state;
}

template<typename T>
inline T Atomic<T>::load_relaxed() const
{
    return state;
}

template<typename T>
inline T Atomic<T>::load_acquire() const
{
    return state;
}

template<typename T>
inline void Atomic<T>::store(T value)
{
    state = value;
}

template<typename T>
inline void Atomic<T>::store_relaxed(T value)
{
    state = value;

}

template<typename T>
inline void Atomic<T>::store_release(T value)
{
    state = value;
}

#elif TIGHTDB_HAVE_AT_LEAST_GCC(4, 7) || TIGHTDB_HAVE_CLANG_FEATURE(c_atomic)
// Modern non-C++11 gcc/clang implementaion

template<typename T>
inline T Atomic<T>::load_acquire() const
{
    return __atomic_load_n(&state, __ATOMIC_ACQUIRE);
}

template<typename T>
inline T Atomic<T>::load_relaxed() const
{
    return __atomic_load_n(&state, __ATOMIC_RELAXED);
}

template<typename T>
inline T Atomic<T>::load() const
{
    return __atomic_load_n(&state, __ATOMIC_SEQ_CST);
}

template<typename T>
inline T Atomic<T>::fetch_sub_relaxed(T v)
{
    return __atomic_fetch_sub(&state, v, __ATOMIC_RELAXED);
}

template<typename T>
inline T Atomic<T>::fetch_sub_release(T v)
{
    return __atomic_fetch_sub(&state, v, __ATOMIC_RELEASE);
}

template<typename T>
inline T Atomic<T>::fetch_add_release(T v)
{
    return __atomic_fetch_add(&state, v, __ATOMIC_RELEASE);
}

template<typename T>
inline T Atomic<T>::fetch_add_acquire(T v)
{
    return __atomic_fetch_add(&state, v, __ATOMIC_ACQUIRE);
}

template<typename T>
inline T Atomic<T>::fetch_sub_acquire(T v)
{
    return __atomic_fetch_sub(&state, v, __ATOMIC_ACQUIRE);
}

template<typename T>
inline void Atomic<T>::store(T value)
{
    __atomic_store_n(&state, value, __ATOMIC_SEQ_CST);
}

template<typename T>
inline void Atomic<T>::store_release(T value)
{
    __atomic_store_n(&state, value, __ATOMIC_RELEASE);
}

template<typename T>
inline void Atomic<T>::store_relaxed(T value)
{
    __atomic_store_n(&state, value, __ATOMIC_RELAXED);
}

template<typename T>
inline bool Atomic<T>::compare_and_swap(T& oldvalue, T newvalue)
{
    return __atomic_compare_exchange_n(&state, &oldvalue, newvalue, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

template<typename T>
inline T Atomic<T>::exchange_acquire(T newvalue)
{
    T old;
    __atomic_exchange(&state, &newvalue, &old, __ATOMIC_ACQUIRE);
    return old;
}

#elif defined(__GNUC__)
// Legacy GCC implementation
template<typename T>
inline T Atomic<T>::load_acquire() const
{
    __sync_synchronize();
    return load_relaxed();
}

template<typename T>
inline T Atomic<T>::load_relaxed() const
{
    T retval;
    if (sizeof(T) >= sizeof(ptrdiff_t)) {
        // do repeated reads until we've seen the same value twice,
        // then we know that the reads were done without changes to the value.
        // under normal circumstances, the loop is never executed
        retval = state;
        asm volatile ("" : : : "memory");
        T val = state;
        while (retval != val) {
            asm volatile ("" : : : "memory");
            val = retval;
            retval = state;
        }
    } else {
        asm volatile ("" : : : "memory");
        retval = state;
    }
    return retval;
}

template<typename T>
inline T Atomic<T>::load() const
{
    __sync_synchronize();
    return load_relaxed();
}

template<typename T>
inline T Atomic<T>::fetch_sub_relaxed(T v)
{
    return __sync_fetch_and_sub(&state, v);
}

template<typename T>
inline T Atomic<T>::fetch_sub_release(T v)
{
    return __sync_fetch_and_sub(&state, v);
}

template<typename T>
inline T Atomic<T>::fetch_add_release(T v)
{
    return __sync_fetch_and_add(&state, v);
}

template<typename T>
inline T Atomic<T>::fetch_add_acquire(T v)
{
    return __sync_fetch_and_add(&state, v);
}

template<typename T>
inline T Atomic<T>::fetch_sub_acquire(T v)
{
    return __sync_fetch_and_sub(&state, v);
}


template<typename T>
inline void Atomic<T>::store(T value)
{
    if (sizeof(T) >= sizeof(ptrdiff_t)) {
        T old_value = state;
        // Ensure atomic store for type larger than largest native word.
        // normally, this loop will not be entered.
        while ( ! __sync_bool_compare_and_swap(&state, old_value, value)) {
            old_value = state;
        };
    } else {
        __sync_synchronize();
        state = value;
    }
    // prevent registerization of state (this is not really needed, I think)
    asm volatile ("" : : : "memory");
}

template<typename T>
inline void Atomic<T>::store_release(T value)
{
    // prior to gcc 4.7 we have no portable way of expressing
    // release semantics, so we do seq_consistent store instead
    store(value);
}

template<typename T>
inline void Atomic<T>::store_relaxed(T value)
{
    // prior to gcc 4.7 we have no portable way of expressing
    // relaxed semantics, so we do seq_consistent store instead
    // FIXME: we did! ordinary stores (with atomicity..)
    store(value);
}

template<typename T>
inline bool Atomic<T>::compare_and_swap(T& oldvalue, T newvalue)
{
    T ov = oldvalue;
    oldvalue = __sync_val_compare_and_swap(&state, oldvalue, newvalue);
    return (ov == oldvalue);
}

template<typename T>
inline T Atomic<T>::exchange_acquire(T newvalue)
{
    return __sync_lock_test_and_set(&state, newvalue);
}
#endif

} // namespace util
} // namespace tightdb

#endif // TIGHTDB_UTIL_THREAD_HPP
