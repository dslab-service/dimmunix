/*
** 2004 May 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains code that is specific to Unix systems.
*/
#include "sqliteInt.h"
#include "os.h"
#include <unistd.h>

#if OS_UNIX              /* This file is used on unix only */

/*
** These #defines should enable >2GB file support on Posix if the
** underlying operating system supports it.  If the OS lacks
** large file support, these should be no-ops.
**
** Large file support can be disabled using the -DSQLITE_DISABLE_LFS switch
** on the compiler command line.  This is necessary if you are compiling
** on a recent machine (ex: RedHat 7.2) but you want your code to work
** on an older machine (ex: RedHat 6.0).  If you compile on RedHat 7.2
** without this option, LFS is enable.  But LFS does not exist in the kernel
** in RedHat 6.0, so the code won't work.  Hence, for maximum binary
** portability you should omit LFS.
*/
#ifndef SQLITE_DISABLE_LFS
# define _LARGE_FILE       1
# ifndef _FILE_OFFSET_BITS
#   define _FILE_OFFSET_BITS 64
# endif
# define _LARGEFILE_SOURCE 1
#endif

/*
** standard include files.
*/
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
//#include <linux/unistd.h>

/*
** If we are to be thread-safe, include the pthreads header and define
** the SQLITE_UNIX_THREADS macro.
*/
#if defined(THREADSAFE) && THREADSAFE
# include <pthread.h>
# define SQLITE_UNIX_THREADS 1
#endif





/*
** Default permissions when creating a new file
*/
#ifndef SQLITE_DEFAULT_FILE_PERMISSIONS
# define SQLITE_DEFAULT_FILE_PERMISSIONS 0644
#endif



/*
** The unixFile structure is subclass of OsFile specific for the unix
** protability layer.
*/
typedef struct unixFile unixFile;
struct unixFile {
  IoMethod const *pMethod;  /* Always the first entry */
  struct openCnt *pOpen;    /* Info about all open fd's on this inode */
  struct lockInfo *pLock;   /* Info about locks on this inode */
  int h;                    /* The file descriptor */
  unsigned char locktype;   /* The type of lock held on this fd */
  unsigned char isOpen;     /* True if needs to be closed */
  unsigned char fullSync;   /* Use F_FULLSYNC if available */
  int dirfd;                /* File descriptor for the directory */
#ifdef SQLITE_UNIX_THREADS
  pthread_t tid;            /* The thread that "owns" this OsFile */
#endif
};

/*
** Provide the ability to override some OS-layer functions during
** testing.  This is used to simulate OS crashes to verify that
** commits are atomic even in the event of an OS crash.
*/
#ifdef SQLITE_CRASH_TEST
  extern int sqlite3CrashTestEnable;
  extern int sqlite3CrashOpenReadWrite(const char*, OsFile**, int*);
  extern int sqlite3CrashOpenExclusive(const char*, OsFile**, int);
  extern int sqlite3CrashOpenReadOnly(const char*, OsFile**, int);
# define CRASH_TEST_OVERRIDE(X,A,B,C) \
    if(sqlite3CrashTestEnable){ return X(A,B,C); }
#else
# define CRASH_TEST_OVERRIDE(X,A,B,C)  /* no-op */
#endif


/*
** Include code that is common to all os_*.c files
*/
#include "os_common.h"

/*
** Do not include any of the File I/O interface procedures if the
** SQLITE_OMIT_DISKIO macro is defined (indicating that the database
** will be in-memory only)
*/
#ifndef SQLITE_OMIT_DISKIO


/*
** Define various macros that are missing from some systems.
*/
#ifndef O_LARGEFILE
# define O_LARGEFILE 0
#endif
#ifdef SQLITE_DISABLE_LFS
# undef O_LARGEFILE
# define O_LARGEFILE 0
#endif
#ifndef O_NOFOLLOW
# define O_NOFOLLOW 0
#endif
#ifndef O_BINARY
# define O_BINARY 0
#endif

/*
** The DJGPP compiler environment looks mostly like Unix, but it
** lacks the fcntl() system call.  So redefine fcntl() to be something
** that always succeeds.  This means that locking does not occur under
** DJGPP.  But it's DOS - what did you expect?
*/
#ifdef __DJGPP__
# define fcntl(A,B,C) 0
#endif

/*
** The threadid macro resolves to the thread-id or to 0.  Used for
** testing and debugging only.
*/
#ifdef SQLITE_UNIX_THREADS
#define threadid pthread_self()
#else
#define threadid 0
#endif

/*
** Set or check the OsFile.tid field.  This field is set when an OsFile
** is first opened.  All subsequent uses of the OsFile verify that the
** same thread is operating on the OsFile.  Some operating systems do
** not allow locks to be overridden by other threads and that restriction
** means that sqlite3* database handles cannot be moved from one thread
** to another.  This logic makes sure a user does not try to do that
** by mistake.
**
** Version 3.3.1 (2006-01-15):  OsFiles can be moved from one thread to
** another as long as we are running on a system that supports threads
** overriding each others locks (which now the most common behavior)
** or if no locks are held.  But the OsFile.pLock field needs to be
** recomputed because its key includes the thread-id.  See the
** transferOwnership() function below for additional information
*/
#if defined(SQLITE_UNIX_THREADS)
# define SET_THREADID(X)   (X)->tid = pthread_self()
# define CHECK_THREADID(X) (threadsOverrideEachOthersLocks==0 && \
                            !pthread_equal((X)->tid, pthread_self()))
#else
# define SET_THREADID(X)
# define CHECK_THREADID(X) 0
#endif

/*
** Here is the dirt on POSIX advisory locks:  ANSI STD 1003.1 (1996)
** section 6.5.2.2 lines 483 through 490 specify that when a process
** sets or clears a lock, that operation overrides any prior locks set
** by the same process.  It does not explicitly say so, but this implies
** that it overrides locks set by the same process using a different
** file descriptor.  Consider this test case:
**
**       int fd1 = open("./file1", O_RDWR|O_CREAT, 0644);
**       int fd2 = open("./file2", O_RDWR|O_CREAT, 0644);
**
** Suppose ./file1 and ./file2 are really the same file (because
** one is a hard or symbolic link to the other) then if you set
** an exclusive lock on fd1, then try to get an exclusive lock
** on fd2, it works.  I would have expected the second lock to
** fail since there was already a lock on the file due to fd1.
** But not so.  Since both locks came from the same process, the
** second overrides the first, even though they were on different
** file descriptors opened on different file names.
**
** Bummer.  If you ask me, this is broken.  Badly broken.  It means
** that we cannot use POSIX locks to synchronize file access among
** competing threads of the same process.  POSIX locks will work fine
** to synchronize access for threads in separate processes, but not
** threads within the same process.
**
** To work around the problem, SQLite has to manage file locks internally
** on its own.  Whenever a new database is opened, we have to find the
** specific inode of the database file (the inode is determined by the
** st_dev and st_ino fields of the stat structure that fstat() fills in)
** and check for locks already existing on that inode.  When locks are
** created or removed, we have to look at our own internal record of the
** locks to see if another thread has previously set a lock on that same
** inode.
**
** The OsFile structure for POSIX is no longer just an integer file
** descriptor.  It is now a structure that holds the integer file
** descriptor and a pointer to a structure that describes the internal
** locks on the corresponding inode.  There is one locking structure
** per inode, so if the same inode is opened twice, both OsFile structures
** point to the same locking structure.  The locking structure keeps
** a reference count (so we will know when to delete it) and a "cnt"
** field that tells us its internal lock status.  cnt==0 means the
** file is unlocked.  cnt==-1 means the file has an exclusive lock.
** cnt>0 means there are cnt shared locks on the file.
**
** Any attempt to lock or unlock a file first checks the locking
** structure.  The fcntl() system call is only invoked to set a
** POSIX lock if the internal lock structure transitions between
** a locked and an unlocked state.
**
** 2004-Jan-11:
** More recent discoveries about POSIX advisory locks.  (The more
** I discover, the more I realize the a POSIX advisory locks are
** an abomination.)
**
** If you close a file descriptor that points to a file that has locks,
** all locks on that file that are owned by the current process are
** released.  To work around this problem, each OsFile structure contains
** a pointer to an openCnt structure.  There is one openCnt structure
** per open inode, which means that multiple OsFiles can point to a single
** openCnt.  When an attempt is made to close an OsFile, if there are
** other OsFiles open on the same inode that are holding locks, the call
** to close() the file descriptor is deferred until all of the locks clear.
** The openCnt structure keeps a list of file descriptors that need to
** be closed and that list is walked (and cleared) when the last lock
** clears.
**
** First, under Linux threads, because each thread has a separate
** process ID, lock operations in one thread do not override locks
** to the same file in other threads.  Linux threads behave like
** separate processes in this respect.  But, if you close a file
** descriptor in linux threads, all locks are cleared, even locks
** on other threads and even though the other threads have different
** process IDs.  Linux threads is inconsistent in this respect.
** (I'm beginning to think that linux threads is an abomination too.)
** The consequence of this all is that the hash table for the lockInfo
** structure has to include the process id as part of its key because
** locks in different threads are treated as distinct.  But the
** openCnt structure should not include the process id in its
** key because close() clears lock on all threads, not just the current
** thread.  Were it not for this goofiness in linux threads, we could
** combine the lockInfo and openCnt structures into a single structure.
**
** 2004-Jun-28:
** On some versions of linux, threads can override each others locks.
** On others not.  Sometimes you can change the behavior on the same
** system by setting the LD_ASSUME_KERNEL environment variable.  The
** POSIX standard is silent as to which behavior is correct, as far
** as I can tell, so other versions of unix might show the same
** inconsistency.  There is no little doubt in my mind that posix
** advisory locks and linux threads are profoundly broken.
**
** To work around the inconsistencies, we have to test at runtime
** whether or not threads can override each others locks.  This test
** is run once, the first time any lock is attempted.  A static
** variable is set to record the results of this test for future
** use.
*/

