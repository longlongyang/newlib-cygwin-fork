/* miscfuncs.cc: misc funcs that don't belong anywhere else

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include "miscfuncs.h"
#include <ntsecapi.h>
#include <sys/uio.h>
#include <sys/param.h>
#include "ntdll.h"
#include "path.h"
#include "fhandler.h"
#include "exception.h"
#include "tls_pbuf.h"
#include "mmap_alloc.h"

/* not yet prototyped in w32api */
extern "C" HRESULT SetThreadDescription (HANDLE hThread, PCWSTR lpThreadDescription);

/* Get handle count of an object. */
ULONG
get_obj_handle_count (HANDLE h)
{
  OBJECT_BASIC_INFORMATION obi;
  NTSTATUS status;
  ULONG hdl_cnt = 0;

  status = NtQueryObject (h, ObjectBasicInformation, &obi, sizeof obi, NULL);
  if (!NT_SUCCESS (status))
    debug_printf ("NtQueryObject: %y", status);
  else
    hdl_cnt = obi.HandleCount;
  return hdl_cnt;
}

static char __attribute__ ((noinline))
dummytest (volatile char *p)
{
  return *p;
}

ssize_t
check_iovec (const struct iovec *iov, int iovcnt, bool forwrite)
{
  if (iovcnt < 0 || iovcnt > IOV_MAX)
    {
      set_errno (EINVAL);
      return -1;
    }

  __try
    {

      size_t tot = 0;

      while (iovcnt > 0)
	{
	  if (iov->iov_len > SSIZE_MAX || (tot += iov->iov_len) > SSIZE_MAX)
	    {
	      set_errno (EINVAL);
	      __leave;
	    }

	  volatile char *p = ((char *) iov->iov_base) + iov->iov_len - 1;
	  if (!iov->iov_len)
	    /* nothing to do */;
	  else if (!forwrite)
	    *p  = dummytest (p);
	  else
	    dummytest (p);

	  iov++;
	  iovcnt--;
	}

      if (tot <= SSIZE_MAX)
	return (ssize_t) tot;

      set_errno (EINVAL);
    }
  __except (EFAULT)
  __endtry
  return -1;
}

/* Try hard to schedule another thread.
   Remember not to call this in a lock condition or you'll potentially
   suffer starvation.  */
void
yield ()
{
  /* MSDN implies that Sleep will force scheduling of other threads.
     Unlike SwitchToThread() the documentation does not mention other
     cpus so, presumably (hah!), this + using a lower priority will
     stall this thread temporarily and cause another to run.
     (stackoverflow and others seem to confirm that setting this thread
     to a lower priority and calling Sleep with a 0 paramenter will
     have this desired effect)

     CV 2017-03-08: Drop lowering the priority.  It leads to potential
		    starvation and it should not be necessary anymore
		    since Server 2003.  See the MSDN Sleep man page. */
  Sleep (0L);
}

/* Get a default value for the nice factor.  When changing these values,
   have a look into the below function nice_to_winprio.  The values must
   match the layout of the static "priority" array. */
int
winprio_to_nice (DWORD prio)
{
  switch (prio)
    {
      case REALTIME_PRIORITY_CLASS:
	return -20;
      case HIGH_PRIORITY_CLASS:
	return -16;
      case ABOVE_NORMAL_PRIORITY_CLASS:
	return -8;
      case NORMAL_PRIORITY_CLASS:
	return 0;
      case BELOW_NORMAL_PRIORITY_CLASS:
	return 8;
      case IDLE_PRIORITY_CLASS:
	return 16;
    }
  return 0;
}

/* Get a Win32 priority matching the incoming nice factor.  The incoming
   nice is limited to the interval [-NZERO,NZERO-1]. */
