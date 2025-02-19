/*  $Id$
 * ===========================================================================
 *
 *                            PUBLIC DOMAIN NOTICE
 *               National Center for Biotechnology Information
 *
 *  This software/database is a "United States Government Work" under the
 *  terms of the United States Copyright Act.  It was written as part of
 *  the author's official duties as a United States Government employee and
 *  thus cannot be copyrighted.  This software/database is freely available
 *  to the public for use. The National Library of Medicine and the U.S.
 *  Government have not placed any restriction on its use or reproduction.
 *
 *  Although all reasonable efforts have been taken to ensure the accuracy
 *  and reliability of the software and data, the NLM and the U.S.
 *  Government do not and cannot warrant the performance or results that
 *  may be obtained by using this software or data. The NLM and the U.S.
 *  Government disclaim all warranties, express or implied, including
 *  warranties of performance, merchantability or fitness for any particular
 *  purpose.
 *
 *  Please cite the author in any work or product based on this material.
 *
 * ===========================================================================
 *
 * Authors:  Denis Vakatov, Aleksey Grichenko, Eugene Vasilchenko
 *
 * File Description:
 *   Multi-threading -- fast mutexes
 *
 *   MUTEX:
 *      CInternalMutex   -- platform-dependent mutex functionality
 *
 */

#include <ncbi_pch.hpp>
#include <corelib/ncbimtx.hpp>
#include <corelib/ncbi_limits.h>
#include <corelib/obj_pool.hpp>
#include "ncbidbg_p.hpp"
#include <stdio.h>
#include <algorithm>
#include <chrono>

//#define LOG_MUTEX_EVENTS

#ifdef NCBI_POSIX_THREADS
#  include <sys/time.h> // for gettimeofday()
#  include <sched.h>    // for sched_yield()
#endif

#if defined(_DEBUG) &&  defined(LOG_MUTEX_EVENTS)

#include <corelib/ncbifile.hpp>
#include <fcntl.h>
#  if defined(NCBI_OS_MSWIN)
#    include <io.h>
#  else
#    include <unistd.h>
#  endif

#endif

#include <corelib/error_codes.hpp>

#define STACK_THRESHOLD (1024)

#define NCBI_USE_ERRCODE_X  Corelib_Mutex


BEGIN_NCBI_SCOPE


/////////////////////////////////////////////////////////////////////////////
//
// Log mutex events if LOG_MUTEX_EVENTS is defined.
//
//    The mutex events (create/destroy/lock/unlock) are logged into
//    ./mutex_events.log or the one specified in MUTEX_EVENTS_LOG_FILE
//    env. variable.
//


#if defined(_DEBUG) &&  defined(LOG_MUTEX_EVENTS)

// Logging function, prints pointer to the mutex, system thread ID
// and the message.
static void s_WriteMutexEvent(void* mutex_ptr, const char* message);
#  define WRITE_MUTEX_EVENT(mutex, message) s_WriteMutexEvent(mutex, message)

#else

#  define WRITE_MUTEX_EVENT(mutex, message) ((void)0)

#endif



#if defined(_DEBUG) &&  defined(LOG_MUTEX_EVENTS)

void s_WriteMutexEvent(void* mutex_ptr, const char* message)
{
    static const int mode = O_WRONLY | O_APPEND | O_CREAT;
    static const mode_t perm = CDirEntry::MakeModeT(
        CDirEntry::fRead | CDirEntry::fWrite,
        CDirEntry::fRead | CDirEntry::fWrite,
        CDirEntry::fRead | CDirEntry::fWrite,
        0);
    static const char* file_name = getenv("MUTEX_EVENTS_LOG_FILE"); /*This can
    lead to multiple opens of a file in multithreaded mode, but left so by
    intention, because:
    1) with C++11-compatible compiler problem will be gone,
       while any modification makes it worse adapted to C++11;
    2) You cannot use mutexes in the function that is called by any mutex
       create/delete/lock/unlock;
    3) File is opened without O_TRUNC, so any write()'s to file will be saved
       even if write()'s are made with different handles
    4) This is a debug-only function.                                       */
    static int handle = open(file_name ? file_name : "mutex_events.log",
        mode, perm);
    CNcbiOstrstream str_os;
    str_os << mutex_ptr << " "
        << GetCurrentThreadSystemID() << " "
        << message << "\n";
    write(handle, str_os.str(), str_os.pcount());
    str_os.rdbuf()->freeze(false);
}

#endif


/////////////////////////////////////////////////////////////////////////////
//  CInternalMutex::
//

void SSystemFastMutex::InitializeHandle(void)
{
    WRITE_MUTEX_EVENT(this, "SSystemFastMutex::InitializeHandle()");

    // Create platform-dependent mutex handle
#if defined(NCBI_WIN32_THREADS)
#  if defined(NCBI_FASTMUTEX_USE_NEW)
    InitializeSRWLock(&m_Handle);
#  elif defined(NCBI_USE_CRITICAL_SECTION)
    InitializeCriticalSection(&m_Handle);
#  else
    xncbi_ValidateAndErrnoReport(
        (m_Handle = CreateMutex(NULL, FALSE, NULL)) != NULL,
        "Mutex creation failed");
#  endif
#elif defined(NCBI_POSIX_THREADS)
#  if defined(NCBI_OS_CYGWIN)
    if (pthread_mutex_init(&m_Handle, 0) != 0) {
        // On Cygwin, there was an attempt to check the mutex state,
        // which in some cases (uninitialized memory) could cause
        // a fake EBUSY error. This bug seems to have been fixed
        // (just looked at the source code; never tested) in the early 2006.
        memset(&m_Handle, 0, sizeof(m_Handle));
#  endif  /* NCBI_OS_CYGWIN */
        xncbi_ValidatePthread(pthread_mutex_init(&m_Handle, 0), 0,
                              "Mutex creation failed");
#  if defined(NCBI_OS_CYGWIN)
    }
#  endif  /* NCBI_OS_CYGWIN */
#endif
}

void SSystemFastMutex::DestroyHandle(void)
{
    WRITE_MUTEX_EVENT(this, "SSystemFastMutex::DestroyHandle()");

    // Destroy system mutex handle
#if defined(NCBI_WIN32_THREADS)
#  if defined(NCBI_FASTMUTEX_USE_NEW)
// noop
#  elif defined(NCBI_USE_CRITICAL_SECTION)
    DeleteCriticalSection(&m_Handle);
#  else
    xncbi_VerifyAndErrorReport(CloseHandle(m_Handle) != 0);
#  endif
#elif defined(NCBI_POSIX_THREADS)
    xncbi_VerifyAndErrorReport(pthread_mutex_destroy(&m_Handle) == 0);
#endif
}

void SSystemFastMutex::InitializeStatic(void)
{
#if !defined(NCBI_NO_THREADS)
    switch ( m_Magic ) {
    case eMutexUninitialized: // ok
        break;
    case eMutexInitialized:
        xncbi_Validate(0, "Double initialization of mutex");
        break;
    default:
        xncbi_Validate(0, "SSystemFastMutex::m_Magic contains invalid value");
        break;
    }

    InitializeHandle();
#endif

    m_Magic = eMutexInitialized;
}


void SSystemFastMutex::InitializeDynamic(void)
{
#if !defined(NCBI_NO_THREADS)
    InitializeHandle();
#endif

    m_Magic = eMutexInitialized;
}


void SSystemFastMutex::Destroy(void)
{
#if !defined(NCBI_NO_THREADS)
    xncbi_Validate(IsInitialized(), "Destruction of uninitialized mutex");
#endif

    m_Magic = eMutexUninitialized;

    DestroyHandle();
}

void SSystemFastMutex::ThrowUninitialized(void)
{
    NCBI_THROW(CMutexException, eUninitialized, "Mutex uninitialized");
}

void SSystemFastMutex::ThrowLockFailed(void)
{
    NCBI_THROW(CMutexException, eLock, "Mutex lock failed");
}

void SSystemFastMutex::ThrowUnlockFailed(void)
{
    NCBI_THROW(CMutexException, eUnlock, "Mutex unlock failed");
}

void SSystemFastMutex::ThrowTryLockFailed(void)
{
    NCBI_THROW(CMutexException, eTryLock,
               "Mutex check (TryLock) failed");
}

void SSystemMutex::Destroy(void)
{
    xncbi_Validate(m_Count == 0, "Destruction of locked mutex");
    m_Mutex.Destroy();
}

#if !defined(NCBI_NO_THREADS)
void SSystemFastMutex::Lock(ELockSemantics lock /*= eNormal*/)
{
    WRITE_MUTEX_EVENT(this, "SSystemFastMutex::Lock()");

    // check
    CheckInitialized();
    if (lock != eNormal) {
        return;
    }

    // Acquire system mutex
#  if defined(NCBI_WIN32_THREADS)
#    if defined(NCBI_FASTMUTEX_USE_NEW)
    AcquireSRWLockExclusive(&m_Handle);
#    elif defined(NCBI_USE_CRITICAL_SECTION)
    EnterCriticalSection(&m_Handle);
#    else
    if (WaitForSingleObject(m_Handle, INFINITE) != WAIT_OBJECT_0) {
        ThrowLockFailed();
    }
#    endif
#  elif defined(NCBI_POSIX_THREADS)
    if ( pthread_mutex_lock(&m_Handle) != 0 ) { // error
        ThrowLockFailed();
    }
#  endif
}

bool SSystemFastMutex::TryLock(void)
{
    WRITE_MUTEX_EVENT(this, "SSystemFastMutex::TryLock()");

    // check
    CheckInitialized();

    // Check if the system mutex is acquired.
    // If not, acquire for the current thread.
#  if defined(NCBI_WIN32_THREADS)
#    if defined(NCBI_FASTMUTEX_USE_NEW)
    return TryAcquireSRWLockExclusive(&m_Handle) != 0;
#    elif defined(NCBI_USE_CRITICAL_SECTION)
    return TryEnterCriticalSection(&m_Handle) != 0;
#    else
    DWORD status = WaitForSingleObject(m_Handle, 0);
    if (status == WAIT_OBJECT_0) { // ok
        return true;
    }
    else {
        if (status != WAIT_TIMEOUT) { // error
            ThrowTryLockFailed();
        }
        return false;
    }
#    endif
#  elif defined(NCBI_POSIX_THREADS)
    int status = pthread_mutex_trylock(&m_Handle);
    if (status == 0) { // ok
        return true;
    }
    else {
        if (status != EBUSY) { // error
            ThrowTryLockFailed();
        }
        return false;
    }
#  endif
}