/*
** An instance of the following structure serves as the key used
** to locate a particular lockInfo structure given its inode.
**
** If threads cannot override each others locks, then we set the
** lockKey.tid field to the thread ID.  If threads can override
** each others locks then tid is always set to zero.  tid is omitted
** if we compile without threading support.
*/
struct lockKey {
  dev_t dev;       /* Device number */
  ino_t ino;       /* Inode number */
#ifdef SQLITE_UNIX_THREADS
  pthread_t tid;   /* Thread ID or zero if threads can override each other */
#endif
};

/*
** An instance of the following structure is allocated for each open
** inode on each thread with a different process ID.  (Threads have
** different process IDs on linux, but not on most other unixes.)
**
** A single inode can have multiple file descriptors, so each OsFile
** structure contains a pointer to an instance of this object and this
** object keeps a count of the number of OsFiles pointing to it.
*/
struct lockInfo {
  struct lockKey key;  /* The lookup key */
  int cnt;             /* Number of SHARED locks held */
  int locktype;        /* One of SHARED_LOCK, RESERVED_LOCK etc. */
  int nRef;            /* Number of pointers to this structure */
};

/*
** An instance of the following structure serves as the key used
** to locate a particular openCnt structure given its inode.  This
** is the same as the lockKey except that the thread ID is omitted.
*/
struct openKey {
  dev_t dev;   /* Device number */
  ino_t ino;   /* Inode number */
};

/*
** An instance of the following structure is allocated for each open
** inode.  This structure keeps track of the number of locks on that
** inode.  If a close is attempted against an inode that is holding
** locks, the close is deferred until all locks clear by adding the
** file descriptor to be closed to the pending list.
*/
struct openCnt {
  struct openKey key;   /* The lookup key */
  int nRef;             /* Number of pointers to this structure */
  int nLock;            /* Number of outstanding locks */
  int nPending;         /* Number of pending close() operations */
  int *aPending;        /* Malloced space holding fd's awaiting a close() */
};

/*
** These hash tables map inodes and file descriptors (really, lockKey and
** openKey structures) into lockInfo and openCnt structures.  Access to
** these hash tables must be protected by a mutex.
*/
static Hash lockHash = { SQLITE_HASH_BINARY, 0, 0, 0, 0, 0 };
static Hash openHash = { SQLITE_HASH_BINARY, 0, 0, 0, 0, 0 };


#ifdef SQLITE_UNIX_THREADS
/*
** This variable records whether or not threads can override each others
** locks.
**
**    0:  No.  Threads cannot override each others locks.
**    1:  Yes.  Threads can override each others locks.
**   -1:  We don't know yet.
**
** On some systems, we know at compile-time if threads can override each
** others locks.  On those systems, the SQLITE_THREAD_OVERRIDE_LOCK macro
** will be set appropriately.  On other systems, we have to check at
** runtime.  On these latter systems, SQLTIE_THREAD_OVERRIDE_LOCK is
** undefined.
**
** This variable normally has file scope only.  But during testing, we make
** it a global so that the test code can change its value in order to verify
** that the right stuff happens in either case.
*/
#ifndef SQLITE_THREAD_OVERRIDE_LOCK
# define SQLITE_THREAD_OVERRIDE_LOCK -1
#endif
#ifdef SQLITE_TEST
int threadsOverrideEachOthersLocks = SQLITE_THREAD_OVERRIDE_LOCK;
#else
static int threadsOverrideEachOthersLocks = SQLITE_THREAD_OVERRIDE_LOCK;
#endif

/*
** This structure holds information passed into individual test
** threads by the testThreadLockingBehavior() routine.
*/
struct threadTestData {
  int fd;                /* File to be locked */
  struct flock lock;     /* The locking operation */
  int result;            /* Result of the locking operation */
};

#ifdef SQLITE_LOCK_TRACE
/*
** Print out information about all locking operations.
**
** This routine is used for troubleshooting locks on multithreaded
** platforms.  Enable by compiling with the -DSQLITE_LOCK_TRACE
** command-line option on the compiler.  This code is normally
** turned off.
*/
static int lockTrace(int fd, int op, struct flock *p){
  char *zOpName, *zType;
  int s;
  int savedErrno;
  if( op==F_GETLK ){
    zOpName = "GETLK";
  }else if( op==F_SETLK ){
    zOpName = "SETLK";
  }else{
    s = fcntl(fd, op, p);
    sqlite3DebugPrintf("fcntl unknown %d %d %d\n", fd, op, s);
    return s;
  }
  if( p->l_type==F_RDLCK ){
    zType = "RDLCK";
  }else if( p->l_type==F_WRLCK ){
    zType = "WRLCK";
  }else if( p->l_type==F_UNLCK ){
    zType = "UNLCK";
  }else{
    assert( 0 );
  }
  assert( p->l_whence==SEEK_SET );
  s = fcntl(fd, op, p);
  savedErrno = errno;
  sqlite3DebugPrintf("fcntl %d %d %s %s %d %d %d %d\n",
     threadid, fd, zOpName, zType, (int)p->l_start, (int)p->l_len,
     (int)p->l_pid, s);
  if( s && op==F_SETLK && (p->l_type==F_RDLCK || p->l_type==F_WRLCK) ){
    struct flock l2;
    l2 = *p;
    fcntl(fd, F_GETLK, &l2);
    if( l2.l_type==F_RDLCK ){
      zType = "RDLCK";
    }else if( l2.l_type==F_WRLCK ){
      zType = "WRLCK";
    }else if( l2.l_type==F_UNLCK ){
      zType = "UNLCK";
    }else{
      assert( 0 );
    }
    sqlite3DebugPrintf("fcntl-failure-reason: %s %d %d %d\n",
       zType, (int)l2.l_start, (int)l2.l_len, (int)l2.l_pid);
  }
  errno = savedErrno;
  return s;
}
#define fcntl lockTrace
#endif /* SQLITE_LOCK_TRACE */

