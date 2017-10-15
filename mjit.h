/**********************************************************************

  mjit.h - Interface to MRI method JIT compiler

  Copyright (C) 2017 Vladimir Makarov <vmakarov@redhat.com>.

**********************************************************************/

#ifndef RUBY_MJIT_H
#define RUBY_MJIT_H 1

#include "ruby/defines.h"

/* MJIT options which can be defined on the MRI command line.  */
struct mjit_options {
    char on; /* flag of MJIT usage  */
    /* Flag to use LLVM Clang instead of default GCC for MJIT . */
    char llvm;
    /* Save temporary files after MRI finish.  The temporary files
       include the pre-compiled header, C code file generated for ISEQ,
       and the corresponding object file.  */
    char save_temps;
    /* Print MJIT warnings to stderr.  */
    char warnings;
    /* Disable compiler optimization and add debug symbols. It can be
       very slow.  */
    char debug;
    /* Force printing info about MJIT work of level VERBOSE or
       less. 0=silence, 1=medium, 2=verbose.  */
    int verbose;
    /* Maximal permitted number of iseq JIT codes in a MJIT memory
       cache.  */
    int max_cache_size;
};

RUBY_SYMBOL_EXPORT_BEGIN
extern struct mjit_options mjit_opts;
extern int mjit_init_p;

extern void mjit_init(struct mjit_options *opts);
RUBY_SYMBOL_EXPORT_END

#endif /* RUBY_MJIT_H */