void SSystemFastMutex::Unlock(ELockSemantics lock /*= eNormal*/)
{
    WRITE_MUTEX_EVENT(this, "SSystemFastMutex::Unlock()");

    // check
    CheckInitialized();
    if (lock != eNormal) {
        return;
    }

    // Release system mutex
# if defined(NCBI_WIN32_THREADS)
#    if defined(NCBI_FASTMUTEX_USE_NEW)
    ReleaseSRWLockExclusive(&m_Handle);
#    elif defined(NCBI_USE_CRITICAL_SECTION)
    LeaveCriticalSection(&m_Handle);
#    else
    if ( !ReleaseMutex(m_Handle) ) { // error
        ThrowUnlockFailed();
    }
#    endif
# elif defined(NCBI_POSIX_THREADS)
    if ( pthread_mutex_unlock(&m_Handle) != 0 ) { // error
        ThrowUnlockFailed();
    }
# endif
}

void SSystemMutex::Lock(SSystemFastMutex::ELockSemantics lock)
{
    m_Mutex.CheckInitialized();

    TThreadSystemID owner = GetCurrentThreadSystemID();
    auto count = m_Count.load(memory_order_acquire);
    if ( count > 0 && m_Owner == owner ) {
        // Don't lock twice, just increase the counter
        m_Count.store(count+1, memory_order_release);
        return;
    }

    // Lock the mutex and remember the owner
    m_Mutex.Lock(lock);
    assert(m_Count == 0);
    m_Owner = owner;
    m_Count.store(1, memory_order_release);
}

bool SSystemMutex::TryLock(void)
{
    m_Mutex.CheckInitialized();

    TThreadSystemID owner = GetCurrentThreadSystemID();
    auto count = m_Count.load(memory_order_acquire);
    if ( count > 0 && m_Owner == owner ) {
        // Don't lock twice, just increase the counter
        m_Count.store(count+1, memory_order_release);
        return true;
    }

    // If TryLock is successful, remember the owner
    if ( m_Mutex.TryLock() ) {
        assert(m_Count == 0);
        m_Owner = owner;
        m_Count.store(1, memory_order_release);
        return true;
    }

    // Cannot lock right now
    return false;
}

void SSystemMutex::Unlock(SSystemFastMutex::ELockSemantics lock)
{
    m_Mutex.CheckInitialized();

    // No unlocks by threads other than owner.
    // This includes no unlocks of unlocked mutex.
    TThreadSystemID owner = GetCurrentThreadSystemID();
    auto count = m_Count.load(memory_order_acquire);
    if ( count == 0 || m_Owner != owner ) {
        ThrowNotOwned();
    }

    // No real unlocks if counter > 1, just decrease it
    m_Count.store(--count, memory_order_release);
    if ( count > 0 ) {
        return;
    }

    // This was the last lock - clear the owner and unlock the mutex
    m_Mutex.Unlock(lock);
}
#endif

void SSystemMutex::ThrowNotOwned(void)
{
    NCBI_THROW(CMutexException, eOwner,
               "Mutex is not owned by current thread");
}


#if defined(NEED_AUTO_INITIALIZE_MUTEX)
//#define USE_STATIC_INIT_MUTEX_HANDLE 1

static const char* kInitMutexName = "NCBI_CAutoInitializeStaticMutex";

#ifdef USE_STATIC_INIT_MUTEX_HANDLE

static volatile TSystemMutex s_InitMutexHandle = 0;
static TSystemMutex s_GetInitMutexHandle(void)
{
    TSystemMutex init_mutex = s_InitMutexHandle;
    if ( !init_mutex ) {
        init_mutex = CreateMutex(NULL, FALSE, kInitMutexName);
        xncbi_VerifyAndErrorReport(init_mutex);
        assert(!s_InitMutexHandle || s_InitMutexHandle == init_mutex);
        s_InitMutexHandle = init_mutex;
    }
    return init_mutex;
}

static inline void s_ReleaseInitMutexHandle(TSystemMutex _DEBUG_ARG(mutex))
{
    assert(mutex == s_InitMutexHandle);
}

#else

static inline HANDLE s_GetInitMutexHandle(void)
{
    HANDLE init_mutex = CreateMutexA(NULL, FALSE, kInitMutexName);
    xncbi_VerifyAndErrorReport(init_mutex);
    return init_mutex;
}

static inline void s_ReleaseInitMutexHandle(HANDLE mutex)
{
    CloseHandle(mutex);
}

#endif

void CAutoInitializeStaticFastMutex::Initialize(void)
{
    if ( m_Mutex.IsInitialized() ) {
        return;
    }
    HANDLE init_mutex = s_GetInitMutexHandle();
    xncbi_VerifyAndErrorReport(
        WaitForSingleObject(init_mutex, INFINITE) == WAIT_OBJECT_0);
    if ( !m_Mutex.IsInitialized() ) {
        m_Mutex.InitializeStatic();
    }
    xncbi_VerifyAndErrorReport(ReleaseMutex(init_mutex));
    assert(m_Mutex.IsInitialized());
    s_ReleaseInitMutexHandle(init_mutex);
}

void CAutoInitializeStaticMutex::Initialize(void)
{
    if ( m_Mutex.IsInitialized() ) {
        return;
    }
    HANDLE init_mutex = s_GetInitMutexHandle();
    xncbi_VerifyAndErrorReport(
        WaitForSingleObject(init_mutex, INFINITE) == WAIT_OBJECT_0);
    if ( !m_Mutex.IsInitialized() ) {
        m_Mutex.InitializeStatic();
    }
    xncbi_VerifyAndErrorReport(ReleaseMutex(init_mutex));
    assert(m_Mutex.IsInitialized());
    s_ReleaseInitMutexHandle(init_mutex);
}

#endif

const char* CMutexException::GetErrCodeString(void) const
{
    switch (GetErrCode()) {
    case eLock:    return "eLock";
    case eUnlock:  return "eUnlock";
    case eTryLock: return "eTryLock";
    case eOwner:   return "eOwner";
    case eUninitialized:  return "eUninitialized";
    default:       return CException::GetErrCodeString();
    }
}

/////////////////////////////////////////////////////////////////////////////
//  CInternalRWLock::
//

#if defined(NCBI_WIN32_THREADS) && !NCBI_SRWLOCK_USE_NEW

class CWindowsHandle
{
public:
    CWindowsHandle(HANDLE h = NULL) : m_Handle(h) {}
    CWindowsHandle(HANDLE h, const char* errorMessage) : m_Handle(h)
    {
        xncbi_Validate(h != NULL, errorMessage);
    }
    ~CWindowsHandle(void) { Close(); }

    void Close(void)
    {
        if ( m_Handle != NULL ) {
            CloseHandle(m_Handle);
            m_Handle = NULL;
        }
    }

    void Set(HANDLE h)
    {
        Close();
        m_Handle = h;
    }
    void Set(HANDLE h, const char* errorMessage)
    {
        xncbi_Validate(h != NULL, errorMessage);
        Set(h);
    }

    HANDLE GetHandle(void) const  { return m_Handle; }
    operator HANDLE(void) const { return m_Handle; }

protected:
    HANDLE m_Handle;

private:
    CWindowsHandle(const CWindowsHandle& h);
    CWindowsHandle& operator=(const CWindowsHandle& h);
};

class CWindowsSemaphore : public CWindowsHandle
{
public:
    CWindowsSemaphore(LONG initialCount = 0, LONG maximumCount = INFINITE)
        : CWindowsHandle(CreateSemaphore(NULL,
                                         initialCount, maximumCount,
                                         NULL),
                         "CreateSemaphore() failed")
    {
    }

    LONG Release(LONG add = 1)
    {
        LONG prev_sema;
        xncbi_ValidateAndErrnoReport(ReleaseSemaphore(*this, add, &prev_sema),
                                     "CWindowsSemaphore::Release() failed");
        return prev_sema;
    }
};

#endif

#if defined(NCBI_POSIX_THREADS)

class CPthreadCond
{
public:
    CPthreadCond(void)
        : m_Initialized(pthread_cond_init(&m_Handle, 0) != 0)
    {
    }
    ~CPthreadCond(void)
    {
        if ( m_Initialized ) {
            pthread_cond_destroy(&m_Handle);
        }
    }

    operator pthread_cond_t*(void) { return &m_Handle; }
    operator pthread_cond_t&(void) { return m_Handle; }

protected:
    pthread_cond_t  m_Handle;
    bool            m_Initialized;
};

#endif

#if !NCBI_SRWLOCK_USE_NEW
class CInternalRWLock
{
public:
    CInternalRWLock(void);

    // Platform-dependent RW-lock data
#if defined(NCBI_WIN32_THREADS)
    CWindowsSemaphore   m_Rsema;  // down when locked for writing
    CWindowsSemaphore   m_Rsema2; // down when writers are waiting
    CWindowsSemaphore   m_Wsema;  // down when locked for reading OR writing
#  if defined(NCBI_USE_CRITICAL_SECTION)
    CWindowsHandle      m_Mutex;
#  else
    CFastMutex          m_Mutex;
#  endif
#elif defined(NCBI_POSIX_THREADS)
    CPthreadCond        m_Rcond;
    CPthreadCond        m_Wcond;
    CFastMutex          m_Mutex;
#endif
};

inline
CInternalRWLock::CInternalRWLock(void)
#if defined(NCBI_WIN32_THREADS)
    : m_Rsema(1, 1), m_Rsema2(1, 1), m_Wsema(1, 1)
#endif
{
#if defined(NCBI_USE_CRITICAL_SECTION)
    m_Mutex.Set(CreateMutex(NULL, FALSE, NULL));
#endif
}
#endif
/////////////////////////////////////////////////////////////////////////////
//  CRWLock::
//

#if NCBI_SRWLOCK_USE_NEW
CRWLock::CRWLock(TFlags flags)
    : m_Owner(0), m_Count(0), m_WaitingWriters(0), m_TrackReaders(false)
{
#if defined(_DEBUG)
    m_TrackReaders = true;
    m_FavorWriters = (flags & fFavorWriters) != 0;
#else
    m_TrackReaders = m_FavorWriters = (flags & fFavorWriters) != 0;
#endif
    if (m_TrackReaders) {
        m_Readers.reserve(16);
    }
}
#else
CRWLock::CRWLock(TFlags flags)
    : m_Flags(flags),
      m_RW(new CInternalRWLock),
      m_Count(0),
      m_WaitingWriters(0)
{
#if defined(_DEBUG)
    m_Flags |= fTrackReaders;
#else
    if (m_Flags & fFavorWriters) {
        m_Flags |= fTrackReaders;
    }
#endif
    if (m_Flags & fTrackReaders) {
        m_Readers.reserve(16);
    }
}
#endif


CRWLock::~CRWLock(void)
{
    _ASSERT(m_Count == 0);
}