/*
** The testThreadLockingBehavior() routine launches two separate
** threads on this routine.  This routine attempts to lock a file
** descriptor then returns.  The success or failure of that attempt
** allows the testThreadLockingBehavior() procedure to determine
** whether or not threads can override each others locks.
*/
static void *threadLockingTest(void *pArg){
  struct threadTestData *pData = (struct threadTestData*)pArg;
  pData->result = fcntl(pData->fd, F_SETLK, &pData->lock);
  return pArg;
}

/*
** This procedure attempts to determine whether or not threads
** can override each others locks then sets the
** threadsOverrideEachOthersLocks variable appropriately.
*/
static void testThreadLockingBehavior(int fd_orig){
  /* int fd; */
/*   struct threadTestData d[2]; */
/*   pthread_t t[2]; */

/*   fd = dup(fd_orig); */
/*   if( fd<0 ) return; */
/*   memset(d, 0, sizeof(d)); */
/*   d[0].fd = fd; */
/*   d[0].lock.l_type = F_RDLCK; */
/*   d[0].lock.l_len = 1; */
/*   d[0].lock.l_start = 0; */
/*   d[0].lock.l_whence = SEEK_SET; */
/*   d[1] = d[0]; */
/*   d[1].lock.l_type = F_WRLCK; */
/*   pthread_create(&t[0], 0, threadLockingTest, &d[0]); */
/*   pthread_create(&t[1], 0, threadLockingTest, &d[1]); */
/*   pthread_join(t[0], 0); */
/*   pthread_join(t[1], 0); */
/*   close(fd); */
/*   threadsOverrideEachOthersLocks =  d[0].result==0 && d[1].result==0;
 */

  threadsOverrideEachOthersLocks = 0;
}
#endif /* SQLITE_UNIX_THREADS */

/*
** Release a lockInfo structure previously allocated by findLockInfo().
*/
static void releaseLockInfo(struct lockInfo *pLock){
  assert( sqlite3OsInMutex(1) );
  pLock->nRef--;
  if( pLock->nRef==0 ){
    sqlite3HashInsert(&lockHash, &pLock->key, sizeof(pLock->key), 0);
    sqliteFree(pLock);
  }
}

/*
** Release a openCnt structure previously allocated by findLockInfo().
*/
static void releaseOpenCnt(struct openCnt *pOpen){
  assert( sqlite3OsInMutex(1) );
  pOpen->nRef--;
  if( pOpen->nRef==0 ){
    sqlite3HashInsert(&openHash, &pOpen->key, sizeof(pOpen->key), 0);
    free(pOpen->aPending);
    sqliteFree(pOpen);
  }
}

/*
** Given a file descriptor, locate lockInfo and openCnt structures that
** describes that file descriptor.  Create new ones if necessary.  The
** return values might be uninitialized if an error occurs.
**
** Return the number of errors.
*/
static int findLockInfo(
  int fd,                      /* The file descriptor used in the key */
  struct lockInfo **ppLock,    /* Return the lockInfo structure here */
  struct openCnt **ppOpen      /* Return the openCnt structure here */
){
  int rc;
  struct lockKey key1;
  struct openKey key2;
  struct stat statbuf;
  struct lockInfo *pLock;
  struct openCnt *pOpen;
  rc = fstat(fd, &statbuf);
  if( rc!=0 ) return 1;

  assert( sqlite3OsInMutex(1) );
  memset(&key1, 0, sizeof(key1));
  key1.dev = statbuf.st_dev;
  key1.ino = statbuf.st_ino;
#ifdef SQLITE_UNIX_THREADS
  if( threadsOverrideEachOthersLocks<0 ){
    testThreadLockingBehavior(fd);
  }
  key1.tid = threadsOverrideEachOthersLocks ? 0 : pthread_self();
#endif
  memset(&key2, 0, sizeof(key2));
  key2.dev = statbuf.st_dev;
  key2.ino = statbuf.st_ino;
  pLock = (struct lockInfo*)sqlite3HashFind(&lockHash, &key1, sizeof(key1));
  if( pLock==0 ){
    struct lockInfo *pOld;
    pLock = sqliteMallocRaw( sizeof(*pLock) );
    if( pLock==0 ){
      rc = 1;
      goto exit_findlockinfo;
    }
    pLock->key = key1;
    pLock->nRef = 1;
    pLock->cnt = 0;
    pLock->locktype = 0;
    pOld = sqlite3HashInsert(&lockHash, &pLock->key, sizeof(key1), pLock);
    if( pOld!=0 ){
      assert( pOld==pLock );
      sqliteFree(pLock);
      rc = 1;
      goto exit_findlockinfo;
    }
  }else{
    pLock->nRef++;
  }
  *ppLock = pLock;
  if( ppOpen!=0 ){
    pOpen = (struct openCnt*)sqlite3HashFind(&openHash, &key2, sizeof(key2));
    if( pOpen==0 ){
      struct openCnt *pOld;
      pOpen = sqliteMallocRaw( sizeof(*pOpen) );
      if( pOpen==0 ){
        releaseLockInfo(pLock);
        rc = 1;
        goto exit_findlockinfo;
      }
      pOpen->key = key2;
      pOpen->nRef = 1;
      pOpen->nLock = 0;
      pOpen->nPending = 0;
      pOpen->aPending = 0;
      pOld = sqlite3HashInsert(&openHash, &pOpen->key, sizeof(key2), pOpen);
      if( pOld!=0 ){
        assert( pOld==pOpen );
        sqliteFree(pOpen);
        releaseLockInfo(pLock);
        rc = 1;
        goto exit_findlockinfo;
      }
    }else{
      pOpen->nRef++;
    }
    *ppOpen = pOpen;
  }

exit_findlockinfo:
  return rc;
}

#ifdef SQLITE_DEBUG
/*
** Helper function for printing out trace information from debugging
** binaries. This returns the string represetation of the supplied
** integer lock-type.
*/
static const char *locktypeName(int locktype){
  switch( locktype ){
  case NO_LOCK: return "NONE";
  case SHARED_LOCK: return "SHARED";
  case RESERVED_LOCK: return "RESERVED";
  case PENDING_LOCK: return "PENDING";
  case EXCLUSIVE_LOCK: return "EXCLUSIVE";
  }
  return "ERROR";
}
#endif

/*
** If we are currently in a different thread than the thread that the
** unixFile argument belongs to, then transfer ownership of the unixFile
** over to the current thread.
**
** A unixFile is only owned by a thread on systems where one thread is
** unable to override locks created by a different thread.  RedHat9 is
** an example of such a system.
**
** Ownership transfer is only allowed if the unixFile is currently unlocked.
** If the unixFile is locked and an ownership is wrong, then return
** SQLITE_MISUSE.  SQLITE_OK is returned if everything works.
*/
#ifdef SQLITE_UNIX_THREADS
static int transferOwnership(unixFile *pFile){
  int rc;
  pthread_t hSelf;
  if( threadsOverrideEachOthersLocks ){
    /* Ownership transfers not needed on this system */
    return SQLITE_OK;
  }
  hSelf = pthread_self();
  if( pthread_equal(pFile->tid, hSelf) ){
    /* We are still in the same thread */
    TRACE1("No-transfer, same thread\n");
    return SQLITE_OK;
  }
  if( pFile->locktype!=NO_LOCK ){
    /* We cannot change ownership while we are holding a lock! */
    return SQLITE_MISUSE;
  }
  TRACE4("Transfer ownership of %d from %d to %d\n", pFile->h,pFile->tid,hSelf);
  pFile->tid = hSelf;
  releaseLockInfo(pFile->pLock);
  rc = findLockInfo(pFile->h, &pFile->pLock, 0);
  TRACE5("LOCK    %d is now %s(%s,%d)\n", pFile->h,
     locktypeName(pFile->locktype),
     locktypeName(pFile->pLock->locktype), pFile->pLock->cnt);
  return rc;
}
#else
  /* On single-threaded builds, ownership transfer is a no-op */