DWORD
nice_to_winprio (int &nice)
{
  static const DWORD priority[] =
    {
      REALTIME_PRIORITY_CLASS,		/*  0 */
      HIGH_PRIORITY_CLASS,		/*  1 */
      HIGH_PRIORITY_CLASS,
      HIGH_PRIORITY_CLASS,
      HIGH_PRIORITY_CLASS,
      HIGH_PRIORITY_CLASS,
      HIGH_PRIORITY_CLASS,
      HIGH_PRIORITY_CLASS,		/*  7 */
      ABOVE_NORMAL_PRIORITY_CLASS,	/*  8 */
      ABOVE_NORMAL_PRIORITY_CLASS,
      ABOVE_NORMAL_PRIORITY_CLASS,
      ABOVE_NORMAL_PRIORITY_CLASS,
      ABOVE_NORMAL_PRIORITY_CLASS,
      ABOVE_NORMAL_PRIORITY_CLASS,
      ABOVE_NORMAL_PRIORITY_CLASS,
      ABOVE_NORMAL_PRIORITY_CLASS,	/* 15 */
      NORMAL_PRIORITY_CLASS,		/* 16 */
      NORMAL_PRIORITY_CLASS,
      NORMAL_PRIORITY_CLASS,
      NORMAL_PRIORITY_CLASS,
      NORMAL_PRIORITY_CLASS,
      NORMAL_PRIORITY_CLASS,
      NORMAL_PRIORITY_CLASS,
      NORMAL_PRIORITY_CLASS,		/* 23 */
      BELOW_NORMAL_PRIORITY_CLASS,	/* 24 */
      BELOW_NORMAL_PRIORITY_CLASS,
      BELOW_NORMAL_PRIORITY_CLASS,
      BELOW_NORMAL_PRIORITY_CLASS,
      BELOW_NORMAL_PRIORITY_CLASS,
      BELOW_NORMAL_PRIORITY_CLASS,
      BELOW_NORMAL_PRIORITY_CLASS,
      BELOW_NORMAL_PRIORITY_CLASS,	/* 31 */
      IDLE_PRIORITY_CLASS,		/* 32 */
      IDLE_PRIORITY_CLASS,
      IDLE_PRIORITY_CLASS,
      IDLE_PRIORITY_CLASS,
      IDLE_PRIORITY_CLASS,
      IDLE_PRIORITY_CLASS,
      IDLE_PRIORITY_CLASS,
      IDLE_PRIORITY_CLASS		/* 39 */
    };
  if (nice < -NZERO)
    nice = -NZERO;
  else if (nice > NZERO - 1)
    nice = NZERO - 1;
  DWORD prio = priority[nice + NZERO];
  return prio;
}

/* Minimal overlapped pipe I/O implementation for signal and commune stuff. */

BOOL
CreatePipeOverlapped (PHANDLE hr, PHANDLE hw, LPSECURITY_ATTRIBUTES sa)
{
  int ret = fhandler_pipe::create (sa, hr, hw, 0, NULL,
				   FILE_FLAG_OVERLAPPED);
  if (ret)
    SetLastError (ret);
  return ret == 0;
}

BOOL
ReadPipeOverlapped (HANDLE h, PVOID buf, DWORD len, LPDWORD ret_len,
		    DWORD timeout)
{
  OVERLAPPED ov;
  BOOL ret;

  memset (&ov, 0, sizeof ov);
  ov.hEvent = CreateEvent (NULL, TRUE, FALSE, NULL);
  ret = ReadFile (h, buf, len, NULL, &ov);
  if (ret || GetLastError () == ERROR_IO_PENDING)
    {
      if (!ret && WaitForSingleObject (ov.hEvent, timeout) != WAIT_OBJECT_0)
	CancelIo (h);
      ret = GetOverlappedResult (h, &ov, ret_len, FALSE);
    }
  CloseHandle (ov.hEvent);
  return ret;
}

