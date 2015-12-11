/* Copyright 2014-2015 Samsung Electronics Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "opcodes-dumper.h"

#include "serializer.h"
#include "stack.h"
#include "jsp-early-error.h"

/**
 * Register allocator's counter
 */
static vm_idx_t jsp_reg_next;

/**
 * Maximum identifier of a register, allocated for intermediate value storage
 *
 * See also:
 *          dumper_new_scope, dumper_finish_scope
 */
static vm_idx_t jsp_reg_max_for_temps;

/**
 * Maximum identifier of a register, allocated for storage of a variable value.
 *
 * The value can be VM_IDX_EMPTY, indicating that no registers were allocated for variable values.
 *
 * Note:
 *      Registers for variable values are always allocated after registers for temporary values,
 *      so the value, if not equal to VM_IDX_EMPTY, is always greater than jsp_reg_max_for_temps.
 *
 * See also:
 *          dumper_try_replace_identifier_name_with_reg
 */
static vm_idx_t jsp_reg_max_for_local_var;

/**
 * Maximum identifier of a register, allocated for storage of an argument value.
 *
 * The value can be VM_IDX_EMPTY, indicating that no registers were allocated for argument values.
 *
 * Note:
 *      Registers for argument values are always allocated after registers for variable values,
 *      so the value, if not equal to VM_IDX_EMPTY, is always greater than jsp_reg_max_for_local_var.
 *
 * See also:
 *          dumper_try_replace_identifier_name_with_reg
 */
static vm_idx_t jsp_reg_max_for_args;

enum
{
  U8_global_size
};
STATIC_STACK (U8, uint8_t)

enum
{
  varg_headers_global_size
};
STATIC_STACK (varg_headers, vm_instr_counter_t)

enum
{
  function_ends_global_size
};
STATIC_STACK (function_ends, vm_instr_counter_t)

enum
{
  logical_and_checks_global_size
};
STATIC_STACK (logical_and_checks, vm_instr_counter_t)

enum
{
  logical_or_checks_global_size
};
STATIC_STACK (logical_or_checks, vm_instr_counter_t)

enum
{
  conditional_checks_global_size
};
STATIC_STACK (conditional_checks, vm_instr_counter_t)

enum
{
  jumps_to_end_global_size
};
STATIC_STACK (jumps_to_end, vm_instr_counter_t)

enum
{
  prop_getters_global_size
};
STATIC_STACK (prop_getters, op_meta)

enum
{
  next_iterations_global_size
};
STATIC_STACK (next_iterations, vm_instr_counter_t)

enum
{
  case_clauses_global_size
};
STATIC_STACK (case_clauses, vm_instr_counter_t)

enum
{
  tries_global_size
};
STATIC_STACK (tries, vm_instr_counter_t)

enum
{
  catches_global_size
};
STATIC_STACK (catches, vm_instr_counter_t)

enum
{
  finallies_global_size
};
STATIC_STACK (finallies, vm_instr_counter_t)

enum
{
  jsp_reg_id_stack_global_size
};
STATIC_STACK (jsp_reg_id_stack, vm_idx_t)

/**
 * Allocate next register for intermediate value
 *
 * @return identifier of the allocated register
 */
static vm_idx_t
jsp_alloc_reg_for_temp (void)
{
  JERRY_ASSERT (jsp_reg_max_for_local_var == VM_IDX_EMPTY);
  JERRY_ASSERT (jsp_reg_max_for_args == VM_IDX_EMPTY);

  vm_idx_t next_reg = jsp_reg_next++;

  if (next_reg > VM_REG_GENERAL_LAST)
  {
    /*
     * FIXME:
     *       Implement mechanism, allowing reusage of register variables
     */
    PARSE_ERROR (JSP_EARLY_ERROR_SYNTAX, "Not enough register variables", LIT_ITERATOR_POS_ZERO);
  }

  if (jsp_reg_max_for_temps < next_reg)
  {
    jsp_reg_max_for_temps = next_reg;
  }

  return next_reg;
} /* jsp_alloc_reg_for_temp */

/**
* Check if given register index is for temps
*/
static bool
is_temp_register (const vm_idx_t reg) /**< register index */
{
  return VM_REG_GENERAL_FIRST <= reg && reg <= jsp_reg_max_for_temps;
}

#ifdef CONFIG_PARSER_ENABLE_PARSE_TIME_BYTE_CODE_OPTIMIZER
/**
 * Start move of variable values to registers optimization pass
 */
void
dumper_start_move_of_vars_to_regs ()
{
  JERRY_ASSERT (jsp_reg_max_for_local_var == VM_IDX_EMPTY);
  JERRY_ASSERT (jsp_reg_max_for_args == VM_IDX_EMPTY);

  jsp_reg_max_for_local_var = jsp_reg_max_for_temps;
} /* dumper_start_move_of_vars_to_regs */

/**
 * Start move of argument values to registers optimization pass
 *
 * @return true - if optimization can be performed successfully (i.e. there are enough free registers),
 *         false - otherwise.
 */
bool
dumper_start_move_of_args_to_regs (uint32_t args_num) /**< number of arguments */
{
  JERRY_ASSERT (jsp_reg_max_for_args == VM_IDX_EMPTY);

  if (jsp_reg_max_for_local_var == VM_IDX_EMPTY)
  {
    if (args_num + jsp_reg_max_for_temps >= VM_REG_GENERAL_LAST)
    {
      return false;
    }

    jsp_reg_max_for_args = jsp_reg_max_for_temps;
  }
  else
  {
    if (args_num + jsp_reg_max_for_local_var >= VM_REG_GENERAL_LAST)
    {
      return false;
    }

    jsp_reg_max_for_args = jsp_reg_max_for_local_var;
  }

  return true;
} /* dumper_start_move_of_args_to_regs */

/**
 * Try to move local variable to a register
 *
 * Note:
 *      First instruction of the scope should be either func_decl_n or func_expr_n, as the scope is function scope,
 *      and the optimization is not applied to 'new Function ()'-like constructed functions.
 *
 * See also:
 *          parse_source_element_list
 *          parser_parse_program
 *
 * @return true, if optimization performed successfully, i.e.:
 *                - there is a free register to use;
 *                - the variable name is not equal to any of the function's argument names;
 *         false - otherwise.
 */
