/**********************************************************************

  mjit_helper.h - Functions used only in JIT-ed code

  Copyright (C) 2017 Takashi Kokubun <takashikkbn@gmail.com>.

**********************************************************************/

/* Cache hit check carved out from vm_search_method. Return TRUE if invalid.
   As inlining whole vm_search_method is too heavy for compiler optimizations,
   we use only this part in JIT-ed code. */
static inline int
mjit_check_invalid_cc(VALUE obj, rb_serial_t method_state, rb_serial_t class_serial)
{
    return GET_GLOBAL_METHOD_STATE() != method_state || RCLASS_SERIAL(CLASS_OF(obj)) != class_serial;
}

static inline VALUE
mjit_call_cfunc_with_frame(rb_execution_context_t *ec, rb_control_frame_t *reg_cfp, struct rb_calling_info *calling, const struct rb_call_info *ci, const rb_callable_method_entry_t *me)
{
    VALUE val;
    const rb_method_cfunc_t *cfunc = vm_method_cfunc_entry(me);
    int len = cfunc->argc;

    VALUE recv = calling->recv;
    VALUE block_handler = calling->block_handler;
    int argc = calling->argc;

    RUBY_DTRACE_CMETHOD_ENTRY_HOOK(rb_ec_thread_ptr(ec), me->owner, me->def->original_id);
    EXEC_EVENT_HOOK(ec, RUBY_EVENT_C_CALL, recv, me->def->original_id, ci->mid, me->owner, Qundef);

    vm_push_frame(ec, NULL, VM_FRAME_MAGIC_CFUNC | VM_FRAME_FLAG_CFRAME | VM_ENV_FLAG_LOCAL, recv,
		  block_handler, (VALUE)me,
		  0, ec->cfp->sp, 0, 0);

    if (len >= 0) rb_check_arity(argc, len, len);

    reg_cfp->sp -= argc + 1;
    VM_PROFILE_UP(R2C_CALL);
    val = (*cfunc->invoker)(cfunc->func, recv, argc, reg_cfp->sp + 1);

    CHECK_CFP_CONSISTENCY("mjit_call_cfunc");

    rb_vm_pop_frame(ec);

    EXEC_EVENT_HOOK(ec, RUBY_EVENT_C_RETURN, recv, me->def->original_id, ci->mid, me->owner, val);
    RUBY_DTRACE_CMETHOD_RETURN_HOOK(rb_ec_thread_ptr(ec), me->owner, me->def->original_id);

    return val;
}

/* Specialized `vm_call_cfunc` that does NOT take cc whose me can be modified after compilation.
   By not using `vm_call_cfunc`, we embed fixed function to JIT-ed code.
   TODO: change upstream's vm_call_cfunc_with_frame to take me */
static VALUE
mjit_call_cfunc(rb_execution_context_t *ec, rb_control_frame_t *reg_cfp, struct rb_calling_info *calling, const struct rb_call_info *ci, const rb_callable_method_entry_t *me)
{
    CALLER_SETUP_ARG(reg_cfp, calling, ci);
    return mjit_call_cfunc_with_frame(ec, reg_cfp, calling, ci, me);
}

static inline VALUE
mjit_ary_entry(VALUE ary, long offset)
{
    long len = RARRAY_LEN(ary);
    const VALUE *ptr = RARRAY_CONST_PTR(ary);
    if (len == 0) return Qnil;
    if (offset < 0) {
        offset += len;
        if (offset < 0) return Qnil;
    }
    else if (len <= offset) {
        return Qnil;
    }
    return ptr[offset];
}

static inline VALUE
mjit_ary_aref1(VALUE ary, VALUE arg)
{
    if (FIXNUM_P(arg)) {
	return mjit_ary_entry(ary, FIX2LONG(arg));
    } else {
	return rb_ary_aref1(arg, arg);
    }
}

static VALUE
mjit_opt_aref(VALUE recv, VALUE obj)
{
    if (SPECIAL_CONST_P(recv)) {
	return Qundef;
    }
    else if (RBASIC_CLASS(recv) == rb_cArray &&
	     BASIC_OP_UNREDEFINED_P(BOP_AREF, ARRAY_REDEFINED_OP_FLAG)) {
	return mjit_ary_aref1(recv, obj);
    }
    else if (RBASIC_CLASS(recv) == rb_cHash &&
	     BASIC_OP_UNREDEFINED_P(BOP_AREF, HASH_REDEFINED_OP_FLAG)) {
	return rb_hash_aref(recv, obj);
    }
    else {
	return Qundef;
    }
}
