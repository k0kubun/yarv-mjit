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