bool
dumper_try_replace_identifier_name_with_reg (scopes_tree tree, /**< a function scope, created for
                                                                *   function declaration or function expresssion */
                                             op_meta *om_p) /**< operation meta of corresponding
                                                             *   variable declaration */
{
  JERRY_ASSERT (tree->type == SCOPE_TYPE_FUNCTION);

  lit_cpointer_t lit_cp;
  bool is_arg;

  if (om_p->op.op_idx == VM_OP_VAR_DECL)
  {
    JERRY_ASSERT (om_p->lit_id[0].packed_value != NOT_A_LITERAL.packed_value);
    JERRY_ASSERT (om_p->lit_id[1].packed_value == NOT_A_LITERAL.packed_value);
    JERRY_ASSERT (om_p->lit_id[2].packed_value == NOT_A_LITERAL.packed_value);

    lit_cp = om_p->lit_id[0];

    is_arg = false;
  }
  else
  {
    JERRY_ASSERT (om_p->op.op_idx == VM_OP_META);

    JERRY_ASSERT (om_p->op.data.meta.type == OPCODE_META_TYPE_VARG);
    JERRY_ASSERT (om_p->lit_id[0].packed_value == NOT_A_LITERAL.packed_value);
    JERRY_ASSERT (om_p->lit_id[1].packed_value != NOT_A_LITERAL.packed_value);
    JERRY_ASSERT (om_p->lit_id[2].packed_value == NOT_A_LITERAL.packed_value);

    lit_cp = om_p->lit_id[1];

    is_arg = true;
  }

  vm_idx_t reg;

  if (is_arg)
  {
    JERRY_ASSERT (jsp_reg_max_for_args != VM_IDX_EMPTY);
    JERRY_ASSERT (jsp_reg_max_for_args < VM_REG_GENERAL_LAST);

    reg = ++jsp_reg_max_for_args;
  }
  else
  {
    JERRY_ASSERT (jsp_reg_max_for_local_var != VM_IDX_EMPTY);

    if (jsp_reg_max_for_local_var == VM_REG_GENERAL_LAST)
    {
      /* not enough registers */
      return false;
    }
    JERRY_ASSERT (jsp_reg_max_for_local_var < VM_REG_GENERAL_LAST);

    reg = ++jsp_reg_max_for_local_var;
  }

  for (vm_instr_counter_t instr_pos = 0;
       instr_pos < tree->instrs_count;
       instr_pos++)
  {
    op_meta om = scopes_tree_op_meta (tree, instr_pos);

    vm_op_t opcode = (vm_op_t) om.op.op_idx;

    int args_num = 0;

#define VM_OP_0(opcode_name, opcode_name_uppercase) \
    if (opcode == VM_OP_ ## opcode_name_uppercase) \
    { \
      args_num = 0; \
    }
#define VM_OP_1(opcode_name, opcode_name_uppercase, arg1, arg1_type) \
    if (opcode == VM_OP_ ## opcode_name_uppercase) \
    { \
      JERRY_STATIC_ASSERT (((arg1_type) & VM_OP_ARG_TYPE_TYPE_OF_NEXT) == 0); \
      args_num = 1; \
    }
#define VM_OP_2(opcode_name, opcode_name_uppercase, arg1, arg1_type, arg2, arg2_type) \
    if (opcode == VM_OP_ ## opcode_name_uppercase) \
    { \
      JERRY_STATIC_ASSERT (((arg1_type) & VM_OP_ARG_TYPE_TYPE_OF_NEXT) == 0); \
      JERRY_STATIC_ASSERT (((arg2_type) & VM_OP_ARG_TYPE_TYPE_OF_NEXT) == 0); \
      args_num = 2; \
    }
#define VM_OP_3(opcode_name, opcode_name_uppercase, arg1, arg1_type, arg2, arg2_type, arg3, arg3_type) \
    if (opcode == VM_OP_ ## opcode_name_uppercase) \
    { \
      /*
       * See also:
       *          The loop below
       */ \
      \
      JERRY_ASSERT ((opcode == VM_OP_ASSIGNMENT && (arg2_type) == VM_OP_ARG_TYPE_TYPE_OF_NEXT) \
                    || (opcode != VM_OP_ASSIGNMENT && ((arg2_type) & VM_OP_ARG_TYPE_TYPE_OF_NEXT) == 0)); \
      JERRY_ASSERT ((opcode == VM_OP_META && ((arg1_type) & VM_OP_ARG_TYPE_TYPE_OF_NEXT) != 0) \
                    || (opcode != VM_OP_META && ((arg1_type) & VM_OP_ARG_TYPE_TYPE_OF_NEXT) == 0)); \
      JERRY_STATIC_ASSERT (((arg3_type) & VM_OP_ARG_TYPE_TYPE_OF_NEXT) == 0); \
      args_num = 3; \
    }

#include "vm-opcodes.inc.h"

    for (int arg_index = 0; arg_index < args_num; arg_index++)
    {
      /*
       * 'assignment' and 'meta' are the only opcodes with statically unspecified argument type
       * (checked by assertions above)
       */
      if (opcode == VM_OP_ASSIGNMENT
          && arg_index == 1
          && om.op.data.assignment.type_value_right != VM_OP_ARG_TYPE_VARIABLE)
      {
        break;
      }

      if (opcode == VM_OP_META
          && (om.op.data.meta.type == OPCODE_META_TYPE_VARG_PROP_DATA
              || om.op.data.meta.type == OPCODE_META_TYPE_VARG_PROP_GETTER
              || om.op.data.meta.type == OPCODE_META_TYPE_VARG_PROP_SETTER)
          && arg_index == 1)
      {
        continue;
      }

      if (om.lit_id[arg_index].packed_value == lit_cp.packed_value)
      {
        om.lit_id[arg_index] = NOT_A_LITERAL;

        JERRY_ASSERT (om.op.data.raw_args[arg_index] == VM_IDX_REWRITE_LITERAL_UID);
        om.op.data.raw_args[arg_index] = reg;
      }
    }

    scopes_tree_set_op_meta (tree, instr_pos, om);
  }

  return true;
} /* dumper_try_replace_identifier_name_with_reg */
#endif /* CONFIG_PARSER_ENABLE_PARSE_TIME_BYTE_CODE_OPTIMIZER */

/**
 * Just allocate register for argument that is never used due to duplicated argument names
 */
void
dumper_alloc_reg_for_unused_arg (void)
{
  JERRY_ASSERT (jsp_reg_max_for_args != VM_IDX_EMPTY);
  JERRY_ASSERT (jsp_reg_max_for_args < VM_REG_GENERAL_LAST);

  ++jsp_reg_max_for_args;
} /* dumper_alloc_reg_for_unused_arg */

/**
 * Generate instruction with specified opcode and operands
 *
 * @return VM instruction
 */
static vm_instr_t
jsp_dmp_gen_instr (vm_op_t opcode, /**< operation code */
                   jsp_operand_t ops[], /**< operands */
                   size_t ops_num) /**< operands number */
{
  vm_instr_t instr;

  instr.op_idx = opcode;

  for (size_t i = 0; i < ops_num; i++)
  {
    if (ops[i].is_empty_operand ())
    {
      instr.data.raw_args[i] = VM_IDX_EMPTY;
    }
    else if (ops[i].is_unknown_operand ())
    {
      instr.data.raw_args[i] = VM_IDX_REWRITE_GENERAL_CASE;
    }
    else if (ops[i].is_idx_const_operand ())
    {
      instr.data.raw_args[i] = ops[i].get_idx_const ();
    }
    else if (ops[i].is_register_operand ())
    {
      instr.data.raw_args[i] = ops[i].get_idx ();
    }
    else
    {
      JERRY_ASSERT (ops[i].is_literal_operand ());

      instr.data.raw_args[i] = VM_IDX_REWRITE_LITERAL_UID;
    }
  }

  for (size_t i = ops_num; i < 3; i++)
  {
    instr.data.raw_args[i] = VM_IDX_EMPTY;
  }

  return instr;
} /* jsp_dmp_gen_instr */

/**
 * Create intermediate instruction description, containing pointers to literals,
 * associated with the instruction's arguments, if there are any.
 *
 * @return intermediate operation description
 */
static op_meta
jsp_dmp_create_op_meta (vm_op_t opcode, /**< opcode */
                        jsp_operand_t ops[], /**< operands */
                        size_t ops_num) /**< operands number */
{
  op_meta ret;

  ret.op = jsp_dmp_gen_instr (opcode, ops, ops_num);

  for (size_t i = 0; i < ops_num; i++)
  {
    if (ops[i].is_literal_operand ())
    {
      ret.lit_id[i] = ops[i].get_literal ();
    }
    else
    {
      ret.lit_id[i] = NOT_A_LITERAL;
    }
  }

  for (size_t i = ops_num; i < 3; i++)
  {
    ret.lit_id[i] = NOT_A_LITERAL;
  }

  return ret;
} /* jsp_dmp_create_op_meta */

/**
 * Create intermediate instruction description (for instructions without arguments)
 *
 * See also:
 *          jsp_dmp_create_op_meta
 *
 * @return intermediate instruction description
 */
static op_meta
jsp_dmp_create_op_meta_0 (vm_op_t opcode) /**< opcode */
{
  return jsp_dmp_create_op_meta (opcode, NULL, 0);
} /* jsp_dmp_create_op_meta_0 */

/**
 * Create intermediate instruction description (for instructions with 1 argument)
 *
 * See also:
 *          jsp_dmp_create_op_meta
 *
 * @return intermediate instruction description
 */
static op_meta
jsp_dmp_create_op_meta_1 (vm_op_t opcode, /**< opcode */
                          jsp_operand_t operand1) /**< first operand */
{
  return jsp_dmp_create_op_meta (opcode, &operand1, 1);
} /* jsp_dmp_create_op_meta_1 */

/**
 * Create intermediate instruction description (for instructions with 2 arguments)
 *
 * See also:
 *          jsp_dmp_create_op_meta
 *
 * @return intermediate instruction description
 */
static op_meta
jsp_dmp_create_op_meta_2 (vm_op_t opcode, /**< opcode */
                          jsp_operand_t operand1, /**< first operand */
                          jsp_operand_t operand2) /**< second operand */
{
  jsp_operand_t ops[] = { operand1, operand2 };
  return jsp_dmp_create_op_meta (opcode, ops, 2);
} /* jsp_dmp_create_op_meta_2 */

/**
 * Create intermediate instruction description (for instructions with 3 arguments)
 *
 * See also:
 *          jsp_dmp_create_op_meta
 *
 * @return intermediate instruction description
 */
static op_meta
jsp_dmp_create_op_meta_3 (vm_op_t opcode, /**< opcode */
                          jsp_operand_t operand1, /**< first operand */
                          jsp_operand_t operand2, /**< second operand */
                          jsp_operand_t operand3) /**< third operand */
{
  jsp_operand_t ops[] = { operand1, operand2, operand3 };
  return jsp_dmp_create_op_meta (opcode, ops, 3);
} /* jsp_dmp_create_op_meta_3 */

static jsp_operand_t
tmp_operand (void)
{
  return jsp_operand_t::make_reg_operand (jsp_alloc_reg_for_temp ());
}

static void
split_instr_counter (vm_instr_counter_t oc, vm_idx_t *id1, vm_idx_t *id2)
{
  JERRY_ASSERT (id1 != NULL);
  JERRY_ASSERT (id2 != NULL);
  *id1 = (vm_idx_t) (oc >> JERRY_BITSINBYTE);
  *id2 = (vm_idx_t) (oc & ((1 << JERRY_BITSINBYTE) - 1));
  JERRY_ASSERT (oc == vm_calc_instr_counter_from_idx_idx (*id1, *id2));
}

static op_meta
last_dumped_op_meta (void)
{
  return serializer_get_op_meta ((vm_instr_counter_t) (serializer_get_current_instr_counter () - 1));
}

static void
rewrite_last_dumped_op_meta (op_meta opm)
{
  serializer_rewrite_op_meta ((vm_instr_counter_t) (serializer_get_current_instr_counter () - 1), opm);
}

static void
dump_single_address (vm_op_t opcode,
                     jsp_operand_t op)
{
  serializer_dump_op_meta (jsp_dmp_create_op_meta_1 (opcode, op));
}

static void
dump_double_address (vm_op_t opcode,
                     jsp_operand_t res,
                     jsp_operand_t obj)
{
  serializer_dump_op_meta (jsp_dmp_create_op_meta_2 (opcode, res, obj));
}

static void
dump_triple_address (vm_op_t opcode,
                     jsp_operand_t res,
                     jsp_operand_t lhs,
                     jsp_operand_t rhs)
{
  serializer_dump_op_meta (jsp_dmp_create_op_meta_3 (opcode, res, lhs, rhs));
}

static jsp_operand_t
create_operand_from_tmp_and_lit (vm_idx_t tmp, lit_cpointer_t lit_id)
{
  if (tmp != VM_IDX_REWRITE_LITERAL_UID)
  {
    JERRY_ASSERT (lit_id.packed_value == MEM_CP_NULL);

    return jsp_operand_t::make_reg_operand (tmp);
  }
  else
  {
    JERRY_ASSERT (lit_id.packed_value != MEM_CP_NULL);

    return jsp_operand_t::make_lit_operand (lit_id);
  }
}

static void
dump_prop_setter_op_meta (op_meta last, jsp_operand_t op)
{
  JERRY_ASSERT (last.op.op_idx == VM_OP_PROP_GETTER);

  dump_triple_address (VM_OP_PROP_SETTER,
                       create_operand_from_tmp_and_lit (last.op.data.prop_getter.obj,
                                                        last.lit_id[1]),
                       create_operand_from_tmp_and_lit (last.op.data.prop_getter.prop,
                                                        last.lit_id[2]),
                       op);
}


static jsp_operand_t
dump_triple_address_and_prop_setter_res (vm_op_t opcode, /**< opcode of triple address operation */
                                         op_meta last,
                                         jsp_operand_t op)
{
  JERRY_ASSERT (last.op.op_idx == VM_OP_PROP_GETTER);

  const jsp_operand_t obj = create_operand_from_tmp_and_lit (last.op.data.prop_getter.obj, last.lit_id[1]);
  const jsp_operand_t prop = create_operand_from_tmp_and_lit (last.op.data.prop_getter.prop, last.lit_id[2]);

  const jsp_operand_t tmp = dump_prop_getter_res (obj, prop);

  dump_triple_address (opcode, tmp, tmp, op);

  dump_prop_setter (obj, prop, tmp);

  return tmp;
}

static jsp_operand_t
dump_prop_setter_or_triple_address_res (vm_op_t opcode,
                                        jsp_operand_t res,
                                        jsp_operand_t op)
{
  if (res.is_register_operand ())
  {
    /*
     * Left-hand-side must be a member expression and corresponding prop_getter
     * op is on top of the stack.
     */
    const op_meta last = STACK_TOP (prop_getters);
    JERRY_ASSERT (last.op.op_idx == VM_OP_PROP_GETTER);

    res = dump_triple_address_and_prop_setter_res (opcode, last, op);

    STACK_DROP (prop_getters, 1);
  }
  else
  {
    dump_triple_address (opcode, res, res, op);
  }
  return res;
}

static vm_instr_counter_t
get_diff_from (vm_instr_counter_t oc)
{
  return (vm_instr_counter_t) (serializer_get_current_instr_counter () - oc);
}

jsp_operand_t
empty_operand (void)
{
  return jsp_operand_t::make_empty_operand ();
}

jsp_operand_t
literal_operand (lit_cpointer_t lit_cp)
{
  return jsp_operand_t::make_lit_operand (lit_cp);
}

/**
 * Creates operand for eval's return value
 *
 * @return constructed operand
 */
jsp_operand_t
eval_ret_operand (void)
{
  return jsp_operand_t::make_reg_operand (VM_REG_SPECIAL_EVAL_RET);
} /* eval_ret_operand */

/**
 * Creates operand for taking iterator value (next property name)
 * from for-in instr handler.
 *
 * @return constructed operand
 */
jsp_operand_t
jsp_create_operand_for_in_special_reg (void)
{
  return jsp_operand_t::make_reg_operand (VM_REG_SPECIAL_FOR_IN_PROPERTY_NAME);
} /* jsp_create_operand_for_in_special_reg */

bool
operand_is_empty (jsp_operand_t op)
{
  return op.is_empty_operand ();
}

void
dumper_new_statement (void)
{
  jsp_reg_next = VM_REG_GENERAL_FIRST;
}

void
dumper_new_scope (void)
{
  JERRY_ASSERT (jsp_reg_max_for_local_var == VM_IDX_EMPTY);
  JERRY_ASSERT (jsp_reg_max_for_args == VM_IDX_EMPTY);

  STACK_PUSH (jsp_reg_id_stack, jsp_reg_next);
  STACK_PUSH (jsp_reg_id_stack, jsp_reg_max_for_temps);

  jsp_reg_next = VM_REG_GENERAL_FIRST;
  jsp_reg_max_for_temps = jsp_reg_next;
}

void
dumper_finish_scope (void)
{
  JERRY_ASSERT (jsp_reg_max_for_local_var == VM_IDX_EMPTY);
  JERRY_ASSERT (jsp_reg_max_for_args == VM_IDX_EMPTY);

  jsp_reg_max_for_temps = STACK_TOP (jsp_reg_id_stack);
  STACK_DROP (jsp_reg_id_stack, 1);
  jsp_reg_next = STACK_TOP (jsp_reg_id_stack);
  STACK_DROP (jsp_reg_id_stack, 1);
}

/**
 * Handle start of argument preparation instruction sequence generation
 *
 * Note:
 *      Values of registers, allocated for the code sequence, are not used outside of the sequence,
 *      so they can be reused, reducing register pressure.
 *
 *      To reuse the registers, counter of register allocator is saved, and restored then,
 *      after finishing generation of the code sequence, using dumper_finish_varg_code_sequence.
 *
 * FIXME:
 *       Implement general register allocation mechanism
 *
 * See also:
 *          dumper_finish_varg_code_sequence
 */
void
dumper_start_varg_code_sequence (void)
{
  STACK_PUSH (jsp_reg_id_stack, jsp_reg_next);
} /* dumper_start_varg_code_sequence */

/**
 * Handle finish of argument preparation instruction sequence generation
 *
 * See also:
 *          dumper_start_varg_code_sequence
 */
void
dumper_finish_varg_code_sequence (void)
{
  jsp_reg_next = STACK_TOP (jsp_reg_id_stack);
  STACK_DROP (jsp_reg_id_stack, 1);
} /* dumper_finish_varg_code_sequence */

/**
 * Check that byte-code operand refers to 'eval' string
 *
 * @return true - if specified byte-code operand's type is literal, and value of corresponding
 *                literal is equal to LIT_MAGIC_STRING_EVAL string,
 *         false - otherwise.
 */
bool
dumper_is_eval_literal (jsp_operand_t obj) /**< byte-code operand */
{
  /*
   * FIXME: Switch to corresponding magic string
   */
  bool is_eval_lit = (obj.is_literal_operand ()
                      && lit_literal_equal_type_cstr (lit_get_literal_by_cp (obj.get_literal ()), "eval"));

  return is_eval_lit;
} /* dumper_is_eval_literal */

/**
 * Dump assignment of an array-hole simple value to a register
 *
 * @return register number, to which the value vas assigned
 */
jsp_operand_t
dump_array_hole_assignment_res (void)
{
  jsp_operand_t op, type_operand, value_operand;

  op = tmp_operand ();

  type_operand = jsp_operand_t::make_idx_const_operand (OPCODE_ARG_TYPE_SIMPLE);
  value_operand = jsp_operand_t::make_idx_const_operand (ECMA_SIMPLE_VALUE_ARRAY_HOLE);

  dump_triple_address (VM_OP_ASSIGNMENT, op, type_operand, value_operand);

  return op;
} /* dump_array_hole_assignment_res */

void
dump_boolean_assignment (jsp_operand_t op, bool is_true)
{
  jsp_operand_t type_operand, value_operand;

  type_operand = jsp_operand_t::make_idx_const_operand (OPCODE_ARG_TYPE_SIMPLE);
  value_operand = jsp_operand_t::make_idx_const_operand (is_true ? ECMA_SIMPLE_VALUE_TRUE : ECMA_SIMPLE_VALUE_FALSE);

  dump_triple_address (VM_OP_ASSIGNMENT, op, type_operand, value_operand);
}

jsp_operand_t
dump_boolean_assignment_res (bool is_true)
{
  jsp_operand_t op = tmp_operand ();
  dump_boolean_assignment (op, is_true);
  return op;
}

void
dump_string_assignment (jsp_operand_t op, lit_cpointer_t lit_id)
{
  jsp_operand_t type_operand, value_operand;

  type_operand = jsp_operand_t::make_idx_const_operand (OPCODE_ARG_TYPE_STRING);
  value_operand = jsp_operand_t::make_lit_operand (lit_id);

  dump_triple_address (VM_OP_ASSIGNMENT, op, type_operand, value_operand);
}

jsp_operand_t
dump_string_assignment_res (lit_cpointer_t lit_id)
{
  jsp_operand_t op = tmp_operand ();
  dump_string_assignment (op, lit_id);
  return op;
}

void
dump_number_assignment (jsp_operand_t op, lit_cpointer_t lit_id)
{
  jsp_operand_t type_operand, value_operand;

  type_operand = jsp_operand_t::make_idx_const_operand (OPCODE_ARG_TYPE_NUMBER);
  value_operand = jsp_operand_t::make_lit_operand (lit_id);

  dump_triple_address (VM_OP_ASSIGNMENT, op, type_operand, value_operand);
}

jsp_operand_t
dump_number_assignment_res (lit_cpointer_t lit_id)
{
  jsp_operand_t op = tmp_operand ();
  dump_number_assignment (op, lit_id);
  return op;
}

void
dump_regexp_assignment (jsp_operand_t op, lit_cpointer_t lit_id)
{
  jsp_operand_t type_operand, value_operand;

  type_operand = jsp_operand_t::make_idx_const_operand (OPCODE_ARG_TYPE_REGEXP);
  value_operand = jsp_operand_t::make_lit_operand (lit_id);

  dump_triple_address (VM_OP_ASSIGNMENT, op, type_operand, value_operand);
}

jsp_operand_t
dump_regexp_assignment_res (lit_cpointer_t lit_id)
{
  jsp_operand_t op = tmp_operand ();
  dump_regexp_assignment (op, lit_id);
  return op;
}

void
dump_smallint_assignment (jsp_operand_t op, vm_idx_t uid)
{
  jsp_operand_t type_operand, value_operand;

  type_operand = jsp_operand_t::make_idx_const_operand (OPCODE_ARG_TYPE_SMALLINT);
  value_operand = jsp_operand_t::make_idx_const_operand (uid);

  dump_triple_address (VM_OP_ASSIGNMENT, op, type_operand, value_operand);
}

jsp_operand_t
dump_smallint_assignment_res (vm_idx_t uid)
{
  jsp_operand_t op = tmp_operand ();
  dump_smallint_assignment (op, uid);
  return op;
}

void
dump_undefined_assignment (jsp_operand_t op)
{
  jsp_operand_t type_operand, value_operand;

  type_operand = jsp_operand_t::make_idx_const_operand (OPCODE_ARG_TYPE_SIMPLE);
  value_operand = jsp_operand_t::make_idx_const_operand (ECMA_SIMPLE_VALUE_UNDEFINED);

  dump_triple_address (VM_OP_ASSIGNMENT, op, type_operand, value_operand);
}

jsp_operand_t
dump_undefined_assignment_res (void)
{
  jsp_operand_t op = tmp_operand ();
  dump_undefined_assignment (op);
  return op;
}

void
dump_null_assignment (jsp_operand_t op)
{
  jsp_operand_t type_operand, value_operand;

  type_operand = jsp_operand_t::make_idx_const_operand (OPCODE_ARG_TYPE_SIMPLE);
  value_operand = jsp_operand_t::make_idx_const_operand (ECMA_SIMPLE_VALUE_NULL);

  dump_triple_address (VM_OP_ASSIGNMENT, op, type_operand, value_operand);
}

jsp_operand_t
dump_null_assignment_res (void)
{
  jsp_operand_t op = tmp_operand ();
  dump_null_assignment (op);
  return op;
}

void
dump_variable_assignment (jsp_operand_t res, jsp_operand_t var)
{
  jsp_operand_t type_operand;

  type_operand = jsp_operand_t::make_idx_const_operand (OPCODE_ARG_TYPE_VARIABLE);

  dump_triple_address (VM_OP_ASSIGNMENT, res, type_operand, var);
}

jsp_operand_t
dump_variable_assignment_res (jsp_operand_t var)
{
  jsp_operand_t op = tmp_operand ();
  dump_variable_assignment (op, var);
  return op;
}

void
dump_varg_header_for_rewrite (varg_list_type vlt, jsp_operand_t obj)
{
  STACK_PUSH (varg_headers, serializer_get_current_instr_counter ());
  switch (vlt)
  {
    case VARG_FUNC_EXPR:
    {
      dump_triple_address (VM_OP_FUNC_EXPR_N,
                           jsp_operand_t::make_unknown_operand (),
                           obj,
                           jsp_operand_t::make_unknown_operand ());
      break;
    }
    case VARG_CONSTRUCT_EXPR:
    {
      dump_triple_address (VM_OP_CONSTRUCT_N,
                           jsp_operand_t::make_unknown_operand (),
                           obj,
                           jsp_operand_t::make_unknown_operand ());
      break;
    }
    case VARG_CALL_EXPR:
    {
      dump_triple_address (VM_OP_CALL_N,
                           jsp_operand_t::make_unknown_operand (),
                           obj,
                           jsp_operand_t::make_unknown_operand ());
      break;
    }
    case VARG_FUNC_DECL:
    {
      dump_double_address (VM_OP_FUNC_DECL_N,
                           obj,
                           jsp_operand_t::make_unknown_operand ());
      break;
    }
    case VARG_ARRAY_DECL:
    {
      dump_double_address (VM_OP_ARRAY_DECL,
                           jsp_operand_t::make_unknown_operand (),
                           jsp_operand_t::make_unknown_operand ());
      break;
    }
    case VARG_OBJ_DECL:
    {
      dump_double_address (VM_OP_OBJ_DECL,
                           jsp_operand_t::make_unknown_operand (),
                           jsp_operand_t::make_unknown_operand ());
      break;
    }
  }
}

jsp_operand_t
rewrite_varg_header_set_args_count (size_t args_count)
{
  /*
   * FIXME:
   *       Remove formal parameters / arguments number from instruction,
   *       after ecma-values collection would become extendable (issue #310).
   *       In the case, each 'varg' instruction would just append corresponding
   *       argument / formal parameter name to values collection.
   */

  op_meta om = serializer_get_op_meta (STACK_TOP (varg_headers));
  switch (om.op.op_idx)
  {
    case VM_OP_FUNC_EXPR_N:
    case VM_OP_CONSTRUCT_N:
    case VM_OP_CALL_N:
    {
      if (args_count > 255)
      {
        PARSE_ERROR (JSP_EARLY_ERROR_SYNTAX,
                     "No more than 255 formal parameters / arguments are currently supported",
                     LIT_ITERATOR_POS_ZERO);
      }
      const jsp_operand_t res = tmp_operand ();
      om.op.data.func_expr_n.arg_list = (vm_idx_t) args_count;
      om.op.data.func_expr_n.lhs = res.get_idx ();
      serializer_rewrite_op_meta (STACK_TOP (varg_headers), om);
      STACK_DROP (varg_headers, 1);
      return res;
    }
    case VM_OP_FUNC_DECL_N:
    {
      if (args_count > 255)
      {
        PARSE_ERROR (JSP_EARLY_ERROR_SYNTAX,
                     "No more than 255 formal parameters are currently supported",
                     LIT_ITERATOR_POS_ZERO);
      }
      om.op.data.func_decl_n.arg_list = (vm_idx_t) args_count;
      serializer_rewrite_op_meta (STACK_TOP (varg_headers), om);
      STACK_DROP (varg_headers, 1);
      return empty_operand ();
    }
    case VM_OP_ARRAY_DECL:
    case VM_OP_OBJ_DECL:
    {
      if (args_count > 65535)
      {
        PARSE_ERROR (JSP_EARLY_ERROR_SYNTAX,
                     "No more than 65535 formal parameters are currently supported",
                     LIT_ITERATOR_POS_ZERO);
      }
      const jsp_operand_t res = tmp_operand ();
      om.op.data.obj_decl.list_1 = (vm_idx_t) (args_count >> 8);
      om.op.data.obj_decl.list_2 = (vm_idx_t) (args_count & 0xffu);
      om.op.data.obj_decl.lhs = res.get_idx ();
      serializer_rewrite_op_meta (STACK_TOP (varg_headers), om);
      STACK_DROP (varg_headers, 1);
      return res;
    }
    default:
    {
      JERRY_UNREACHABLE ();
    }
  }
}

/**
 * Dump 'meta' instruction of 'call additional information' type,
 * containing call flags and, optionally, 'this' argument
 */
void
dump_call_additional_info (opcode_call_flags_t flags, /**< call flags */
                           jsp_operand_t this_arg) /**< 'this' argument - if flags
                                                    *   include OPCODE_CALL_FLAGS_HAVE_THIS_ARG,
                                                    *   or empty operand - otherwise */
{
  if (flags & OPCODE_CALL_FLAGS_HAVE_THIS_ARG)
  {
    JERRY_ASSERT (this_arg.is_register_operand ());
    JERRY_ASSERT (!operand_is_empty (this_arg));
  }
  else
  {
    JERRY_ASSERT (operand_is_empty (this_arg));
  }

  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_CALL_SITE_INFO),
                       jsp_operand_t::make_idx_const_operand (flags),
                       this_arg);
} /* dump_call_additional_info */

void
dump_varg (jsp_operand_t op)
{
  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_VARG),
                       op,
                       jsp_operand_t::make_empty_operand ());
}