BOOL
WritePipeOverlapped (HANDLE h, LPCVOID buf, DWORD len, LPDWORD ret_len,
		     DWORD timeout)
{
  OVERLAPPED ov;
  BOOL ret;

  memset (&ov, 0, sizeof ov);
  ov.hEvent = CreateEvent (NULL, TRUE, FALSE, NULL);
  ret = WriteFile (h, buf, len, NULL, &ov);
  if (ret || GetLastError () == ERROR_IO_PENDING)
    {
      if (!ret && WaitForSingleObject (ov.hEvent, timeout) != WAIT_OBJECT_0)
	CancelIo (h);
      ret = GetOverlappedResult (h, &ov, ret_len, FALSE);
    }
  CloseHandle (ov.hEvent);
  return ret;
}

bool
NT_readline::init (POBJECT_ATTRIBUTES attr, PCHAR in_buf, ULONG in_buflen)
{
  NTSTATUS status;
  IO_STATUS_BLOCK io;

  status = NtOpenFile (&fh, SYNCHRONIZE | FILE_READ_DATA, attr, &io,
                       FILE_SHARE_VALID_FLAGS,
                       FILE_SYNCHRONOUS_IO_NONALERT
                       | FILE_OPEN_FOR_BACKUP_INTENT);
  if (!NT_SUCCESS (status))
    {
      paranoid_printf ("NtOpenFile(%S) failed, status %y",
		       attr->ObjectName, status);
      return false;
    }
  buf = in_buf;
  buflen = in_buflen;
  got = end = buf;
  len = 0;
  line = 1;
  return true;
}

PCHAR
NT_readline::gets ()
{
  IO_STATUS_BLOCK io;

  while (true)
    {
      /* len == 0 indicates we have to read from the file. */
      if (!len)
	{
	  if (!NT_SUCCESS (NtReadFile (fh, NULL, NULL, NULL, &io, got,
				       (buflen - 2) - (got - buf), NULL, NULL)))
	    return NULL;
	  len = io.Information;
	  /* Set end marker. */
	  got[len] = got[len + 1] = '\0';
	  /* Set len to the absolute len of bytes in buf. */
	  len += got - buf;
	  /* Reset got to start reading at the start of the buffer again. */
	  got = end = buf;
	}
      else
	{
	  got = end + 1;
	  ++line;
	}
      /* Still some valid full line? */
      if (got < buf + len)
	{
	  if ((end = strchr (got, '\n')))
	    {
	      end[end[-1] == '\r' ? -1 : 0] = '\0';
	      return got;
	    }
	  /* Last line missing a \n at EOF? */
	  if (len < buflen - 2)
	    {
	      len = 0;
	      return got;
	    }
	}
      /* We have to read once more.  Move remaining bytes to the start of
         the buffer and reposition got so that it points to the end of
         the remaining bytes. */
      len = buf + len - got;
      memmove (buf, got, len);
      got = buf + len;
      buf[len] = buf[len + 1] = '\0';
      len = 0;
    }
}

/* Helper function to generate the correct caller address.  For external
   calls, the return address on the stack is _sigbe.  In that case the
   actual caller return address is on the cygtls stack.  Use this function
   via the macro caller_return_address. */
extern "C" void _sigbe ();

void *
__caller_return_address (void *builtin_ret_addr)
{
  return builtin_ret_addr == &_sigbe
	 ? (void *) _my_tls.retaddr () : builtin_ret_addr;
}

/* CygwinCreateThread.

   Replacement function for CreateThread.  What we do here is to remove
   parameters we don't use and instead to add parameters we need to make
   the function pthreads compatible. */

struct pthread_wrapper_arg
{
  LPTHREAD_START_ROUTINE func;
  PVOID arg;
  PBYTE stackaddr;
  PBYTE stackbase;
  PBYTE stacklimit;
  ULONG guardsize;
};

