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

/* Defined in the client thread before starting MJIT threads:  */
/* Used C compiler path.  */
static const char *cc_path;
/* Name of the header file.  */
static char *header_file;
/* Name of the precompiled header file.  */
static char *pch_file;

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

/* Return an unique file name in /tmp with PREFIX and SUFFIX and
   number ID.  Use getpid if ID == 0.  The return file name exists
   until the next function call.  */
static char *
get_uniq_filename(unsigned long id, const char *prefix, const char *suffix)
{
    char str[70];

    sprintf(str, "/tmp/%sp%luu%lu%s", prefix, (unsigned long) getpid(), id, suffix);
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
	if (mjit_opts.verbose) {
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

/* XXX_COMMONN_ARGS define the command line arguments of XXX C
   compiler used by MJIT.

   XXX_EMIT_PCH_ARGS define additional options to generate the
   precomiled header.

   XXX_USE_PCH_ARAGS define additional options to use the precomiled
   header.  */
static const char *GCC_COMMON_ARGS_DEBUG[] = {"gcc", "-O0", "-g", "-Wfatal-errors", "-fPIC", "-shared", "-w", "-pipe", "-nostartfiles", "-nodefaultlibs", "-nostdlib", NULL};
static const char *GCC_COMMON_ARGS[] = {"gcc", "-O2", "-Wfatal-errors", "-fPIC", "-shared", "-w", "-pipe", "-nostartfiles", "-nodefaultlibs", "-nostdlib", NULL};
/* static const char *GCC_USE_PCH_ARGS[] = {"-I/tmp", NULL}; */
static const char *GCC_EMIT_PCH_ARGS[] = {NULL};

#ifdef __MACH__

static const char *LLVM_COMMON_ARGS_DEBUG[] = {"clang", "-O0", "-g", "-dynamic", "-I/usr/local/include", "-L/usr/local/lib", "-w", "-bundle", NULL};
static const char *LLVM_COMMON_ARGS[] = {"clang", "-O2", "-dynamic", "-I/usr/local/include", "-L/usr/local/lib", "-w", "-bundle", NULL};

#else

static const char *LLVM_COMMON_ARGS_DEBUG[] = {"clang", "-O0", "-g", "-fPIC", "-shared", "-I/usr/local/include", "-L/usr/local/lib", "-w", "-bundle", NULL};
static const char *LLVM_COMMON_ARGS[] = {"clang", "-O2", "-fPIC", "-shared", "-I/usr/local/include", "-L/usr/local/lib", "-w", "-bundle", NULL};

#endif /* #if __MACH__ */

/* static const char *LLVM_USE_PCH_ARGS[] = {"-include-pch", NULL, "-Wl,-undefined", "-Wl,dynamic_lookup", NULL}; */
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
    return NULL;
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
	|| pthread_cond_init(&mjit_pch_wakeup, NULL) != 0) {
	mjit_init_p = FALSE;
	verbose(1, "Failure in MJIT mutex initialization\n");
	return;
    }

    /* Initialize worker thread */
    pthread_atfork(NULL, NULL, child_after_fork);
    if (pthread_attr_init(&attr) == 0
	&& pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM) == 0
	&& pthread_create(&worker_pid, &attr, worker, NULL) == 0) {
	/* jit_worker thread is not to be joined */
	pthread_detach(worker_pid);
    } else {
	mjit_init_p = FALSE;
	pthread_mutex_destroy(&mjit_engine_mutex);
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

    /* start to finish compilation */
    mjit_init_p = FALSE;

    pthread_mutex_destroy(&mjit_engine_mutex);
    pthread_cond_destroy(&mjit_pch_wakeup);

    /* cleanup temps */
    if (!mjit_opts.save_temps)
	remove(pch_file);

    /* TODO: free unit_queue */

    verbose(1, "Successful MJIT finish");
}