#if NCBI_SRWLOCK_USE_NEW
inline
vector<TThreadSystemID>::const_iterator
CRWLock::x_FindReader(TThreadSystemID self_id)
{
    return find(m_Readers.begin(), m_Readers.end(), self_id);
}

inline
bool 
CRWLock::x_HasReader(TThreadSystemID self_id)
{
    return m_Readers.empty() ? false : x_FindReader(self_id) != m_Readers.end();
}

inline
bool CRWLock::x_MayAcquireForReading(TThreadSystemID self_id)
{
    return (m_WaitingWriters == 0 && m_Count >= 0) || (m_TrackReaders && x_HasReader(self_id));
}

inline 
bool CRWLock::x_TryWriteLock()
{
    long expected = 0;
    return m_Count.compare_exchange_strong(expected, expected-1);
}

inline 
bool CRWLock::x_TryReadLock()
{
    long expected = 0;
    do {
        if (m_Count.compare_exchange_weak(expected, expected+1)) {
            return true;
        }
    } while (expected >= 0);
    return false;
}
#else
inline bool CRWLock::x_MayAcquireForReading(TThreadSystemID self_id)
{
    _ASSERT(self_id == GetCurrentThreadSystemID());
    if (m_Count < 0) { // locked for writing, possibly by self
        // return m_Owner.Is(self_id);
        return false; // allow special handling of self-locked cases
    } else if ( !(m_Flags & fFavorWriters) ) {
        return true; // no other concerns
    } else if (find(m_Readers.begin(), m_Readers.end(), self_id)
        != m_Readers.end()) {
        return true; // allow recursive read locks
    } else {
        return !m_WaitingWriters;
    }
}
#endif


#if defined(NCBI_USE_CRITICAL_SECTION) && !NCBI_SRWLOCK_USE_NEW

// Need special guard for system handle since mutex uses critical section
class CWin32MutexHandleGuard
{
public:
    CWin32MutexHandleGuard(HANDLE mutex);
    ~CWin32MutexHandleGuard(void);
private:
    HANDLE m_Handle;
};


inline
CWin32MutexHandleGuard::CWin32MutexHandleGuard(HANDLE mutex)
    : m_Handle(mutex)
{
    WaitForSingleObject(m_Handle, INFINITE);
}


inline
CWin32MutexHandleGuard::~CWin32MutexHandleGuard(void)
{
    ReleaseMutex(m_Handle);
}
#endif


// Helper functions for changing readers/writers counter without
// locking the mutex. Most of the functions check a condition
// and return false if it's not met before the new value is set.

inline void interlocked_set(volatile long* val, long new_val)
{
#if defined(NCBI_WIN32_THREADS)
    for (long old_val = *val; ; old_val = *val) {
        long cur_val = InterlockedCompareExchange(val, new_val, old_val);
        if (cur_val == old_val) {
            break;
        }
    }
#else
    *val = new_val;
#endif
}


inline bool interlocked_inc_min(volatile long* val, long min)
{
#if defined(NCBI_WIN32_THREADS)
    for (long old_val = *val; old_val > min; old_val = *val) {
        long new_val = old_val + 1;
        long cur_val = InterlockedCompareExchange(val, new_val, old_val);
        if (cur_val == old_val) {
            return true;
        }
    }
    return false;
#else
    (*val)++;
    return true;
#endif
}


inline bool interlocked_inc_max(volatile long* val, long max)
{
#if defined(NCBI_WIN32_THREADS)
    for (long old_val = *val; old_val < max; old_val = *val) {
        long new_val = old_val + 1;
        long cur_val = InterlockedCompareExchange(val, new_val, old_val);
        if (cur_val == old_val) {
            return true;
        }
    }
    return false;
#else
    (*val)++;
    return true;
#endif
}


inline bool interlocked_dec_max(volatile long* val, long max)
{
#if defined(NCBI_WIN32_THREADS)
    for (long old_val = *val; old_val < max; old_val = *val) {
        long new_val = old_val - 1;
        long cur_val = InterlockedCompareExchange(val, new_val, old_val);
        if (cur_val == old_val) {
            return true;
        }
    }
    return false;
#else
    (*val)--;
    return true;
#endif
}


inline bool interlocked_dec_min(volatile long* val, long min)
{
#if defined(NCBI_WIN32_THREADS)
    for (long old_val = *val; old_val > min; old_val = *val) {
        long new_val = old_val - 1;
        long cur_val = InterlockedCompareExchange(val, new_val, old_val);
        if (cur_val == old_val) {
            return true;
        }
    }
    return false;
#else
    (*val)--;
    return true;
#endif
}


void CRWLock::ReadLock(void)
{
#if defined(NCBI_NO_THREADS)
    return;
#else

#if NCBI_SRWLOCK_USE_NEW
    TThreadSystemID self_id = GetCurrentThreadSystemID();
    if (m_Owner == self_id) {
        --m_Count;
    } else {
        if (!m_TrackReaders && x_TryReadLock()) {
            return;
        }
        unique_lock<mutex> lck( m_Mtx);
        do {
            if ( !x_MayAcquireForReading(self_id) ) {
                while (m_WaitingWriters > 0 || m_Count < 0) {
                    m_Cv.wait(lck);
                }
            }
        } while (!x_TryReadLock());
        if (m_TrackReaders) {
            m_Readers.push_back(self_id);
        }
    }
#else

#if defined(NCBI_WIN32_THREADS)
    if ((m_Flags & fTrackReaders) == 0) {
        // Try short way if there are other readers.
        if (interlocked_inc_min(&m_Count, 0)) return;
    }
#endif

    TThreadSystemID self_id = GetCurrentThreadSystemID();

    // Lock mutex now, unlock before exit.
    // (in fact, it will be unlocked by the waiting function for a while)
#if defined(NCBI_USE_CRITICAL_SECTION)
    CWin32MutexHandleGuard guard(m_RW->m_Mutex);
#else
    CFastMutexGuard guard(m_RW->m_Mutex);
#endif
    if ( !x_MayAcquireForReading(self_id) ) {
        if (m_Count < 0  &&  m_Owner == self_id) {
            _VERIFY(interlocked_dec_max(&m_Count, 0));
        }
        else {
            // (due to be) W-locked by another thread
#if defined(NCBI_WIN32_THREADS)
            HANDLE obj[3];
            obj[0] = m_RW->m_Mutex.GetHandle();
            obj[1] = m_RW->m_Rsema;
            obj[2] = m_RW->m_Rsema2;
            xncbi_ValidateAndErrnoReport(
                ReleaseMutex(m_RW->m_Mutex.GetHandle()),
                "CRWLock::ReadLock() - release mutex error");
            DWORD wait_res =
                WaitForMultipleObjects(3, obj, TRUE, INFINITE)-WAIT_OBJECT_0;
            xncbi_Validate(wait_res < 3,
                           "CRWLock::ReadLock() - R-lock waiting error");
            // Success, check the semaphore
            xncbi_Validate(m_RW->m_Rsema.Release() == 0
                           &&  m_RW->m_Rsema2.Release() == 0,
                           "CRWLock::ReadLock() - invalid R-semaphore state");
            if (m_Count == 0) {
                xncbi_Validate(WaitForSingleObject(m_RW->m_Wsema,
                                                   0) == WAIT_OBJECT_0,
                               "CRWLock::ReadLock() - "
                               "failed to lock W-semaphore");
            }
#elif defined(NCBI_POSIX_THREADS)
            while ( !x_MayAcquireForReading(self_id) ) {
                xncbi_ValidatePthread(
                    pthread_cond_wait(m_RW->m_Rcond,
                                      m_RW->m_Mutex.GetHandle()), 0,
                    "CRWLock::ReadLock() - R-lock waiting error");
            }
#else
            // Can not be already W-locked by another thread without MT
            xncbi_Validate(0,
                           "CRWLock::ReadLock() - "
                           "weird R-lock error in non-MT mode");
#endif
            xncbi_Validate(m_Count >= 0,
                           "CRWLock::ReadLock() - invalid readers counter");
            _VERIFY(interlocked_inc_min(&m_Count, -1));
        }
    }
    else {
#if defined(NCBI_WIN32_THREADS)
        if (m_Count == 0) {
            // Unlocked
            // Lock against writers
            xncbi_Validate(WaitForSingleObject(m_RW->m_Wsema, 0)
                           == WAIT_OBJECT_0,
                           "CRWLock::ReadLock() - "
                           "can not lock W-semaphore");
        }
#endif
        _VERIFY(interlocked_inc_min(&m_Count, -1));
    }

    // Remember new reader
    if ((m_Flags & fTrackReaders) != 0  &&  m_Count > 0) {
        m_Readers.push_back(self_id);
    }
#endif
#endif
}


bool CRWLock::TryReadLock(void)
{
#if defined(NCBI_NO_THREADS)
    return true;
#else

#if NCBI_SRWLOCK_USE_NEW
    TThreadSystemID self_id = GetCurrentThreadSystemID();
    if (m_Owner == self_id) {
        --m_Count;
    } else {
        if (!m_TrackReaders && x_TryReadLock()) {
            return true;
        }
        unique_lock<mutex> lck( m_Mtx);
        if (!x_MayAcquireForReading(self_id)) {
            return false;
        }
        if (!x_TryReadLock()) {
            return false;
        }
        if (m_TrackReaders) {
            m_Readers.push_back(self_id);
        }
    }
    return true;
#else

#if defined(NCBI_WIN32_THREADS)
    if ((m_Flags & fTrackReaders) == 0) {
        if (interlocked_inc_min(&m_Count, 0)) return true;
    }
#endif

    TThreadSystemID self_id = GetCurrentThreadSystemID();

#if defined(NCBI_USE_CRITICAL_SECTION)
    CWin32MutexHandleGuard guard(m_RW->m_Mutex);
#else
    CFastMutexGuard guard(m_RW->m_Mutex);
#endif

    if ( !x_MayAcquireForReading(self_id) ) {
        if (m_Count >= 0  ||  m_Owner != self_id) {
            // (due to be) W-locked by another thread
            return false;
        }
        else {
            // W-locked, try to set R after W if in the same thread
            _VERIFY(interlocked_dec_max(&m_Count, 0));
            return true;
        }
    }

    // Unlocked - do R-lock
#if defined(NCBI_WIN32_THREADS)
    if (m_Count == 0) {
        // Lock W-semaphore in MSWIN
        xncbi_Validate(WaitForSingleObject(m_RW->m_Wsema, 0)
                       == WAIT_OBJECT_0,
                       "CRWLock::TryReadLock() - "
                       "can not lock W-semaphore");
    }
#endif
    _VERIFY(interlocked_inc_min(&m_Count, -1));
    if (m_Flags & fTrackReaders) {
        m_Readers.push_back(self_id);
    }
    return true;
#endif
#endif
}