void
dump_prop_name_and_value (jsp_operand_t name, jsp_operand_t value)
{
  JERRY_ASSERT (name.is_literal_operand ());
  literal_t lit = lit_get_literal_by_cp (name.get_literal ());
  JERRY_ASSERT (lit->get_type () == LIT_STR_T
                || lit->get_type () == LIT_MAGIC_STR_T
                || lit->get_type () == LIT_MAGIC_STR_EX_T);

  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_VARG_PROP_DATA),
                       name,
                       value);
}

void
dump_prop_getter_decl (jsp_operand_t name, jsp_operand_t func)
{
  JERRY_ASSERT (name.is_literal_operand ());
  JERRY_ASSERT (func.is_register_operand ());
  literal_t lit = lit_get_literal_by_cp (name.get_literal ());
  JERRY_ASSERT (lit->get_type () == LIT_STR_T
                || lit->get_type () == LIT_MAGIC_STR_T
                || lit->get_type () == LIT_MAGIC_STR_EX_T);

  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_VARG_PROP_GETTER),
                       name,
                       func);
}

void
dump_prop_setter_decl (jsp_operand_t name, jsp_operand_t func)
{
  JERRY_ASSERT (name.is_literal_operand ());
  JERRY_ASSERT (func.is_register_operand ());
  literal_t lit = lit_get_literal_by_cp (name.get_literal ());
  JERRY_ASSERT (lit->get_type () == LIT_STR_T
                || lit->get_type () == LIT_MAGIC_STR_T
                || lit->get_type () == LIT_MAGIC_STR_EX_T);

  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_VARG_PROP_SETTER),
                       name,
                       func);
}