DWORD
pthread_wrapper (PVOID arg)
{
  /* Just plain paranoia. */
  if (!arg)
    return ERROR_INVALID_PARAMETER;

  /* The process is now threaded.  Note for later usage by arc4random. */
  __isthreaded = 1;

  /* Fetch thread wrapper info and free from cygheap. */
  pthread_wrapper_arg wrapper_arg = *(pthread_wrapper_arg *) arg;
  cfree (arg);

  /* Set stack values in TEB */
  PTEB teb = NtCurrentTeb ();
  teb->Tib.StackBase = wrapper_arg.stackbase;
  teb->Tib.StackLimit = wrapper_arg.stacklimit ?: wrapper_arg.stackaddr;
  /* Set DeallocationStack value.  If we have an application-provided stack,
     we set DeallocationStack to NULL, so NtTerminateThread does not deallocate
     any stack.  If we created the stack in CygwinCreateThread, we set
     DeallocationStack to the stackaddr of our own stack, so it's automatically
     deallocated when the thread is terminated. */
  PBYTE dealloc_addr = (PBYTE) teb->DeallocationStack;
  teb->DeallocationStack = wrapper_arg.stacklimit ? wrapper_arg.stackaddr
						  : NULL;
  /* Store the OS-provided DeallocationStack address in wrapper_arg.stackaddr.
     The below assembler code will release the OS stack after switching to our
     new stack. */
  wrapper_arg.stackaddr = dealloc_addr;
  /* Set thread stack guarantee matching the guardsize.
     Note that the guardsize is one page bigger than the guarantee. */
  if (wrapper_arg.guardsize > wincap.def_guard_page_size ())
    {
      wrapper_arg.guardsize -= wincap.page_size ();
      SetThreadStackGuarantee (&wrapper_arg.guardsize);
    }
  /* Initialize new _cygtls. */
  _my_tls.init_thread (wrapper_arg.stackbase - __CYGTLS_PADSIZE__,
		       (DWORD (*)(void*, void*)) wrapper_arg.func);
#ifdef __x86_64__
  __asm__ ("\n\
	   leaq  %[WRAPPER_ARG], %%rbx	# Load &wrapper_arg into rbx	\n\
	   movq  (%%rbx), %%r12		# Load thread func into r12	\n\
	   movq  8(%%rbx), %%r13	# Load thread arg into r13	\n\
	   movq  16(%%rbx), %%rcx	# Load stackaddr into rcx	\n\
	   movq  24(%%rbx), %%rsp	# Load stackbase into rsp	\n\
	   subq  %[CYGTLS], %%rsp	# Subtract __CYGTLS_PADSIZE__	\n\
					# (here we are 16 bytes aligned)\n\
	   subq  $32, %%rsp		# Subtract another 32 bytes	\n\
					# (shadow space for arg regs)	\n\
	   xorq  %%rbp, %%rbp		# Set rbp to 0			\n\
	   # We moved to the new stack.					\n\
	   # Now it's safe to release the OS stack.			\n\
	   movl  $0x8000, %%r8d		# dwFreeType: MEM_RELEASE	\n\
	   xorl  %%edx, %%edx		# dwSize:     0			\n\
	   # dwAddress is already in the correct arg register rcx	\n\
	   call  VirtualFree						\n\
	   # All set.  We can copy the thread arg from the safe		\n\
	   # register r13 and then just call the function.		\n\
	   movq  %%r13, %%rcx		# Move thread arg to 1st arg reg\n\
	   call  *%%r12			# Call thread func		\n"
	   : : [WRAPPER_ARG] "o" (wrapper_arg),
	       [CYGTLS] "i" (__CYGTLS_PADSIZE__));
#else
#error unimplemented for this target
#endif
  /* pthread::thread_init_wrapper calls pthread::exit, which
     in turn calls ExitThread, so we should never arrive here. */
  api_fatal ("Dumb thinko in pthread handling.  Whip the developer.");
}

/* The memory region used for thread stacks. The memory layout is outlined
   in heap.cc, function eval_start_address(). */