bool CRWLock::TryReadLock(const CTimeout& timeout)
{
#if defined(NCBI_NO_THREADS)
    return true;
#else

    if ( timeout.IsInfinite() ) {
        ReadLock();
        return true;
    }
    if ( timeout.IsZero() ) {
        return TryReadLock();
    }

#if NCBI_SRWLOCK_USE_NEW
    TThreadSystemID self_id = GetCurrentThreadSystemID();
    if (m_Owner == self_id) {
        --m_Count;
    } else {
        if (!m_TrackReaders && x_TryReadLock()) {
            return true;
        }
        unsigned int sec=0, nanosec=0;
        timeout.GetNano(&sec, &nanosec);
        chrono::time_point<chrono::steady_clock> to = chrono::steady_clock::now() + chrono::seconds(sec) + chrono::nanoseconds(nanosec);
        cv_status res = cv_status::no_timeout;
        bool ok = true;

        unique_lock<mutex> lck( m_Mtx);
        do {
            if (res == cv_status::timeout) {
                return false;
            }
            if (!x_MayAcquireForReading(self_id)) {
                while (!(ok = m_WaitingWriters == 0 && m_Count >= 0) && res == cv_status::no_timeout) {
                    res = m_Cv.wait_until(lck, to);
                }
            }
            if (!ok) {
                return false;
            }
        } while (!x_TryReadLock());
        if (m_TrackReaders) {
            m_Readers.push_back(self_id);
        }
    }
    return true;
#else

#if defined(NCBI_WIN32_THREADS)
    if ((m_Flags & fTrackReaders) == 0) {
        // Try short way if there are other readers.
        if (interlocked_inc_min(&m_Count, 0)) return true;
    }
#endif

    TThreadSystemID self_id = GetCurrentThreadSystemID();

    // Lock mutex now, unlock before exit.
    // (in fact, it will be unlocked by the waiting function for a while)
#if defined(NCBI_USE_CRITICAL_SECTION)
    CWin32MutexHandleGuard guard(m_RW->m_Mutex);
#else
    CFastMutexGuard guard(m_RW->m_Mutex);
#endif
    if ( !x_MayAcquireForReading(self_id) ) {
        if (m_Count < 0  &&  m_Owner == self_id) {
            _VERIFY(interlocked_dec_max(&m_Count, 0));
        }
        else {
            // (due to be) W-locked by another thread
#if defined(NCBI_WIN32_THREADS)
            HANDLE obj[3];
            obj[0] = m_RW->m_Mutex.GetHandle();
            obj[1] = m_RW->m_Rsema;
            obj[2] = m_RW->m_Rsema2;
            xncbi_ValidateAndErrnoReport(
                ReleaseMutex(m_RW->m_Mutex.GetHandle()),
                "CRWLock::TryReadLock() - release mutex error");
            DWORD timeout_msec = timeout.GetAsMilliSeconds();
            DWORD wait_res =
                WaitForMultipleObjects(3, obj, TRUE, timeout_msec);
            if (wait_res == WAIT_TIMEOUT) {
                return false;
            }
            wait_res -= WAIT_OBJECT_0;
            xncbi_Validate(wait_res < 3,
                           "CRWLock::TryReadLock() - R-lock waiting error");
            // Success, check the semaphore
            xncbi_Validate(m_RW->m_Rsema.Release() == 0
                           &&  m_RW->m_Rsema2.Release() == 0,
                           "CRWLock::TryReadLock() - invalid R-semaphore state");
            if (m_Count == 0) {
                xncbi_Validate(WaitForSingleObject(m_RW->m_Wsema,
                                                   0) == WAIT_OBJECT_0,
                               "CRWLock::TryReadLock() - "
                               "failed to lock W-semaphore");
            }
#elif defined(NCBI_POSIX_THREADS)
            CDeadline deadline(timeout);
            time_t s;
            unsigned int ns;
            deadline.GetExpirationTime(&s, &ns);
            struct timespec ts;
            ts.tv_sec = s;
            ts.tv_nsec = ns;
            int res = 0;
            while ( !x_MayAcquireForReading(self_id)  &&  res != ETIMEDOUT ) {
                res = pthread_cond_timedwait(m_RW->m_Rcond,
                    m_RW->m_Mutex.GetHandle(), &ts);
            }
            if (res == ETIMEDOUT) {
                return false;
            }
            xncbi_Validate(res == 0,
                           "CRWLock::TryReadLock() - R-lock waiting error");
#else
            // Can not be already W-locked by another thread without MT
            xncbi_Validate(0,
                           "CRWLock::TryReadLock() - "
                           "weird R-lock error in non-MT mode");
#endif
            xncbi_Validate(m_Count >= 0,
                           "CRWLock::TryReadLock() - invalid readers counter");
            _VERIFY(interlocked_inc_min(&m_Count, -1));
        }
    }
    else {
#if defined(NCBI_WIN32_THREADS)
        if (m_Count == 0) {
            // Unlocked
            // Lock against writers
            xncbi_Validate(WaitForSingleObject(m_RW->m_Wsema, 0)
                           == WAIT_OBJECT_0,
                           "CRWLock::TryReadLock() - "
                           "can not lock W-semaphore");
        }
#endif
        _VERIFY(interlocked_inc_min(&m_Count, -1));
    }

    // Remember new reader
    if ((m_Flags & fTrackReaders) != 0  &&  m_Count > 0) {
        m_Readers.push_back(self_id);
    }
    return true;
#endif
#endif
}


void CRWLock::WriteLock(void)
{
#if defined(NCBI_NO_THREADS)
    return;
#else

#if NCBI_SRWLOCK_USE_NEW
    TThreadSystemID self_id = GetCurrentThreadSystemID();
    if (m_Owner == self_id) {
        --m_Count;
    } else {
        if (x_TryWriteLock()) {
            m_Owner = self_id;
            return;
        }
        unique_lock<mutex> lck( m_Mtx);
        _ASSERT(!m_TrackReaders || !x_HasReader(self_id));
        if (m_FavorWriters) {
            ++m_WaitingWriters;
        }
        while (!x_TryWriteLock()) {
            m_Cv.wait(lck);
        }
        m_Owner = self_id;
        if (m_FavorWriters) {
            --m_WaitingWriters;
        }
    }
#else

    TThreadSystemID self_id = GetCurrentThreadSystemID();

#if defined(NCBI_USE_CRITICAL_SECTION)
    CWin32MutexHandleGuard guard(m_RW->m_Mutex);
#else
    CFastMutexGuard guard(m_RW->m_Mutex);
#endif

    if ( m_Count < 0 && m_Owner == self_id ) {
        // W-locked by the same thread
        _VERIFY(interlocked_dec_max(&m_Count, 0));
    }
    else {
        // Unlocked or RW-locked by another thread
        // Look in readers - must not be there
        xncbi_Validate(find(m_Readers.begin(), m_Readers.end(), self_id)
                       == m_Readers.end(),
                       "CRWLock::WriteLock() - "
                       "attempt to set W-after-R lock");

#if defined(NCBI_WIN32_THREADS)
        HANDLE obj[3];
        obj[0] = m_RW->m_Rsema;
        obj[1] = m_RW->m_Wsema;
        obj[2] = m_RW->m_Mutex.GetHandle();
        if (m_Count == 0) {
            // Unlocked - lock both semaphores
            DWORD wait_res =
                WaitForMultipleObjects(2, obj, TRUE, 0)-WAIT_OBJECT_0;
            xncbi_Validate(wait_res < 2,
                           "CRWLock::WriteLock() - "
                           "error locking R&W-semaphores");
        }
        else {
            // Locked by another thread - wait for unlock
            if (m_Flags & fFavorWriters) {
                if (++m_WaitingWriters == 1) {
                    // First waiting writer - lock out readers
                    xncbi_Validate(WaitForSingleObject(m_RW->m_Rsema2, 0)
                                   == WAIT_OBJECT_0,
                                   "CRWLock::WriteLock() - "
                                   "error locking R-semaphore 2");
                }
            }
            xncbi_ValidateAndErrnoReport(
                ReleaseMutex(m_RW->m_Mutex.GetHandle()),
                "CRWLock::WriteLock() - release mutex error");
            DWORD wait_res =
                WaitForMultipleObjects(3, obj, TRUE, INFINITE)-WAIT_OBJECT_0;
            xncbi_Validate(wait_res < 3,
                           "CRWLock::WriteLock() - "
                           "error locking R&W-semaphores");
            if (m_Flags & fFavorWriters) {
                if (--m_WaitingWriters == 0) {
                    xncbi_Validate(m_RW->m_Rsema2.Release() == 0,
                                   "CRWLock::WriteLock() - "
                                   "invalid R-semaphore 2 state");
                    // Readers still won't be able to proceed, but releasing
                    // the semaphore here simplifies bookkeeping.
                }
            }
        }
#elif defined(NCBI_POSIX_THREADS)
        if (m_Flags & fFavorWriters) {
            ++m_WaitingWriters;
        }
        while (m_Count != 0) {
            xncbi_ValidatePthread(
                pthread_cond_wait(m_RW->m_Wcond,
                                  m_RW->m_Mutex.GetHandle()), 0,
                "CRWLock::WriteLock() - error locking R&W-conditionals");
        }
        if (m_Flags & fFavorWriters) {
            --m_WaitingWriters;
        }
#endif
        xncbi_Validate(m_Count >= 0,
                       "CRWLock::WriteLock() - invalid readers counter");
        interlocked_set(&m_Count, -1);
        m_Owner = self_id;
    }

    // No readers allowed
    _ASSERT(m_Readers.empty());
#endif
#endif
}


bool CRWLock::TryWriteLock(void)
{
#if defined(NCBI_NO_THREADS)
    return true;
#else

#if NCBI_SRWLOCK_USE_NEW
    TThreadSystemID self_id = GetCurrentThreadSystemID();
    if (m_Owner == self_id) {
        --m_Count;
    } else {
        if (!x_TryWriteLock()) {
            return false;
        }
        m_Owner = self_id;
    }
    return true;
#else

    TThreadSystemID self_id = GetCurrentThreadSystemID();

#if defined(NCBI_USE_CRITICAL_SECTION)
    CWin32MutexHandleGuard guard(m_RW->m_Mutex);
#else
    CFastMutexGuard guard(m_RW->m_Mutex);
#endif

    if ( m_Count < 0 ) {
        // W-locked
        if ( m_Owner != self_id ) {
            // W-locked by another thread
            return false;
        }
        // W-locked by same thread
        _VERIFY(interlocked_dec_max(&m_Count, 0));
    }
    else if ( m_Count > 0 ) {
        // R-locked
        return false;
    }
    else {
        // Unlocked - do W-lock
#if defined(NCBI_WIN32_THREADS)
        // In MSWIN lock semaphores
        HANDLE obj[2];
        obj[0] = m_RW->m_Rsema;
        obj[1] = m_RW->m_Wsema;
        DWORD wait_res =
            WaitForMultipleObjects(2, obj, TRUE, 0) - WAIT_OBJECT_0;
        xncbi_Validate(wait_res < 2,
                       "CRWLock::TryWriteLock() - "
                       "error locking R&W-semaphores");
#endif
        interlocked_set(&m_Count, -1);
        m_Owner = self_id;
    }

    // No readers allowed
    _ASSERT(m_Readers.empty());

    return true;
#endif
#endif
}


