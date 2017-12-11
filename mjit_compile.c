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
#include "vm_insnhelper.h"

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

static void
fprint_setlocal(FILE *f, unsigned int pop_pos, lindex_t idx, rb_num_t level)
{
    /* COLLECT_USAGE_REGISTER_HELPER is necessary? */
    fprintf(f, "  vm_env_write(vm_get_ep(cfp->ep, 0x%"PRIxVALUE"), -(int)0x%"PRIxVALUE", stack[%d]);\n", level, idx, pop_pos);
    fprintf(f, "  RB_DEBUG_COUNTER_INC(lvar_set);\n");
    if (level > 0) {
	fprintf(f, "  RB_DEBUG_COUNTER_INC(lvar_set_dynamic);\n");
    }
}

/* push back stack in local variable to YARV's stack pointer */
static void
fprint_args(FILE *f, unsigned int argc, unsigned int base_pos)
{
    if (argc) {
	unsigned int i;
	/* TODO: use memmove or memcopy, if not optimized by compiler */
	for (i = 0; i < argc; i++) {
	    fprintf(f, "    *(cfp->sp) = stack[%d];\n", base_pos + i);
	    fprintf(f, "    cfp->sp++;\n");
	}
    }
}

extern int simple_iseq_p(const rb_iseq_t *iseq);

static int
inlinable_cfunc_p(CALL_CACHE cc)
{
    return GET_GLOBAL_METHOD_STATE() == cc->method_state && cc->me && cc->me->def->type == VM_METHOD_TYPE_CFUNC;
}

/* Returns iseq from cc if it's available and still not obsoleted. */
static const rb_iseq_t *
get_iseq_if_available(CALL_CACHE cc)
{
    if (GET_GLOBAL_METHOD_STATE() == cc->method_state
	&& cc->me && cc->me->def->type == VM_METHOD_TYPE_ISEQ) {
	return rb_iseq_check(cc->me->def->body.iseq.iseqptr);
    }
    return NULL;
}

/* TODO: move to somewhere shared with vm_args.c */
#define IS_ARGS_SPLAT(ci)   ((ci)->flag & VM_CALL_ARGS_SPLAT)
#define IS_ARGS_KEYWORD(ci) ((ci)->flag & VM_CALL_KWARG)

/* Returns TRUE if iseq is inlinable, otherwise NULL. This becomes TRUE in the same condition
   as CI_SET_FASTPATH (in vm_callee_setup_arg) is called from vm_call_iseq_setup. */
static int
inlinable_iseq_p(CALL_INFO ci, CALL_CACHE cc, const rb_iseq_t *iseq)
{
    return iseq != NULL
	&& simple_iseq_p(iseq) && !(ci->flag & VM_CALL_KW_SPLAT) /* top of vm_callee_setup_arg */
	&& (!IS_ARGS_SPLAT(ci) && !IS_ARGS_KEYWORD(ci) && !(METHOD_ENTRY_VISI(cc->me) == METHOD_VISI_PROTECTED)); /* CI_SET_FASTPATH */
}

/* Compiles CALL_METHOD macro to f. `calling` should be already defined in `f`.
   This method inlines fast path of vm_call_method_each_type for some types assuming
   that cc passes mjit_check_invalid_cc. */
static void
fprint_call_method(FILE *f, VALUE ci_v, VALUE cc_v, unsigned int result_pos)
{
    const rb_iseq_t *iseq;
    CALL_CACHE cc = (CALL_CACHE)cc_v;

    if (inlinable_cfunc_p(cc)) {
	fprintf(f, "    stack[%d] = mjit_call_cfunc(ec, cfp, &calling, 0x%"PRIxVALUE", 0x%"PRIxVALUE");\n", result_pos, ci_v, (VALUE)cc->me);
	return;
    }

    fprintf(f, "    {\n");
    fprintf(f, "      VALUE v;\n");

    iseq = get_iseq_if_available(cc);
    /* In the condition that CI_SET_FASTPATH (in vm_callee_setup_arg) is called from vm_call_iseq_setup, this inlines vm_call_iseq_setup_normal */
    if (inlinable_iseq_p((CALL_INFO)ci_v, cc, iseq)) {
	/* TODO: check calling->argc for argument_arity_error */
	int param_size = iseq->body->param.size;
	fprintf(f, "      VALUE *argv = cfp->sp - calling.argc;\n");
	fprintf(f, "      cfp->sp = argv - 1;\n"); /* recv */
	fprintf(f, "      vm_push_frame(ec, 0x%"PRIxVALUE", VM_FRAME_MAGIC_METHOD | VM_ENV_FLAG_LOCAL, calling.recv, "
		"calling.block_handler, 0x%"PRIxVALUE", 0x%"PRIxVALUE", argv + %d, %d, %d);\n",
		(VALUE)iseq, (VALUE)cc->me, (VALUE)iseq->body->iseq_encoded, param_size, iseq->body->local_table_size - param_size, iseq->body->stack_max);
	fprintf(f, "      v = Qundef;\n");
    } else {
	fprintf(f, "      v = (*((CALL_CACHE)0x%"PRIxVALUE")->call)(ec, cfp, &calling, 0x%"PRIxVALUE", 0x%"PRIxVALUE");\n", cc_v, ci_v, cc_v);
    }

    if (iseq && iseq->body->catch_table == NULL) {
	/* TODO: Probably it's better to discard this code if method is changed */
	fprintf(f, "      if (v == Qundef && (v = mjit_exec(ec)) == Qundef) {\n");
    } else {
	/* When catch table exists, we want to call setjmp and use vm_exec's fallback handler. */
	fprintf(f, "      if (v == Qundef) {\n");
    }
    fprintf(f, "        VM_ENV_FLAGS_SET(ec->cfp->ep, VM_FRAME_FLAG_FINISH);\n"); /* This is vm_call0_body's code after vm_call_iseq_setup */
    fprintf(f, "        stack[%d] = vm_exec(ec);\n", result_pos);
    fprintf(f, "      } else {\n");
    fprintf(f, "        stack[%d] = v;\n", result_pos);
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
}

