/* Functions for memory limit warnings.
   Copyright (C) 1990, 1992, 2001-2012  Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <http://www.gnu.org/licenses/>.  */

#include <config.h>
#include <unistd.h> /* for 'environ', on AIX */
#include "lisp.h"
#include "mem-limits.h"

/*
  Level number of warnings already issued.
  0 -- no warnings issued.
  1 -- 75% warning already issued.
  2 -- 85% warning already issued.
  3 -- 95% warning issued; keep warning frequently.
*/
enum warnlevel { not_warned, warned_75, warned_85, warned_95 };
static enum warnlevel warnlevel;

typedef void *POINTER;

/* Function to call to issue a warning;
   0 means don't issue them.  */
static void (*warn_function) (const char *);

/* Start of data space; can be changed by calling malloc_init.  */
static POINTER data_space_start;

/* Number of bytes of writable memory we can expect to be able to get.  */
static size_t lim_data;


#ifdef HAVE_GETRLIMIT

# ifndef RLIMIT_AS
#  define RLIMIT_AS RLIMIT_DATA
# endif

static void
get_lim_data (void)
{
  /* Set LIM_DATA to the minimum of the maximum object size and the
     maximum address space.  Don't bother to check for values like
     RLIM_INFINITY since in practice they are not much less than SIZE_MAX.  */
  struct rlimit rlimit;
  lim_data
    = (getrlimit (RLIMIT_AS, &rlimit) == 0 && rlimit.rlim_cur <= SIZE_MAX
       ? rlimit.rlim_cur
       : SIZE_MAX);
}

#elif defined WINDOWSNT

#include "w32heap.h"

static void
get_lim_data (void)
{
  extern size_t reserved_heap_size;
  lim_data = reserved_heap_size;
}

#elif defined MSDOS

void
get_lim_data (void)
{
  _go32_dpmi_meminfo info;
  unsigned long lim1, lim2;

  _go32_dpmi_get_free_memory_information (&info);
  /* DPMI server of Windows NT and its descendants reports in
     info.available_memory a much lower amount that is really
     available, which causes bogus "past 95% of memory limit"
     warnings.  Try to overcome that via circumstantial evidence.  */
  lim1 = info.available_memory;
  lim2 = info.available_physical_pages;
  /* DPMI Spec: "Fields that are unavailable will hold -1."  */
  if ((long)lim1 == -1L)
    lim1 = 0;
  if ((long)lim2 == -1L)
    lim2 = 0;
  else
    lim2 *= 4096;
  /* Surely, the available memory is at least what we have physically
     available, right?  */
  if (lim1 >= lim2)
    lim_data = lim1;
  else
    lim_data = lim2;
  /* Don't believe they will give us more that 0.5 GB.   */
  if (lim_data > 512U * 1024U * 1024U)
    lim_data = 512U * 1024U * 1024U;
}

unsigned long
ret_lim_data (void)
{
  get_lim_data ();
  return lim_data;
}
#else
# error "get_lim_data not implemented on this machine"
#endif

/* Verify amount of memory available, complaining if we're near the end. */

static void
check_memory_limits (void)
{
#ifdef REL_ALLOC
  extern POINTER (*real_morecore) (ptrdiff_t);
#endif
  extern POINTER (*__morecore) (ptrdiff_t);

  register POINTER cp;
  size_t five_percent;
  size_t data_size;
  enum warnlevel new_warnlevel;

  if (lim_data == 0)
    get_lim_data ();
  five_percent = lim_data / 20;

  /* Find current end of memory and issue warning if getting near max */
#ifdef REL_ALLOC
  if (real_morecore)
    cp = (char *) (*real_morecore) (0);
  else
#endif
  cp = (char *) (*__morecore) (0);
  data_size = (char *) cp - (char *) data_space_start;

  if (!warn_function)
    return;

  /* What level of warning does current memory usage demand?  */
  new_warnlevel
    = (data_size > five_percent * 19) ? warned_95
    : (data_size > five_percent * 17) ? warned_85
    : (data_size > five_percent * 15) ? warned_75
    : not_warned;

  /* If we have gone up a level, give the appropriate warning.  */
  if (new_warnlevel > warnlevel || new_warnlevel == warned_95)
    {
      warnlevel = new_warnlevel;
      switch (warnlevel)
	{
	case warned_75:
	  (*warn_function) ("Warning: past 75% of memory limit");
	  break;

	case warned_85:
	  (*warn_function) ("Warning: past 85% of memory limit");
	  break;

	case warned_95:
	  (*warn_function) ("Warning: past 95% of memory limit");
	}
    }
  /* Handle going down in usage levels, with some hysteresis.  */
  else
    {
      /* If we go down below 70% full, issue another 75% warning
	 when we go up again.  */
      if (data_size < five_percent * 14)
	warnlevel = not_warned;
      /* If we go down below 80% full, issue another 85% warning
	 when we go up again.  */
      else if (warnlevel > warned_75 && data_size < five_percent * 16)
	warnlevel = warned_75;
      /* If we go down below 90% full, issue another 95% warning
	 when we go up again.  */
      else if (warnlevel > warned_85 && data_size < five_percent * 18)
	warnlevel = warned_85;
    }

  if (EXCEEDS_LISP_PTR (cp))
    (*warn_function) ("Warning: memory in use exceeds lisp pointer size");
}

#if !defined (CANNOT_DUMP) || !defined (SYSTEM_MALLOC)
/* Some systems that cannot dump also cannot implement these.  */

/*
 *	Return the address of the start of the data segment prior to
 *	doing an unexec.  After unexec the return value is undefined.
 *	See crt0.c for further information and definition of data_start.
 *
 *	Apparently, on BSD systems this is etext at startup.  On
 *	USG systems (swapping) this is highly mmu dependent and
 *	is also dependent on whether or not the program is running
 *	with shared text.  Generally there is a (possibly large)
 *	gap between end of text and start of data with shared text.
 *
 */

char *
start_of_data (void)
{
#ifdef BSD_SYSTEM
  extern char etext;
  return (POINTER)(&etext);
#elif defined DATA_START
  return ((POINTER) DATA_START);
#elif defined ORDINARY_LINK
  /*
   * This is a hack.  Since we're not linking crt0.c or pre_crt0.c,
   * data_start isn't defined.  We take the address of environ, which
   * is known to live at or near the start of the system crt0.c, and
   * we don't sweat the handful of bytes that might lose.
   */
  return ((POINTER) &environ);
#else
  extern int data_start;
  return ((POINTER) &data_start);
#endif
}
#endif /* (not CANNOT_DUMP or not SYSTEM_MALLOC) */

/* Enable memory usage warnings.
   START says where the end of pure storage is.
   WARNFUN specifies the function to call to issue a warning.  */

void
memory_warnings (POINTER start, void (*warnfun) (const char *))
{
  extern void (* __after_morecore_hook) (void);     /* From gmalloc.c */

  if (start)
    data_space_start = start;
  else
    data_space_start = start_of_data ();

  warn_function = warnfun;
  __after_morecore_hook = check_memory_limits;

  /* Force data limit to be recalculated on each run.  */
  lim_data = 0;
}