#define THREAD_STORAGE_LOW	0x600000000L
#define THREAD_STORAGE_HIGH	0x800000000L
/* We provide the stacks always in 1 Megabyte slots */
#define THREAD_STACK_SLOT	0x000100000L	/* 1 Meg */
/* Maximum stack size returned from the pool. */
#define THREAD_STACK_MAX	0x040000000L	/* 1 Gig */

class thread_allocator
{
  UINT_PTR current;
  PVOID (thread_allocator::*alloc_func) (SIZE_T);
  PVOID _alloc (SIZE_T size)
  {
    static const MEM_ADDRESS_REQUIREMENTS thread_req = {
      (PVOID) THREAD_STORAGE_LOW,
      (PVOID) (THREAD_STORAGE_HIGH - 1),
      THREAD_STACK_SLOT
    };
    /* g++ 11.2 workaround: don't use initializer */
    MEM_EXTENDED_PARAMETER thread_ext = { 0 };
    thread_ext.Type = MemExtendedParameterAddressRequirements;
    thread_ext.Pointer = (PVOID) &thread_req;

    SIZE_T real_size = roundup2 (size, THREAD_STACK_SLOT);
    PVOID real_stackaddr = NULL;

    if (real_size <= THREAD_STACK_MAX)
      real_stackaddr = VirtualAlloc2 (GetCurrentProcess(), NULL, real_size,
				      MEM_RESERVE | MEM_TOP_DOWN,
				      PAGE_READWRITE, &thread_ext, 1);
    /* If the thread area allocation failed, or if the application requests a
       monster stack, fulfill request from mmap area. */
    if (!real_stackaddr)
      {
	static const MEM_ADDRESS_REQUIREMENTS mmap_req = {
	  (PVOID) MMAP_STORAGE_LOW,
	  (PVOID) (MMAP_STORAGE_HIGH - 1),
	  THREAD_STACK_SLOT
	};
	/* g++ 11.2 workaround: don't use initializer */
	MEM_EXTENDED_PARAMETER mmap_ext = { 0 };
	mmap_ext.Type = MemExtendedParameterAddressRequirements;
	mmap_ext.Pointer = (PVOID) &mmap_req;

	real_stackaddr = VirtualAlloc2 (GetCurrentProcess(), NULL, real_size,
					MEM_RESERVE | MEM_TOP_DOWN,
					PAGE_READWRITE, &mmap_ext, 1);
      }
    return real_stackaddr;
  }
  PVOID _alloc_old (SIZE_T size)
  {
    SIZE_T real_size = roundup2 (size, THREAD_STACK_SLOT);
    BOOL overflow = FALSE;
    PVOID real_stackaddr = NULL;

    /* If an application requests a monster stack, fulfill request
       from mmap area. */
    if (real_size > THREAD_STACK_MAX)
      {
	PVOID addr = mmap_alloc.alloc (NULL, real_size, false);
	return VirtualAlloc (addr, real_size, MEM_RESERVE, PAGE_READWRITE);
      }
    /* Simple round-robin.  Keep looping until VirtualAlloc succeeded, or
       until we overflowed and hit the current address. */
    for (UINT_PTR addr = current - real_size;
	 !real_stackaddr && (!overflow || addr >= current);
	 addr -= THREAD_STACK_SLOT)
      {
	if (addr < THREAD_STORAGE_LOW)
	  {
	    addr = THREAD_STORAGE_HIGH - real_size;
	    overflow = TRUE;
	  }
	real_stackaddr = VirtualAlloc ((PVOID) addr, real_size,
				       MEM_RESERVE, PAGE_READWRITE);
	if (!real_stackaddr)
	  {
	    /* So we couldn't grab this space.  Let's check the state.
	       If this area is free, simply try the next lower 1 Meg slot.
	       Otherwise, shift the next try down to the AllocationBase
	       of the current address, minus the requested slot size.
	       Add THREAD_STACK_SLOT since that's subtracted in the next
	       run of the loop anyway. */
	    MEMORY_BASIC_INFORMATION mbi;
	    VirtualQuery ((PVOID) addr, &mbi, sizeof mbi);
	    if (mbi.State != MEM_FREE)
	      addr = (UINT_PTR) mbi.AllocationBase - real_size
						    + THREAD_STACK_SLOT;
	  }
      }
    /* If we got an address, remember it for the next allocation attempt. */
    if (real_stackaddr)
      current = (UINT_PTR) real_stackaddr;
    else
      set_errno (EAGAIN);
    return real_stackaddr;
  }
public:
  thread_allocator () : current (THREAD_STORAGE_HIGH)
  {
    alloc_func = wincap.has_extended_mem_api () ? &_alloc : &_alloc_old;
  }
  PVOID alloc (SIZE_T size)
  {
    return (this->*alloc_func) (size);
  }
};