/* Compile send and opt_send_without_block instructions to `f`, and return stack size change */
static int
compile_send(FILE *f, const VALUE *operands, unsigned int stack_size, int with_block)
{
    CALL_INFO ci = (CALL_INFO)operands[0];
    CALL_CACHE cc = (CALL_CACHE)operands[1];
    unsigned int argc = ci->orig_argc; /* unlike `ci->orig_argc`, `argc` may include blockarg */
    if (with_block) {
	argc += ((ci->flag & VM_CALL_ARGS_BLOCKARG) ? 1 : 0);
    }

    if (inlinable_cfunc_p(cc) || inlinable_iseq_p(ci, cc, get_iseq_if_available(cc))) {
	fprintf(f, "  if (UNLIKELY(mjit_check_invalid_cc(stack[%d], %llu, %llu))) {\n", stack_size - 1 - argc, cc->method_state, cc->class_serial);
    } else {
	fprintf(f, "  if (UNLIKELY(GET_GLOBAL_METHOD_STATE() != ((CALL_CACHE)0x%"PRIxVALUE")->method_state)) {\n", (VALUE)cc);
    }
    fprintf(f, "    cfp->sp = cfp->bp + %d;\n", stack_size + 1);
    fprintf(f, "    goto cancel;\n");
    fprintf(f, "  }\n");

    fprintf(f, "  {\n");
    fprintf(f, "    struct rb_calling_info calling;\n");
    fprint_args(f, argc + 1, stack_size - argc - 1); /* +1 is for recv */
    if (with_block) {
	fprintf(f, "    vm_caller_setup_arg_block(ec, cfp, &calling, 0x%"PRIxVALUE", 0x%"PRIxVALUE", FALSE);\n", operands[0], operands[2]);
    } else {
	fprintf(f, "    calling.block_handler = VM_BLOCK_HANDLER_NONE;\n");
    }
    fprintf(f, "    calling.argc = %d;\n", ci->orig_argc);
    fprintf(f, "    calling.recv = stack[%d];\n", stack_size - 1 - argc);
    fprint_call_method(f, operands[0], operands[1], stack_size - argc - 1);
    fprintf(f, "  }\n");
    return -argc;
}

static void
fprint_opt_call_variables(FILE *f, unsigned int stack_size, unsigned int argc)
{
    fprintf(f, "    VALUE recv = stack[%d];\n", stack_size - argc);
    if (argc >= 2) {
	fprintf(f, "    VALUE obj = stack[%d];\n", stack_size - (argc - 1));
    }
    if (argc >= 3) {
	fprintf(f, "    VALUE obj2 = stack[%d];\n", stack_size - (argc - 2));
    }
}

static void
fprint_opt_call_fallback(FILE *f, VALUE ci, VALUE cc, unsigned int stack_size, unsigned int argc, VALUE key)
{
    fprintf(f, "    if (result == Qundef) {\n");
    fprintf(f, "      cfp->sp = cfp->bp + %d;\n", stack_size + 1);
    fprintf(f, "      goto cancel;\n");
    fprintf(f, "    }\n");
    fprintf(f, "    stack[%d] = result;\n", stack_size - argc);
}

/* Print optimized call with redefinition fallback and return stack size change.
   `format` should call function with `recv`, `obj` and `obj2` depending on `argc`. */
PRINTF_ARGS(static int, 6, 7)
fprint_opt_call(FILE *f, VALUE ci, VALUE cc, unsigned int stack_size, unsigned int argc, const char *format, ...)
{
    va_list va;

    fprintf(f, "  {\n");
    fprint_opt_call_variables(f, stack_size, argc);

    fprintf(f, "    VALUE result = ");
    va_start(va, format);
    vfprintf(f, format, va);
    va_end(va);
    fprintf(f, ";\n");

    fprint_opt_call_fallback(f, ci, cc, stack_size, argc, (VALUE)0);
    fprintf(f, "  }\n");

    return 1 - argc;
}

/* Same as `fprint_opt_call`, but `key` will be `rb_str_resurrect`ed and pushed. */
PRINTF_ARGS(static int, 7, 8)
fprint_opt_call_with_key(FILE *f, VALUE ci, VALUE cc, VALUE key, unsigned int stack_size, unsigned int argc, const char *format, ...)
{
    va_list va;

    fprintf(f, "  {\n");
    fprint_opt_call_variables(f, stack_size, argc);

    fprintf(f, "    VALUE result = ");
    va_start(va, format);
    vfprintf(f, format, va);
    va_end(va);
    fprintf(f, ";\n");

    fprint_opt_call_fallback(f, ci, cc, stack_size, argc, key);
    fprintf(f, "  }\n");

    return 1 - argc;
}

