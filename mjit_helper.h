/**********************************************************************

  mjit_helper.h - Functions used only in JIT-ed code

  Copyright (C) 2017 Takashi Kokubun <takashikkbn@gmail.com>.

**********************************************************************/

/* Cache hit check carved out from vm_search_method. Return TRUE if invalid.
   As inlining whole vm_search_method is too heavy for compiler optimizations,
   we use only this part in JIT-ed code. */
static inline int
mjit_check_invalid_cc(VALUE obj, CALL_CACHE cc)
{
    return GET_GLOBAL_METHOD_STATE() != cc->method_state || RCLASS_SERIAL(CLASS_OF(obj)) != cc->class_serial;
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