thread_allocator thr_alloc NO_COPY;

/* Just set up a system-like main thread stack from the pthread stack area
   maintained by the thr_alloc class.  See the description in the x86_64-only
   code in _dll_crt0 to understand why we have to do this. */
PVOID
create_new_main_thread_stack (PVOID &allocationbase)
{
  PIMAGE_DOS_HEADER dosheader;
  PIMAGE_NT_HEADERS ntheader;
  SIZE_T stacksize;
  ULONG guardsize;
  SIZE_T commitsize;
  PBYTE stacklimit;

  dosheader = (PIMAGE_DOS_HEADER) GetModuleHandle (NULL);
  ntheader = (PIMAGE_NT_HEADERS)
	     ((PBYTE) dosheader + dosheader->e_lfanew);
  stacksize = ntheader->OptionalHeader.SizeOfStackReserve;
  stacksize = roundup2 (stacksize, wincap.allocation_granularity ());

  allocationbase
	= thr_alloc.alloc (ntheader->OptionalHeader.SizeOfStackReserve);
  guardsize = wincap.def_guard_page_size ();
  commitsize = ntheader->OptionalHeader.SizeOfStackCommit;
  commitsize = roundup2 (commitsize, wincap.page_size ());
  if (commitsize > stacksize - guardsize - wincap.page_size ())
    commitsize = stacksize - guardsize - wincap.page_size ();
  stacklimit = (PBYTE) allocationbase + stacksize - commitsize - guardsize;
  /* Setup guardpage. */
  if (!VirtualAlloc (stacklimit, guardsize,
		     MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD))
    return NULL;
  /* Setup committed region. */
  stacklimit += guardsize;
  if (!VirtualAlloc (stacklimit, commitsize, MEM_COMMIT, PAGE_READWRITE))
    return NULL;
  NtCurrentTeb()->Tib.StackBase = ((PBYTE) allocationbase + stacksize);
  NtCurrentTeb()->Tib.StackLimit = stacklimit;
  return ((PBYTE) allocationbase + stacksize - 16);
}