struct case_dispatch_var {
    FILE *f;
    unsigned int base_pos;
    VALUE last_value;
};

static int
compile_case_dispatch_each(VALUE key, VALUE value, VALUE arg)
{
    struct case_dispatch_var *var = (struct case_dispatch_var *)arg;
    unsigned int offset;

    if (var->last_value != value) {
	offset = FIX2INT(value);
	var->last_value = value;
	fprintf(var->f, "    case %d:\n", offset);
	fprintf(var->f, "      goto label_%d;\n", var->base_pos + offset);
	fprintf(var->f, "      break;\n");
    }
    return ST_CONTINUE;
}

static void compile_insns(FILE *f, const struct rb_iseq_constant_body *body, unsigned int stack_size,
	                  unsigned int pos, struct compile_status *status);

/* Compile one insn to F, may modify b->stack_size and return next position. */
static unsigned int
compile_insn(FILE *f, const struct rb_iseq_constant_body *body, const int insn, const VALUE *operands,
	     const unsigned int pos, struct compile_status *status, struct compile_branch *b)
{
    unsigned int next_pos = pos + insn_len(insn);

    /* Move program counter to meet catch table condition and for JIT execution cancellation. */
    fprintf(f, "  cfp->pc = (VALUE *)0x%"PRIxVALUE";\n", (VALUE)(body->iseq_encoded + pos));

    switch (insn) {
      case YARVINSN_nop:
	/* nop */
        break;
      case YARVINSN_getlocal:
	fprint_getlocal(f, b->stack_size++, operands[0], operands[1]);
        break;
      case YARVINSN_setlocal:
	fprint_setlocal(f, --b->stack_size, operands[0], operands[1]);
        break;
      /* case YARVINSN_getblockparam:
	break;
      case YARVINSN_setblockparam:
	break; */
      case YARVINSN_getspecial:
	fprintf(f, "  stack[%d] = vm_getspecial(ec, VM_EP_LEP(cfp->ep), 0x%"PRIxVALUE", 0x%"PRIxVALUE");\n", b->stack_size++, operands[0], operands[1]);
        break;
      case YARVINSN_setspecial:
        fprintf(f, "  lep_svar_set(ec, VM_EP_LEP(cfp->ep), 0x%"PRIxVALUE", stack[%d]);\n", operands[0], --b->stack_size);
        break;
      case YARVINSN_getinstancevariable:
	fprintf(f, "  stack[%d] = vm_getinstancevariable(cfp->self, 0x%"PRIxVALUE", 0x%"PRIxVALUE");\n", b->stack_size++, operands[0], operands[1]);
        break;
      case YARVINSN_setinstancevariable:
	fprintf(f, "  vm_setinstancevariable(cfp->self, 0x%"PRIxVALUE", stack[%d], 0x%"PRIxVALUE");\n", operands[0], --b->stack_size, operands[1]);
        break;
      case YARVINSN_getclassvariable:
	fprintf(f, "  stack[%d] = rb_cvar_get(vm_get_cvar_base(rb_vm_get_cref(cfp->ep), cfp), 0x%"PRIxVALUE");\n", b->stack_size++, operands[0]);
        break;
      case YARVINSN_setclassvariable:
	fprintf(f, "  vm_ensure_not_refinement_module(cfp->self);\n");
	fprintf(f, "  rb_cvar_set(vm_get_cvar_base(rb_vm_get_cref(cfp->ep), cfp), 0x%"PRIxVALUE", stack[%d]);\n", operands[0], --b->stack_size);
        break;
      case YARVINSN_getconstant:
	fprintf(f, "  stack[%d] = vm_get_ev_const(ec, stack[%d], 0x%"PRIxVALUE", 0);\n", b->stack_size-1, b->stack_size-1, operands[0]);
        break;
      case YARVINSN_setconstant:
	fprintf(f, "  vm_check_if_namespace(stack[%d]);\n", b->stack_size-2);
	fprintf(f, "  vm_ensure_not_refinement_module(cfp->self);\n");
	fprintf(f, "  rb_const_set(stack[%d], 0x%"PRIxVALUE", stack[%d]);\n", b->stack_size-2, operands[0], b->stack_size-1);
        break;
      case YARVINSN_getglobal:
	fprintf(f, "  stack[%d] = GET_GLOBAL((VALUE)0x%"PRIxVALUE");\n", b->stack_size++, operands[0]);
        break;
      case YARVINSN_setglobal:
	fprintf(f, "  SET_GLOBAL((VALUE)0x%"PRIxVALUE", stack[%d]);\n", operands[0], --b->stack_size);
        break;
      case YARVINSN_putnil:
	fprintf(f, "  stack[%d] = Qnil;\n", b->stack_size++);
        break;
      case YARVINSN_putself:
	fprintf(f, "  stack[%d] = cfp->self;\n", b->stack_size++);
        break;
      case YARVINSN_putobject:
	fprintf(f, "  stack[%d] = (VALUE)0x%"PRIxVALUE";\n", b->stack_size++, operands[0]);
        break;
      case YARVINSN_putspecialobject:
	fprintf(f, "  stack[%d] = vm_get_special_object(cfp->ep, (enum vm_special_object_type)0x%"PRIxVALUE");\n", b->stack_size++, operands[0]);
        break;
      case YARVINSN_putiseq:
	fprintf(f, "  stack[%d] = (VALUE)0x%"PRIxVALUE";\n", b->stack_size++, operands[0]);
        break;
      case YARVINSN_putstring:
	fprintf(f, "  stack[%d] = rb_str_resurrect(0x%"PRIxVALUE");\n", b->stack_size++, operands[0]);
        break;
      case YARVINSN_concatstrings:
	fprintf(f, "  stack[%d] = rb_str_concat_literals(0x%"PRIxVALUE", stack + %d);\n",
		b->stack_size - (unsigned int)operands[0], operands[0], b->stack_size - (unsigned int)operands[0]);
	b->stack_size += 1 - (unsigned int)operands[0];
        break;
      case YARVINSN_tostring:
	fprintf(f, "  stack[%d] = rb_obj_as_string_result(stack[%d], stack[%d]);\n", b->stack_size-2, b->stack_size-1, b->stack_size-2);
	b->stack_size--;
        break;
      case YARVINSN_freezestring:
	fprintf(f, "  vm_freezestring(stack[%d], 0x%"PRIxVALUE");\n", b->stack_size-1, operands[0]);
        break;
      case YARVINSN_toregexp:
	fprintf(f, "  {\n");
	fprintf(f, "    VALUE rb_reg_new_ary(VALUE ary, int options);\n");
        fprintf(f, "    VALUE rb_ary_tmp_new_from_values(VALUE, long, const VALUE *);\n");
	fprintf(f, "    const VALUE ary = rb_ary_tmp_new_from_values(0, 0x%"PRIxVALUE", stack + %d);\n", operands[1], b->stack_size - (unsigned int)operands[1]);
	fprintf(f, "    stack[%d] = rb_reg_new_ary(ary, (int)0x%"PRIxVALUE");\n", b->stack_size - (unsigned int)operands[1], operands[0]);
	fprintf(f, "    rb_ary_clear(ary);\n");
	fprintf(f, "  }\n");
	b->stack_size += 1 - (unsigned int)operands[1];
        break;
      case YARVINSN_intern:
	fprintf(f, "  stack[%d] = rb_str_intern(stack[%d]);\n", b->stack_size-1, b->stack_size-1);
        break;
      case YARVINSN_newarray:
	fprintf(f, "  stack[%d] = rb_ary_new4(0x%"PRIxVALUE", stack + %d);\n",
		b->stack_size - (unsigned int)operands[0], operands[0], b->stack_size - (unsigned int)operands[0]);
	b->stack_size += 1 - (unsigned int)operands[0];
        break;
      case YARVINSN_duparray:
	fprintf(f, "  stack[%d] = rb_ary_resurrect(0x%"PRIxVALUE");\n", b->stack_size++, operands[0]);
        break;
      case YARVINSN_expandarray:
	{
	    unsigned int i, space_size;
	    space_size = (unsigned int)operands[0] + (unsigned int)((int)operands[1] & 0x01);

	    /* probably vm_expandarray should be optimized for JIT */
	    fprintf(f, "  vm_expandarray(cfp, stack[%d], 0x%"PRIxVALUE", (int)0x%"PRIxVALUE");\n", --b->stack_size, operands[0], operands[1]);
	    for (i = 0; i < space_size; i++) {
		fprintf(f, "  cfp->sp--;\n");
		fprintf(f, "  stack[%d] = *(cfp->sp);\n", b->stack_size + space_size - 1 - i);
	    }
	    b->stack_size += space_size;
	}
        break;
      case YARVINSN_concatarray:
	fprintf(f, "  stack[%d] = vm_concat_array(stack[%d], stack[%d]);\n", b->stack_size-2, b->stack_size-2, b->stack_size-1);
	b->stack_size--;
        break;
      case YARVINSN_splatarray:
	fprintf(f, "  stack[%d] = vm_splat_array(0x%"PRIxVALUE", stack[%d]);\n", b->stack_size-1, operands[0], b->stack_size-1);
        break;
      case YARVINSN_newhash:
	fprintf(f, "  {\n");
	fprintf(f, "    VALUE val;\n");
	fprintf(f, "    RUBY_DTRACE_CREATE_HOOK(HASH, 0x%"PRIxVALUE");\n", operands[0]);
	fprintf(f, "    val = rb_hash_new_with_size(0x%"PRIxVALUE" / 2);\n", operands[0]);
	if (operands[0]) {
	    fprintf(f, "    rb_hash_bulk_insert(0x%"PRIxVALUE", stack + %d, val);\n", operands[0], b->stack_size - (unsigned int)operands[0]);
	}
	fprintf(f, "    stack[%d] = val;\n", b->stack_size - (unsigned int)operands[0]);
	fprintf(f, "  }\n");
	b->stack_size += 1 - (unsigned int)operands[0];
        break;
      case YARVINSN_newrange:
	fprintf(f, "  stack[%d] = rb_range_new(stack[%d], stack[%d], (int)0x%"PRIxVALUE");\n", b->stack_size-2, b->stack_size-2, b->stack_size-1, operands[0]);
	b->stack_size--;
        break;
      case YARVINSN_pop:
	b->stack_size--;
        break;
      case YARVINSN_dup:
	fprintf(f, "  stack[%d] = stack[%d];\n", b->stack_size, b->stack_size-1);
	b->stack_size++;
        break;
      case YARVINSN_dupn:
	fprintf(f, "  MEMCPY(stack + %d, stack + %d, VALUE, 0x%"PRIxVALUE");\n",
		b->stack_size, b->stack_size - (unsigned int)operands[0], operands[0]);
	b->stack_size += (unsigned int)operands[0];
        break;
      case YARVINSN_swap:
	fprintf(f, "  {\n");
	fprintf(f, "    VALUE tmp = stack[%d];\n", b->stack_size-1);
	fprintf(f, "    stack[%d] = stack[%d];\n", b->stack_size-1, b->stack_size-2);
	fprintf(f, "    stack[%d] = tmp;\n", b->stack_size-2);
	fprintf(f, "  }\n");
        break;
      case YARVINSN_reverse:
	{
	    unsigned int n, i, base;
	    n = (unsigned int)operands[0];
	    base = b->stack_size - n;

	    fprintf(f, "  {\n");
	    fprintf(f, "    VALUE v0;\n");
	    fprintf(f, "    VALUE v1;\n");
	    for (i = 0; i < n/2; i++) {
		fprintf(f, "    v0 = stack[%d];\n", base + i);
		fprintf(f, "    v1 = stack[%d];\n", base + n - i - 1);
		fprintf(f, "    stack[%d] = v1;\n", base + i);
		fprintf(f, "    stack[%d] = v0;\n", base + n - i - 1);
	    }
	    fprintf(f, "  }\n");
	}
        break;
      case YARVINSN_reput:
	fprintf(f, "  stack[%d] = stack[%d];\n", b->stack_size-1, b->stack_size-1);
        break;
      case YARVINSN_topn:
	fprintf(f, "  stack[%d] = stack[%d];\n", b->stack_size, b->stack_size - (unsigned int)operands[0]);
	b->stack_size++;
        break;
      case YARVINSN_setn:
	fprintf(f, "  stack[%d] = stack[%d];\n", b->stack_size - 1 - (unsigned int)operands[0], b->stack_size-1);
        break;
      case YARVINSN_adjuststack:
	b->stack_size -= (unsigned int)operands[0];
        break;
      case YARVINSN_defined:
	fprintf(f, "  stack[%d] = vm_defined(ec, cfp, 0x%"PRIxVALUE", 0x%"PRIxVALUE", 0x%"PRIxVALUE", stack[%d]);\n",
		b->stack_size-1, operands[0], operands[1], operands[2], b->stack_size-1);
        break;
      case YARVINSN_checkmatch:
	fprintf(f, "  stack[%d] = vm_check_match(ec, stack[%d], stack[%d], 0x%"PRIxVALUE");\n", b->stack_size-2, b->stack_size-2, b->stack_size-1, operands[0]);
	b->stack_size--;
        break;
      case YARVINSN_checkkeyword:
	fprintf(f, "  stack[%d] = vm_check_keyword(0x%"PRIxVALUE", 0x%"PRIxVALUE", cfp->ep);\n",
		b->stack_size++, operands[0], operands[1]);
        break;
      case YARVINSN_trace2:
	fprintf(f, "  vm_dtrace((rb_event_flag_t)0x%"PRIxVALUE", ec);\n", operands[0]);
	fprintf(f, "  EXEC_EVENT_HOOK(ec, (rb_event_flag_t)0x%"PRIxVALUE", cfp->self, 0, 0, 0, 0x%"PRIxVALUE");\n", operands[0], operands[1]);
        break;
      /* case YARVINSN_defineclass:
        break; */
      case YARVINSN_send:
	b->stack_size += compile_send(f, operands, b->stack_size, TRUE);
        break;
      case YARVINSN_opt_str_freeze:
	fprintf(f, "  if (BASIC_OP_UNREDEFINED_P(BOP_FREEZE, STRING_REDEFINED_OP_FLAG)) {\n");
	fprintf(f, "    stack[%d] = 0x%"PRIxVALUE";\n", b->stack_size, operands[0]);
	fprintf(f, "  } else {\n");
	fprintf(f, "    stack[%d] = rb_funcall(rb_str_resurrect(0x%"PRIxVALUE"), idFreeze, 0);\n", b->stack_size, operands[0]);
	fprintf(f, "  }\n");
	b->stack_size++;
        break;
      case YARVINSN_opt_str_uminus:
	fprintf(f, "  if (BASIC_OP_UNREDEFINED_P(BOP_UMINUS, STRING_REDEFINED_OP_FLAG)) {\n");
	fprintf(f, "    stack[%d] = 0x%"PRIxVALUE";\n", b->stack_size, operands[0]);
	fprintf(f, "  } else {\n");
	fprintf(f, "    stack[%d] = rb_funcall(rb_str_resurrect(0x%"PRIxVALUE"), idUMinus, 0);\n", b->stack_size, operands[0]);
	fprintf(f, "  }\n");
	b->stack_size++;
        break;
      case YARVINSN_opt_newarray_max:
	fprintf(f, "  stack[%d] = vm_opt_newarray_max(0x%"PRIxVALUE", stack + %d);\n",
		b->stack_size - (unsigned int)operands[0], operands[0], b->stack_size - (unsigned int)operands[0]);
	b->stack_size += 1 - (unsigned int)operands[0];
        break;
      case YARVINSN_opt_newarray_min:
	fprintf(f, "  stack[%d] = vm_opt_newarray_min(0x%"PRIxVALUE", stack + %d);\n",
		b->stack_size - (unsigned int)operands[0], operands[0], b->stack_size - (unsigned int)operands[0]);
	b->stack_size += 1 - (unsigned int)operands[0];
        break;
      case YARVINSN_opt_send_without_block:
	b->stack_size += compile_send(f, operands, b->stack_size, FALSE);
        break;
      case YARVINSN_invokesuper:
	{
	    CALL_INFO ci = (CALL_INFO)operands[0];
	    unsigned int push_count = ci->orig_argc + ((ci->flag & VM_CALL_ARGS_BLOCKARG) ? 1 : 0);

	    fprintf(f, "  {\n");
	    fprintf(f, "    struct rb_calling_info calling;\n");
	    fprintf(f, "    calling.argc = %d;\n", ci->orig_argc);
	    fprint_args(f, push_count + 1, b->stack_size - push_count - 1);
	    fprintf(f, "    vm_caller_setup_arg_block(ec, cfp, &calling, 0x%"PRIxVALUE", 0x%"PRIxVALUE", TRUE);\n", operands[0], operands[2]);
	    fprintf(f, "    calling.recv = cfp->self;\n");
	    fprintf(f, "    vm_search_super_method(ec, cfp, &calling, 0x%"PRIxVALUE", 0x%"PRIxVALUE");\n", operands[0], operands[1]);
	    fprintf(f, "    {\n");
	    fprintf(f, "      VALUE v = (*((CALL_CACHE)0x%"PRIxVALUE")->call)(ec, cfp, &calling, 0x%"PRIxVALUE", 0x%"PRIxVALUE");\n", operands[1], operands[0], operands[1]);
	    fprintf(f, "      if (v == Qundef && (v = mjit_exec(ec)) == Qundef) {\n"); /* TODO: we need some check to call `mjit_exec` directly (skipping setjmp), but not done yet */
	    fprintf(f, "        VM_ENV_FLAGS_SET(ec->cfp->ep, VM_FRAME_FLAG_FINISH);\n"); /* This is vm_call0_body's code after vm_call_iseq_setup */
	    fprintf(f, "        stack[%d] = vm_exec(ec);\n", b->stack_size - push_count - 1);
	    fprintf(f, "      } else {\n");
	    fprintf(f, "        stack[%d] = v;\n", b->stack_size - push_count - 1);
	    fprintf(f, "      }\n");
	    fprintf(f, "    }\n");
	    fprintf(f, "  }\n");
	    b->stack_size -= push_count;
	}
        break;
      case YARVINSN_invokeblock:
	{
	    CALL_INFO ci = (CALL_INFO)operands[0];
	    fprintf(f, "  {\n");
	    fprintf(f, "    struct rb_calling_info calling;\n");
	    fprintf(f, "    calling.argc = %d;\n", ci->orig_argc);
	    fprintf(f, "    calling.block_handler = VM_BLOCK_HANDLER_NONE;\n");
	    fprintf(f, "    calling.recv = cfp->self;\n");

	    fprint_args(f, ci->orig_argc, b->stack_size - ci->orig_argc);
	    fprintf(f, "    stack[%d] = vm_invoke_block(ec, cfp, &calling, 0x%"PRIxVALUE");\n", b->stack_size - ci->orig_argc, operands[0]);
	    fprintf(f, "    if (stack[%d] == Qundef) {\n", b->stack_size - ci->orig_argc);
	    fprintf(f, "      VM_ENV_FLAGS_SET(ec->cfp->ep, VM_FRAME_FLAG_FINISH);\n");
	    fprintf(f, "      stack[%d] = vm_exec(ec);\n", b->stack_size - ci->orig_argc);
	    fprintf(f, "    }\n");
	    fprintf(f, "  }\n");
	    b->stack_size += 1 - ci->orig_argc;
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

	fprintf(f, "  RUBY_VM_CHECK_INTS(ec);\n");
	/* TODO: is there a case that vm_pop_frame returns 0? */
	fprintf(f, "  vm_pop_frame(ec, cfp, cfp->ep);\n");
#if OPT_CALL_THREADED_CODE
	fprintf(f, "  ec->retval = stack[%d];\n", b->stack_size-1);
	fprintf(f, "  return 0;\n");
#else
	fprintf(f, "  return stack[%d];\n", b->stack_size-1);
#endif
	/* stop compilation in this branch. to simulate stack properly,
	   remaining insns should be compiled from another branch */
	b->finish_p = TRUE;
	break;
      case YARVINSN_throw:
	fprintf(f, "  RUBY_VM_CHECK_INTS(ec);\n");
	fprintf(f, "  THROW_EXCEPTION(vm_throw(ec, cfp, 0x%"PRIxVALUE", stack[%d]));\n", operands[0], --b->stack_size);
	b->finish_p = TRUE;
	break;
      case YARVINSN_jump:
	next_pos = pos + insn_len(insn) + (unsigned int)operands[0];
	fprintf(f, "  RUBY_VM_CHECK_INTS(ec);\n");
	fprintf(f, "  goto label_%d;\n", next_pos);
        break;
      case YARVINSN_branchif:
	fprintf(f, "  if (RTEST(stack[%d])) {\n", --b->stack_size);
	fprintf(f, "    RUBY_VM_CHECK_INTS(ec);\n");
	fprintf(f, "    goto label_%d;\n", pos + insn_len(insn) + (unsigned int)operands[0]);
	fprintf(f, "  }\n");
	compile_insns(f, body, b->stack_size, pos + insn_len(insn), status);
	next_pos = pos + insn_len(insn) + (unsigned int)operands[0];
        break;
      case YARVINSN_branchunless:
	fprintf(f, "  if (!RTEST(stack[%d])) {\n", --b->stack_size);
	fprintf(f, "    RUBY_VM_CHECK_INTS(ec);\n");
	fprintf(f, "    goto label_%d;\n", pos + insn_len(insn) + (unsigned int)operands[0]);
	fprintf(f, "  }\n");
	compile_insns(f, body, b->stack_size, pos + insn_len(insn), status);
	next_pos = pos + insn_len(insn) + (unsigned int)operands[0];
        break;
      case YARVINSN_branchnil:
	fprintf(f, "  if (NIL_P(stack[%d])) {\n", --b->stack_size);
	fprintf(f, "    RUBY_VM_CHECK_INTS(ec);\n");
	fprintf(f, "    goto label_%d;\n", pos + insn_len(insn) + (unsigned int)operands[0]);
	fprintf(f, "  }\n");
	compile_insns(f, body, b->stack_size, pos + insn_len(insn), status);
	next_pos = pos + insn_len(insn) + (unsigned int)operands[0];
        break;
      case YARVINSN_branchiftype:
	fprintf(f, "  if (TYPE(stack[%d]) == (int)0x%"PRIxVALUE") {\n", --b->stack_size, operands[0]);
	fprintf(f, "    RUBY_VM_CHECK_INTS(ec);\n");
	fprintf(f, "    goto label_%d;\n", pos + insn_len(insn) + (unsigned int)operands[1]);
	fprintf(f, "  }\n");
        break;
      case YARVINSN_getinlinecache:
	fprintf(f, "  stack[%d] = vm_ic_hit_p(0x%"PRIxVALUE", cfp->ep);", b->stack_size, operands[1]);
	fprintf(f, "  if (stack[%d] != Qnil) {\n", b->stack_size);
	fprintf(f, "    goto label_%d;\n", pos + insn_len(insn) + (unsigned int)operands[0]);
	fprintf(f, "  }\n");
	b->stack_size++;
        break;
      case YARVINSN_setinlinecache:
	fprintf(f, "  vm_ic_update(0x%"PRIxVALUE", stack[%d], cfp->ep);\n", operands[0], b->stack_size-1);
        break;
      /*case YARVINSN_once:
        fprintf(f, "  stack[%d] = vm_once_dispatch(0x%"PRIxVALUE", 0x%"PRIxVALUE", ec);\n", b->stack_size++, operands[0], operands[1]);
        break; */
      case YARVINSN_opt_case_dispatch:
	{
	    struct case_dispatch_var arg;
	    arg.f = f;
	    arg.base_pos = pos + insn_len(insn);
	    arg.last_value = Qundef;

	    fprintf(f, "  switch (vm_case_dispatch(0x%"PRIxVALUE", 0x%"PRIxVALUE", stack[%d])) {\n", operands[0], operands[1], --b->stack_size);
	    rb_hash_foreach(operands[0], compile_case_dispatch_each, (VALUE)&arg);
	    fprintf(f, "    case %lu:\n", operands[1]);
	    fprintf(f, "      goto label_%lu;\n", arg.base_pos + operands[1]);
	    fprintf(f, "  }\n");
	}
        break;
      case YARVINSN_opt_plus:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2, "vm_opt_plus(recv, obj)");
        break;
      case YARVINSN_opt_minus:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2, "vm_opt_minus(recv, obj)");
        break;
      case YARVINSN_opt_mult:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2, "vm_opt_mult(recv, obj)");
        break;
      case YARVINSN_opt_div:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2, "vm_opt_div(recv, obj)");
        break;
      case YARVINSN_opt_mod:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2, "vm_opt_mod(recv, obj)");
        break;
      case YARVINSN_opt_eq:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2,
		"opt_eq_func(recv, obj, 0x%"PRIxVALUE", 0x%"PRIxVALUE")", operands[0], operands[1]);
        break;
      case YARVINSN_opt_neq:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2,
		"vm_opt_neq(0x%"PRIxVALUE", 0x%"PRIxVALUE", 0x%"PRIxVALUE", 0x%"PRIxVALUE", recv, obj)",
		operands[0], operands[1], operands[2], operands[3]);
        break;
      case YARVINSN_opt_lt:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2, "vm_opt_lt(recv, obj)");
        break;
      case YARVINSN_opt_le:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2, "vm_opt_le(recv, obj)");
        break;
      case YARVINSN_opt_gt:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2, "vm_opt_gt(recv, obj)");
        break;
      case YARVINSN_opt_ge:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2, "vm_opt_ge(recv, obj)");
        break;
      case YARVINSN_opt_ltlt:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2, "vm_opt_ltlt(recv, obj)");
        break;
      case YARVINSN_opt_aref:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2, "mjit_opt_aref(recv, obj)");
        break;
      case YARVINSN_opt_aset:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 3, "vm_opt_aset(recv, obj, obj2)");
        break;
      case YARVINSN_opt_aset_with:
	b->stack_size += fprint_opt_call_with_key(f, operands[0], operands[1], operands[2], b->stack_size, 2,
		"vm_opt_aset_with(recv, 0x%"PRIxVALUE", obj)", operands[2]);
        break;
      case YARVINSN_opt_aref_with:
	b->stack_size += fprint_opt_call_with_key(f, operands[0], operands[1], operands[2], b->stack_size, 1,
		"vm_opt_aref_with(recv, 0x%"PRIxVALUE")", operands[2]);
        break;
      case YARVINSN_opt_length:
	fprint_opt_call(f, operands[0], operands[1], b->stack_size, 1, "vm_opt_length(recv, BOP_LENGTH)");
        break;
      case YARVINSN_opt_size:
	fprint_opt_call(f, operands[0], operands[1], b->stack_size, 1, "vm_opt_length(recv, BOP_SIZE)");
        break;
      case YARVINSN_opt_empty_p:
	fprint_opt_call(f, operands[0], operands[1], b->stack_size, 1, "vm_opt_empty_p(recv)");
        break;
      case YARVINSN_opt_succ:
	fprint_opt_call(f, operands[0], operands[1], b->stack_size, 1, "vm_opt_succ(recv)");
        break;
      case YARVINSN_opt_not:
	fprint_opt_call(f, operands[0], operands[1], b->stack_size, 1,
		"vm_opt_not(0x%"PRIxVALUE", 0x%"PRIxVALUE", recv)", operands[0], operands[1]);
        break;
      case YARVINSN_opt_regexpmatch1:
	fprintf(f, "  stack[%d] = vm_opt_regexpmatch1((VALUE)0x%"PRIxVALUE", stack[%d]);\n", b->stack_size-1, operands[0], b->stack_size-1);
        break;
      case YARVINSN_opt_regexpmatch2:
	b->stack_size += fprint_opt_call(f, operands[0], operands[1], b->stack_size, 2, "vm_opt_regexpmatch2(recv, obj)");
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
      case YARVINSN_setlocal_OP__WC__0:
	fprint_setlocal(f, --b->stack_size, operands[0], 0);
        break;
      case YARVINSN_setlocal_OP__WC__1:
	fprint_setlocal(f, --b->stack_size, operands[0], 1);
        break;
      case YARVINSN_putobject_OP_INT2FIX_O_0_C_:
	fprintf(f, "  stack[%d] = INT2FIX(0);\n", b->stack_size++);
        break;
      case YARVINSN_putobject_OP_INT2FIX_O_1_C_:
	fprintf(f, "  stack[%d] = INT2FIX(1);\n", b->stack_size++);
        break;
      default:
	if (mjit_opts.warnings || mjit_opts.verbose >= 3)
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

	fprintf(f, "\nlabel_%d: /* %s */\n", pos, insn_name(insn));
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