void
dump_prop_getter (jsp_operand_t res, jsp_operand_t obj, jsp_operand_t prop)
{
  dump_triple_address (VM_OP_PROP_GETTER, res, obj, prop);
}

jsp_operand_t
dump_prop_getter_res (jsp_operand_t obj, jsp_operand_t prop)
{
  const jsp_operand_t res = tmp_operand ();
  dump_prop_getter (res, obj, prop);
  return res;
}

void
dump_prop_setter (jsp_operand_t res, jsp_operand_t obj, jsp_operand_t prop)
{
  dump_triple_address (VM_OP_PROP_SETTER, res, obj, prop);
}

void
dump_function_end_for_rewrite (void)
{
  STACK_PUSH (function_ends, serializer_get_current_instr_counter ());

  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_FUNCTION_END),
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());
}

void
rewrite_function_end ()
{
  vm_instr_counter_t oc;
  {
    oc = (vm_instr_counter_t) (get_diff_from (STACK_TOP (function_ends))
                               + serializer_count_instrs_in_subscopes ());
  }

  vm_idx_t id1, id2;
  split_instr_counter (oc, &id1, &id2);

  op_meta function_end_op_meta = serializer_get_op_meta (STACK_TOP (function_ends));
  JERRY_ASSERT (function_end_op_meta.op.op_idx == VM_OP_META);
  JERRY_ASSERT (function_end_op_meta.op.data.meta.type == OPCODE_META_TYPE_FUNCTION_END);
  JERRY_ASSERT (function_end_op_meta.op.data.meta.data_1 == VM_IDX_REWRITE_GENERAL_CASE);
  JERRY_ASSERT (function_end_op_meta.op.data.meta.data_2 == VM_IDX_REWRITE_GENERAL_CASE);

  function_end_op_meta.op.data.meta.data_1 = id1;
  function_end_op_meta.op.data.meta.data_2 = id2;

  serializer_rewrite_op_meta (STACK_TOP (function_ends), function_end_op_meta);

  STACK_DROP (function_ends, 1);
}