# define transferOwnership(X) SQLITE_OK
#endif

/*
** Delete the named file
*/
int sqlite3UnixDelete(const char *zFilename){
  unlink(zFilename);
  return SQLITE_OK;
}

/*
** Return TRUE if the named file exists.
*/
int sqlite3UnixFileExists(const char *zFilename){
  return access(zFilename, 0)==0;
}

/* Forward declaration */
static int allocateUnixFile(unixFile *pInit, OsFile **pId);

/*
** Attempt to open a file for both reading and writing.  If that
** fails, try opening it read-only.  If the file does not exist,
** try to create it.
**
** On success, a handle for the open file is written to *id
** and *pReadonly is set to 0 if the file was opened for reading and
** writing or 1 if the file was opened read-only.  The function returns
** SQLITE_OK.
**
** On failure, the function returns SQLITE_CANTOPEN and leaves
** *id and *pReadonly unchanged.
*/
int sqlite3UnixOpenReadWrite(
  const char *zFilename,
  OsFile **pId,
  int *pReadonly
){
  int rc;
  unixFile f;

  CRASH_TEST_OVERRIDE(sqlite3CrashOpenReadWrite, zFilename, pId, pReadonly);
  assert( 0==*pId );
  f.h = open(zFilename, O_RDWR|O_CREAT|O_LARGEFILE|O_BINARY,
                          SQLITE_DEFAULT_FILE_PERMISSIONS);
  if( f.h<0 ){
#ifdef EISDIR
    if( errno==EISDIR ){
      return SQLITE_CANTOPEN;
    }
#endif
    f.h = open(zFilename, O_RDONLY|O_LARGEFILE|O_BINARY);
    if( f.h<0 ){
      return SQLITE_CANTOPEN;
    }
    *pReadonly = 1;
  }else{
    *pReadonly = 0;
  }
  sqlite3OsEnterMutex();
  rc = findLockInfo(f.h, &f.pLock, &f.pOpen);
  sqlite3OsLeaveMutex();
  if( rc ){
    close(f.h);
    return SQLITE_NOMEM;
  }
  TRACE3("OPEN    %-3d %s\n", f.h, zFilename);
  return allocateUnixFile(&f, pId);
}


/*
** Attempt to open a new file for exclusive access by this process.
** The file will be opened for both reading and writing.  To avoid
** a potential security problem, we do not allow the file to have
** previously existed.  Nor do we allow the file to be a symbolic
** link.
**
** If delFlag is true, then make arrangements to automatically delete
** the file when it is closed.
**
** On success, write the file handle into *id and return SQLITE_OK.
**
** On failure, return SQLITE_CANTOPEN.
*/
int sqlite3UnixOpenExclusive(const char *zFilename, OsFile **pId, int delFlag){
  int rc;
  unixFile f;

  CRASH_TEST_OVERRIDE(sqlite3CrashOpenExclusive, zFilename, pId, delFlag);
  assert( 0==*pId );
  if( access(zFilename, 0)==0 ){
    return SQLITE_CANTOPEN;
  }
  f.h = open(zFilename,
                O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_LARGEFILE|O_BINARY,
                SQLITE_DEFAULT_FILE_PERMISSIONS);
  if( f.h<0 ){
    return SQLITE_CANTOPEN;
  }
  sqlite3OsEnterMutex();
  rc = findLockInfo(f.h, &f.pLock, &f.pOpen);
  sqlite3OsLeaveMutex();
  if( rc ){
    close(f.h);
    unlink(zFilename);
    return SQLITE_NOMEM;
  }
  if( delFlag ){
    unlink(zFilename);
  }
  TRACE3("OPEN-EX %-3d %s\n", f.h, zFilename);
  return allocateUnixFile(&f, pId);
}

/*
** Attempt to open a new file for read-only access.
**
** On success, write the file handle into *id and return SQLITE_OK.
**
** On failure, return SQLITE_CANTOPEN.
*/
int sqlite3UnixOpenReadOnly(const char *zFilename, OsFile **pId){
  int rc;
  unixFile f;

  CRASH_TEST_OVERRIDE(sqlite3CrashOpenReadOnly, zFilename, pId, 0);
  assert( 0==*pId );
  f.h = open(zFilename, O_RDONLY|O_LARGEFILE|O_BINARY);
  if( f.h<0 ){
    return SQLITE_CANTOPEN;
  }
  sqlite3OsEnterMutex();
  rc = findLockInfo(f.h, &f.pLock, &f.pOpen);
  sqlite3OsLeaveMutex();
  if( rc ){
    close(f.h);
    return SQLITE_NOMEM;
  }
  TRACE3("OPEN-RO %-3d %s\n", f.h, zFilename);
  return allocateUnixFile(&f, pId);
}

/*
** Attempt to open a file descriptor for the directory that contains a
** file.  This file descriptor can be used to fsync() the directory
** in order to make sure the creation of a new file is actually written
** to disk.
**
** This routine is only meaningful for Unix.  It is a no-op under
** windows since windows does not support hard links.
**
** On success, a handle for a previously open file at *id is
** updated with the new directory file descriptor and SQLITE_OK is
** returned.
**
** On failure, the function returns SQLITE_CANTOPEN and leaves
** *id unchanged.
*/
static int unixOpenDirectory(
  OsFile *id,
  const char *zDirname
){
  unixFile *pFile = (unixFile*)id;
  if( pFile==0 ){
    /* Do not open the directory if the corresponding file is not already
    ** open. */
    return SQLITE_CANTOPEN;
  }
  SET_THREADID(pFile);
  assert( pFile->dirfd<0 );
  pFile->dirfd = open(zDirname, O_RDONLY|O_BINARY, 0);
  if( pFile->dirfd<0 ){
    return SQLITE_CANTOPEN;
  }
  TRACE3("OPENDIR %-3d %s\n", pFile->dirfd, zDirname);
  return SQLITE_OK;
}

/*
** If the following global variable points to a string which is the
** name of a directory, then that directory will be used to store
** temporary files.
**
** See also the "PRAGMA temp_store_directory" SQL command.
*/
char *sqlite3_temp_directory = 0;

