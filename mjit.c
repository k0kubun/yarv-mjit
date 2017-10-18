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

#include <sys/wait.h>
#include <sys/time.h>
#include <dlfcn.h>
#include "vm_core.h"
#include "mjit.h"
#include "version.h"

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
/* A thread conditional to wake up `mjit_finish` at the end of PCH thread.  */
static pthread_cond_t mjit_pch_wakeup;
/* A thread conditional to wake up the client if there is a change in
   executed unit status.  */
static pthread_cond_t mjit_client_wakeup;
/* A thread conditional to wake up a worker if there we have something
   to add or we need to stop MJIT engine.  */
static pthread_cond_t mjit_worker_wakeup;
/* A thread conditional to wake up workers if at the end of GC.  */
static pthread_cond_t mjit_gc_wakeup;
/* True when GC is working.  */
static int in_gc;
/* True when JIT is working.  */
static int in_jit;

/* Defined in the client thread before starting MJIT threads:  */
/* Used C compiler path.  */
static const char *cc_path;
/* Name of the header file.  */
static char *header_file;
/* Name of the precompiled header file.  */
static char *pch_file;

/* Return time in milliseconds as a double.  */
static double
real_ms_time()
{
    struct timeval  tv;

    gettimeofday(&tv, NULL);
    return tv.tv_usec / 1000.0 + tv.tv_sec * 1000.0;
}

/* Make and return copy of STR in the heap.  Return NULL in case of a
   failure.  */
static char *
get_string(const char *str)
{
    char *res;

    if ((res = xmalloc(strlen(str) + 1)) != NULL)
	strcpy(res, str);
    return res;
}

static void
sprint_uniq_filename(char *str, unsigned long id, const char *prefix, const char *suffix)
{
    sprintf(str, "/tmp/%sp%luu%lu%s", prefix, (unsigned long) getpid(), id, suffix);
}

/* Return an unique file name in /tmp with PREFIX and SUFFIX and
   number ID.  Use getpid if ID == 0.  The return file name exists
   until the next function call.  */
static char *
get_uniq_filename(unsigned long id, const char *prefix, const char *suffix)
{
    char str[70];
    sprint_uniq_filename(str, id, prefix, suffix);
    return get_string(str);
}

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

/* Return length of NULL-terminated array ARGS excluding the NULL
   marker.  */
static size_t
args_len(char *const *args)
{
    size_t i;

    for (i = 0; (args[i]) != NULL;i++)
	;
    return i;
}

/* Concatenate NUM passed NULL-terminated arrays of strings, put the
   result (with NULL end marker) into the heap, and return the
   result.  */
static char **
form_args(int num, ...)
{
    va_list argp, argp2;
    size_t len, disp;
    int i;
    char **args, **res;

    va_start(argp, num);
    va_copy(argp2, argp);
    for (i = len = 0; i < num; i++) {
	args = va_arg(argp, char **);
	len += args_len(args);
    }
    va_end(argp);
    if ((res = xmalloc((len + 1) * sizeof(char *))) == NULL)
	return NULL;
    for (i = disp = 0; i < num; i++) {
	args = va_arg(argp2, char **);
	len = args_len(args);
	memmove(res + disp, args, len * sizeof(char *));
	disp += len;
    }
    res[disp] = NULL;
    va_end(argp2);
    return res;
}

/* Start an OS process of executable PATH with arguments ARGV.  Return
   PID of the process.  */
static pid_t
start_process(const char *path, char *const argv[])
{
    pid_t pid;

    if (mjit_opts.verbose >= 2) {
	int i;
	const char *arg;

	fprintf(stderr, "Starting process: %s", path);
	for (i = 0; (arg = argv[i]) != NULL; i++)
	    fprintf(stderr, " %s", arg);
	fprintf(stderr, "\n");
    }
    if ((pid = vfork()) == 0) {
	if (mjit_opts.verbose == 0) {
	    /* CC can be started in a thread using a file which has been
	       already removed while MJIT is finishing.  Discard the
	       messages about missing files.  */
	    FILE *f = fopen("/dev/null", "w");

	    dup2(fileno(f), STDERR_FILENO);
	    dup2(fileno(f), STDOUT_FILENO);
	}
	pid = execvp(path, argv); /* Pid will be negative on an error */
	/* Even if we successfully found CC to compile PCH we still can
	 fail with loading the CC in very rare cases for some reasons.
	 Stop the forked process in this case.  */
	fprintf(stderr, "MJIT: Error in execvp: %s", path);
	_exit(1);
    }
    return pid;
}