/**
 * Decrement position of 'function_end' instruction (the position is stored in "function_ends" stack)
 *
 * Note:
 *      The operation is used upon deleting a 'varg' meta, describing element of a function's formal parameters list
 */
void
dumper_decrement_function_end_pos (void)
{
  vm_instr_counter_t oc = STACK_TOP (function_ends);

  oc--;

  STACK_DROP (function_ends, 1);
  STACK_PUSH (function_ends, oc);
} /* dumper_decrement_function_end_pos */

jsp_operand_t
dump_this_res (void)
{
  return jsp_operand_t::make_reg_operand (VM_REG_SPECIAL_THIS_BINDING);
}

void
dump_post_increment (jsp_operand_t res, jsp_operand_t obj)
{
  dump_double_address (VM_OP_POST_INCR, res, obj);
}

jsp_operand_t
dump_post_increment_res (jsp_operand_t op)
{
  const jsp_operand_t res = tmp_operand ();
  dump_post_increment (res, op);
  return res;
}

void
dump_post_decrement (jsp_operand_t res, jsp_operand_t obj)
{
  dump_double_address (VM_OP_POST_DECR, res, obj);
}

jsp_operand_t
dump_post_decrement_res (jsp_operand_t op)
{
  const jsp_operand_t res = tmp_operand ();
  dump_post_decrement (res, op);
  return res;
}

/**
 * Check if operand of prefix operation is correct
 */
static void
check_operand_in_prefix_operation (jsp_operand_t obj) /**< operand, which type should be Reference */
{
  const op_meta last = last_dumped_op_meta ();
  if (last.op.op_idx != VM_OP_PROP_GETTER)
  {
    if (obj.is_register_operand ())
    {
      /*
       * FIXME:
       *       Implement correct handling of references through parser operands
       */
      PARSE_ERROR (JSP_EARLY_ERROR_REFERENCE,
                   "Invalid left-hand-side expression in prefix operation",
                   LIT_ITERATOR_POS_ZERO);
    }
  }
} /* check_operand_in_prefix_operation */

void
dump_pre_increment (jsp_operand_t res, jsp_operand_t obj)
{
  check_operand_in_prefix_operation (obj);
  dump_double_address (VM_OP_PRE_INCR, res, obj);
}

jsp_operand_t
dump_pre_increment_res (jsp_operand_t op)
{
  const jsp_operand_t res = tmp_operand ();
  dump_pre_increment (res, op);
  return res;
}

void
dump_pre_decrement (jsp_operand_t res, jsp_operand_t obj)
{
  check_operand_in_prefix_operation (obj);
  dump_double_address (VM_OP_PRE_DECR, res, obj);
}

jsp_operand_t
dump_pre_decrement_res (jsp_operand_t op)
{
  const jsp_operand_t res = tmp_operand ();
  dump_pre_decrement (res, op);
  return res;
}

void
dump_unary_plus (jsp_operand_t res, jsp_operand_t obj)
{
  dump_double_address (VM_OP_UNARY_PLUS, res, obj);
}

jsp_operand_t
dump_unary_plus_res (jsp_operand_t op)
{
  const jsp_operand_t res = tmp_operand ();
  dump_unary_plus (res, op);
  return res;
}

void
dump_unary_minus (jsp_operand_t res, jsp_operand_t obj)
{
  dump_double_address (VM_OP_UNARY_MINUS, res, obj);
}

jsp_operand_t
dump_unary_minus_res (jsp_operand_t op)
{
  const jsp_operand_t res = tmp_operand ();
  dump_unary_minus (res, op);
  return res;
}

void
dump_bitwise_not (jsp_operand_t res, jsp_operand_t obj)
{
  dump_double_address (VM_OP_B_NOT, res, obj);
}

jsp_operand_t
dump_bitwise_not_res (jsp_operand_t op)
{
  const jsp_operand_t res = tmp_operand ();
  dump_bitwise_not (res, op);
  return res;
}

void
dump_logical_not (jsp_operand_t res, jsp_operand_t obj)
{
  dump_double_address (VM_OP_LOGICAL_NOT, res, obj);
}

jsp_operand_t
dump_logical_not_res (jsp_operand_t op)
{
  const jsp_operand_t res = tmp_operand ();
  dump_logical_not (res, op);
  return res;
}

void
dump_delete (jsp_operand_t res, jsp_operand_t op, bool is_strict, locus loc)
{
  if (op.is_literal_operand ())
  {
    literal_t lit = lit_get_literal_by_cp (op.get_literal ());
    if (lit->get_type () == LIT_STR_T
        || lit->get_type () == LIT_MAGIC_STR_T
        || lit->get_type () == LIT_MAGIC_STR_EX_T)
    {
      jsp_early_error_check_delete (is_strict, loc);

      dump_double_address (VM_OP_DELETE_VAR, res, op);
    }
    else if (lit->get_type ()  == LIT_NUMBER_T)
    {
      dump_boolean_assignment (res, true);
    }
  }
  else
  {
    JERRY_ASSERT (op.is_register_operand ());

    const op_meta last_op_meta = last_dumped_op_meta ();
    switch (last_op_meta.op.op_idx)
    {
      case VM_OP_PROP_GETTER:
      {
        const vm_instr_counter_t oc = (vm_instr_counter_t) (serializer_get_current_instr_counter () - 1);
        serializer_set_writing_position (oc);
        dump_triple_address (VM_OP_DELETE_PROP,
                             res,
                             create_operand_from_tmp_and_lit (last_op_meta.op.data.prop_getter.obj,
                                                              last_op_meta.lit_id[1]),
                             create_operand_from_tmp_and_lit (last_op_meta.op.data.prop_getter.prop,
                                                              last_op_meta.lit_id[2]));
        break;
      }
      default:
      {
        dump_boolean_assignment (res, true);
      }
    }
  }
}

jsp_operand_t
dump_delete_res (jsp_operand_t op, bool is_strict, locus loc)
{
  const jsp_operand_t res = tmp_operand ();
  dump_delete (res, op, is_strict, loc);
  return res;
}

void
dump_typeof (jsp_operand_t res, jsp_operand_t op)
{
  dump_double_address (VM_OP_TYPEOF, res, op);
}

jsp_operand_t
dump_typeof_res (jsp_operand_t op)
{
  const jsp_operand_t res = tmp_operand ();
  dump_typeof (res, op);
  return res;
}

void
dump_multiplication (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_MULTIPLICATION, res, lhs, rhs);
}

jsp_operand_t
dump_multiplication_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_multiplication (res, lhs, rhs);
  return res;
}

void
dump_division (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_DIVISION, res, lhs, rhs);
}

jsp_operand_t
dump_division_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_division (res, lhs, rhs);
  return res;
}

void
dump_remainder (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_REMAINDER, res, lhs, rhs);
}

jsp_operand_t
dump_remainder_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_remainder (res, lhs, rhs);
  return res;
}

void
dump_addition (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_ADDITION, res, lhs, rhs);
}

jsp_operand_t
dump_addition_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_addition (res, lhs, rhs);
  return res;
}

void
dump_substraction (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_SUBSTRACTION, res, lhs, rhs);
}

jsp_operand_t
dump_substraction_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_substraction (res, lhs, rhs);
  return res;
}

void
dump_left_shift (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_B_SHIFT_LEFT, res, lhs, rhs);
}

jsp_operand_t
dump_left_shift_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_left_shift (res, lhs, rhs);
  return res;
}

void
dump_right_shift (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_B_SHIFT_RIGHT, res, lhs, rhs);
}

jsp_operand_t
dump_right_shift_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_right_shift (res, lhs, rhs);
  return res;
}

void
dump_right_shift_ex (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_B_SHIFT_URIGHT, res, lhs, rhs);
}

jsp_operand_t
dump_right_shift_ex_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_right_shift_ex (res, lhs, rhs);
  return res;
}

void
dump_less_than (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_LESS_THAN, res, lhs, rhs);
}

jsp_operand_t
dump_less_than_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_less_than (res, lhs, rhs);
  return res;
}

void
dump_greater_than (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_GREATER_THAN, res, lhs, rhs);
}

jsp_operand_t
dump_greater_than_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_greater_than (res, lhs, rhs);
  return res;
}

void
dump_less_or_equal_than (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_LESS_OR_EQUAL_THAN, res, lhs, rhs);
}

jsp_operand_t
dump_less_or_equal_than_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_less_or_equal_than (res, lhs, rhs);
  return res;
}

void
dump_greater_or_equal_than (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_GREATER_OR_EQUAL_THAN, res, lhs, rhs);
}

jsp_operand_t
dump_greater_or_equal_than_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_greater_or_equal_than (res, lhs, rhs);
  return res;
}

void
dump_instanceof (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_INSTANCEOF, res, lhs, rhs);
}

jsp_operand_t
dump_instanceof_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_instanceof (res, lhs, rhs);
  return res;
}

void
dump_in (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_IN, res, lhs, rhs);
}

jsp_operand_t
dump_in_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_in (res, lhs, rhs);
  return res;
}

void
dump_equal_value (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_EQUAL_VALUE, res, lhs, rhs);
}

jsp_operand_t
dump_equal_value_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_equal_value (res, lhs, rhs);
  return res;
}

void
dump_not_equal_value (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_NOT_EQUAL_VALUE, res, lhs, rhs);
}

jsp_operand_t
dump_not_equal_value_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_not_equal_value (res, lhs, rhs);
  return res;
}

void
dump_equal_value_type (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_EQUAL_VALUE_TYPE, res, lhs, rhs);
}

jsp_operand_t
dump_equal_value_type_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_equal_value_type (res, lhs, rhs);
  return res;
}

void
dump_not_equal_value_type (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_NOT_EQUAL_VALUE_TYPE, res, lhs, rhs);
}

jsp_operand_t
dump_not_equal_value_type_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_not_equal_value_type (res, lhs, rhs);
  return res;
}

void
dump_bitwise_and (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_B_AND, res, lhs, rhs);
}