bool CRWLock::TryWriteLock(const CTimeout& timeout)
{
#if defined(NCBI_NO_THREADS)
    return true;
#else

    if ( timeout.IsInfinite() ) {
        WriteLock();
        return true;
    }
    if ( timeout.IsZero() ) {
        return TryWriteLock();
    }

#if NCBI_SRWLOCK_USE_NEW
    TThreadSystemID self_id = GetCurrentThreadSystemID();
    if (m_Owner == self_id) {
        --m_Count;
    } else {
        if (x_TryWriteLock()) {
            m_Owner = self_id;
            return true;
        }
        unsigned int sec=0, nanosec=0;
        timeout.GetNano(&sec, &nanosec);
        chrono::time_point<chrono::steady_clock> to = chrono::steady_clock::now() + chrono::seconds(sec) + chrono::nanoseconds(nanosec);
        cv_status res = cv_status::no_timeout;
        bool ok = true;

        unique_lock<mutex> lck( m_Mtx);
        do {
            if (res == cv_status::timeout) {
                return false;
            }
            if (m_FavorWriters) {
                ++m_WaitingWriters;
            }
            while (!(ok = m_Count == 0) && res == cv_status::no_timeout) {
                res = m_Cv.wait_until(lck, to);
            }
            if (m_FavorWriters) {
                --m_WaitingWriters;
            }
            if (!ok) {
                return false;
            }
        } while (!x_TryWriteLock());
        m_Owner = self_id;
    }
    return true;
#else
    TThreadSystemID self_id = GetCurrentThreadSystemID();

#if defined(NCBI_USE_CRITICAL_SECTION)
    CWin32MutexHandleGuard guard(m_RW->m_Mutex);
#else
    CFastMutexGuard guard(m_RW->m_Mutex);
#endif

    if ( m_Count < 0 && m_Owner == self_id ) {
        // W-locked by the same thread
        _VERIFY(interlocked_dec_max(&m_Count, 0));
    }
    else {
        // Unlocked or RW-locked by another thread
        // Look in readers - must not be there
        xncbi_Validate(find(m_Readers.begin(), m_Readers.end(), self_id)
                       == m_Readers.end(),
                       "CRWLock::TryWriteLock() - "
                       "attempt to set W-after-R lock");

#if defined(NCBI_WIN32_THREADS)
        HANDLE obj[3];
        obj[0] = m_RW->m_Rsema;
        obj[1] = m_RW->m_Wsema;
        obj[2] = m_RW->m_Mutex.GetHandle();
        if (m_Count == 0) {
            // Unlocked - lock both semaphores
            DWORD wait_res =
                WaitForMultipleObjects(2, obj, TRUE, 0)-WAIT_OBJECT_0;
            xncbi_Validate(wait_res < 2,
                           "CRWLock::TryWriteLock() - "
                           "error locking R&W-semaphores");
        }
        else {
            // Locked by another thread - wait for unlock
            if (m_Flags & fFavorWriters) {
                if (++m_WaitingWriters == 1) {
                    // First waiting writer - lock out readers
                    xncbi_Validate(WaitForSingleObject(m_RW->m_Rsema2, 0)
                                   == WAIT_OBJECT_0,
                                   "CRWLock::TryWriteLock() - "
                                   "error locking R-semaphore 2");
                }
            }
            xncbi_ValidateAndErrnoReport(
                ReleaseMutex(m_RW->m_Mutex.GetHandle()),
                "CRWLock::TryWriteLock() - release mutex error");
            DWORD timeout_msec = timeout.GetAsMilliSeconds();
            DWORD wait_res =
                WaitForMultipleObjects(3, obj, TRUE, timeout_msec);
            if (wait_res == WAIT_TIMEOUT) {
                if (m_Flags & fFavorWriters) {
                    // Need temp lock to synchronize the counter and the semaphore.
                    CWin32MutexHandleGuard counter_guard(m_RW->m_Mutex);
                    if (--m_WaitingWriters == 0) {
                        xncbi_Validate(m_RW->m_Rsema2.Release() == 0,
                                       "CRWLock::TryWriteLock() - "
                                       "invalid R-semaphore 2 state");
                        // Readers still won't be able to proceed, but releasing
                        // the semaphore here simplifies bookkeeping.
                    }
                }
                return false;
            }
            wait_res -= WAIT_OBJECT_0;
            xncbi_Validate(wait_res < 3,
                           "CRWLock::TryWriteLock() - "
                           "error locking R&W-semaphores");
            if (m_Flags & fFavorWriters) {
                if (--m_WaitingWriters == 0) {
                    xncbi_Validate(m_RW->m_Rsema2.Release() == 0,
                                   "CRWLock::TryWriteLock() - "
                                   "invalid R-semaphore 2 state");
                    // Readers still won't be able to proceed, but releasing
                    // the semaphore here simplifies bookkeeping.
                }
            }
        }
#elif defined(NCBI_POSIX_THREADS)
        if (m_Flags & fFavorWriters) {
            ++m_WaitingWriters;
        }
        CDeadline deadline(timeout);
        time_t s;
        unsigned int ns;
        deadline.GetExpirationTime(&s, &ns);
        struct timespec ts;
        ts.tv_sec = s;
        ts.tv_nsec = ns;
        int res = 0;
        while (m_Count != 0  &&  res != ETIMEDOUT ) {
            res = pthread_cond_timedwait(m_RW->m_Wcond,
                m_RW->m_Mutex.GetHandle(), &ts);
        }
        if (res == ETIMEDOUT) {
            if (m_Flags & fFavorWriters) {
                --m_WaitingWriters;
            }
            return false;
        }
        xncbi_Validate(res == 0,
                       "CRWLock::TryWriteLock() - "
                       "error locking R&W-conditionals");
        if (m_Flags & fFavorWriters) {
            --m_WaitingWriters;
        }
#endif
        xncbi_Validate(m_Count >= 0,
                       "CRWLock::TryWriteLock() - invalid readers counter");
        interlocked_set(&m_Count, -1);
        m_Owner = self_id;
    }

    // No readers allowed
    _ASSERT(m_Readers.empty());
    return true;
#endif
#endif
}


void CRWLock::Unlock(void)
{
#if defined(NCBI_NO_THREADS)
    return;
#else

#if NCBI_SRWLOCK_USE_NEW
    TThreadSystemID self_id = GetCurrentThreadSystemID();
    if (m_Owner == self_id && m_Count < -1) {
        ++m_Count;
        return;
    }
    unique_lock<mutex> lck( m_Mtx/*, defer_lock*/);
    if (m_Owner == self_id) {
        m_Owner = 0;
        ++m_Count;
//        lck.lock();
    } else {
        long cnt = --m_Count;
        if (m_TrackReaders) {
//            lck.lock();
            m_Readers.erase( x_FindReader(self_id) );
        } else if (cnt != 0) {
            return;
        }
    }
    m_Cv.notify_all();
#else

#if defined(NCBI_WIN32_THREADS)
    if ((m_Flags & fTrackReaders) == 0) {
        if (interlocked_dec_min(&m_Count, 1)) return;
    }
#endif

    TThreadSystemID self_id = GetCurrentThreadSystemID();

#if defined(NCBI_USE_CRITICAL_SECTION)
    CWin32MutexHandleGuard guard(m_RW->m_Mutex);
#else
    CFastMutexGuard guard(m_RW->m_Mutex);
#endif

    if (m_Count < 0) {
        // Check it is R-locked or W-locked by the same thread
        xncbi_Validate(m_Owner == self_id,
                       "CRWLock::Unlock() - "
                       "RWLock is locked by another thread");
        _VERIFY(interlocked_inc_max(&m_Count, 0));
        if ( m_Count == 0 ) {
            // Unlock the last W-lock
#if defined(NCBI_WIN32_THREADS)
            xncbi_Validate(m_RW->m_Rsema.Release() == 0,
                           "CRWLock::Unlock() - invalid R-semaphore state");
            xncbi_Validate(m_RW->m_Wsema.Release() == 0,
                           "CRWLock::Unlock() - invalid R-semaphore state");
#elif defined(NCBI_POSIX_THREADS)
            if ( !m_WaitingWriters ) {
                xncbi_ValidatePthread(
                    pthread_cond_broadcast(m_RW->m_Rcond), 0,
                    "CRWLock::Unlock() - error signalling unlock");
            }
            xncbi_ValidatePthread(
                pthread_cond_signal(m_RW->m_Wcond), 0,
                "CRWLock::Unlock() - error signalling unlock");
#endif
        }
        if (m_Flags & fTrackReaders) {
            // Check if the unlocking thread is in the owners list
            _ASSERT(find(m_Readers.begin(), m_Readers.end(), self_id)
                    == m_Readers.end());
        }
    }
    else {
        xncbi_Validate(m_Count != 0,
                       "CRWLock::Unlock() - RWLock is not locked");
        _VERIFY(interlocked_dec_min(&m_Count, -1));
        if ( m_Count == 0 ) {
            // Unlock the last R-lock
#if defined(NCBI_WIN32_THREADS)
            xncbi_Validate(m_RW->m_Wsema.Release() == 0,
                           "CRWLock::Unlock() - invalid W-semaphore state");
#elif defined(NCBI_POSIX_THREADS)
            xncbi_ValidatePthread(
                pthread_cond_signal(m_RW->m_Wcond), 0,
                "CRWLock::Unlock() - error signaling unlock");
#endif
        }
        if (m_Flags & fTrackReaders) {
            // Check if the unlocking thread is in the owners list
            vector<TThreadSystemID>::iterator found =
                find(m_Readers.begin(), m_Readers.end(), self_id);
            _ASSERT(found != m_Readers.end());
            m_Readers.erase(found);
            if ( m_Count == 0 ) {
                _ASSERT(m_Readers.empty());
            }
        }
    }
#endif
#endif
}




/////////////////////////////////////////////////////////////////////////////
//  SEMAPHORE
//