HANDLE
CygwinCreateThread (LPTHREAD_START_ROUTINE thread_func, PVOID thread_arg,
		    PVOID stackaddr, ULONG stacksize, ULONG guardsize,
		    DWORD creation_flags, LPDWORD thread_id)
{
  PVOID real_stackaddr = NULL;
  ULONG real_stacksize = 0;
  ULONG real_guardsize = 0;
  pthread_wrapper_arg *wrapper_arg;
  HANDLE thread = NULL;

  wrapper_arg = (pthread_wrapper_arg *) ccalloc (HEAP_STR, 1,
						sizeof *wrapper_arg);
  if (!wrapper_arg)
    {
      SetLastError (ERROR_OUTOFMEMORY);
      return NULL;
    }
  wrapper_arg->func = thread_func;
  wrapper_arg->arg = thread_arg;

  if (stackaddr)
    {
      /* If the application provided the stack, just use it.  There won't
	 be any stack overflow handling! */
      wrapper_arg->stackaddr = (PBYTE) stackaddr;
      wrapper_arg->stackbase = (PBYTE) stackaddr + stacksize;
    }
  else
    {
      PBYTE real_stacklimit;

      /* If not, we have to create the stack here. */
      real_stacksize = roundup2 (stacksize, wincap.page_size ());
      real_guardsize = roundup2 (guardsize, wincap.page_size ());
      /* Add the guardsize to the stacksize */
      real_stacksize += real_guardsize;
      /* Take dead zone page into account, which always stays uncommited. */
      real_stacksize += wincap.page_size ();
      /* Now roundup the result to the next allocation boundary. */
      real_stacksize = roundup2 (real_stacksize,
				 wincap.allocation_granularity ());
      /* Reserve stack. */
      real_stackaddr = thr_alloc.alloc (real_stacksize);
      if (!real_stackaddr)
	return NULL;
      /* Set up committed region.  We set up the stack like the OS does,
	 with a reserved region, the guard pages, and a commited region.
	 We commit the stack commit size from the executable header, but
	 at least PTHREAD_STACK_MIN (64K). */
      static ULONG exe_commitsize;

      if (!exe_commitsize)
	{
	  PIMAGE_DOS_HEADER dosheader;
	  PIMAGE_NT_HEADERS ntheader;

	  dosheader = (PIMAGE_DOS_HEADER) GetModuleHandle (NULL);
	  ntheader = (PIMAGE_NT_HEADERS)
		     ((PBYTE) dosheader + dosheader->e_lfanew);
	  exe_commitsize = ntheader->OptionalHeader.SizeOfStackCommit;
	  exe_commitsize = roundup2 (exe_commitsize, wincap.page_size ());
	}
      ULONG commitsize = exe_commitsize;
      if (commitsize > real_stacksize - real_guardsize - wincap.page_size ())
	commitsize = real_stacksize - real_guardsize - wincap.page_size ();
      else if (commitsize < PTHREAD_STACK_MIN)
	commitsize = PTHREAD_STACK_MIN;
      real_stacklimit = (PBYTE) real_stackaddr + real_stacksize
			- commitsize - real_guardsize;
      if (!VirtualAlloc (real_stacklimit, real_guardsize, MEM_COMMIT,
			 PAGE_READWRITE | PAGE_GUARD))
	goto err;
      real_stacklimit += real_guardsize;
      if (!VirtualAlloc (real_stacklimit, commitsize, MEM_COMMIT,
			 PAGE_READWRITE))
	goto err;

      wrapper_arg->stackaddr = (PBYTE) real_stackaddr;
      wrapper_arg->stackbase = (PBYTE) real_stackaddr + real_stacksize;
      wrapper_arg->stacklimit = real_stacklimit;
      wrapper_arg->guardsize = real_guardsize;
    }
  /* Use the STACK_SIZE_PARAM_IS_A_RESERVATION parameter so only the
     minimum size for a thread stack is reserved by the OS.  Note that we
     reserve a 256K stack, not 64K, otherwise the thread creation might
     crash the process due to a stack overflow. */
  thread = CreateThread (&sec_none_nih, 4 * PTHREAD_STACK_MIN,
			 pthread_wrapper, wrapper_arg,
			 creation_flags | STACK_SIZE_PARAM_IS_A_RESERVATION,
			 thread_id);

err:
  if (!thread && real_stackaddr)
    {
      /* Don't report the wrong error even though VirtualFree is very unlikely
	 to fail. */
      DWORD err = GetLastError ();
      VirtualFree (real_stackaddr, 0, MEM_RELEASE);
      SetLastError (err);
    }
  return thread;
}

/* Signal the thread name to any attached debugger

   (See "How to: Set a Thread Name in Native Code"
   https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx) */