jsp_operand_t
dump_bitwise_and_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_bitwise_and (res, lhs, rhs);
  return res;
}

void
dump_bitwise_xor (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_B_XOR, res, lhs, rhs);
}

jsp_operand_t
dump_bitwise_xor_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_bitwise_xor (res, lhs, rhs);
  return res;
}

void
dump_bitwise_or (jsp_operand_t res, jsp_operand_t lhs, jsp_operand_t rhs)
{
  dump_triple_address (VM_OP_B_OR, res, lhs, rhs);
}

jsp_operand_t
dump_bitwise_or_res (jsp_operand_t lhs, jsp_operand_t rhs)
{
  const jsp_operand_t res = tmp_operand ();
  dump_bitwise_or (res, lhs, rhs);
  return res;
}

void
start_dumping_logical_and_checks (void)
{
  STACK_PUSH (U8, (uint8_t) STACK_SIZE (logical_and_checks));
}

void
dump_logical_and_check_for_rewrite (jsp_operand_t op)
{
  STACK_PUSH (logical_and_checks, serializer_get_current_instr_counter ());

  dump_triple_address (VM_OP_IS_FALSE_JMP_DOWN,
                       op,
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());
}

void
rewrite_logical_and_checks (void)
{
  for (uint8_t i = STACK_TOP (U8); i < STACK_SIZE (logical_and_checks); i++)
  {
    vm_idx_t id1, id2;
    split_instr_counter (get_diff_from (STACK_ELEMENT (logical_and_checks, i)), &id1, &id2);

    op_meta jmp_op_meta = serializer_get_op_meta (STACK_ELEMENT (logical_and_checks, i));
    JERRY_ASSERT (jmp_op_meta.op.op_idx == VM_OP_IS_FALSE_JMP_DOWN);

    jmp_op_meta.op.data.is_false_jmp_down.oc_idx_1 = id1;
    jmp_op_meta.op.data.is_false_jmp_down.oc_idx_2 = id2;

    serializer_rewrite_op_meta (STACK_ELEMENT (logical_and_checks, i), jmp_op_meta);
  }
  STACK_DROP (logical_and_checks, STACK_SIZE (logical_and_checks) - STACK_TOP (U8));
  STACK_DROP (U8, 1);
}

void
start_dumping_logical_or_checks (void)
{
  STACK_PUSH (U8, (uint8_t) STACK_SIZE (logical_or_checks));
}

void
dump_logical_or_check_for_rewrite (jsp_operand_t op)
{
  STACK_PUSH (logical_or_checks, serializer_get_current_instr_counter ());

  dump_triple_address (VM_OP_IS_TRUE_JMP_DOWN,
                       op,
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());
}

void
rewrite_logical_or_checks (void)
{
  for (uint8_t i = STACK_TOP (U8); i < STACK_SIZE (logical_or_checks); i++)
  {
    vm_idx_t id1, id2;
    split_instr_counter (get_diff_from (STACK_ELEMENT (logical_or_checks, i)), &id1, &id2);

    op_meta jmp_op_meta = serializer_get_op_meta (STACK_ELEMENT (logical_or_checks, i));
    JERRY_ASSERT (jmp_op_meta.op.op_idx == VM_OP_IS_TRUE_JMP_DOWN);

    jmp_op_meta.op.data.is_true_jmp_down.oc_idx_1 = id1;
    jmp_op_meta.op.data.is_true_jmp_down.oc_idx_2 = id2;

    serializer_rewrite_op_meta (STACK_ELEMENT (logical_or_checks, i), jmp_op_meta);
  }
  STACK_DROP (logical_or_checks, STACK_SIZE (logical_or_checks) - STACK_TOP (U8));
  STACK_DROP (U8, 1);
}

void
dump_conditional_check_for_rewrite (jsp_operand_t op)
{
  STACK_PUSH (conditional_checks, serializer_get_current_instr_counter ());

  dump_triple_address (VM_OP_IS_FALSE_JMP_DOWN,
                       op,
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());
}

void
rewrite_conditional_check (void)
{
  vm_idx_t id1, id2;
  split_instr_counter (get_diff_from (STACK_TOP (conditional_checks)), &id1, &id2);

  op_meta jmp_op_meta = serializer_get_op_meta (STACK_TOP (conditional_checks));
  JERRY_ASSERT (jmp_op_meta.op.op_idx == VM_OP_IS_FALSE_JMP_DOWN);

  jmp_op_meta.op.data.is_false_jmp_down.oc_idx_1 = id1;
  jmp_op_meta.op.data.is_false_jmp_down.oc_idx_2 = id2;

  serializer_rewrite_op_meta (STACK_TOP (conditional_checks), jmp_op_meta);

  STACK_DROP (conditional_checks, 1);
}

void
dump_jump_to_end_for_rewrite (void)
{
  STACK_PUSH (jumps_to_end, serializer_get_current_instr_counter ());

  dump_double_address (VM_OP_JMP_DOWN,
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());
}

void
rewrite_jump_to_end (void)
{
  vm_idx_t id1, id2;
  split_instr_counter (get_diff_from (STACK_TOP (jumps_to_end)), &id1, &id2);

  op_meta jmp_op_meta = serializer_get_op_meta (STACK_TOP (jumps_to_end));
  JERRY_ASSERT (jmp_op_meta.op.op_idx == VM_OP_JMP_DOWN);

  jmp_op_meta.op.data.jmp_down.oc_idx_1 = id1;
  jmp_op_meta.op.data.jmp_down.oc_idx_2 = id2;

  serializer_rewrite_op_meta (STACK_TOP (jumps_to_end), jmp_op_meta);

  STACK_DROP (jumps_to_end, 1);
}

void
start_dumping_assignment_expression (jsp_operand_t lhs, locus loc __attr_unused___)
{
  if (lhs.is_register_operand ())
  {
    /*
     * Having left-handside of assignment expression as a temporary register
     * means it's either a member expression or something else. Under the condition,
     * only member expression could be a L-value for assignment expression, otherwise
     * it's an invalid lhs expression.
     */

    const op_meta last = last_dumped_op_meta ();

    if (last.op.op_idx == VM_OP_PROP_GETTER)
    {
      /*
       * If lhs is a member expression, last dumped op code should be
       * prop_getter. For we are going to use the expression as L-value, it will
       * be a prop_setter later, save the op to stack.
       */
      serializer_set_writing_position ((vm_instr_counter_t) (serializer_get_current_instr_counter () - 1));
      STACK_PUSH (prop_getters, last);
    }
    else
    {
      /*
       * If lhs is a temporary register operand but not a member expression, It
       * is an invalid left-hand-side expression.
       */
      PARSE_ERROR (JSP_EARLY_ERROR_REFERENCE, "Invalid left-hand-side expression", loc);
    }
  }
}

jsp_operand_t
dump_prop_setter_or_variable_assignment_res (jsp_operand_t res, jsp_operand_t op)
{
  if (res.is_register_operand ())
  {
    /*
     * Left-hand-side must be a member expression and corresponding prop_getter
     * op is on top of the stack.
     */
    const op_meta last = STACK_TOP (prop_getters);
    JERRY_ASSERT (last.op.op_idx == VM_OP_PROP_GETTER);

    dump_prop_setter_op_meta (last, op);

    STACK_DROP (prop_getters, 1);
  }
  else
  {
    op_meta last = last_dumped_op_meta ();
    if (STACK_SIZE (varg_headers) == 0 /* not in the middle of function call */
        && (last.op.op_idx == VM_OP_ASSIGNMENT
            || last.op.op_idx == VM_OP_ADDITION)
        && is_temp_register (last.op.data.assignment.var_left))
    {
      last.op.data.assignment.var_left = res.get_idx ();
      last.lit_id[0] = res.get_literal ();

      rewrite_last_dumped_op_meta (last);
      op = res;
    }
    else
    {
      dump_variable_assignment (res, op);
    }
  }
  return op;
}

jsp_operand_t
dump_prop_setter_or_addition_res (jsp_operand_t res, jsp_operand_t op)
{
  return dump_prop_setter_or_triple_address_res (VM_OP_ADDITION, res, op);
}

jsp_operand_t
dump_prop_setter_or_multiplication_res (jsp_operand_t res, jsp_operand_t op)
{
  return dump_prop_setter_or_triple_address_res (VM_OP_MULTIPLICATION, res, op);
}

jsp_operand_t
dump_prop_setter_or_division_res (jsp_operand_t res, jsp_operand_t op)
{
  return dump_prop_setter_or_triple_address_res (VM_OP_DIVISION, res, op);
}

jsp_operand_t
dump_prop_setter_or_remainder_res (jsp_operand_t res, jsp_operand_t op)
{
  return dump_prop_setter_or_triple_address_res (VM_OP_REMAINDER, res, op);
}

jsp_operand_t
dump_prop_setter_or_substraction_res (jsp_operand_t res, jsp_operand_t op)
{
  return dump_prop_setter_or_triple_address_res (VM_OP_SUBSTRACTION, res, op);
}

jsp_operand_t
dump_prop_setter_or_left_shift_res (jsp_operand_t res, jsp_operand_t op)
{
  return dump_prop_setter_or_triple_address_res (VM_OP_B_SHIFT_LEFT, res, op);
}

jsp_operand_t
dump_prop_setter_or_right_shift_res (jsp_operand_t res, jsp_operand_t op)
{
  return dump_prop_setter_or_triple_address_res (VM_OP_B_SHIFT_RIGHT, res, op);
}

jsp_operand_t
dump_prop_setter_or_right_shift_ex_res (jsp_operand_t res, jsp_operand_t op)
{
  return dump_prop_setter_or_triple_address_res (VM_OP_B_SHIFT_URIGHT, res, op);
}

jsp_operand_t
dump_prop_setter_or_bitwise_and_res (jsp_operand_t res, jsp_operand_t op)
{
  return dump_prop_setter_or_triple_address_res (VM_OP_B_AND, res, op);
}