/* Print basic block code to cancel JIT execution. */
static void
compile_cancel_handler(FILE *f, const struct rb_iseq_constant_body *body)
{
    unsigned int i;
    fprintf(f, "cancel:\n");
    for (i = 0; i < body->stack_max; i++) {
	fprintf(f, "  *((VALUE *)cfp->bp + %d) = stack[%d];\n", i + 1, i);
    }
    fprintf(f, "  return Qundef;\n");
}

/* Compile ISeq to C code in F.  It returns 1 if it succeeds to compile. */
int
mjit_compile(FILE *f, const struct rb_iseq_constant_body *body, const char *funcname)
{
    struct compile_status status;
    status.success = TRUE;
    status.compiled_for_pos = ZALLOC_N(int, body->iseq_size);

    fprintf(f, "VALUE %s(rb_execution_context_t *ec, rb_control_frame_t *cfp) {\n", funcname);
    if (body->stack_max > 0) {
	fprintf(f, "  VALUE stack[%d];\n", body->stack_max);
    }

    /* Simulate `opt_pc` in setup_parameters_complex */
    if (body->param.flags.has_opt) {
	int i;
	fprintf(f, "\n");
	fprintf(f, "  switch (cfp->pc - cfp->iseq->body->iseq_encoded) {\n");
	for (i = 0; i <= body->param.opt_num; i++) {
	    VALUE pc_offset = body->param.opt_table[i];
	    fprintf(f, "    case %"PRIdVALUE":\n", pc_offset);
	    fprintf(f, "      goto label_%"PRIdVALUE";\n", pc_offset);
	}
	fprintf(f, "  }\n");
    }

    compile_insns(f, body, 0, 0, &status);
    compile_cancel_handler(f, body);
    fprintf(f, "}\n");

    xfree(status.compiled_for_pos);
    return status.success;
}