// Platform-specific representation (or emulation) of semaphore
struct SSemaphore
{
#if defined(NCBI_POSIX_THREADS)
    unsigned int     max_count;
    unsigned int     count;
    unsigned int     wait_count;  // # of threads currently waiting on the sema
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;

#elif defined(NCBI_WIN32_THREADS)
    HANDLE           sem;
#else
    unsigned int     max_count;
    unsigned int     count;
#endif
};


#if NCBI_SEMAPHORE_USE_NEW
CSemaphore::CSemaphore(unsigned int init_count, unsigned int max_count)
    : m_Max(max_count), m_Count(init_count)
{
    _ASSERT(max_count != 0);
    _ASSERT(init_count <= max_count);
}
#else
CSemaphore::CSemaphore(unsigned int init_count, unsigned int max_count)
{
#if defined(NCBI_WIN32_THREADS)
    // On Windows max_count is LONG and can not exceed LONG_MAX.
    max_count = min(max_count, (unsigned int)LONG_MAX);
#endif
    xncbi_Validate(max_count != 0,
                   "CSemaphore::CSemaphore() - max_count passed zero");
    xncbi_Validate(init_count <= max_count,
                   "CSemaphore::CSemaphore() - init_count "
                   "greater than max_count");

    m_Sem = new SSemaphore;
    unique_ptr<SSemaphore> auto_sem(m_Sem);

#if defined(NCBI_POSIX_THREADS)
    m_Sem->max_count = max_count;
    m_Sem->count     = init_count;
    m_Sem->wait_count = 0;

#  if defined(NCBI_OS_CYGWIN)
    if (pthread_mutex_init(&m_Sem->mutex, 0) != 0) {
        memset(&m_Sem->mutex, 0, sizeof(m_Sem->mutex));
#  endif  /* NCBI_OS_CYGWIN */
        xncbi_ValidatePthread(
            pthread_mutex_init(&m_Sem->mutex, 0), 0,
            "CSemaphore::CSemaphore() - pthread_mutex_init() failed");
#  if defined(NCBI_OS_CYGWIN)
    }
#  endif  /* NCBI_OS_CYGWIN */

#  if defined(NCBI_OS_CYGWIN)
    if (pthread_cond_init(&m_Sem->cond, 0) != 0) {
        memset(&m_Sem->cond, 0, sizeof(m_Sem->cond));
#  endif  /* NCBI_OS_CYGWIN */
        xncbi_ValidatePthread(
            pthread_cond_init(&m_Sem->cond, 0), 0,
            "CSemaphore::CSemaphore() - pthread_cond_init() failed");
#  if defined(NCBI_OS_CYGWIN)
    }
#  endif  /* NCBI_OS_CYGWIN */

#elif defined(NCBI_WIN32_THREADS)
    m_Sem->sem = CreateSemaphore(NULL, init_count, max_count, NULL);
#ifdef _DEBUG
    if (m_Sem->sem == NULL) {
        DWORD dwErr = GetLastError();
        ERR_POST(Error << "GetLastError returns " << dwErr);
    }
#endif
    xncbi_Validate(m_Sem->sem != NULL,
                   "CSemaphore::CSemaphore() - CreateSemaphore() failed");
#else
    m_Sem->max_count = max_count;
    m_Sem->count     = init_count;
#endif

    auto_sem.release();
}
#endif


CSemaphore::~CSemaphore(void)
{
#if NCBI_SEMAPHORE_USE_NEW
#else
#if defined(NCBI_POSIX_THREADS)
    _ASSERT(m_Sem->wait_count == 0);
    xncbi_VerifyAndErrorReport(pthread_mutex_destroy(&m_Sem->mutex) == 0);
    xncbi_VerifyAndErrorReport(pthread_cond_destroy(&m_Sem->cond)  == 0);

#elif defined(NCBI_WIN32_THREADS)
    xncbi_VerifyAndErrorReport( CloseHandle(m_Sem->sem) );
#endif

    delete m_Sem;
#endif
}

#if NCBI_SEMAPHORE_USE_NEW
inline
bool CSemaphore::x_TryAcquire(void)
{
    if (m_Count > 0) {
        --m_Count;
        return true;
    }
    return false;
}
#endif

void CSemaphore::Wait(void)
{
#if NCBI_SEMAPHORE_USE_NEW
    unique_lock<mutex> lck(m_Mtx);
    while (!x_TryAcquire()) {
        m_Cv.wait(lck);
    }
#else
#if defined(NCBI_POSIX_THREADS)
    xncbi_ValidatePthread(
        pthread_mutex_lock(&m_Sem->mutex), 0,
        "CSemaphore::Wait() - pthread_mutex_lock() failed");

    if (m_Sem->count != 0) {
        m_Sem->count--;
    }
    else {
        m_Sem->wait_count++;
        do {
            int status = pthread_cond_wait(&m_Sem->cond, &m_Sem->mutex);
            if (status != 0  &&  status != EINTR) {
                xncbi_ValidatePthread(
                    pthread_mutex_unlock(&m_Sem->mutex), 0,
                    "CSemaphore::Wait() - pthread_cond_wait() and "
                    "pthread_mutex_unlock() failed");
                xncbi_Validate(0,
                               "CSemaphore::Wait() - "
                               "pthread_cond_wait() failed");
            }
        } while (m_Sem->count == 0);
        m_Sem->wait_count--;
        m_Sem->count--;
    }

    xncbi_ValidatePthread(
        pthread_mutex_unlock(&m_Sem->mutex), 0,
        "CSemaphore::Wait() - pthread_mutex_unlock() failed");

#elif defined(NCBI_WIN32_THREADS)
    xncbi_Validate(WaitForSingleObject(m_Sem->sem, INFINITE) == WAIT_OBJECT_0,
                   "CSemaphore::Wait() - WaitForSingleObject() failed");

#else
    xncbi_Validate(m_Sem->count != 0,
                   "CSemaphore::Wait() - "
                   "wait with zero count in one-thread mode(?!)");
    m_Sem->count--;
#endif
#endif
}

#if defined(NCBI_NO_THREADS)
# define NCBI_THREADS_ARG(arg)
#else
# define NCBI_THREADS_ARG(arg) arg
#endif

bool CSemaphore::TryWait(unsigned int NCBI_THREADS_ARG(timeout_sec),
                         unsigned int NCBI_THREADS_ARG(timeout_nsec))
{
#if NCBI_SEMAPHORE_USE_NEW
    unique_lock<mutex> lck(m_Mtx);
#if defined(NCBI_NO_THREADS)
    return x_TryAcquire();
#else
    if (x_TryAcquire()) {
        return true;
    }
    if (timeout_sec == 0 && timeout_nsec == 0) {
        return x_TryAcquire();
    }
    chrono::time_point<chrono::steady_clock> to = chrono::steady_clock::now() + chrono::seconds(timeout_sec) + chrono::nanoseconds(timeout_nsec);
    cv_status res = cv_status::no_timeout;
    while (!x_TryAcquire()) {
        res = m_Cv.wait_until(lck, to);
        if (res == cv_status::timeout) {
            return false;
        }
    }
    return true;
#endif
#else
#if defined(NCBI_POSIX_THREADS)
    xncbi_ValidatePthread(
        pthread_mutex_lock(&m_Sem->mutex), 0,
        "CSemaphore::TryWait() - pthread_mutex_lock() failed");

    bool retval = false;
    if (m_Sem->count != 0) {
        m_Sem->count--;
        retval = true;
    }
    else if (timeout_sec > 0  ||  timeout_nsec > 0) {
# ifdef NCBI_OS_SOLARIS
        // arbitrary limit of 100Ms (~3.1 years) -- supposedly only for
        // native threads, but apparently also for POSIX threads :-/
        if (timeout_sec >= 100 * 1000 * 1000) {
            timeout_sec  = 100 * 1000 * 1000;
            timeout_nsec = 0;
        }
# endif
        static const unsigned int kBillion = 1000 * 1000 * 1000;
        struct timeval  now;
        struct timespec timeout = { 0, 0 };
        gettimeofday(&now, 0);
        // timeout_sec added below to avoid overflow
        timeout.tv_sec  = now.tv_sec;
        timeout.tv_nsec = now.tv_usec * 1000 + timeout_nsec;
        if ((unsigned int)timeout.tv_nsec >= kBillion) {
            timeout.tv_sec  += timeout.tv_nsec / kBillion;
            timeout.tv_nsec %= kBillion;
        }
        if (timeout_sec > (unsigned int)(kMax_Int - timeout.tv_sec)) {
            // Max out rather than overflowing
            timeout.tv_sec  = kMax_Int;
            timeout.tv_nsec = kBillion - 1;
        } else {
            timeout.tv_sec += timeout_sec;
        }

        m_Sem->wait_count++;
        do {
            int status = pthread_cond_timedwait(&m_Sem->cond, &m_Sem->mutex,
                                                &timeout);
            if (status == ETIMEDOUT) {
                break;
            } else if (status != 0  &&  status != EINTR) {
                // EINVAL, presumably?
                xncbi_ValidatePthread(
                    pthread_mutex_unlock(&m_Sem->mutex), 0,
                    "CSemaphore::TryWait() - pthread_cond_timedwait() and "
                    "pthread_mutex_unlock() failed");
                xncbi_Validate(0, "CSemaphore::TryWait() - "
                               "pthread_cond_timedwait() failed");
            }
        } while (m_Sem->count == 0);
        m_Sem->wait_count--;
        if (m_Sem->count != 0) {
            m_Sem->count--;
            retval = true;
        }
    }

    xncbi_ValidatePthread(
        pthread_mutex_unlock(&m_Sem->mutex), 0,
        "CSemaphore::TryWait() - pthread_mutex_unlock() failed");

    return retval;

#elif defined(NCBI_WIN32_THREADS)
    DWORD timeout_msec; // DWORD == unsigned long
    if (timeout_sec >= kMax_ULong / 1000) {
        timeout_msec = kMax_ULong;
    } else {
        timeout_msec = timeout_sec * 1000 + timeout_nsec / (1000 * 1000);
    }
    DWORD res = WaitForSingleObject(m_Sem->sem, timeout_msec);
    xncbi_Validate(res == WAIT_OBJECT_0  ||  res == WAIT_TIMEOUT,
                   "CSemaphore::TryWait() - WaitForSingleObject() failed");
    return (res == WAIT_OBJECT_0);

#else
    if (m_Sem->count == 0)
        return false;
    m_Sem->count--;
    return true;
#endif
#endif
}


bool CSemaphore::TryWait(const CTimeout& timeout)
{
    if (timeout.IsInfinite()) {
        Wait();
        return true;
    }

    unsigned int s, ns;
    timeout.GetNano(&s, &ns);
    return TryWait(s, ns);
}