jsp_operand_t
dump_prop_setter_or_bitwise_xor_res (jsp_operand_t res, jsp_operand_t op)
{
  return dump_prop_setter_or_triple_address_res (VM_OP_B_XOR, res, op);
}

jsp_operand_t
dump_prop_setter_or_bitwise_or_res (jsp_operand_t res, jsp_operand_t op)
{
  return dump_prop_setter_or_triple_address_res (VM_OP_B_OR, res, op);
}

void
dumper_set_next_interation_target (void)
{
  STACK_PUSH (next_iterations, serializer_get_current_instr_counter ());
}

void
dump_continue_iterations_check (jsp_operand_t op)
{
  const vm_instr_counter_t next_iteration_target_diff = (vm_instr_counter_t) (serializer_get_current_instr_counter ()
                                                                          - STACK_TOP (next_iterations));
  vm_idx_t id1, id2;
  split_instr_counter (next_iteration_target_diff, &id1, &id2);

  if (operand_is_empty (op))
  {
    dump_double_address (VM_OP_JMP_UP,
                         jsp_operand_t::make_idx_const_operand (id1),
                         jsp_operand_t::make_idx_const_operand (id2));
  }
  else
  {
    dump_triple_address (VM_OP_IS_TRUE_JMP_UP,
                         op,
                         jsp_operand_t::make_idx_const_operand (id1),
                         jsp_operand_t::make_idx_const_operand (id2));
  }
  STACK_DROP (next_iterations, 1);
}

/**
 * Dump template of 'jmp_break_continue' or 'jmp_down' instruction (depending on is_simple_jump argument).
 *
 * Note:
 *      the instruction's flags field is written later (see also: rewrite_simple_or_nested_jump_get_next).
 *
 * @return position of dumped instruction
 */
vm_instr_counter_t
dump_simple_or_nested_jump_for_rewrite (bool is_simple_jump, /**< flag indicating, whether simple jump
                                                              *   or 'jmp_break_continue' template should be dumped */
                                        vm_instr_counter_t next_jump_for_tgt_oc) /**< instr counter of next
                                                                                  *   template targetted to
                                                                                  *   the same target - if any,
                                                                                  *   or MAX_OPCODES - otherwise */
{
  vm_idx_t id1, id2;
  split_instr_counter (next_jump_for_tgt_oc, &id1, &id2);

  vm_instr_counter_t ret = serializer_get_current_instr_counter ();

  if (is_simple_jump)
  {
    dump_double_address (VM_OP_JMP_DOWN,
                         jsp_operand_t::make_idx_const_operand (id1),
                         jsp_operand_t::make_idx_const_operand (id2));
  }
  else
  {
    dump_double_address (VM_OP_JMP_BREAK_CONTINUE,
                         jsp_operand_t::make_idx_const_operand (id1),
                         jsp_operand_t::make_idx_const_operand (id2));
  }

  return ret;
} /* dump_simple_or_nested_jump_for_rewrite */

/**
 * Write jump target position into previously dumped template of jump (simple or nested) instruction
 *
 * @return instr counter value that was encoded in the jump before rewrite
 */
vm_instr_counter_t
rewrite_simple_or_nested_jump_and_get_next (vm_instr_counter_t jump_oc, /**< position of jump to rewrite */
                                            vm_instr_counter_t target_oc) /**< the jump's target */
{
  op_meta jump_op_meta = serializer_get_op_meta (jump_oc);

  bool is_simple_jump = (jump_op_meta.op.op_idx == VM_OP_JMP_DOWN);

  JERRY_ASSERT (is_simple_jump
                || (jump_op_meta.op.op_idx == VM_OP_JMP_BREAK_CONTINUE));

  vm_idx_t id1, id2, id1_prev, id2_prev;
  split_instr_counter ((vm_instr_counter_t) (target_oc - jump_oc), &id1, &id2);

  if (is_simple_jump)
  {
    id1_prev = jump_op_meta.op.data.jmp_down.oc_idx_1;
    id2_prev = jump_op_meta.op.data.jmp_down.oc_idx_2;

    jump_op_meta.op.data.jmp_down.oc_idx_1 = id1;
    jump_op_meta.op.data.jmp_down.oc_idx_2 = id2;
  }
  else
  {
    JERRY_ASSERT (jump_op_meta.op.op_idx == VM_OP_JMP_BREAK_CONTINUE);

    id1_prev = jump_op_meta.op.data.jmp_break_continue.oc_idx_1;
    id2_prev = jump_op_meta.op.data.jmp_break_continue.oc_idx_2;

    jump_op_meta.op.data.jmp_break_continue.oc_idx_1 = id1;
    jump_op_meta.op.data.jmp_break_continue.oc_idx_2 = id2;
  }

  serializer_rewrite_op_meta (jump_oc, jump_op_meta);

  return vm_calc_instr_counter_from_idx_idx (id1_prev, id2_prev);
} /* rewrite_simple_or_nested_jump_get_next */

void
start_dumping_case_clauses (void)
{
  STACK_PUSH (U8, (uint8_t) STACK_SIZE (case_clauses));
  STACK_PUSH (U8, (uint8_t) STACK_SIZE (case_clauses));
}

void
dump_case_clause_check_for_rewrite (jsp_operand_t switch_expr, jsp_operand_t case_expr)
{
  const jsp_operand_t res = tmp_operand ();
  dump_triple_address (VM_OP_EQUAL_VALUE_TYPE, res, switch_expr, case_expr);
  STACK_PUSH (case_clauses, serializer_get_current_instr_counter ());
  dump_triple_address (VM_OP_IS_TRUE_JMP_DOWN,
                       res,
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());
}

void
dump_default_clause_check_for_rewrite (void)
{
  STACK_PUSH (case_clauses, serializer_get_current_instr_counter ());

  dump_double_address (VM_OP_JMP_DOWN,
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());
}

void
rewrite_case_clause (void)
{
  const vm_instr_counter_t jmp_oc = STACK_ELEMENT (case_clauses, STACK_HEAD (U8, 2));
  vm_idx_t id1, id2;
  split_instr_counter (get_diff_from (jmp_oc), &id1, &id2);

  op_meta jmp_op_meta = serializer_get_op_meta (jmp_oc);
  JERRY_ASSERT (jmp_op_meta.op.op_idx == VM_OP_IS_TRUE_JMP_DOWN);

  jmp_op_meta.op.data.is_true_jmp_down.oc_idx_1 = id1;
  jmp_op_meta.op.data.is_true_jmp_down.oc_idx_2 = id2;

  serializer_rewrite_op_meta (jmp_oc, jmp_op_meta);

  STACK_INCR_HEAD (U8, 2);
}

void
rewrite_default_clause (void)
{
  const vm_instr_counter_t jmp_oc = STACK_TOP (case_clauses);

  vm_idx_t id1, id2;
  split_instr_counter (get_diff_from (jmp_oc), &id1, &id2);

  op_meta jmp_op_meta = serializer_get_op_meta (jmp_oc);
  JERRY_ASSERT (jmp_op_meta.op.op_idx == VM_OP_JMP_DOWN);

  jmp_op_meta.op.data.jmp_down.oc_idx_1 = id1;
  jmp_op_meta.op.data.jmp_down.oc_idx_2 = id2;

  serializer_rewrite_op_meta (jmp_oc, jmp_op_meta);
}

void
finish_dumping_case_clauses (void)
{
  STACK_DROP (case_clauses, STACK_SIZE (case_clauses) - STACK_TOP (U8));
  STACK_DROP (U8, 1);
  STACK_DROP (U8, 1);
}

/**
 * Dump template of 'with' instruction.
 *
 * Note:
 *      the instruction's flags field is written later (see also: rewrite_with).
 *
 * @return position of dumped instruction
 */
vm_instr_counter_t
dump_with_for_rewrite (jsp_operand_t op) /**< jsp_operand_t - result of evaluating Expression
                                          *   in WithStatement */
{
  vm_instr_counter_t oc = serializer_get_current_instr_counter ();

  dump_triple_address (VM_OP_WITH,
                       op,
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());

  return oc;
} /* dump_with_for_rewrite */

/**
 * Write position of 'with' block's end to specified 'with' instruction template,
 * dumped earlier (see also: dump_with_for_rewrite).
 */
void
rewrite_with (vm_instr_counter_t oc) /**< instr counter of the instruction template */
{
  vm_idx_t id1, id2;
  split_instr_counter (get_diff_from (oc), &id1, &id2);

  op_meta with_op_meta = serializer_get_op_meta (oc);

  with_op_meta.op.data.with.oc_idx_1 = id1;
  with_op_meta.op.data.with.oc_idx_2 = id2;

  serializer_rewrite_op_meta (oc, with_op_meta);
} /* rewrite_with */

/**
 * Dump 'meta' instruction of 'end with' type
 */
void
dump_with_end (void)
{
  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_END_WITH),
                       jsp_operand_t::make_empty_operand (),
                       jsp_operand_t::make_empty_operand ());
} /* dump_with_end */

/**
 * Dump template of 'for_in' instruction.
 *
 * Note:
 *      the instruction's flags field is written later (see also: rewrite_for_in).
 *
 * @return position of dumped instruction
 */
vm_instr_counter_t
dump_for_in_for_rewrite (jsp_operand_t op) /**< jsp_operand_t - result of evaluating Expression
                                            *   in for-in statement */
{
  vm_instr_counter_t oc = serializer_get_current_instr_counter ();

  dump_triple_address (VM_OP_FOR_IN,
                       op,
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());

  return oc;
} /* dump_for_in_for_rewrite */

/**
 * Write position of 'for_in' block's end to specified 'for_in' instruction template,
 * dumped earlier (see also: dump_for_in_for_rewrite).
 */
void
rewrite_for_in (vm_instr_counter_t oc) /**< instr counter of the instruction template */
{
  vm_idx_t id1, id2;
  split_instr_counter (get_diff_from (oc), &id1, &id2);

  op_meta for_in_op_meta = serializer_get_op_meta (oc);

  for_in_op_meta.op.data.for_in.oc_idx_1 = id1;
  for_in_op_meta.op.data.for_in.oc_idx_2 = id2;

  serializer_rewrite_op_meta (oc, for_in_op_meta);
} /* rewrite_for_in */

