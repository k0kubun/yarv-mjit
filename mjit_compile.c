/**********************************************************************

  mjit_compile.c - MRI method JIT compiler

  Copyright (C) 2017 Takashi Kokubun <takashikkbn@gmail.com>.

**********************************************************************/

#include "internal.h"
#include "vm_core.h"
#include "vm_exec.h"
#include "mjit.h"
#include "insns.inc"
#include "insns_info.inc"

/* Storage to keep compiler's status.  This should have information
   which is global during one `mjit_compile` call.  Ones conditional
   in each branch should be stored in `compile_branch`.  */
struct compile_status {
    int success; /* has TRUE if compilation has had no issue */
    int *compiled_for_pos; /* compiled_for_pos[pos] has TRUE if the pos is compiled */
};

/* Storage to keep data which is consistent in each conditional branch.
   This is created and used for one `compile_insns` call and its values
   should be copied for extra `compile_insns` call. */
struct compile_branch {
    unsigned int stack_size; /* this simulates sp (stack pointer) of YARV */
    int finish_p; /* if TRUE, compilation in this branch should stop and let another branch to be compiled */
};

static void
fprint_getlocal(FILE *f, unsigned int push_pos, lindex_t idx, rb_num_t level)
{
    /* COLLECT_USAGE_REGISTER_HELPER is necessary? */
    fprintf(f, "  stack[%d] = *(vm_get_ep(cfp->ep, 0x%"PRIxVALUE") - 0x%"PRIxVALUE");\n", push_pos, level, idx);
    fprintf(f, "  RB_DEBUG_COUNTER_INC(lvar_get);\n");
    if (level > 0) {
	fprintf(f, "  RB_DEBUG_COUNTER_INC(lvar_get_dynamic);\n");
    }
}

/* push back stack in local variable to YARV's stack pointer */
static void
fprint_args(FILE *f, unsigned int argc, unsigned int pos)
{
    if (argc) {
	unsigned int i;
	/* TODO: use memmove or memcopy, if not optimized by compiler */
	for (i = 0; i < argc; i++) {
	    fprintf(f, "    *(cfp->sp) = stack[%d];\n", pos + i);
	    fprintf(f, "    cfp->sp++;\n");
	}
    }
}

/* Compiles CALL_METHOD macro to f. `calling` should be already defined in `f`. */
static void
fprint_call_method(FILE *f, VALUE ci, VALUE cc, unsigned int result_pos)
{
    fprintf(f, "    {\n");
    fprintf(f, "      VALUE v = (*((CALL_CACHE)0x%"PRIxVALUE")->call)(th, cfp, &calling, 0x%"PRIxVALUE", 0x%"PRIxVALUE");\n", cc, ci, cc);
    fprintf(f, "      if (v == Qundef) {\n"); /* TODO: also call jit_exec */
    fprintf(f, "        VM_ENV_FLAGS_SET(th->ec.cfp->ep, VM_FRAME_FLAG_FINISH);\n"); /* This is vm_call0_body's code after vm_call_iseq_setup */
    fprintf(f, "        stack[%d] = vm_exec(th);\n", result_pos);
    fprintf(f, "      } else {\n");
    fprintf(f, "        stack[%d] = v;\n", result_pos);
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
}