void CSemaphore::Post(unsigned int count)
{
    if (count == 0)
        return;

#if NCBI_SEMAPHORE_USE_NEW
    unique_lock<mutex> lck( m_Mtx);
    xncbi_Validate(m_Count <= kMax_UInt - count && m_Count + count <= m_Max,
                   "CSemaphore::Post() - attempt to exceed max_count");
    m_Count += count;
    m_Cv.notify_all();
#else
#if defined (NCBI_POSIX_THREADS)
    xncbi_ValidatePthread(
        pthread_mutex_lock(&m_Sem->mutex), 0,
        "CSemaphore::Post() - pthread_mutex_lock() failed");

    if (m_Sem->count > kMax_UInt - count  ||
        m_Sem->count + count > m_Sem->max_count) {
        xncbi_ValidatePthread(
            pthread_mutex_unlock(&m_Sem->mutex), 0,
            "CSemaphore::Post() - attempt to exceed max_count and "
            "pthread_mutex_unlock() failed");
        xncbi_Validate(false,
                       "CSemaphore::Post() - attempt to exceed max_count");
    }

    // Signal some (or all) of the threads waiting on this semaphore
    int err_code = 0;
    if (m_Sem->count + count >= m_Sem->wait_count) {
        err_code = pthread_cond_broadcast(&m_Sem->cond);
    } else {
        // Do not use broadcast here to avoid waking up more threads
        // than really needed...
        for (unsigned int n_sig = 0;  n_sig < count;  n_sig++) {
            err_code = pthread_cond_signal(&m_Sem->cond);
            if (err_code != 0) {
                err_code = pthread_cond_broadcast(&m_Sem->cond);
                break;
            }
        }
    }

    // Success
    if (err_code == 0) {
        m_Sem->count += count;
        xncbi_ValidatePthread(
            pthread_mutex_unlock(&m_Sem->mutex), 0,
            "CSemaphore::Post() - pthread_mutex_unlock() failed");
        return;
    }

    // Error
    xncbi_ValidatePthread(
        pthread_mutex_unlock(&m_Sem->mutex), 0,
        "CSemaphore::Post() - pthread_cond_signal/broadcast() and "
        "pthread_mutex_unlock() failed");
    xncbi_Validate(0,
                   "CSemaphore::Post() - "
                   "pthread_cond_signal/broadcast() failed");

#elif defined(NCBI_WIN32_THREADS)
    xncbi_ValidateAndErrnoReport(
        ReleaseSemaphore(m_Sem->sem, count, NULL),
        "CSemaphore::Post() - ReleaseSemaphore() failed");

#else
    xncbi_Validate(m_Sem->count + count <= m_Sem->max_count,
                   "CSemaphore::Post() - attempt to exceed max_count");
    m_Sem->count += count;
#endif
#endif
}


// All methods must be not inline to avoid unwanted optimizations by compiler
void
CFastRWLock::ReadLock(void)
{
#if defined(NCBI_WIN32_THREADS) && defined(NCBI_FASTRWLOCK_USE_NEW)
    AcquireSRWLockShared(&m_Lock);
#else
    while (m_LockCount.Add(1) > kWriteLockValue) {
        m_LockCount.Add(-1);
        m_WriteLock.Lock();
        m_WriteLock.Unlock();
    }
#endif
}

void
CFastRWLock::ReadUnlock(void)
{
#if defined(NCBI_WIN32_THREADS) && defined(NCBI_FASTRWLOCK_USE_NEW)
    ReleaseSRWLockShared(&m_Lock);
#else
    m_LockCount.Add(-1);
#endif
}

void
CFastRWLock::WriteLock(void)
{
#if defined(NCBI_WIN32_THREADS) && defined(NCBI_FASTRWLOCK_USE_NEW)
    AcquireSRWLockExclusive(&m_Lock);
#else
    m_WriteLock.Lock();
    m_LockCount.Add(kWriteLockValue);
    while (m_LockCount.Get() != kWriteLockValue) {
        NCBI_SCHED_YIELD();
    }
#endif
}

void
CFastRWLock::WriteUnlock(void)
{
#if defined(NCBI_WIN32_THREADS) && defined(NCBI_FASTRWLOCK_USE_NEW)
    ReleaseSRWLockExclusive(&m_Lock);
#else
    m_LockCount.Add(-kWriteLockValue);
    m_WriteLock.Unlock();
#endif
}


IRWLockHolder_Listener::~IRWLockHolder_Listener(void)
{}

IRWLockHolder_Factory::~IRWLockHolder_Factory(void)
{}


CRWLockHolder::~CRWLockHolder(void)
{
    if (m_Lock) {
        ReleaseLock();
    }
}

void
CRWLockHolder::DeleteThis(void)
{
    m_Factory->DeleteHolder(this);
}

void
CRWLockHolder::x_OnLockAcquired(void)
{
    TListenersList listeners;

    m_ObjLock.Lock();
    listeners = m_Listeners;
    m_ObjLock.Unlock();

    NON_CONST_ITERATE(TListenersList, it, listeners) {
        TRWLockHolder_ListenerRef lstn(it->Lock());
        if (lstn.NotNull()) {
            lstn->OnLockAcquired(this);
        }
    }
}

void
CRWLockHolder::x_OnLockReleased(void)
{
    TListenersList listeners;

    m_ObjLock.Lock();
    listeners = m_Listeners;
    m_ObjLock.Unlock();

    NON_CONST_ITERATE(TListenersList, it, listeners) {
        TRWLockHolder_ListenerRef lstn(it->Lock());
        if (lstn.NotNull()) {
            lstn->OnLockReleased(this);
        }
    }
}


/// Default implementation of IRWLockHolder_Factory.
/// Essentially pool of CRWLockHolder objects.
class CRWLockHolder_Pool : public IRWLockHolder_Factory
{
public:
    CRWLockHolder_Pool(void);
    virtual ~CRWLockHolder_Pool(void);

    /// Obtain new CRWLockHolder object for given CYieldingRWLock and
    /// necessary lock type.
    virtual CRWLockHolder* CreateHolder(CYieldingRWLock* lock,
                                        ERWLockType      typ);

    /// Free unnecessary (and unreferenced by anybody) CRWLockHolder object
    virtual void DeleteHolder(CRWLockHolder* holder);

private:
    typedef CObjFactory_NewParam<CRWLockHolder,
                                 CRWLockHolder_Pool*>    THolderPoolFactory;
    typedef CObjPool<CRWLockHolder, THolderPoolFactory>  THolderPool;

    /// Implementation of CRWLockHolder objects pool
    THolderPool  m_Pool;
};


inline
CRWLockHolder_Pool::CRWLockHolder_Pool(void)
    : m_Pool(THolderPoolFactory(this))
{}

CRWLockHolder_Pool::~CRWLockHolder_Pool(void)
{}

CRWLockHolder*
CRWLockHolder_Pool::CreateHolder(CYieldingRWLock* lock, ERWLockType typ)
{
    CRWLockHolder* holder = m_Pool.Get();
    holder->Init(lock, typ);
    return holder;
}

void
CRWLockHolder_Pool::DeleteHolder(CRWLockHolder* holder)
{
    _ASSERT(!holder->Referenced());

    holder->Reset();
    m_Pool.Return(holder);
}


/// Default CRWLockHolder pool used in CYieldingRWLock
static CSafeStatic<CRWLockHolder_Pool> s_RWHolderPool;


CYieldingRWLock::CYieldingRWLock(IRWLockHolder_Factory* factory /* = NULL */)
    : m_Factory(factory)
{
    if (!m_Factory) {
        m_Factory = &s_RWHolderPool.Get();
    }
    m_Locks[eReadLock] = m_Locks[eWriteLock] = 0;
}

CYieldingRWLock::~CYieldingRWLock(void)
{
#ifdef _DEBUG
# define RWLockFatal Fatal
#else
# define RWLockFatal Critical
#endif

    CSpinGuard guard(m_ObjLock);

    if (m_Locks[eReadLock] + m_Locks[eWriteLock] != 0) {
        ERR_POST_X(1, RWLockFatal
                      << "Destroying YieldingRWLock with unreleased locks");
    }
    if (!m_LockWaits.empty()) {
        ERR_POST_X(2, RWLockFatal
                      << "Destroying YieldingRWLock with "
                         "some locks waiting to acquire");
    }

#undef RWLockFatal
}

TRWLockHolderRef
CYieldingRWLock::AcquireLock(ERWLockType lock_type)
{
    int other_type = 1 - lock_type;
    TRWLockHolderRef holder(m_Factory->CreateHolder(this, lock_type));

    {{
        CSpinGuard guard(m_ObjLock);

        if (m_Locks[other_type] != 0  ||  !m_LockWaits.empty()
            ||  (lock_type == eWriteLock  &&  m_Locks[lock_type] != 0))
        {
            m_LockWaits.push_back(holder);
            return holder;
        }

        ++m_Locks[lock_type];
        holder->m_LockAcquired = true;
    }}

    holder->x_OnLockAcquired();
    return holder;
}

void
CYieldingRWLock::x_ReleaseLock(CRWLockHolder* holder)
{
    // Extract one lock holder from next_holders to avoid unnecessary memory
    // allocations in deque in case when we grant lock to only one next holder
    // (the majority of cases).
    TRWLockHolderRef first_next;
    THoldersList next_holders;
    bool save_acquired;

    {{
        CSpinGuard guard(m_ObjLock);

        save_acquired = holder->m_LockAcquired;
        if (save_acquired) {
            --m_Locks[holder->m_Type];
            holder->m_LockAcquired = false;

            if (m_Locks[eReadLock] + m_Locks[eWriteLock] == 0
                &&  !m_LockWaits.empty())
            {
                first_next = m_LockWaits.front();
                m_LockWaits.pop_front();
                ERWLockType next_type = first_next->m_Type;
                first_next->m_LockAcquired = true;
                ++m_Locks[next_type];

                while (next_type == eReadLock  &&  !m_LockWaits.empty()) {
                    TRWLockHolderRef next_hldr = m_LockWaits.front();
                    if (next_hldr->m_Type != next_type)
                        break;

                    next_hldr->m_LockAcquired = true;
                    ++m_Locks[next_type];
                    next_holders.push_back(next_hldr);
                    m_LockWaits.pop_front();
                }
            }
        }
        else {
            THoldersList::iterator it
                       = find(m_LockWaits.begin(), m_LockWaits.end(), holder);
            if (it != m_LockWaits.end()) {
                m_LockWaits.erase(it);
            }
        }
    }}

    if (save_acquired) {
        holder->x_OnLockReleased();
    }
    if (first_next.NotNull()) {
        first_next->x_OnLockAcquired();
    }
    NON_CONST_ITERATE(THoldersList, it, next_holders) {
        (*it)->x_OnLockAcquired();
    }
}