/* Execute an OS process of executable PATH with arguments ARGV.
   Return -1 if failed to execute, otherwise exit code of the process.  */
static int
exec_process(const char *path, char *const argv[])
{
    int stat, exit_code;
    pid_t pid;

    pid = start_process(path, argv);
    if (pid <= 0)
	return -1;

    for (;;) {
	waitpid(pid, &stat, 0);
	if (WIFEXITED(stat)) {
	    exit_code = WEXITSTATUS(stat);
	    break;
	} else if (WIFSIGNALED(stat)) {
	    exit_code = -1;
	    break;
	}
    }
    return exit_code;
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

/* Wait until workers don't compile any iseq.  It is called at the
   start of GC.  */
void
mjit_gc_start_hook()
{
    if (!mjit_init_p)
	return;
    CRITICAL_SECTION_START(4, "mjit_gc_start_hook");
    while (in_jit) {
	verbose(4, "Waiting wakeup from a worker for GC");
	pthread_cond_wait(&mjit_client_wakeup, &mjit_engine_mutex);
	verbose(4, "Getting wakeup from a worker for GC");
    }
    in_gc = TRUE;
    CRITICAL_SECTION_FINISH(4, "mjit_gc_start_hook");
}

/* Send a signal to workers to continue iseq compilations.  It is
   called at the end of GC.  */
void
mjit_gc_finish_hook()
{
    if (!mjit_init_p)
	return;
    CRITICAL_SECTION_START(4, "mjit_gc_finish_hook");
    in_gc = FALSE;
    verbose(4, "Sending wakeup signal to workers after GC");
    if (pthread_cond_broadcast(&mjit_gc_wakeup) != 0) {
        fprintf(stderr, "Cannot send wakeup signal to workers in mjit_gc_finish_hook\n");
    }
    CRITICAL_SECTION_FINISH(4, "mjit_gc_finish_hook");
}

/* Iseqs can be garbage collected.  This function should call when it
   happens.  It removes iseq from the unit.  */
void
mjit_free_iseq(const rb_iseq_t *iseq)
{
    if (!mjit_init_p)
	return;
    CRITICAL_SECTION_START(4, "mjit_free_iseq");
    if (iseq->body->jit_unit) {
	iseq->body->jit_unit->iseq = NULL;
    }
    /* TODO: unload unit */
    CRITICAL_SECTION_FINISH(4, "mjit_free_iseq");
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

static void
remove_from_unit_queue(struct rb_mjit_unit *unit)
{
    if (unit->prev && unit->next) {
	unit->prev->next = unit->next;
	unit->next->prev = unit->prev;
    } else if (unit->prev == NULL && unit->next) {
	unit_queue = unit->next;
	unit->next->prev = NULL;
    } else if (unit->prev && unit->next == NULL) {
	unit->prev->next = NULL;
    } else {
	unit_queue = NULL;
    }
}

/* Remove and return the best unit from unit_queue.  The best
   is the first high priority unit or the unit whose iseq has the
   biggest number of calls so far.  */
static struct rb_mjit_unit *
get_from_unit_queue()
{
    struct rb_mjit_unit *unit, *dequeued = NULL;

    if (unit_queue == NULL)
	return NULL;

    /* Find iseq with max total_calls */
    for (unit = unit_queue; unit != NULL; unit = unit ? unit->next : NULL) {
	if (unit->iseq == NULL) {
	    continue; /* TODO: GCed. remove from queue and free */
	}

	if (dequeued == NULL || dequeued->iseq->body->total_calls < unit->iseq->body->total_calls) {
	    dequeued = unit;
	}
    }

    if (dequeued)
	remove_from_unit_queue(dequeued);
    return dequeued;
}

/* XXX_COMMONN_ARGS define the command line arguments of XXX C
   compiler used by MJIT.

   XXX_EMIT_PCH_ARGS define additional options to generate the
   precomiled header.

   XXX_USE_PCH_ARAGS define additional options to use the precomiled
   header.  */
static const char *GCC_COMMON_ARGS_DEBUG[] = {"gcc", "-O0", "-g", "-Wfatal-errors", "-fPIC", "-shared", "-w", "-pipe", "-nostartfiles", "-nodefaultlibs", "-nostdlib", NULL};
static const char *GCC_COMMON_ARGS[] = {"gcc", "-O2", "-Wfatal-errors", "-fPIC", "-shared", "-w", "-pipe", "-nostartfiles", "-nodefaultlibs", "-nostdlib", NULL};
static const char *GCC_USE_PCH_ARGS[] = {"-I/tmp", NULL};
static const char *GCC_EMIT_PCH_ARGS[] = {NULL};

#ifdef __MACH__

static const char *LLVM_COMMON_ARGS_DEBUG[] = {"clang", "-O0", "-g", "-dynamic", "-I/usr/local/include", "-L/usr/local/lib", "-w", "-bundle", NULL};
static const char *LLVM_COMMON_ARGS[] = {"clang", "-O2", "-dynamic", "-I/usr/local/include", "-L/usr/local/lib", "-w", "-bundle", NULL};

#else

static const char *LLVM_COMMON_ARGS_DEBUG[] = {"clang", "-O0", "-g", "-fPIC", "-shared", "-I/usr/local/include", "-L/usr/local/lib", "-w", "-bundle", NULL};
static const char *LLVM_COMMON_ARGS[] = {"clang", "-O2", "-fPIC", "-shared", "-I/usr/local/include", "-L/usr/local/lib", "-w", "-bundle", NULL};

#endif /* #if __MACH__ */

static const char *LLVM_USE_PCH_ARGS[] = {"-include-pch", NULL, "-Wl,-undefined", "-Wl,dynamic_lookup", NULL};
static const char *LLVM_EMIT_PCH_ARGS[] = {"-emit-pch", NULL};

/* Status of the the precompiled header creation.  The status is
   shared by the workers and the pch thread.  */
static enum {PCH_NOT_READY, PCH_FAILED, PCH_SUCCESS} pch_status;

/* The function producing the pre-compiled header. */
static void
make_pch()
{
    int exit_code;
    static const char *input[] = {NULL, NULL};
    static const char *output[] = {"-o",  NULL, NULL};
    char **args;

    verbose(2, "Creating precompiled header");
    input[0] = header_file;
    output[1] = pch_file;
    if (mjit_opts.llvm)
	args = form_args(4, (mjit_opts.debug ? LLVM_COMMON_ARGS_DEBUG : LLVM_COMMON_ARGS),
			 LLVM_EMIT_PCH_ARGS, input, output);
    else
	args = form_args(4, (mjit_opts.debug ? GCC_COMMON_ARGS_DEBUG : GCC_COMMON_ARGS),
			 GCC_EMIT_PCH_ARGS, input, output);
    if (args == NULL) {
	if (mjit_opts.warnings || mjit_opts.verbose)
	    fprintf(stderr, "MJIT warning: making precompiled header failed on forming args\n");
	CRITICAL_SECTION_START(3, "in make_pch");
	pch_status = PCH_FAILED;
	CRITICAL_SECTION_FINISH(3, "in make_pch");
	return;
    }

    exit_code = exec_process(cc_path, args);
    xfree(args);

    CRITICAL_SECTION_START(3, "in make_pch");
    if (exit_code == 0) {
	pch_status = PCH_SUCCESS;
    } else {
	if (mjit_opts.warnings || mjit_opts.verbose)
	    fprintf(stderr, "MJIT warning: making precompiled header failed on compilation\n");
	pch_status = PCH_FAILED;
    }
    /* wakeup `mjit_finish` */
    if (pthread_cond_broadcast(&mjit_pch_wakeup) != 0) {
	fprintf(stderr, "Cannot send client wakeup signal in make_pch\n");
    }
    CRITICAL_SECTION_FINISH(3, "in make_pch");
}

/* Compile C file to so. It returns 1 if it succeeds. */
static int
compile_c_to_so(const char *c_file, const char *so_file)
{
    int exit_code;
    static const char *input[] = {NULL, NULL};
    static const char *output[] = {"-o",  NULL, NULL};
    char **args;

    input[0] = c_file;
    output[1] = so_file;
    if (mjit_opts.llvm) {
	LLVM_USE_PCH_ARGS[1] = pch_file;
	args = form_args(4, (mjit_opts.debug ? LLVM_COMMON_ARGS_DEBUG : LLVM_COMMON_ARGS),
			 LLVM_USE_PCH_ARGS, input, output);
    } else {
	args = form_args(4, (mjit_opts.debug ? GCC_COMMON_ARGS_DEBUG : GCC_COMMON_ARGS),
			 GCC_USE_PCH_ARGS, input, output);
    }
    if (args == NULL)
	return FALSE;

    exit_code = exec_process(cc_path, args);
    xfree(args);

    verbose(3, "compile exit_status: %d", exit_code);
    return exit_code == 0;
}

static void *
load_func_from_so(const char *so_file, const char *funcname, struct rb_mjit_unit *unit)
{
    void *handle, *func;

    handle = dlopen(so_file, RTLD_NOW);
    if (handle == NULL) {
	if (mjit_opts.warnings || mjit_opts.verbose)
	    fprintf(stderr, "MJIT warning: failure in loading code from '%s': %s\n", so_file, dlerror());
	return (void *)NOT_ADDED_JIT_ISEQ_FUNC;
    }

    func = dlsym(handle, funcname);
    /* TODO: dlclose(handle); on unloading or GCing ISeq */
    unit->handle = handle;
    return func;
}

/* Compile ISeq in UNIT and return function pointer of JIT-ed code.
   It may return NOT_COMPILABLE_JIT_ISEQ_FUNC if something went wrong. */
static void *
convert_unit_to_func(struct rb_mjit_unit *unit)
{
    char c_file[70], so_file[70], funcname[35];
    int success;
    FILE *f;
    void *func;
    double start_time, end_time;

    sprint_uniq_filename(c_file, unit->id, "_mjit", ".c");
    sprint_uniq_filename(so_file, unit->id, "_mjit", ".so");
    sprintf(funcname, "_mjit%d", unit->id);

    f = fopen(c_file, "w");
    if (!mjit_opts.llvm) { /* -include-pch is used for LLVM */
	const char *s;
	fprintf(f, "#include \"");
	/* print pch_file except .gch */
	for (s = pch_file; strcmp(s, ".gch") != 0; s++)
	    fprintf(f, "%c", *s);
	fprintf(f, "\"\n");
    }

    /* wait until mjit_gc_finish_hook is called */
    CRITICAL_SECTION_START(3, "before mjit_compile to wait GC finish");
    while (in_gc) {
	verbose(3, "Waiting wakeup from GC");
	pthread_cond_wait(&mjit_gc_wakeup, &mjit_engine_mutex);
    }
    in_jit = TRUE;
    CRITICAL_SECTION_FINISH(3, "before mjit_compile to wait GC finish");

    verbose(2, "start compile: %s@%s:%d -> %s", RSTRING_PTR(unit->iseq->body->location.label),
	    RSTRING_PTR(rb_iseq_path(unit->iseq)), FIX2INT(unit->iseq->body->location.first_lineno), c_file);
    success = mjit_compile(f, unit->iseq->body, funcname);

    /* release blocking mjit_gc_start_hook */
    CRITICAL_SECTION_START(3, "after mjit_compile to wakeup client for GC");
    in_jit = FALSE;
    verbose(3, "Sending wakeup signal to client in a mjit-worker for GC");
    if (pthread_cond_signal(&mjit_client_wakeup) != 0) {
	fprintf(stderr, "Cannot send wakeup signal to client in mjit-worker\n");
    }
    CRITICAL_SECTION_FINISH(3, "in worker to wakeup client for GC");

    fclose(f);
    if (!success) {
	if (!mjit_opts.save_temps)
	    remove(c_file);
	return (void *)NOT_COMPILABLE_JIT_ISEQ_FUNC;
    }

    start_time = real_ms_time();
    success = compile_c_to_so(c_file, so_file);
    end_time = real_ms_time();

    if (!mjit_opts.save_temps)
	remove(c_file);
    if (!success) {
	verbose(2, "Failed to load so: %s", so_file);
	return (void *)NOT_COMPILABLE_JIT_ISEQ_FUNC;
    }

    func = load_func_from_so(so_file, funcname, unit);
    if (!mjit_opts.save_temps)
	remove(so_file);

    if ((ptrdiff_t)func > (ptrdiff_t)LAST_JIT_ISEQ_FUNC) {
	verbose(1, "JIT success (%.1fms): %s@%s:%d", end_time - start_time, RSTRING_PTR(unit->iseq->body->location.label),
		RSTRING_PTR(rb_iseq_path(unit->iseq)), FIX2INT(unit->iseq->body->location.first_lineno));
    }
    return func;
}

/* Set to TRUE to finish worker.  */
static int finish_worker_p;
/* Set to TRUE if worker is finished.  */
static int worker_finished;

/* The function implementing a worker. It is executed in a separate
   thread started by pthread_create. It compiles precompiled header
   and then compiles requested ISeqs. */
static void *
worker(void *arg)
{
    if (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) != 0) {
	fprintf(stderr, "Cannot enable cancelation in MJIT worker\n");
    }

    make_pch();
    if (pch_status == PCH_FAILED) {
	mjit_init_p = FALSE;
	CRITICAL_SECTION_START(3, "in worker to update worker_finished");
	worker_finished = TRUE;
	verbose(3, "Sending wakeup signal to client in a mjit-worker");
	if (pthread_cond_signal(&mjit_client_wakeup) != 0) {
	    fprintf(stderr, "Cannot send wakeup signal to client in mjit-worker\n");
	}
	CRITICAL_SECTION_FINISH(3, "in worker to update worker_finished");
	return NULL;
    }

    /* main worker loop */
    while (!finish_worker_p) {
	struct rb_mjit_unit *unit;

	/* wait until unit is available */
	CRITICAL_SECTION_START(3, "in worker dequeue");
	while (unit_queue == NULL && !finish_worker_p) {
	    pthread_cond_wait(&mjit_worker_wakeup, &mjit_engine_mutex);
	    verbose(3, "Getting wakeup from client");
	}
	unit = get_from_unit_queue();
	CRITICAL_SECTION_FINISH(3, "in worker dequeue");

	if (unit) {
	    void *func;
	    func = convert_unit_to_func(unit);

	    CRITICAL_SECTION_START(3, "in jit func replace");
	    if (unit->iseq) { /* Check whether GCed or not */
		/* Usage of jit_code might be not in a critical section.  */
		ATOMIC_SET(unit->iseq->body->jit_func, func);
	    }
	    CRITICAL_SECTION_FINISH(3, "in jit func replace");
	}
    }

    CRITICAL_SECTION_START(3, "in the end of worker to update worker_finished");
    worker_finished = TRUE;
    CRITICAL_SECTION_FINISH(3, "in the end of worker to update worker_finished");
    return NULL;
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
    verbose(3, "Sending wakeup signal to workers in mjit_add_iseq_to_process");
    if (pthread_cond_broadcast(&mjit_worker_wakeup) != 0) {
	fprintf(stderr, "Cannot send wakeup signal to workers in add_iseq_to_process\n");
    }
    CRITICAL_SECTION_FINISH(3, "in add_iseq_to_process");
}

/* A name of the header file included in any C file generated by MJIT for iseqs.  */
#define RUBY_MJIT_HEADER_FILE ("rb_mjit_header-" RUBY_VERSION ".h")
/* GCC and LLVM executable paths.  TODO: The paths should absolute
   ones to prevent changing C compiler for security reasons.  */
#define GCC_PATH "gcc"
#define LLVM_PATH "clang"

static void
init_header_filename()
{
    FILE *f;

    header_file = xmalloc(strlen(BUILD_DIR) + 2 + strlen(RUBY_MJIT_HEADER_FILE));
    if (header_file == NULL)
	return;
    strcpy(header_file, BUILD_DIR);
    strcat(header_file, "/");
    strcat(header_file, RUBY_MJIT_HEADER_FILE);

    if ((f = fopen(header_file, "r")) == NULL) {
	xfree(header_file);
	header_file = xmalloc(strlen(DEST_INCDIR) + 2 + strlen(RUBY_MJIT_HEADER_FILE));
	if (header_file == NULL)
	    return;
	strcpy(header_file, DEST_INCDIR);
	strcat(header_file, "/");
	strcat(header_file, RUBY_MJIT_HEADER_FILE);
	if ((f = fopen(header_file, "r")) == NULL) {
	    xfree(header_file);
	    header_file = NULL;
	    return;
	}
    }
    fclose(f);
}

/* This is called after each fork in the child in to switch off MJIT
   engine in the child as it does not inherit MJIT threads.  */
static void
child_after_fork(void)
{
    verbose(3, "Switching off MJIT in a forked child");
    mjit_init_p = FALSE;
    /* TODO: Should we initiate MJIT in the forked Ruby.  */
}

/* Initialize MJIT.  Start a thread creating the precompiled header and
   processing ISeqs.  The function should be called first for using MJIT.
   If everything is successfull, MJIT_INIT_P will be TRUE.  */
void
mjit_init(struct mjit_options *opts)
{
    pthread_attr_t attr;
    pthread_t worker_pid;

    mjit_opts = *opts;
    mjit_init_p = TRUE;

    /* Initialize variables for compilation */
    pch_status = PCH_NOT_READY;
    cc_path = mjit_opts.llvm ? LLVM_PATH : GCC_PATH;

    init_header_filename();
    pch_file = get_uniq_filename(0, "_mjit_h", ".h.gch");
    if (header_file == NULL || pch_file == NULL) {
	mjit_init_p = FALSE;
	verbose(1, "Failure in MJIT header file name initialization\n");
	return;
    }

    /* Initialize mutex */
    if (pthread_mutex_init(&mjit_engine_mutex, NULL) != 0
	|| pthread_cond_init(&mjit_pch_wakeup, NULL) != 0
	|| pthread_cond_init(&mjit_client_wakeup, NULL) != 0
	|| pthread_cond_init(&mjit_worker_wakeup, NULL) != 0
	|| pthread_cond_init(&mjit_gc_wakeup, NULL) != 0) {
	mjit_init_p = FALSE;
	verbose(1, "Failure in MJIT mutex initialization\n");
	return;
    }

    /* Initialize worker thread */
    finish_worker_p = FALSE;
    worker_finished = FALSE;
    pthread_atfork(NULL, NULL, child_after_fork);
    if (pthread_attr_init(&attr) == 0
	&& pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM) == 0
	&& pthread_create(&worker_pid, &attr, worker, NULL) == 0) {
	/* jit_worker thread is not to be joined */
	pthread_detach(worker_pid);
    } else {
	mjit_init_p = FALSE;
	pthread_mutex_destroy(&mjit_engine_mutex);
	pthread_cond_destroy(&mjit_pch_wakeup);
	pthread_cond_destroy(&mjit_client_wakeup);
	pthread_cond_destroy(&mjit_worker_wakeup);
	pthread_cond_destroy(&mjit_gc_wakeup);
	verbose(1, "Failure in MJIT thread initialization\n");
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

    /* Wait for pch finish */
    verbose(2, "Canceling pch and worker threads");
    CRITICAL_SECTION_START(3, "in mjit_finish to wakeup from pch");
    /* As our threads are detached, we could just cancel them.  But it
       is a bad idea because OS processes (C compiler) started by
       threads can produce temp files.  And even if the temp files are
       removed, the used C compiler still complaint about their
       absence.  So wait for a clean finish of the threads.  */
    while (pch_status == PCH_NOT_READY) {
	verbose(3, "Waiting wakeup from make_pch");
	pthread_cond_wait(&mjit_pch_wakeup, &mjit_engine_mutex);
    }
    CRITICAL_SECTION_FINISH(3, "in mjit_finish to wakeup from pch");

    /* Stop worker */
    finish_worker_p = TRUE;
    while (!worker_finished) {
	verbose(3, "Sending cancel signal to workers");
	CRITICAL_SECTION_START(3, "in mjit_finish");
	if (pthread_cond_broadcast(&mjit_worker_wakeup) != 0) {
	    fprintf(stderr, "Cannot send wakeup signal to workers in mjit_finish\n");
	}
	CRITICAL_SECTION_FINISH(3, "in mjit_finish");
    }

    pthread_mutex_destroy(&mjit_engine_mutex);
    pthread_cond_destroy(&mjit_pch_wakeup);
    pthread_cond_destroy(&mjit_client_wakeup);
    pthread_cond_destroy(&mjit_worker_wakeup);
    pthread_cond_destroy(&mjit_gc_wakeup);

    /* cleanup temps */
    if (!mjit_opts.save_temps)
	remove(pch_file);

    xfree(pch_file); pch_file = NULL;
    xfree(header_file); header_file = NULL;
    /* TODO: free unit_queue */

    mjit_init_p = FALSE;
    verbose(1, "Successful MJIT finish");
}