#define MS_VC_EXCEPTION 0x406D1388

static void
SetThreadNameExc (DWORD dwThreadID, const char* threadName)
{
  if (!IsDebuggerPresent ())
    return;

  ULONG_PTR info[] =
    {
      0x1000,                 /* type, must be 0x1000 */
      (ULONG_PTR) threadName, /* pointer to threadname */
      dwThreadID,             /* thread ID (+ flags on x86_64) */
    };

  __try
    {
      RaiseException (MS_VC_EXCEPTION, 0, sizeof (info) / sizeof (ULONG_PTR),
		      info);
    }
  __except (NO_ERROR)
  __endtry
}

void
SetThreadName (DWORD dwThreadID, const char* threadName)
{
  HANDLE hThread = OpenThread (THREAD_SET_LIMITED_INFORMATION, FALSE, dwThreadID);
  if (hThread)
    {
      /* SetThreadDescription only exists in a wide-char version, so we must
	 convert threadname to wide-char.  The encoding of threadName is
	 unclear, so use UTF8 until we know better. */
      int bufsize = MultiByteToWideChar (CP_UTF8, 0, threadName, -1, NULL, 0);
      WCHAR buf[bufsize];
      bufsize = MultiByteToWideChar (CP_UTF8, 0, threadName, -1, buf, bufsize);
      HRESULT hr = SetThreadDescription (hThread, buf);
      if (hr != S_OK)
	{
	  debug_printf ("SetThreadDescription() failed. %08x %08x\n",
			GetLastError (), hr);
	}
      CloseHandle (hThread);
    }

  /* also use the older, exception-based method of setting threadname for the
     benefit of things which don't known about GetThreadDescription. */
  SetThreadNameExc (dwThreadID, threadName);
}

#define add_size(p,s) ((p) = ((__typeof__(p))((PBYTE)(p)+(s))))

static WORD num_cpu_per_group = 0;
static WORD group_count = 0;

WORD
__get_cpus_per_group (void)
{
  tmp_pathbuf tp;

  if (num_cpu_per_group)
    return num_cpu_per_group;

  num_cpu_per_group = 64;
  group_count = 1;

  PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX lpi =
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) tp.c_get ();
  DWORD lpi_size = NT_MAX_PATH;

  /* Fake a SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX group info block if
     GetLogicalProcessorInformationEx fails for some reason. */
  if (!GetLogicalProcessorInformationEx (RelationGroup, lpi, &lpi_size))
    {
      lpi_size = sizeof *lpi;
      lpi->Relationship = RelationGroup;
      lpi->Size = lpi_size;
      lpi->Group.MaximumGroupCount = 1;
      lpi->Group.ActiveGroupCount = 1;
      lpi->Group.GroupInfo[0].MaximumProcessorCount = wincap.cpu_count ();
      lpi->Group.GroupInfo[0].ActiveProcessorCount
        = __builtin_popcountl (wincap.cpu_mask ());
      lpi->Group.GroupInfo[0].ActiveProcessorMask = wincap.cpu_mask ();
    }

  PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX plpi = lpi;
  for (DWORD size = lpi_size; size > 0;
       size -= plpi->Size, add_size (plpi, plpi->Size))
    if (plpi->Relationship == RelationGroup)
      {
        /* There are systems with a MaximumProcessorCount not reflecting the
	   actually available CPUs.  The ActiveProcessorCount is correct
	   though.  So we just use ActiveProcessorCount for now, hoping for
	   the best. */
        num_cpu_per_group = plpi->Group.GroupInfo[0].ActiveProcessorCount;

	/* Follow that lead to get the group count. */
	group_count = plpi->Group.ActiveGroupCount;
        break;
      }

  return num_cpu_per_group;
}

WORD
__get_group_count (void)
{
  if (group_count == 0)
    (void) __get_cpus_per_group (); // caller should have called this first
  return group_count;
}