/*
** Create a temporary file name in zBuf.  zBuf must be big enough to
** hold at least SQLITE_TEMPNAME_SIZE characters.
*/
int sqlite3UnixTempFileName(char *zBuf){
  static const char *azDirs[] = {
     0,
     "/var/tmp",
     "/usr/tmp",
     "/tmp",
     ".",
  };
  static const unsigned char zChars[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789";
  int i, j;
  struct stat buf;
  const char *zDir = ".";
  azDirs[0] = sqlite3_temp_directory;
  for(i=0; i<sizeof(azDirs)/sizeof(azDirs[0]); i++){
    if( azDirs[i]==0 ) continue;
    if( stat(azDirs[i], &buf) ) continue;
    if( !S_ISDIR(buf.st_mode) ) continue;
    if( access(azDirs[i], 07) ) continue;
    zDir = azDirs[i];
    break;
  }
  do{
    sprintf(zBuf, "%s/"TEMP_FILE_PREFIX, zDir);
    j = strlen(zBuf);
    sqlite3Randomness(15, &zBuf[j]);
    for(i=0; i<15; i++, j++){
      zBuf[j] = (char)zChars[ ((unsigned char)zBuf[j])%(sizeof(zChars)-1) ];
    }
    zBuf[j] = 0;
  }while( access(zBuf,0)==0 );
  return SQLITE_OK;
}

/*
** Check that a given pathname is a directory and is writable
**
*/
int sqlite3UnixIsDirWritable(char *zBuf){
#ifndef SQLITE_OMIT_PAGER_PRAGMAS
  struct stat buf;
  if( zBuf==0 ) return 0;
  if( zBuf[0]==0 ) return 0;
  if( stat(zBuf, &buf) ) return 0;
  if( !S_ISDIR(buf.st_mode) ) return 0;
  if( access(zBuf, 07) ) return 0;
#endif /* SQLITE_OMIT_PAGER_PRAGMAS */
  return 1;
}

/*
** Read data from a file into a buffer.  Return SQLITE_OK if all
** bytes were read successfully and SQLITE_IOERR if anything goes
** wrong.
*/
static int unixRead(OsFile *id, void *pBuf, int amt){
  int got;
  assert( id );
  SimulateIOError(SQLITE_IOERR);
  TIMER_START;
  got = read(((unixFile*)id)->h, pBuf, amt);
  TIMER_END;
  TRACE5("READ    %-3d %5d %7d %d\n", ((unixFile*)id)->h, got,
          last_page, TIMER_ELAPSED);
  SEEK(0);
  /* if( got<0 ) got = 0; */
  if( got==amt ){
    return SQLITE_OK;
  }else{
    return SQLITE_IOERR;
  }
}

/*
** Write data from a buffer into a file.  Return SQLITE_OK on success
** or some other error code on failure.
*/
static int unixWrite(OsFile *id, const void *pBuf, int amt){
  int wrote = 0;
  assert( id );
  assert( amt>0 );
  SimulateIOError(SQLITE_IOERR);
  SimulateDiskfullError;
  TIMER_START;
  while( amt>0 && (wrote = write(((unixFile*)id)->h, pBuf, amt))>0 ){
    amt -= wrote;
    pBuf = &((char*)pBuf)[wrote];
  }
  TIMER_END;
  TRACE5("WRITE   %-3d %5d %7d %d\n", ((unixFile*)id)->h, wrote,
          last_page, TIMER_ELAPSED);
  SEEK(0);
  if( amt>0 ){
    return SQLITE_FULL;
  }
  return SQLITE_OK;
}

/*
** Move the read/write pointer in a file.
*/
static int unixSeek(OsFile *id, i64 offset){
  assert( id );
  SEEK(offset/1024 + 1);
#ifdef SQLITE_TEST
  if( offset ) SimulateDiskfullError
#endif
  lseek(((unixFile*)id)->h, offset, SEEK_SET);
  return SQLITE_OK;
}

#ifdef SQLITE_TEST
/*
** Count the number of fullsyncs and normal syncs.  This is used to test
** that syncs and fullsyncs are occuring at the right times.
*/
int sqlite3_sync_count = 0;
int sqlite3_fullsync_count = 0;
#endif

/*
** Use the fdatasync() API only if the HAVE_FDATASYNC macro is defined.
** Otherwise use fsync() in its place.
*/
#ifndef HAVE_FDATASYNC
# define fdatasync fsync
#endif


/*
** The fsync() system call does not work as advertised on many
** unix systems.  The following procedure is an attempt to make
** it work better.
**
** The SQLITE_NO_SYNC macro disables all fsync()s.  This is useful
** for testing when we want to run through the test suite quickly.
** You are strongly advised *not* to deploy with SQLITE_NO_SYNC
** enabled, however, since with SQLITE_NO_SYNC enabled, an OS crash
** or power failure will likely corrupt the database file.
*/
static int full_fsync(int fd, int fullSync, int dataOnly){
  int rc;

  /* Record the number of times that we do a normal fsync() and
  ** FULLSYNC.  This is used during testing to verify that this procedure
  ** gets called with the correct arguments.
  */
#ifdef SQLITE_TEST
  if( fullSync ) sqlite3_fullsync_count++;
  sqlite3_sync_count++;
#endif

  /* If we compiled with the SQLITE_NO_SYNC flag, then syncing is a
  ** no-op
  */
#ifdef SQLITE_NO_SYNC
  rc = SQLITE_OK;
#else

#ifdef F_FULLFSYNC
  if( fullSync ){
    rc = fcntl(fd, F_FULLFSYNC, 0);
  }else{
    rc = 1;
  }
  /* If the FULLSYNC failed, try to do a normal fsync() */
  if( rc ) rc = fsync(fd);

#else /* if !defined(F_FULLSYNC) */
  if( dataOnly ){
    rc = fdatasync(fd);
  }else{
    rc = fsync(fd);
  }
#endif /* defined(F_FULLFSYNC) */
#endif /* defined(SQLITE_NO_SYNC) */

  return rc;
}

/*
** Make sure all writes to a particular file are committed to disk.
**
** If dataOnly==0 then both the file itself and its metadata (file
** size, access time, etc) are synced.  If dataOnly!=0 then only the
** file data is synced.
**
** Under Unix, also make sure that the directory entry for the file
** has been created by fsync-ing the directory that contains the file.
** If we do not do this and we encounter a power failure, the directory
** entry for the journal might not exist after we reboot.  The next
** SQLite to access the file will not know that the journal exists (because
** the directory entry for the journal was never created) and the transaction
** will not roll back - possibly leading to database corruption.
*/
static int unixSync(OsFile *id, int dataOnly){
  unixFile *pFile = (unixFile*)id;
  assert( pFile );
  SimulateIOError(SQLITE_IOERR);
  TRACE2("SYNC    %-3d\n", pFile->h);
  if( full_fsync(pFile->h, pFile->fullSync, dataOnly) ){
    return SQLITE_IOERR;
  }
  if( pFile->dirfd>=0 ){
    TRACE2("DIRSYNC %-3d\n", pFile->dirfd);
#ifndef SQLITE_DISABLE_DIRSYNC
    if( full_fsync(pFile->dirfd, pFile->fullSync, 0) ){
        return SQLITE_IOERR;
    }
#endif
    close(pFile->dirfd);  /* Only need to sync once, so close the directory */
    pFile->dirfd = -1;    /* when we are done. */
  }
  return SQLITE_OK;
}

/*
** Sync the directory zDirname. This is a no-op on operating systems other
** than UNIX.
**
** This is used to make sure the master journal file has truely been deleted
** before making changes to individual journals on a multi-database commit.
** The F_FULLFSYNC option is not needed here.
*/
int sqlite3UnixSyncDirectory(const char *zDirname){
#ifdef SQLITE_DISABLE_DIRSYNC
  return SQLITE_OK;
#else
  int fd;
  int r;
  SimulateIOError(SQLITE_IOERR);
  fd = open(zDirname, O_RDONLY|O_BINARY, 0);
  TRACE3("DIRSYNC %-3d (%s)\n", fd, zDirname);
  if( fd<0 ){
    return SQLITE_CANTOPEN;
  }
  r = fsync(fd);
  close(fd);
  return ((r==0)?SQLITE_OK:SQLITE_IOERR);
#endif
}

/*
** Truncate an open file to a specified size
*/
static int unixTruncate(OsFile *id, i64 nByte){
  assert( id );
  SimulateIOError(SQLITE_IOERR);
  return ftruncate(((unixFile*)id)->h, nByte)==0 ? SQLITE_OK : SQLITE_IOERR;
}

/*
** Determine the current size of a file in bytes
*/
static int unixFileSize(OsFile *id, i64 *pSize){
  struct stat buf;
  assert( id );
  SimulateIOError(SQLITE_IOERR);
  if( fstat(((unixFile*)id)->h, &buf)!=0 ){
    return SQLITE_IOERR;
  }
  *pSize = buf.st_size;
  return SQLITE_OK;
}

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, return
** non-zero.  If the file is unlocked or holds only SHARED locks, then
** return zero.
*/
static int unixCheckReservedLock(OsFile *id){
  int r = 0;
  unixFile *pFile = (unixFile*)id;

  assert( pFile );
  sqlite3OsEnterMutex(); /* Because pFile->pLock is shared across threads */

  /* Check if a thread in this process holds such a lock */
  if( pFile->pLock->locktype>SHARED_LOCK ){
    r = 1;
  }

  /* Otherwise see if some other process holds it.
  */
  if( !r ){
    struct flock lock;
    lock.l_whence = SEEK_SET;
    lock.l_start = RESERVED_BYTE;
    lock.l_len = 1;
    lock.l_type = F_WRLCK;
    fcntl(pFile->h, F_GETLK, &lock);
    if( lock.l_type!=F_UNLCK ){
      r = 1;
    }
  }

  sqlite3OsLeaveMutex();
  TRACE3("TEST WR-LOCK %d %d\n", pFile->h, r);

  return r;
}

/*
** Lock the file with the lock specified by parameter locktype - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** This routine will only increase a lock.  Use the sqlite3OsUnlock()
** routine to lower a locking level.
*/
static int unixLock(OsFile *id, int locktype){
  /* The following describes the implementation of the various locks and
  ** lock transitions in terms of the POSIX advisory shared and exclusive
  ** lock primitives (called read-locks and write-locks below, to avoid
  ** confusion with SQLite lock names). The algorithms are complicated
  ** slightly in order to be compatible with windows systems simultaneously
  ** accessing the same database file, in case that is ever required.
  **
  ** Symbols defined in os.h indentify the 'pending byte' and the 'reserved
  ** byte', each single bytes at well known offsets, and the 'shared byte
  ** range', a range of 510 bytes at a well known offset.
  **
  ** To obtain a SHARED lock, a read-lock is obtained on the 'pending
  ** byte'.  If this is successful, a random byte from the 'shared byte
  ** range' is read-locked and the lock on the 'pending byte' released.
  **
  ** A process may only obtain a RESERVED lock after it has a SHARED lock.
  ** A RESERVED lock is implemented by grabbing a write-lock on the
  ** 'reserved byte'.
  **
  ** A process may only obtain a PENDING lock after it has obtained a
  ** SHARED lock. A PENDING lock is implemented by obtaining a write-lock
  ** on the 'pending byte'. This ensures that no new SHARED locks can be
  ** obtained, but existing SHARED locks are allowed to persist. A process
  ** does not have to obtain a RESERVED lock on the way to a PENDING lock.
  ** This property is used by the algorithm for rolling back a journal file
  ** after a crash.
  **
  ** An EXCLUSIVE lock, obtained after a PENDING lock is held, is
  ** implemented by obtaining a write-lock on the entire 'shared byte
  ** range'. Since all other locks require a read-lock on one of the bytes
  ** within this range, this ensures that no other locks are held on the
  ** database.
  **
  ** The reason a single byte cannot be used instead of the 'shared byte
  ** range' is that some versions of windows do not support read-locks. By
  ** locking a random byte from a range, concurrent SHARED locks may exist
  ** even if the locking primitive used is always a write-lock.
  */
  int rc = SQLITE_OK;
  unixFile *pFile = (unixFile*)id;
  struct lockInfo *pLock = pFile->pLock;
  struct flock lock;
  int s;

  assert( pFile );
  TRACE7("LOCK    %d %s was %s(%s,%d) pid=%d\n", pFile->h,
      locktypeName(locktype), locktypeName(pFile->locktype),
      locktypeName(pLock->locktype), pLock->cnt , getpid());

  /* If there is already a lock of this type or more restrictive on the
  ** OsFile, do nothing. Don't use the end_lock: exit path, as
  ** sqlite3OsEnterMutex() hasn't been called yet.
  */
  if( pFile->locktype>=locktype ){
    TRACE3("LOCK    %d %s ok (already held)\n", pFile->h,
            locktypeName(locktype));
    return SQLITE_OK;
  }

  /* Make sure the locking sequence is correct
  */
  assert( pFile->locktype!=NO_LOCK || locktype==SHARED_LOCK );
  assert( locktype!=PENDING_LOCK );
  assert( locktype!=RESERVED_LOCK || pFile->locktype==SHARED_LOCK );

  /* This mutex is needed because pFile->pLock is shared across threads
  */
  sqlite3OsEnterMutex();

  /* Make sure the current thread owns the pFile.
  */
  rc = transferOwnership(pFile);
  if( rc!=SQLITE_OK ){
    sqlite3OsLeaveMutex();
    return rc;
  }
  pLock = pFile->pLock;

  /* If some thread using this PID has a lock via a different OsFile*
  ** handle that precludes the requested lock, return BUSY.
  */
  if( (pFile->locktype!=pLock->locktype &&
          (pLock->locktype>=PENDING_LOCK || locktype>SHARED_LOCK))
  ){
    rc = SQLITE_BUSY;
    goto end_lock;
  }

  /* If a SHARED lock is requested, and some thread using this PID already
  ** has a SHARED or RESERVED lock, then increment reference counts and
  ** return SQLITE_OK.
  */
  if( locktype==SHARED_LOCK &&
      (pLock->locktype==SHARED_LOCK || pLock->locktype==RESERVED_LOCK) ){
    assert( locktype==SHARED_LOCK );
    assert( pFile->locktype==0 );
    assert( pLock->cnt>0 );
    pFile->locktype = SHARED_LOCK;
    pLock->cnt++;
    pFile->pOpen->nLock++;
    goto end_lock;
  }

  lock.l_len = 1L;

  lock.l_whence = SEEK_SET;

  /* A PENDING lock is needed before acquiring a SHARED lock and before
  ** acquiring an EXCLUSIVE lock.  For the SHARED lock, the PENDING will
  ** be released.
  */
  if( locktype==SHARED_LOCK
      || (locktype==EXCLUSIVE_LOCK && pFile->locktype<PENDING_LOCK)
  ){
    lock.l_type = (locktype==SHARED_LOCK?F_RDLCK:F_WRLCK);
    lock.l_start = PENDING_BYTE;
    s = fcntl(pFile->h, F_SETLK, &lock);
    if( s ){
      rc = (errno==EINVAL) ? SQLITE_NOLFS : SQLITE_BUSY;
      goto end_lock;
    }
  }


  /* If control gets to this point, then actually go ahead and make
  ** operating system calls for the specified lock.
  */
  if( locktype==SHARED_LOCK ){
    assert( pLock->cnt==0 );
    assert( pLock->locktype==0 );

    /* Now get the read-lock */
    lock.l_start = SHARED_FIRST;
    lock.l_len = SHARED_SIZE;
    s = fcntl(pFile->h, F_SETLK, &lock);

    /* Drop the temporary PENDING lock */
    lock.l_start = PENDING_BYTE;
    lock.l_len = 1L;
    lock.l_type = F_UNLCK;
    if( fcntl(pFile->h, F_SETLK, &lock)!=0 ){
      rc = SQLITE_IOERR;  /* This should never happen */
      goto end_lock;
    }
    if( s ){
      rc = (errno==EINVAL) ? SQLITE_NOLFS : SQLITE_BUSY;
    }else{
      pFile->locktype = SHARED_LOCK;
      pFile->pOpen->nLock++;
      pLock->cnt = 1;
    }
  }else if( locktype==EXCLUSIVE_LOCK && pLock->cnt>1 ){
    /* We are trying for an exclusive lock but another thread in this
    ** same process is still holding a shared lock. */
    rc = SQLITE_BUSY;
  }else{
    /* The request was for a RESERVED or EXCLUSIVE lock.  It is
    ** assumed that there is a SHARED or greater lock on the file
    ** already.
    */
    assert( 0!=pFile->locktype );
    lock.l_type = F_WRLCK;
    switch( locktype ){
      case RESERVED_LOCK:
        lock.l_start = RESERVED_BYTE;
        break;
      case EXCLUSIVE_LOCK:
        lock.l_start = SHARED_FIRST;
        lock.l_len = SHARED_SIZE;
        break;
      default:
        assert(0);
    }
    s = fcntl(pFile->h, F_SETLK, &lock);
    if( s ){
      rc = (errno==EINVAL) ? SQLITE_NOLFS : SQLITE_BUSY;
    }
  }

  if( rc==SQLITE_OK ){
    pFile->locktype = locktype;
    pLock->locktype = locktype;
  }else if( locktype==EXCLUSIVE_LOCK ){
    pFile->locktype = PENDING_LOCK;
    pLock->locktype = PENDING_LOCK;
  }

end_lock:
  sqlite3OsLeaveMutex();
  TRACE4("LOCK    %d %s %s\n", pFile->h, locktypeName(locktype),
      rc==SQLITE_OK ? "ok" : "failed");
  return rc;
}

/*
** Lower the locking level on file descriptor pFile to locktype.  locktype
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
*/
static int unixUnlock(OsFile *id, int locktype){
  struct lockInfo *pLock;
  struct flock lock;
  int rc = SQLITE_OK;
  unixFile *pFile = (unixFile*)id;

  assert( pFile );
  TRACE7("UNLOCK  %d %d was %d(%d,%d) pid=%d\n", pFile->h, locktype,
      pFile->locktype, pFile->pLock->locktype, pFile->pLock->cnt, getpid());

  assert( locktype<=SHARED_LOCK );
  if( pFile->locktype<=locktype ){
    return SQLITE_OK;
  }
  if( CHECK_THREADID(pFile) ){
    return SQLITE_MISUSE;
  }
  sqlite3OsEnterMutex();
  pLock = pFile->pLock;
  assert( pLock->cnt!=0 );
  if( pFile->locktype>SHARED_LOCK ){
    assert( pLock->locktype==pFile->locktype );
    if( locktype==SHARED_LOCK ){
      lock.l_type = F_RDLCK;
      lock.l_whence = SEEK_SET;
      lock.l_start = SHARED_FIRST;
      lock.l_len = SHARED_SIZE;
      if( fcntl(pFile->h, F_SETLK, &lock)!=0 ){
        /* This should never happen */
        rc = SQLITE_IOERR;
      }
    }
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = PENDING_BYTE;
    lock.l_len = 2L;  assert( PENDING_BYTE+1==RESERVED_BYTE );
    if( fcntl(pFile->h, F_SETLK, &lock)==0 ){
      pLock->locktype = SHARED_LOCK;
    }else{
      rc = SQLITE_IOERR;  /* This should never happen */
    }
  }
  if( locktype==NO_LOCK ){
    struct openCnt *pOpen;

    /* Decrement the shared lock counter.  Release the lock using an
    ** OS call only when all threads in this same process have released
    ** the lock.
    */
    pLock->cnt--;
    if( pLock->cnt==0 ){
      lock.l_type = F_UNLCK;
      lock.l_whence = SEEK_SET;
      lock.l_start = lock.l_len = 0L;
      if( fcntl(pFile->h, F_SETLK, &lock)==0 ){
        pLock->locktype = NO_LOCK;
      }else{
        rc = SQLITE_IOERR;  /* This should never happen */
      }
    }

    /* Decrement the count of locks against this same file.  When the
    ** count reaches zero, close any other file descriptors whose close
    ** was deferred because of outstanding locks.
    */
    pOpen = pFile->pOpen;
    pOpen->nLock--;
    assert( pOpen->nLock>=0 );
    if( pOpen->nLock==0 && pOpen->nPending>0 ){
      int i;
      for(i=0; i<pOpen->nPending; i++){
        close(pOpen->aPending[i]);
      }
      free(pOpen->aPending);
      pOpen->nPending = 0;
      pOpen->aPending = 0;
    }
  }
  sqlite3OsLeaveMutex();
  pFile->locktype = locktype;
  return rc;
}

/*
** Close a file.
*/
static int unixClose(OsFile **pId){
  unixFile *id = (unixFile*)*pId;

  if( !id ) return SQLITE_OK;
  unixUnlock(*pId, NO_LOCK);
  if( id->dirfd>=0 ) close(id->dirfd);
  id->dirfd = -1;
  sqlite3OsEnterMutex();

  if( id->pOpen->nLock ){
    /* If there are outstanding locks, do not actually close the file just
    ** yet because that would clear those locks.  Instead, add the file
    ** descriptor to pOpen->aPending.  It will be automatically closed when
    ** the last lock is cleared.
    */
    int *aNew;
    struct openCnt *pOpen = id->pOpen;
    aNew = realloc( pOpen->aPending, (pOpen->nPending+1)*sizeof(int) );
    if( aNew==0 ){
      /* If a malloc fails, just leak the file descriptor */
    }else{
      pOpen->aPending = aNew;
      pOpen->aPending[pOpen->nPending] = id->h;
      pOpen->nPending++;
    }
  }else{
    /* There are no outstanding locks so we can close the file immediately */
    close(id->h);
  }
  releaseLockInfo(id->pLock);
  releaseOpenCnt(id->pOpen);

  sqlite3OsLeaveMutex();
  id->isOpen = 0;
  TRACE2("CLOSE   %-3d\n", id->h);
  OpenCounter(-1);
  sqliteFree(id);
  *pId = 0;
  return SQLITE_OK;
}

/*
** Turn a relative pathname into a full pathname.  Return a pointer
** to the full pathname stored in space obtained from sqliteMalloc().
** The calling function is responsible for freeing this space once it
** is no longer needed.
*/
char *sqlite3UnixFullPathname(const char *zRelative){
  char *zFull = 0;
  if( zRelative[0]=='/' ){
    sqlite3SetString(&zFull, zRelative, (char*)0);
  }else{
    char *zBuf = sqliteMalloc(5000);
    if( zBuf==0 ){
      return 0;
    }
    zBuf[0] = 0;
    sqlite3SetString(&zFull, getcwd(zBuf, 5000), "/", zRelative,
                    (char*)0);
    sqliteFree(zBuf);
  }
  return zFull;
}

/*
** Change the value of the fullsync flag in the given file descriptor.
*/
static void unixSetFullSync(OsFile *id, int v){
  ((unixFile*)id)->fullSync = v;
}

/*
** Return the underlying file handle for an OsFile
*/
static int unixFileHandle(OsFile *id){
  return ((unixFile*)id)->h;
}

/*
** Return an integer that indices the type of lock currently held
** by this handle.  (Used for testing and analysis only.)
*/
static int unixLockState(OsFile *id){
  return ((unixFile*)id)->locktype;
}

/*
** This vector defines all the methods that can operate on an OsFile
** for unix.
*/
static const IoMethod sqlite3UnixIoMethod = {
  unixClose,
  unixOpenDirectory,
  unixRead,
  unixWrite,
  unixSeek,
  unixTruncate,
  unixSync,
  unixSetFullSync,
  unixFileHandle,
  unixFileSize,
  unixLock,
  unixUnlock,
  unixLockState,
  unixCheckReservedLock,
};

/*
** Allocate memory for a unixFile.  Initialize the new unixFile
** to the value given in pInit and return a pointer to the new
** OsFile.  If we run out of memory, close the file and return NULL.
*/
static int allocateUnixFile(unixFile *pInit, OsFile **pId){
  unixFile *pNew;
  pInit->dirfd = -1;
  pInit->fullSync = 0;
  pInit->locktype = 0;
  SET_THREADID(pInit);
  pNew = sqliteMalloc( sizeof(unixFile) );
  if( pNew==0 ){
    close(pInit->h);
    sqlite3OsEnterMutex();
    releaseLockInfo(pInit->pLock);
    releaseOpenCnt(pInit->pOpen);
    sqlite3OsLeaveMutex();
    *pId = 0;
    return SQLITE_NOMEM;
  }else{
    *pNew = *pInit;
    pNew->pMethod = &sqlite3UnixIoMethod;
    *pId = (OsFile*)pNew;
    OpenCounter(+1);
    return SQLITE_OK;
  }
}


#endif /* SQLITE_OMIT_DISKIO */
/***************************************************************************
** Everything above deals with file I/O.  Everything that follows deals
** with other miscellanous aspects of the operating system interface
****************************************************************************/


/*
** Get information to seed the random number generator.  The seed
** is written into the buffer zBuf[256].  The calling function must
** supply a sufficiently large buffer.
*/
int sqlite3UnixRandomSeed(char *zBuf){
  /* We have to initialize zBuf to prevent valgrind from reporting
  ** errors.  The reports issued by valgrind are incorrect - we would
  ** prefer that the randomness be increased by making use of the
  ** uninitialized space in zBuf - but valgrind errors tend to worry
  ** some users.  Rather than argue, it seems easier just to initialize
  ** the whole array and silence valgrind, even if that means less randomness
  ** in the random seed.
  **
  ** When testing, initializing zBuf[] to zero is all we do.  That means
  ** that we always use the same random number sequence.  This makes the
  ** tests repeatable.
  */
  memset(zBuf, 0, 256);
#if !defined(SQLITE_TEST)
  {
    int pid, fd;
    fd = open("/dev/urandom", O_RDONLY);
    if( fd<0 ){
      time_t t;
      time(&t);
      memcpy(zBuf, &t, sizeof(t));
      pid = getpid();
      memcpy(&zBuf[sizeof(time_t)], &pid, sizeof(pid));
    }else{
      read(fd, zBuf, 256);
      close(fd);
    }
  }
#endif
  return SQLITE_OK;
}

/*
** Sleep for a little while.  Return the amount of time slept.
** The argument is the number of milliseconds we want to sleep.
*/
int sqlite3UnixSleep(int ms){
#if defined(HAVE_USLEEP) && HAVE_USLEEP
  usleep(ms*1000);
  return ms;
#else
  sleep((ms+999)/1000);
  return 1000*((ms+999)/1000);
#endif
}

/*
** Static variables used for thread synchronization
*/
static int inMutex = 0;
#ifdef SQLITE_UNIX_THREADS
static pthread_t mutexOwner;
static pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex2 = PTHREAD_MUTEX_INITIALIZER;
#endif

//inline int gettid() {
//	return syscall(__NR_gettid);
//}

/*
** The following pair of routine implement mutual exclusion for
** multi-threaded processes.  Only a single thread is allowed to
** executed code that is surrounded by EnterMutex() and LeaveMutex().
**
** SQLite uses only a single Mutex.  There is not much critical
** code and what little there is executes quickly and without blocking.
**
** As of version 3.3.2, this mutex must be recursive.
*/
void sqlite3UnixEnterMutex(){
#ifdef SQLITE_UNIX_THREADS
//	printf("%d waiting for m1\n", gettid());
  pthread_mutex_lock(&mutex1);
//    printf("%d holding m1\n", gettid());
  if( inMutex==0 ){
//	  printf("%d waiting for m2\n", gettid());
    pthread_mutex_lock(&mutex2);
    mutexOwner = pthread_self();
  }
  pthread_mutex_unlock(&mutex1);

#endif
//  printf("%d holding m2\n", gettid());
  sleep(2);
  inMutex++;
}
void sqlite3UnixLeaveMutex(){
  assert( inMutex>0 );
#ifdef SQLITE_UNIX_THREADS
  assert( pthread_equal(mutexOwner, pthread_self()) );
  pthread_mutex_lock(&mutex1);
  inMutex--;
  if( inMutex==0 ){
    pthread_mutex_unlock(&mutex2);
  }
  pthread_mutex_unlock(&mutex1);
#else
  inMutex--;
#endif
}

/*
** Return TRUE if the mutex is currently held.
**
** If the thisThreadOnly parameter is true, return true only if the
** calling thread holds the mutex.  If the parameter is false, return
** true if any thread holds the mutex.
*/
int sqlite3UnixInMutex(int thisThreadOnly){
#ifdef SQLITE_UNIX_THREADS
  return inMutex>0 &&
           (thisThreadOnly==0 || pthread_equal(mutexOwner, pthread_self()));
#else
  return inMutex>0;
#endif
}

/*
** Remember the number of thread-specific-data blocks allocated.
** Use this to verify that we are not leaking thread-specific-data.
** Ticket #1601
*/
#ifdef SQLITE_TEST
int sqlite3_tsd_count = 0;
# ifdef SQLITE_UNIX_THREADS
    static pthread_mutex_t tsd_counter_mutex = PTHREAD_MUTEX_INITIALIZER;
#   define TSD_COUNTER(N) \
             pthread_mutex_lock(&tsd_counter_mutex); \
             sqlite3_tsd_count += N; \
             pthread_mutex_unlock(&tsd_counter_mutex);
# else
#   define TSD_COUNTER(N)  sqlite3_tsd_count += N
# endif
#else
# define TSD_COUNTER(N)  /* no-op */
#endif

/*
** If called with allocateFlag>0, then return a pointer to thread
** specific data for the current thread.  Allocate and zero the
** thread-specific data if it does not already exist.
**
** If called with allocateFlag==0, then check the current thread
** specific data.  Return it if it exists.  If it does not exist,
** then return NULL.
**
** If called with allocateFlag<0, check to see if the thread specific
** data is allocated and is all zero.  If it is then deallocate it.
** Return a pointer to the thread specific data or NULL if it is
** unallocated or gets deallocated.
*/
ThreadData *sqlite3UnixThreadSpecificData(int allocateFlag){
  static const ThreadData zeroData = {0};  /* Initializer to silence warnings
                                           ** from broken compilers */
#ifdef SQLITE_UNIX_THREADS
  static pthread_key_t key;
  static int keyInit = 0;
  ThreadData *pTsd;

  if( !keyInit ){
    sqlite3OsEnterMutex();
    if( !keyInit ){
      int rc;
      rc = pthread_key_create(&key, 0);
      if( rc ){
        sqlite3OsLeaveMutex();
        return 0;
      }
      keyInit = 1;
    }
    sqlite3OsLeaveMutex();
  }

  pTsd = pthread_getspecific(key);
  if( allocateFlag>0 ){
    if( pTsd==0 ){
      if( !sqlite3TestMallocFail() ){
        pTsd = sqlite3OsMalloc(sizeof(zeroData));
      }
#ifdef SQLITE_MEMDEBUG
      sqlite3_isFail = 0;
#endif
      if( pTsd ){
        *pTsd = zeroData;
        pthread_setspecific(key, pTsd);
        TSD_COUNTER(+1);
      }
    }
  }else if( pTsd!=0 && allocateFlag<0
            && memcmp(pTsd, &zeroData, sizeof(ThreadData))==0 ){
    sqlite3OsFree(pTsd);
    pthread_setspecific(key, 0);
    TSD_COUNTER(-1);
    pTsd = 0;
  }
  return pTsd;
#else
  static ThreadData *pTsd = 0;
  if( allocateFlag>0 ){
    if( pTsd==0 ){
      if( !sqlite3TestMallocFail() ){
        pTsd = sqlite3OsMalloc( sizeof(zeroData) );
      }
#ifdef SQLITE_MEMDEBUG
      sqlite3_isFail = 0;
#endif
      if( pTsd ){
        *pTsd = zeroData;
        TSD_COUNTER(+1);
      }
    }
  }else if( pTsd!=0 && allocateFlag<0
            && memcmp(pTsd, &zeroData, sizeof(ThreadData))==0 ){
    sqlite3OsFree(pTsd);
    TSD_COUNTER(-1);
    pTsd = 0;
  }
  return pTsd;
#endif
}

/*
** The following variable, if set to a non-zero value, becomes the result
** returned from sqlite3OsCurrentTime().  This is used for testing.
*/
#ifdef SQLITE_TEST
int sqlite3_current_time = 0;
#endif

/*
** Find the current time (in Universal Coordinated Time).  Write the
** current time and date as a Julian Day number into *prNow and
** return 0.  Return 1 if the time and date cannot be found.
*/
int sqlite3UnixCurrentTime(double *prNow){
#ifdef NO_GETTOD
  time_t t;
  time(&t);
  *prNow = t/86400.0 + 2440587.5;
#else
  struct timeval sNow;
  struct timezone sTz;  /* Not used */
  gettimeofday(&sNow, &sTz);
  *prNow = 2440587.5 + sNow.tv_sec/86400.0 + sNow.tv_usec/86400000000.0;
#endif
#ifdef SQLITE_TEST
  if( sqlite3_current_time ){
    *prNow = sqlite3_current_time/86400.0 + 2440587.5;
  }
#endif
  return 0;
}

#endif /* OS_UNIX */