/**
 * Dump 'meta' instruction of 'end for_in' type
 */
void
dump_for_in_end (void)
{
  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_END_FOR_IN),
                       jsp_operand_t::make_empty_operand (),
                       jsp_operand_t::make_empty_operand ());
} /* dump_for_in_end */

void
dump_try_for_rewrite (void)
{
  STACK_PUSH (tries, serializer_get_current_instr_counter ());

  dump_double_address (VM_OP_TRY_BLOCK,
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());
}

void
rewrite_try (void)
{
  vm_idx_t id1, id2;
  split_instr_counter (get_diff_from (STACK_TOP (tries)), &id1, &id2);

  op_meta try_op_meta = serializer_get_op_meta (STACK_TOP (tries));
  JERRY_ASSERT (try_op_meta.op.op_idx == VM_OP_TRY_BLOCK);

  try_op_meta.op.data.try_block.oc_idx_1 = id1;
  try_op_meta.op.data.try_block.oc_idx_2 = id2;

  serializer_rewrite_op_meta (STACK_TOP (tries), try_op_meta);

  STACK_DROP (tries, 1);
}

void
dump_catch_for_rewrite (jsp_operand_t op)
{
  JERRY_ASSERT (op.is_literal_operand ());
  STACK_PUSH (catches, serializer_get_current_instr_counter ());

  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_CATCH),
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());

  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_CATCH_EXCEPTION_IDENTIFIER),
                       op,
                       jsp_operand_t::make_empty_operand ());
}

void
rewrite_catch (void)
{
  vm_idx_t id1, id2;
  split_instr_counter (get_diff_from (STACK_TOP (catches)), &id1, &id2);

  op_meta catch_op_meta = serializer_get_op_meta (STACK_TOP (catches));
  JERRY_ASSERT (catch_op_meta.op.op_idx == VM_OP_META
                && catch_op_meta.op.data.meta.type == OPCODE_META_TYPE_CATCH);

  catch_op_meta.op.data.meta.data_1 = id1;
  catch_op_meta.op.data.meta.data_2 = id2;

  serializer_rewrite_op_meta (STACK_TOP (catches), catch_op_meta);

  STACK_DROP (catches, 1);
}

void
dump_finally_for_rewrite (void)
{
  STACK_PUSH (finallies, serializer_get_current_instr_counter ());

  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_FINALLY),
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());
}

void
rewrite_finally (void)
{
  vm_idx_t id1, id2;
  split_instr_counter (get_diff_from (STACK_TOP (finallies)), &id1, &id2);

  op_meta finally_op_meta = serializer_get_op_meta (STACK_TOP (finallies));
  JERRY_ASSERT (finally_op_meta.op.op_idx == VM_OP_META
                && finally_op_meta.op.data.meta.type == OPCODE_META_TYPE_FINALLY);

  finally_op_meta.op.data.meta.data_1 = id1;
  finally_op_meta.op.data.meta.data_2 = id2;

  serializer_rewrite_op_meta (STACK_TOP (finallies), finally_op_meta);

  STACK_DROP (finallies, 1);
}

void
dump_end_try_catch_finally (void)
{
  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_END_TRY_CATCH_FINALLY),
                       jsp_operand_t::make_empty_operand (),
                       jsp_operand_t::make_empty_operand ());
}

void
dump_throw (jsp_operand_t op)
{
  dump_single_address (VM_OP_THROW_VALUE, op);
}

/**
 * Dump instruction designating variable declaration
 */
void
dump_variable_declaration (lit_cpointer_t lit_id) /**< literal which holds variable's name */
{
  jsp_operand_t op_var_name = jsp_operand_t::make_lit_operand (lit_id);
  serializer_dump_var_decl (jsp_dmp_create_op_meta (VM_OP_VAR_DECL, &op_var_name, 1));
} /* dump_variable_declaration */

/**
 * Dump template of 'meta' instruction for scope's code flags.
 *
 * Note:
 *      the instruction's flags field is written later (see also: rewrite_scope_code_flags).
 *
 * @return position of dumped instruction
 */
vm_instr_counter_t
dump_scope_code_flags_for_rewrite (void)
{
  vm_instr_counter_t oc = serializer_get_current_instr_counter ();

  dump_triple_address (VM_OP_META,
                       jsp_operand_t::make_idx_const_operand (OPCODE_META_TYPE_SCOPE_CODE_FLAGS),
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_empty_operand ());

  return oc;
} /* dump_scope_code_flags_for_rewrite */

/**
 * Write scope's code flags to specified 'meta' instruction template,
 * dumped earlier (see also: dump_scope_code_flags_for_rewrite).
 */
void
rewrite_scope_code_flags (vm_instr_counter_t scope_code_flags_oc, /**< position of instruction to rewrite */
                          opcode_scope_code_flags_t scope_flags) /**< scope's code properties flags set */
{
  JERRY_ASSERT ((vm_idx_t) scope_flags == scope_flags);

  op_meta opm = serializer_get_op_meta (scope_code_flags_oc);
  JERRY_ASSERT (opm.op.op_idx == VM_OP_META);
  JERRY_ASSERT (opm.op.data.meta.type == OPCODE_META_TYPE_SCOPE_CODE_FLAGS);
  JERRY_ASSERT (opm.op.data.meta.data_1 == VM_IDX_REWRITE_GENERAL_CASE);
  JERRY_ASSERT (opm.op.data.meta.data_2 == VM_IDX_EMPTY);

  opm.op.data.meta.data_1 = (vm_idx_t) scope_flags;
  serializer_rewrite_op_meta (scope_code_flags_oc, opm);
} /* rewrite_scope_code_flags */

void
dump_ret (void)
{
  serializer_dump_op_meta (jsp_dmp_create_op_meta_0 (VM_OP_RET));
}

/**
 * Dump 'reg_var_decl' instruction template
 *
 * @return position of the dumped instruction
 */
vm_instr_counter_t
dump_reg_var_decl_for_rewrite (void)
{
  vm_instr_counter_t oc = serializer_get_current_instr_counter ();

  dump_triple_address (VM_OP_REG_VAR_DECL,
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand (),
                       jsp_operand_t::make_unknown_operand ());

  return oc;
} /* dump_reg_var_decl_for_rewrite */

/**
 * Rewrite 'reg_var_decl' instruction's template with current scope's register counts
 */
void
rewrite_reg_var_decl (vm_instr_counter_t reg_var_decl_oc) /**< position of dumped 'reg_var_decl' template */
{
  op_meta opm = serializer_get_op_meta (reg_var_decl_oc);
  JERRY_ASSERT (opm.op.op_idx == VM_OP_REG_VAR_DECL);

  opm.op.data.reg_var_decl.tmp_regs_num = (vm_idx_t) (jsp_reg_max_for_temps - VM_REG_GENERAL_FIRST + 1);

  if (jsp_reg_max_for_local_var != VM_IDX_EMPTY)
  {
    JERRY_ASSERT (jsp_reg_max_for_local_var >= jsp_reg_max_for_temps);
    opm.op.data.reg_var_decl.local_var_regs_num = (vm_idx_t) (jsp_reg_max_for_local_var - jsp_reg_max_for_temps);

    jsp_reg_max_for_local_var = VM_IDX_EMPTY;
  }
  else
  {
    opm.op.data.reg_var_decl.local_var_regs_num = 0;
  }

  if (jsp_reg_max_for_args != VM_IDX_EMPTY)
  {
    if (jsp_reg_max_for_local_var != VM_IDX_EMPTY)
    {
      JERRY_ASSERT (jsp_reg_max_for_args >= jsp_reg_max_for_local_var);
      opm.op.data.reg_var_decl.arg_regs_num = (vm_idx_t) (jsp_reg_max_for_args - jsp_reg_max_for_local_var);
    }
    else
    {
      JERRY_ASSERT (jsp_reg_max_for_args >= jsp_reg_max_for_temps);
      opm.op.data.reg_var_decl.arg_regs_num = (vm_idx_t) (jsp_reg_max_for_args - jsp_reg_max_for_temps);
    }

    jsp_reg_max_for_args = VM_IDX_EMPTY;
  }
  else
  {
    opm.op.data.reg_var_decl.arg_regs_num = 0;
  }

  serializer_rewrite_op_meta (reg_var_decl_oc, opm);
} /* rewrite_reg_var_decl */

void
dump_retval (jsp_operand_t op)
{
  dump_single_address (VM_OP_RETVAL, op);
}

void
dumper_init (void)
{
  jsp_reg_next = VM_REG_GENERAL_FIRST;
  jsp_reg_max_for_temps = VM_REG_GENERAL_FIRST;
  jsp_reg_max_for_local_var = VM_IDX_EMPTY;
  jsp_reg_max_for_args = VM_IDX_EMPTY;

  STACK_INIT (U8);
  STACK_INIT (varg_headers);
  STACK_INIT (function_ends);
  STACK_INIT (logical_and_checks);
  STACK_INIT (logical_or_checks);
  STACK_INIT (conditional_checks);
  STACK_INIT (jumps_to_end);
  STACK_INIT (prop_getters);
  STACK_INIT (next_iterations);
  STACK_INIT (case_clauses);
  STACK_INIT (catches);
  STACK_INIT (finallies);
  STACK_INIT (tries);
  STACK_INIT (jsp_reg_id_stack);
}

void
dumper_free (void)
{
  STACK_FREE (U8);
  STACK_FREE (varg_headers);
  STACK_FREE (function_ends);
  STACK_FREE (logical_and_checks);
  STACK_FREE (logical_or_checks);
  STACK_FREE (conditional_checks);
  STACK_FREE (jumps_to_end);
  STACK_FREE (prop_getters);
  STACK_FREE (next_iterations);
  STACK_FREE (case_clauses);
  STACK_FREE (catches);
  STACK_FREE (finallies);
  STACK_FREE (tries);
  STACK_FREE (jsp_reg_id_stack);
}