// All methods must be not inline to avoid unwanted optimizations by compiler
void
CSpinLock::Lock(void)
{
retry:
    while (m_Value != NULL)
        NCBI_SCHED_YIELD();
    if (SwapPointers(&m_Value, (void*)1) != NULL)
        goto retry;
}

bool
CSpinLock::TryLock(void)
{
    return SwapPointers(&m_Value, (void*)1) == NULL;
}

void
CSpinLock::Unlock(void)
{
#ifdef _DEBUG
    _VERIFY(SwapPointers(&m_Value, NULL) != NULL);
#else
    m_Value = NULL;
#endif
}




/////////////////////////////////////////////////////////////////////////////
//  CONDITION VARIABLE
//

bool CConditionVariable::IsSupported(void)
{
    return true;
}


CConditionVariable::CConditionVariable(void)
#if defined(NCBI_THREADS)
    : m_WaitCounter(0),
      m_WaitMutex(NULL)
#endif      // defined(NCBI_THREADS)
{
#if defined(NCBI_THREADS)
#if defined(NCBI_OS_MSWIN)
    InitializeConditionVariable(&m_ConditionVar);
#else
    int res = pthread_cond_init(&m_ConditionVar, NULL);
    switch (res) {
    case 0:
        break;
    case EAGAIN:
        NCBI_THROW(CConditionVariableException, eInvalidValue,
                   "CConditionVariable: not enough resources");
    case ENOMEM:
        NCBI_THROW(CConditionVariableException, eInvalidValue,
                   "CConditionVariable: not enough memory");
    case EBUSY:
        NCBI_THROW(CConditionVariableException, eInvalidValue,
                   "CConditionVariable: attempt to reinitialize"
                   " already used variable");
    case EINVAL:
        NCBI_THROW(CConditionVariableException, eInvalidValue,
                   "CConditionVariable: invalid attribute value");
    default:
        NCBI_THROW(CConditionVariableException, eInvalidValue,
                   "CConditionVariable: unknown error");
    }
#endif
#endif      // defined(NCBI_THREADS)
}


CConditionVariable::~CConditionVariable(void)
{
#if defined(NCBI_THREADS)
#if !defined(NCBI_OS_MSWIN)
    int res = pthread_cond_destroy(&m_ConditionVar);
    switch (res) {
    case 0:
        return;
    case EBUSY:
        ERR_POST(Critical <<
                 "~CConditionVariable: attempt to destroy variable that"
                 " is currently in use");
        break;
    case EINVAL:
        ERR_POST(Critical <<
                 "~CConditionVariable: invalid condition variable");
        break;
    default:
        ERR_POST(Critical <<
                 "~CConditionVariable: unknown error");
    }
    NCBI_TROUBLE("CConditionVariable: pthread_cond_destroy() failed");
#endif
#endif      // defined(NCBI_THREADS)
}


#if defined(NCBI_THREADS)
template <class T>
class CQuickAndDirtySamePointerGuard
{
public:
    typedef T* TPointer;

    CQuickAndDirtySamePointerGuard(CAtomicCounter&    atomic_counter,
                                   TPointer volatile& guarded_ptr,
                                   TPointer           new_ptr)
        : m_AtomicCounter (atomic_counter),
          m_GuardedPtr    (guarded_ptr),
          m_SavedPtr      (new_ptr)
    {
        _ASSERT(new_ptr != NULL);
        m_AtomicCounter.Add(1);
        // If two threads enter the guard simultaneously, and with different
        // pointers, then we may not detect this. There is chance however that
        // we 'll detect it -- in DEBUG mode only -- in dtor. And if not, then:
        // Oh well. We don't promise 100% protection.  It is good enough
        // though, and what's important -- no false negatives.
        m_GuardedPtr = new_ptr;
    }

    bool DetectedDifferentPointers(void)
    {
        if ((m_SavedPtr == NULL)  ||
            (m_GuardedPtr != NULL  &&  m_GuardedPtr != m_SavedPtr)) {
            NCBI_TROUBLE("Different pointers detected");
            m_SavedPtr = NULL;
            return true;
        }
        return false;
    }

    ~CQuickAndDirtySamePointerGuard() {
        _ASSERT( !DetectedDifferentPointers() );
        if (m_AtomicCounter.Add(-1) == 0) {
            // If other thread goes into the guard at this very moment, then
            // this thread can NULL up the protection, and won't detect
            // any same-ptr cases until after the counter goes back to zero.
            // Oh well. We don't promise 100% protection.  It is good enough
            // though, and what's important -- no false negatives.
            m_GuardedPtr = NULL;
        }
    }

private:
    CAtomicCounter&     m_AtomicCounter;
    volatile TPointer&  m_GuardedPtr;
    TPointer            m_SavedPtr;
};


inline void s_ThrowIfDifferentMutexes
(CQuickAndDirtySamePointerGuard<SSystemFastMutex>& mutex_guard)
{
    if ( mutex_guard.DetectedDifferentPointers() ) {
        NCBI_THROW(CConditionVariableException, eMutexDifferent,
                   "WaitForSignal called with different mutexes");
    }
}


bool CConditionVariable::x_WaitForSignal
(SSystemFastMutex&  mutex, const CDeadline&  deadline)
{
    CQuickAndDirtySamePointerGuard<SSystemFastMutex> mutex_guard
        (m_WaitCounter, m_WaitMutex, &mutex);
    s_ThrowIfDifferentMutexes(mutex_guard);

#if defined(NCBI_OS_MSWIN)
    DWORD timeout_msec = deadline.IsInfinite() ?
        INFINITE : deadline.GetRemainingTime().GetAsMilliSeconds();
#if defined(NCBI_FASTMUTEX_USE_NEW)
    BOOL res = SleepConditionVariableSRW(&m_ConditionVar, &mutex.m_Handle, timeout_msec, 0);
#else
    BOOL res = SleepConditionVariableCS(&m_ConditionVar, &mutex.m_Handle, timeout_msec);
#endif
    s_ThrowIfDifferentMutexes(mutex_guard);

    if ( res )
        return true;

    DWORD err_code = GetLastError();
    if (err_code == ERROR_TIMEOUT  ||  err_code == WAIT_TIMEOUT)
        return false;

    NCBI_THROW(CConditionVariableException, eInvalidValue,
               "WaitForSignal failed");
#else
    int res;
    if (deadline.IsInfinite()) {
        res = pthread_cond_wait(&m_ConditionVar, &mutex.m_Handle);
    } else {
        struct timespec ts;
        time_t s;
        unsigned int ns;
        deadline.GetExpirationTime(&s, &ns);
        ts.tv_sec = s;
        ts.tv_nsec = ns;
        res = pthread_cond_timedwait(&m_ConditionVar, &mutex.m_Handle, &ts);
    }
    s_ThrowIfDifferentMutexes(mutex_guard);

    if (res != 0) {
        switch (res) {
        case ETIMEDOUT:
            return false;
        case EINVAL:
            NCBI_THROW(CConditionVariableException, eInvalidValue,
                       "WaitForSignal failed: invalid paramater");
        case EPERM:
            NCBI_THROW(CConditionVariableException, eMutexOwner,
                       "WaitForSignal: mutex not owned by the current thread");
        default:
            NCBI_THROW(CConditionVariableException, eInvalidValue,
                       "WaitForSignal failed: unknown error");
        }
    }
#endif

    return true;
}
#endif      // defined(NCBI_THREADS)


bool CConditionVariable::WaitForSignal(CMutex&           mutex,
                                       const CDeadline&  deadline)
{
#if defined(NCBI_THREADS)
    SSystemMutex& sys_mtx = mutex;
    if (sys_mtx.m_Count != 1) {
        NCBI_THROW(CConditionVariableException, eMutexLockCount,
                   "WaitForSignal: mutex lock count not 1");
    }
#if defined(NCBI_OS_MSWIN)
    if ( sys_mtx.m_Owner != GetCurrentThreadSystemID() ) {
        NCBI_THROW(CConditionVariableException, eMutexOwner,
                   "WaitForSignal: mutex not owned by the current thread");
    }
#endif
    sys_mtx.Unlock(SSystemFastMutex::ePseudo);
    bool res = x_WaitForSignal(sys_mtx.m_Mutex, deadline);
    sys_mtx.Lock(SSystemFastMutex::ePseudo);
    return res;
#else
    return true;
#endif      // defined(NCBI_THREADS)
}


bool CConditionVariable::WaitForSignal(CFastMutex&       mutex,
                                       const CDeadline&  deadline)
{
#if defined(NCBI_THREADS)
    SSystemFastMutex& sys_mtx = mutex;
    sys_mtx.Unlock(SSystemFastMutex::ePseudo);
    bool res = x_WaitForSignal(sys_mtx, deadline);
    sys_mtx.Lock(SSystemFastMutex::ePseudo);
    return res;
#else
    return true;
#endif      // defined(NCBI_THREADS)
}


void CConditionVariable::SignalSome(void)
{
#if defined(NCBI_THREADS)
#if defined(NCBI_OS_MSWIN)
    WakeConditionVariable(&m_ConditionVar);
#else
    int res = pthread_cond_signal(&m_ConditionVar);
    if (res != 0) {
        switch (res) {
        case EINVAL:
            NCBI_THROW(CConditionVariableException, eInvalidValue,
                       "SignalSome failed: invalid paramater");
        default:
            NCBI_THROW(CConditionVariableException, eInvalidValue,
                       "SignalSome failed: unknown error");
        }
    }
#endif
#endif      // defined(NCBI_THREADS)
}


void CConditionVariable::SignalAll(void)
{
#if defined(NCBI_THREADS)
#if defined(NCBI_OS_MSWIN)
    WakeAllConditionVariable(&m_ConditionVar);
#else
    int res = pthread_cond_broadcast(&m_ConditionVar);
    if (res != 0) {
        switch (res) {
        case EINVAL:
            NCBI_THROW(CConditionVariableException, eInvalidValue,
                       "SignalAll failed: invalid paramater");
        default:
            NCBI_THROW(CConditionVariableException, eInvalidValue,
                       "SignalAll failed: unknown error");
        }
    }
#endif
#endif      // defined(NCBI_THREADS)
}


const char* CConditionVariableException::GetErrCodeString(void) const
{
    switch ( GetErrCode() ) {
    case eInvalidValue:   return "eInvalidValue";
    case eMutexLockCount: return "eMutexLockCount";
    case eMutexOwner:     return "eMutexOwner";
    case eMutexDifferent: return "eMutexDifferent";
    case eUnsupported:    return "eUnsupported";
    default:              return CException::GetErrCodeString();
    }
}

END_NCBI_SCOPE