/* Compile one insn to F, may modify b->stack_size and return next position. */
static unsigned int
compile_insn(FILE *f, const struct rb_iseq_constant_body *body, const int insn, const VALUE *operands,
	     const unsigned int pos, struct compile_status *status, struct compile_branch *b)
{
    unsigned int next_pos = pos + insn_len(insn);

    /* Move program counter to meet catch table condition and for JIT execution cancellation. */
    fprintf(f, "  cfp->pc = (VALUE *)0x%"PRIxVALUE";\n", (VALUE)(body->iseq_encoded + pos));

    switch (insn) {
      case YARVINSN_putnil:
	fprintf(f, "  stack[%d] = Qnil;\n", b->stack_size++);
        break;
      case YARVINSN_putself:
	fprintf(f, "  stack[%d] = cfp->self;\n", b->stack_size++);
        break;
      case YARVINSN_putobject:
	fprintf(f, "  stack[%d] = (VALUE)0x%"PRIxVALUE";\n", b->stack_size++, operands[0]);
        break;
      case YARVINSN_trace:
	fprintf(f, "  vm_dtrace((rb_event_flag_t)0x%"PRIxVALUE", th);\n", operands[0]);
	if ((rb_event_flag_t)operands[0] & (RUBY_EVENT_RETURN | RUBY_EVENT_B_RETURN)) {
	    fprintf(f, "  EXEC_EVENT_HOOK(th, (rb_event_flag_t)0x%"PRIxVALUE", cfp->self, 0, 0, 0, stack[%d]);\n", operands[0], b->stack_size-1);
	} else {
	    fprintf(f, "  EXEC_EVENT_HOOK(th, (rb_event_flag_t)0x%"PRIxVALUE", cfp->self, 0, 0, 0, Qundef);\n", operands[0]);
	}
        break;
      case YARVINSN_opt_send_without_block:
	{
	    CALL_INFO ci = (CALL_INFO)operands[0];
	    fprintf(f, "  {\n");
	    fprintf(f, "    struct rb_calling_info calling;\n");
	    fprintf(f, "    calling.block_handler = VM_BLOCK_HANDLER_NONE;\n");
	    fprintf(f, "    calling.argc = %d;\n", ci->orig_argc);
	    fprintf(f, "    vm_search_method(0x%"PRIxVALUE", 0x%"PRIxVALUE", calling.recv = stack[%d]);\n",
		    operands[0], operands[1], b->stack_size - 1 - ci->orig_argc);
	    fprint_args(f, ci->orig_argc + 1, b->stack_size - ci->orig_argc - 1);
	    fprint_call_method(f, operands[0], operands[1], b->stack_size - ci->orig_argc - 1);
	    fprintf(f, "  }\n");
	    b->stack_size -= ci->orig_argc;
	}
        break;
      case YARVINSN_leave:
	/* NOTE: We don't use YARV's stack on JIT. So vm_stack_consistency_error isn't run
	   during execution and we check stack_size here instead. */
	if (b->stack_size != 1) {
	    if (mjit_opts.warnings || mjit_opts.verbose)
		fprintf(stderr, "MJIT warning: Unexpected JIT stack_size on leave: %d\n", b->stack_size);
	    status->success = FALSE;
	}

	fprintf(f, "  RUBY_VM_CHECK_INTS(th);\n");
	/* TODO: is there a case that vm_pop_frame returns 0? */
	fprintf(f, "  vm_pop_frame(th, cfp, cfp->ep);\n");
#if OPT_CALL_THREADED_CODE
	fprintf(f, "  th->retval = stack[%d];\n", b->stack_size-1);
	fprintf(f, "  return 0;\n");
#else
	fprintf(f, "  return stack[%d];\n", b->stack_size-1);
#endif
	/* stop compilation in this branch. to simulate stack properly,
	   remaining insns should be compiled from another branch */
	b->finish_p = TRUE;
	break;
      case YARVINSN_bitblt:
	fprintf(f, "  stack[%d] = rb_str_new2(\"a bit of bacon, lettuce and tomato\");\n", b->stack_size++);
        break;
      case YARVINSN_answer:
	fprintf(f, "  stack[%d] = INT2FIX(42);\n", b->stack_size++);
        break;
      case YARVINSN_getlocal_OP__WC__0:
	fprint_getlocal(f, b->stack_size++, operands[0], 0);
        break;
      case YARVINSN_getlocal_OP__WC__1:
	fprint_getlocal(f, b->stack_size++, operands[0], 1);
        break;
      default:
	if (mjit_opts.warnings || mjit_opts.verbose)
	    /* passing excessive arguments to suppress warning in insns_info.inc as workaround... */
	    fprintf(stderr, "MJIT warning: Failed to compile instruction: %s (%s: %d...)\n",
		    insn_name(insn), insn_op_types(insn), insn_len(insn) > 0 ? insn_op_type(insn, 0) : 0);
	status->success = FALSE;
	break;
    }

    /* if next_pos is already compiled, next instruction won't be compiled in C code and needs `goto`. */
    if ((next_pos < body->iseq_size && status->compiled_for_pos[next_pos]) || insn == YARVINSN_jump)
	fprintf(f, "  goto label_%d;\n", next_pos);

    return next_pos;
}

/* Compile one conditional branch.  If it has branchXXX insn, this should be
   called multiple times for each branch.  */
static void
compile_insns(FILE *f, const struct rb_iseq_constant_body *body, unsigned int stack_size,
	      unsigned int pos, struct compile_status *status)
{
    int insn;
    struct compile_branch branch;

    branch.stack_size = stack_size;
    branch.finish_p = FALSE;

    while (pos < body->iseq_size && !status->compiled_for_pos[pos] && !branch.finish_p) {
#if OPT_DIRECT_THREADED_CODE || OPT_CALL_THREADED_CODE
	insn = rb_vm_insn_addr2insn((void *)body->iseq_encoded[pos]);
#else
	insn = (int)body->iseq_encoded[pos];
#endif
	status->compiled_for_pos[pos] = TRUE;

	fprintf(f, "\nl%04d: /* %s */\n", pos, insn_name(insn));
	pos = compile_insn(f, body, insn, body->iseq_encoded + (pos+1), pos, status, &branch);
	if (status->success && branch.stack_size > body->stack_max) {
	    if (mjit_opts.warnings || mjit_opts.verbose)
		fprintf(stderr, "MJIT warning: JIT stack exceeded its max\n");
	    status->success = FALSE;
	}
	if (!status->success)
	    break;
    }
}

/* Compile ISeq to C code in F.  It returns 1 if it succeeds to compile. */
int
mjit_compile(FILE *f, const struct rb_iseq_constant_body *body, const char *funcname)
{
    struct compile_status status;
    status.success = TRUE;
    status.compiled_for_pos = ZALLOC_N(int, body->iseq_size);

    fprintf(f, "VALUE %s(rb_thread_t *th, rb_control_frame_t *cfp) {\n", funcname);
    if (body->stack_max > 0) {
	fprintf(f, "  VALUE stack[%d];\n", body->stack_max);
    }
    compile_insns(f, body, 0, 0, &status);
    fprintf(f, "}\n");

    xfree(status.compiled_for_pos);
    return status.success;
}
