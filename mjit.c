/**********************************************************************

  mjit.c - MRI method JIT infrastructure

  Copyright (C) 2017 Vladimir Makarov <vmakarov@redhat.com>.

**********************************************************************/

/* We utilize widely used C compilers (GCC and LLVM Clang) to
   implement MJIT.  We feed them a C code generated from ISEQ.  The
   industrial C compilers are slower than regular JIT engines.
   Generated code performance of the used C compilers has a higher
   priority over the compilation speed.

   So our major goal is to minimize the ISEQ compilation time when we
   use widely optimization level (-O2).  It is achieved by

   o Using a precompiled version of the header
   o Keeping all files in `/tmp`.  On modern Linux `/tmp` is a file
     system in memory. So it is pretty fast
   o Implementing MJIT as a multi-threaded code because we want to
     compile ISEQs in parallel with iseq execution to speed up Ruby
     code execution.  MJIT has one thread (*worker*) to do
     parallel compilations:
      o It prepares a precompiled code of the minimized header.
	It starts at the MRI execution start
      o It generates PIC object files of ISEQs
      o It takes one JIT unit from a priority queue unless it is empty.
      o It translates the JIT unit ISEQ into C-code using the precompiled
        header, calls CC and load PIC code when it is ready
      o Currently MJIT put ISEQ in the queue when ISEQ is called
      o MJIT can reorder ISEQs in the queue if some ISEQ has been called
        many times and its compilation did not start yet
      o MRI reuses the machine code if it already exists for ISEQ
      o The machine code we generate can stop and switch to the ISEQ
        interpretation if some condition is not satisfied as the machine
        code can be speculative or some exception raises
      o Speculative machine code can be canceled.

   Here is a diagram showing the MJIT organization:

                 _______
                |header |
                |_______|
                    |                         MRI building
      --------------|----------------------------------------
                    |                         MRI execution
     	            |
       _____________|_____
      |             |     |
      |          ___V__   |  CC      ____________________
      |         |      |----------->| precompiled header |
      |         |      |  |         |____________________|
      |         |      |  |              |
      |         | MJIT |  |              |
      |         |      |  |              |
      |         |      |  |          ____V___  CC  __________
      |         |______|----------->| C code |--->| .so file |
      |                   |         |________|    |__________|
      |                   |                              |
      |                   |                              |
      | MRI machine code  |<-----------------------------
      |___________________|             loading


   We don't use SIGCHLD signal and WNOHANG waitpid in MJIT as it
   might mess with ruby code dealing with signals.  Also as SIGCHLD
   signal can be delivered to non-main thread, the stack might have a
   constraint.  So the correct version of code based on SIGCHLD and
   WNOHANG waitpid would be very complicated.  */

#include "vm_core.h"
#include "mjit.h"

/* A copy of MJIT portion of MRI options since MJIT initialization.  We
   need them as MJIT threads still can work when the most MRI data were
   freed. */
struct mjit_options mjit_opts;

/* The unit structure.  */
struct rb_mjit_unit {
    /* Unique order number of unit.  */
    int id;
    /* Dlopen handle of the loaded object file.  */
    void *handle;
    /* Units in lists are linked with the following members.  */
    struct rb_mjit_unit *next, *prev;
    const rb_iseq_t *iseq;
};

/* TRUE if MJIT is initialized and will be used.  */
int mjit_init_p = FALSE;

/* Priority queue of iseqs waiting for JIT compilation.
   This variable is a pointer to head unit of the queue. */
static struct rb_mjit_unit *unit_queue;
/* The number of so far processed ISEQs.  */
static int current_unit_num;
/* A mutex for conitionals and critical sections.  */
static pthread_mutex_t mjit_engine_mutex;

/* Print the arguments according to FORMAT to stderr only if MJIT
   verbose option value is more or equal to LEVEL.  */
PRINTF_ARGS(static void, 2, 3)
verbose(int level, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    if (mjit_opts.verbose >= level)
	vfprintf(stderr, format, args);
    va_end(args);
    if (mjit_opts.verbose >= level)
	fprintf(stderr, "\n");
}

/* Start a critical section.  Use message MSG to print debug info at
   LEVEL.  */
static inline void
CRITICAL_SECTION_START(int level, const char *msg)
{
    int err_code;

    verbose(level, "Locking %s", msg);
    if ((err_code = pthread_mutex_lock(&mjit_engine_mutex)) != 0) {
	fprintf(stderr, "Cannot lock MJIT mutex '%s'\n", msg);
	fprintf(stderr, "error: %s\n", strerror(err_code));
    }
    verbose(level, "Locked %s", msg);
}

/* Finish the current critical section.  Use message MSG to print
   debug info at LEVEL. */
static inline void
CRITICAL_SECTION_FINISH(int level, const char *msg)
{
    verbose(level, "Unlocked %s", msg);
    pthread_mutex_unlock(&mjit_engine_mutex);
}

/* Add unit UNIT to the tail of doubly linked LIST.  It should be not in
   the list before.  */
static void
add_to_unit_queue(struct rb_mjit_unit *unit)
{
    /* Append iseq to list */
    if (unit_queue == NULL) {
	unit_queue = unit;
    } else {
	struct rb_mjit_unit *tail = unit_queue;
	while (tail->next != NULL) {
	    tail = tail->next;
	}
	tail->next = unit;
	unit->prev = tail;
    }
}

/* Create unit for ISEQ. */
static void
create_unit(const rb_iseq_t *iseq)
{
    struct rb_mjit_unit *unit;

    unit = ZALLOC(struct rb_mjit_unit);
    if (unit == NULL)
	return;

    unit->id = current_unit_num++;
    unit->iseq = iseq;
    iseq->body->jit_unit = unit;
}

/* Add ISEQ to be JITed in parallel with the current thread.
   Unload some JIT codes if there are too many of them.  */
void
mjit_add_iseq_to_process(const rb_iseq_t *iseq)
{
    struct rb_mjit_unit *unit;

    if (!mjit_init_p)
	return;

    create_unit(iseq);
    if ((unit = iseq->body->jit_unit) == NULL)
	/* Failure in creating the unit.  */
	return;

    CRITICAL_SECTION_START(3, "in add_iseq_to_process");
    add_to_unit_queue(unit);
    /* TODO: Unload some units if it's >= max_cache_size */
    /* TODO: wakeup worker */
    CRITICAL_SECTION_FINISH(3, "in add_iseq_to_process");
}

/* Initialize MJIT.  Start a thread creating the precompiled header and
   processing ISeqs.  The function should be called first for using MJIT.
   If everything is successfull, MJIT_INIT_P will be TRUE.  */
void
mjit_init(struct mjit_options *opts)
{
    mjit_opts = *opts;
    mjit_init_p = TRUE;

    if (pthread_mutex_init(&mjit_engine_mutex, NULL) != 0) {
	mjit_init_p = FALSE;
	verbose(1, "Failure in MJIT initialization\n");
	return;
    }
}

/* Finish the threads processing units and creating PCH, finalize
   and free MJIT data.  It should be called last during MJIT
   life.  */
void
mjit_finish()
{
    if (!mjit_init_p)
	return;

    mjit_init_p = FALSE;
    pthread_mutex_destroy(&mjit_engine_mutex);

    /* TODO: free unit_queue */

    verbose(1, "Successful MJIT finish");
}
