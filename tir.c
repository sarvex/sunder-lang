// Copyright 2021 The Sunder Project Authors
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "sunder.h"

static struct type*
type_new(char const* name, size_t size, enum type_kind kind)
{
    assert(name != NULL);

    struct type* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->name = name;
    self->size = size;
    self->kind = kind;
    return self;
}

struct type*
type_new_void(void)
{
    return type_new(context()->interned.void_, 0u, TYPE_VOID);
}

struct type*
type_new_bool(void)
{
    return type_new(context()->interned.bool_, 1u, TYPE_BOOL);
}

struct type*
type_new_byte(void)
{
    return type_new(context()->interned.byte, 1u, TYPE_BYTE);
}

struct type*
type_new_u8(void)
{
    struct type* const self = type_new(context()->interned.u8, 1u, TYPE_U8);
    self->data.integer.min = context()->u8_min;
    self->data.integer.max = context()->u8_max;
    return self;
}

struct type*
type_new_s8(void)
{
    struct type* const self = type_new(context()->interned.s8, 1u, TYPE_S8);
    self->data.integer.min = context()->s8_min;
    self->data.integer.max = context()->s8_max;
    return self;
}

struct type*
type_new_u16(void)
{
    struct type* const self = type_new(context()->interned.u16, 2u, TYPE_U16);
    self->data.integer.min = context()->u16_min;
    self->data.integer.max = context()->u16_max;
    return self;
}

struct type*
type_new_s16(void)
{
    struct type* const self = type_new(context()->interned.s16, 2u, TYPE_S16);
    self->data.integer.min = context()->s16_min;
    self->data.integer.max = context()->s16_max;
    return self;
}

struct type*
type_new_u32(void)
{
    struct type* const self = type_new(context()->interned.u32, 4u, TYPE_U32);
    self->data.integer.min = context()->u32_min;
    self->data.integer.max = context()->u32_max;
    return self;
}

struct type*
type_new_s32(void)
{
    struct type* const self = type_new(context()->interned.s32, 4u, TYPE_S32);
    self->data.integer.min = context()->s32_min;
    self->data.integer.max = context()->s32_max;
    return self;
}

struct type*
type_new_u64(void)
{
    struct type* const self = type_new(context()->interned.u64, 8u, TYPE_U64);
    self->data.integer.min = context()->u64_min;
    self->data.integer.max = context()->u64_max;
    return self;
}

struct type*
type_new_s64(void)
{
    struct type* const self = type_new(context()->interned.s64, 8u, TYPE_S64);
    self->data.integer.min = context()->s64_min;
    self->data.integer.max = context()->s64_max;
    return self;
}

struct type*
type_new_usize(void)
{
    struct type* const self =
        type_new(context()->interned.usize, 8u, TYPE_USIZE);
    self->data.integer.min = context()->usize_min;
    self->data.integer.max = context()->usize_max;
    return self;
}

struct type*
type_new_ssize(void)
{
    struct type* const self =
        type_new(context()->interned.ssize, 8u, TYPE_SSIZE);
    self->data.integer.min = context()->ssize_min;
    self->data.integer.max = context()->ssize_max;
    return self;
}

struct type*
type_new_function(
    struct type const* const* parameter_types, struct type const* return_type)
{
    assert(return_type != NULL);

    struct autil_string* const name_string = autil_string_new_cstr("func(");
    if (autil_sbuf_count(parameter_types) != 0) {
        autil_string_append_cstr(name_string, parameter_types[0]->name);
    }
    for (size_t i = 1; i < autil_sbuf_count(parameter_types); ++i) {
        autil_string_append_fmt(name_string, ", %s", parameter_types[i]->name);
    }
    autil_string_append_fmt(name_string, ") %s", return_type->name);
    char const* const name = autil_sipool_intern(
        context()->sipool,
        autil_string_start(name_string),
        autil_string_count(name_string));
    autil_string_del(name_string);

    struct type* const self = type_new(name, 8u, TYPE_FUNCTION);
    self->data.function.parameter_types = parameter_types;
    self->data.function.return_type = return_type;
    return self;
}

struct type*
type_new_pointer(struct type const* base)
{
    assert(base != NULL);

    struct autil_string* const name_string =
        autil_string_new_fmt("*%s", base->name);
    char const* const name = autil_sipool_intern(
        context()->sipool,
        autil_string_start(name_string),
        autil_string_count(name_string));
    autil_string_del(name_string);

    struct type* const self = type_new(name, 8u, TYPE_POINTER);
    self->data.pointer.base = base;
    return self;
}

struct type*
type_new_array(size_t count, struct type const* base)
{
    assert(base != NULL);

    struct autil_string* const name_string =
        autil_string_new_fmt("[%zu]%s", count, base->name);
    char const* const name = autil_sipool_intern(
        context()->sipool,
        autil_string_start(name_string),
        autil_string_count(name_string));
    autil_string_del(name_string);

    size_t const size = count * base->size;
    assert((count == 0 || size / count == base->size) && "array size overflow");

    struct type* const self = type_new(name, size, TYPE_ARRAY);
    self->data.array.count = count;
    self->data.array.base = base;
    return self;
}

struct type*
type_new_slice(struct type const* base)
{
    assert(base != NULL);

    struct autil_string* const name_string =
        autil_string_new_fmt("[]%s", base->name);
    char const* const name = autil_sipool_intern(
        context()->sipool,
        autil_string_start(name_string),
        autil_string_count(name_string));
    autil_string_del(name_string);

    struct type* const self = type_new(name, 8u * 2u, TYPE_SLICE);
    self->data.pointer.base = base;
    return self;
}

struct type const*
type_unique_function(
    struct type const* const* parameter_types, struct type const* return_type)
{
    assert(return_type != NULL);

    struct type* const type = type_new_function(parameter_types, return_type);
    struct symbol const* const existing =
        symbol_table_lookup(context()->global_symbol_table, type->name);
    if (existing != NULL) {
        autil_xalloc(type, AUTIL_XALLOC_FREE);
        return existing->type;
    }

    struct symbol* const symbol =
        symbol_new_type(&context()->builtin.location, type);
    symbol_table_insert(context()->global_symbol_table, symbol->name, symbol);
    autil_freezer_register(context()->freezer, type);
    autil_freezer_register(context()->freezer, symbol);
    return type;
}

struct type const*
type_unique_pointer(struct type const* base)
{
    assert(base != NULL);

    struct type* const type = type_new_pointer(base);
    struct symbol const* const existing =
        symbol_table_lookup(context()->global_symbol_table, type->name);
    if (existing != NULL) {
        autil_xalloc(type, AUTIL_XALLOC_FREE);
        return existing->type;
    }

    struct symbol* const symbol =
        symbol_new_type(&context()->builtin.location, type);
    symbol_table_insert(context()->global_symbol_table, symbol->name, symbol);
    autil_freezer_register(context()->freezer, type);
    autil_freezer_register(context()->freezer, symbol);
    return type;
}

struct type const*
type_unique_array(size_t count, struct type const* base)
{
    assert(base != NULL);

    struct type* const type = type_new_array(count, base);
    struct symbol const* const existing =
        symbol_table_lookup(context()->global_symbol_table, type->name);
    if (existing != NULL) {
        autil_xalloc(type, AUTIL_XALLOC_FREE);
        return existing->type;
    }

    struct symbol* const symbol =
        symbol_new_type(&context()->builtin.location, type);
    symbol_table_insert(context()->global_symbol_table, symbol->name, symbol);
    autil_freezer_register(context()->freezer, type);
    autil_freezer_register(context()->freezer, symbol);
    return type;
}

struct type const*
type_unique_slice(struct type const* base)
{
    assert(base != NULL);

    struct type* const type = type_new_slice(base);
    struct symbol const* const existing =
        symbol_table_lookup(context()->global_symbol_table, type->name);
    if (existing != NULL) {
        autil_xalloc(type, AUTIL_XALLOC_FREE);
        return existing->type;
    }

    struct symbol* const symbol =
        symbol_new_type(&context()->builtin.location, type);
    symbol_table_insert(context()->global_symbol_table, symbol->name, symbol);
    autil_freezer_register(context()->freezer, type);
    autil_freezer_register(context()->freezer, symbol);
    return type;
}

bool
type_is_integer(struct type const* self)
{
    assert(self != NULL);

    enum type_kind const kind = self->kind;
    return kind == TYPE_U8 || kind == TYPE_S8 || kind == TYPE_U16
        || kind == TYPE_S16 || kind == TYPE_U32 || kind == TYPE_S32
        || kind == TYPE_U64 || kind == TYPE_S64 || kind == TYPE_USIZE
        || kind == TYPE_SSIZE;
}

bool
type_is_uinteger(struct type const* self)
{
    assert(self != NULL);

    enum type_kind const kind = self->kind;
    return kind == TYPE_U8 || kind == TYPE_U16 || kind == TYPE_U32
        || kind == TYPE_U64 || kind == TYPE_USIZE;
}

bool
type_is_sinteger(struct type const* self)
{
    assert(self != NULL);

    enum type_kind const kind = self->kind;
    return kind == TYPE_S8 || kind == TYPE_S16 || kind == TYPE_S32
        || kind == TYPE_S64 || kind == TYPE_SSIZE;
}

bool
type_can_compare_equality(struct type const* self)
{
    assert(self != NULL);

    enum type_kind const kind = self->kind;
    return kind == TYPE_BOOL || kind == TYPE_BYTE || type_is_integer(self)
        || kind == TYPE_FUNCTION || kind == TYPE_POINTER;
}

bool
type_can_compare_order(struct type const* self)
{
    assert(self != NULL);

    enum type_kind const kind = self->kind;
    return kind == TYPE_BOOL || kind == TYPE_BYTE || type_is_integer(self)
        || kind == TYPE_POINTER;
}

struct address
address_init_static(char const* name, size_t offset)
{
    assert(name != NULL);

    struct address self = {0};
    self.kind = ADDRESS_STATIC;
    self.data.static_.name = name;
    self.data.static_.offset = offset;
    return self;
}

struct address
address_init_local(int rbp_offset)
{
    struct address self = {0};
    self.kind = ADDRESS_LOCAL;
    self.data.local.rbp_offset = rbp_offset;
    return self;
}

struct address*
address_new(struct address from)
{
    struct address* const self = autil_xalloc(NULL, sizeof(*self));
    *self = from;
    return self;
}

static struct symbol*
symbol_new(
    enum symbol_kind kind,
    struct source_location const* location,
    char const* name,
    struct type const* type,
    struct address const* address,
    struct value const* value)
{
    assert(location != NULL);
    assert(name != NULL);
    assert(kind == SYMBOL_NAMESPACE || type != NULL);
    assert(kind != SYMBOL_TYPE || kind != SYMBOL_NAMESPACE || address == NULL);
    assert(kind != SYMBOL_TYPE || kind != SYMBOL_NAMESPACE || value == NULL);

    struct symbol* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->kind = kind;
    self->location = location;
    self->name = name;
    self->type = type;
    self->address = address;
    self->value = value;
    return self;
}

struct symbol*
symbol_new_type(struct source_location const* location, struct type const* type)
{
    assert(location != NULL);
    assert(type != NULL);

    struct symbol* const self =
        symbol_new(SYMBOL_TYPE, location, type->name, type, NULL, NULL);
    return self;
}

struct symbol*
symbol_new_variable(
    struct source_location const* location,
    char const* name,
    struct type const* type,
    struct address const* address,
    struct value const* value)
{
    assert(location != NULL);
    assert(name != NULL);
    assert(type != NULL);
    assert(address != NULL);
    assert(address->kind != ADDRESS_STATIC || value != NULL);

    struct symbol* const self =
        symbol_new(SYMBOL_VARIABLE, location, name, type, address, value);
    return self;
}

struct symbol*
symbol_new_constant(
    struct source_location const* location,
    char const* name,
    struct type const* type,
    struct address const* address,
    struct value const* value)
{
    assert(location != NULL);
    assert(name != NULL);
    assert(type != NULL);
    assert(address != NULL);
    assert(value != NULL);

    struct symbol* const self =
        symbol_new(SYMBOL_CONSTANT, location, name, type, address, value);
    return self;
}

struct symbol*
symbol_new_function(
    struct source_location const* location,
    char const* name,
    struct type const* type,
    struct address const* address,
    struct value const* value)
{
    assert(location != NULL);
    assert(name != NULL);
    assert(type != NULL);
    assert(address != NULL);
    assert(value != NULL);

    struct symbol* const self =
        symbol_new(SYMBOL_FUNCTION, location, name, type, address, value);
    return self;
}

struct symbol*
symbol_new_namespace(
    struct source_location const* location,
    char const* name,
    struct symbol_table* symbols)
{
    assert(location != NULL);

    struct symbol* const self =
        symbol_new(SYMBOL_NAMESPACE, location, name, NULL, NULL, NULL);
    self->symbols = symbols;
    return self;
}

struct symbol_table*
symbol_table_new(struct symbol_table const* parent)
{
    struct symbol_table* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->parent = parent;
    self->symbols = autil_map_new(
        sizeof(SYMBOL_MAP_KEY_TYPE),
        sizeof(SYMBOL_MAP_VAL_TYPE),
        SYMBOL_MAP_CMP_FUNC);
    return self;
}

void
symbol_table_freeze(struct symbol_table* self, struct autil_freezer* freezer)
{
    assert(self != NULL);
    assert(freezer != NULL);

    autil_freezer_register(freezer, self);
    autil_map_freeze(self->symbols, freezer);
}

void
symbol_table_insert(
    struct symbol_table* self, char const* name, struct symbol const* symbol)
{
    assert(self != NULL);
    assert(symbol != NULL);

    SYMBOL_MAP_KEY_TYPE const key = name;
    struct symbol const* const local = symbol_table_lookup_local(self, key);
    if (local != NULL) {
        fatal(
            symbol->location,
            "redeclaration of `%s` previously declared at [%s:%zu]",
            key,
            local->location->path,
            local->location->line);
    }
    SYMBOL_MAP_VAL_TYPE const val = symbol;
    autil_map_insert(self->symbols, &key, &val, NULL, NULL);
}

struct symbol const*
symbol_table_lookup(struct symbol_table const* self, char const* name)
{
    assert(self != NULL);
    assert(name != NULL);

    struct symbol const* const local = symbol_table_lookup_local(self, name);
    if (local != NULL) {
        return local;
    }
    if (self->parent != NULL) {
        return symbol_table_lookup(self->parent, name);
    }
    return NULL;
}

struct symbol const*
symbol_table_lookup_local(struct symbol_table const* self, char const* name)
{
    assert(self != NULL);
    assert(name != NULL);

    SYMBOL_MAP_VAL_TYPE const* const existing =
        autil_map_lookup_const(self->symbols, &name);
    return existing != NULL ? *existing : NULL;
}

static struct tir_stmt*
tir_stmt_new(struct source_location const* location, enum tir_stmt_kind kind)
{
    struct tir_stmt* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = location;
    self->kind = kind;
    return self;
}

struct tir_stmt*
tir_stmt_new_if(struct tir_conditional const* const* conditionals)
{
    assert(autil_sbuf_count(conditionals) > 0u);

    struct tir_stmt* const self =
        tir_stmt_new(conditionals[0]->location, TIR_STMT_IF);
    self->data.if_.conditionals = conditionals;
    return self;
}

struct tir_stmt*
tir_stmt_new_for_range(
    struct source_location const* location,
    struct symbol const* loop_variable,
    struct tir_expr const* begin,
    struct tir_expr const* end,
    struct tir_block const* body)
{
    assert(location != NULL);
    assert(loop_variable != NULL);
    assert(loop_variable->kind == SYMBOL_VARIABLE);
    assert(loop_variable->type == context()->builtin.usize);
    assert(begin != NULL);
    assert(begin->type == context()->builtin.usize);
    assert(end != NULL);
    assert(end->type == context()->builtin.usize);
    assert(body != NULL);

    struct tir_stmt* const self = tir_stmt_new(location, TIR_STMT_FOR_RANGE);
    self->data.for_range.loop_variable = loop_variable;
    self->data.for_range.begin = begin;
    self->data.for_range.end = end;
    self->data.for_range.body = body;
    return self;
}

struct tir_stmt*
tir_stmt_new_for_expr(
    struct source_location const* location,
    struct tir_expr const* expr,
    struct tir_block const* body)
{
    assert(location != NULL);
    assert(expr != NULL);
    assert(body != NULL);

    struct tir_stmt* const self = tir_stmt_new(location, TIR_STMT_FOR_EXPR);
    self->data.for_expr.expr = expr;
    self->data.for_expr.body = body;
    return self;
}

struct tir_stmt*
tir_stmt_new_dump(
    struct source_location const* location, struct tir_expr const* expr)
{
    assert(location != NULL);
    assert(expr != NULL);

    struct tir_stmt* const self = tir_stmt_new(location, TIR_STMT_DUMP);
    self->data.dump.expr = expr;
    return self;
}

struct tir_stmt*
tir_stmt_new_return(
    struct source_location const* location, struct tir_expr const* expr)
{
    assert(location != NULL);

    struct tir_stmt* const self = tir_stmt_new(location, TIR_STMT_RETURN);
    self->data.return_.expr = expr;
    return self;
}

struct tir_stmt*
tir_stmt_new_assign(
    struct source_location const* location,
    struct tir_expr const* lhs,
    struct tir_expr const* rhs)
{
    assert(location != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);

    struct tir_stmt* const self = tir_stmt_new(location, TIR_STMT_ASSIGN);
    self->data.assign.lhs = lhs;
    self->data.assign.rhs = rhs;
    return self;
}

struct tir_stmt*
tir_stmt_new_expr(
    struct source_location const* location, struct tir_expr const* expr)
{
    assert(expr != NULL);

    struct tir_stmt* const self = tir_stmt_new(location, TIR_STMT_EXPR);
    self->data.expr = expr;
    return self;
}

static struct tir_expr*
tir_expr_new(
    struct source_location const* location,
    struct type const* type,
    enum tir_expr_kind kind)
{
    assert(location != NULL);
    assert(type != NULL);

    struct tir_expr* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = location;
    self->type = type;
    self->kind = kind;
    return self;
}

struct tir_expr*
tir_expr_new_identifier(
    struct source_location const* location, struct symbol const* identifier)
{
    assert(location != NULL);
    assert(identifier != NULL);
    assert(identifier->kind != SYMBOL_TYPE);

    struct tir_expr* const self =
        tir_expr_new(location, identifier->type, TIR_EXPR_IDENTIFIER);
    self->data.identifier = identifier;
    return self;
}

struct tir_expr*
tir_expr_new_boolean(struct source_location const* location, bool value)
{
    assert(location != NULL);

    struct type const* const type = context()->builtin.bool_;
    struct tir_expr* const self =
        tir_expr_new(location, type, TIR_EXPR_BOOLEAN);
    self->data.boolean = value;
    return self;
}

struct tir_expr*
tir_expr_new_integer(
    struct source_location const* location,
    struct type const* type,
    struct autil_bigint const* value)
{
    assert(location != NULL);
    assert(type != NULL);
    assert(type->kind == TYPE_BYTE || type_is_integer(type));
    assert(value != NULL);

    bool const is_byte = type->kind == TYPE_BYTE;
    bool const is_integer = type_is_integer(type);

    if (is_byte && autil_bigint_cmp(value, context()->u8_min) < 0) {
        char* const lit_cstr = autil_bigint_to_new_cstr(value, NULL);
        char* const min_cstr =
            autil_bigint_to_new_cstr(context()->u8_min, NULL);
        fatal(
            location,
            "out-of-range byte literal (%s < %s)",
            lit_cstr,
            min_cstr);
    }
    if (is_byte && autil_bigint_cmp(value, context()->u8_max) > 0) {
        char* const lit_cstr = autil_bigint_to_new_cstr(value, NULL);
        char* const max_cstr =
            autil_bigint_to_new_cstr(context()->u8_max, NULL);
        fatal(
            location,
            "out-of-range byte literal (%s > %s)",
            lit_cstr,
            max_cstr);
    }
    if (is_integer && autil_bigint_cmp(value, type->data.integer.min) < 0) {
        char* const lit_cstr = autil_bigint_to_new_cstr(value, NULL);
        char* const min_cstr =
            autil_bigint_to_new_cstr(type->data.integer.min, NULL);
        fatal(
            location,
            "out-of-range integer literal (%s < %s)",
            lit_cstr,
            min_cstr);
    }
    if (is_integer && autil_bigint_cmp(value, type->data.integer.max) > 0) {
        char* const lit_cstr = autil_bigint_to_new_cstr(value, NULL);
        char* const max_cstr =
            autil_bigint_to_new_cstr(type->data.integer.max, NULL);
        fatal(
            location,
            "out-of-range integer literal (%s > %s)",
            lit_cstr,
            max_cstr);
    }
    struct tir_expr* const self =
        tir_expr_new(location, type, TIR_EXPR_INTEGER);
    self->data.integer = value;
    return self;
}

struct tir_expr*
tir_expr_new_bytes(
    struct source_location const* location,
    struct address const* address,
    size_t count)
{
    assert(location != NULL);
    assert(address != NULL);

    struct type const* const type = type_unique_slice(context()->builtin.byte);
    struct tir_expr* const self = tir_expr_new(location, type, TIR_EXPR_BYTES);
    self->data.bytes.address = address;
    self->data.bytes.count = count;
    return self;
}

struct tir_expr*
tir_expr_new_literal_array(
    struct source_location const* location,
    struct type const* type,
    struct tir_expr const* const* elements)
{
    assert(location != NULL);
    assert(type != NULL);
    assert(type->kind == TYPE_ARRAY);

    struct tir_expr* const self =
        tir_expr_new(location, type, TIR_EXPR_LITERAL_ARRAY);
    self->data.literal_array.elements = elements;
    return self;
}

struct tir_expr*
tir_expr_new_literal_slice(
    struct source_location const* location,
    struct type const* type,
    struct tir_expr const* pointer,
    struct tir_expr const* count)
{
    assert(location != NULL);
    assert(type != NULL);
    assert(type->kind == TYPE_SLICE);
    assert(pointer != NULL);
    assert(count != NULL);

    struct tir_expr* const self =
        tir_expr_new(location, type, TIR_EXPR_LITERAL_SLICE);
    self->data.literal_slice.pointer = pointer;
    self->data.literal_slice.count = count;
    return self;
}

struct tir_expr*
tir_expr_new_cast(
    struct source_location const* location,
    struct type const* type,
    struct tir_expr const* expr)
{
    assert(location != NULL);
    assert(type != NULL);
    assert(expr != NULL);

    struct tir_expr* const self = tir_expr_new(location, type, TIR_EXPR_CAST);
    self->data.cast.expr = expr;
    return self;
}

struct tir_expr*
tir_expr_new_syscall(
    struct source_location const* location,
    struct tir_expr const* const* arguments)
{
    assert(location != NULL);
    assert(arguments != NULL);

    struct tir_expr* const self =
        tir_expr_new(location, context()->builtin.ssize, TIR_EXPR_SYSCALL);
    self->data.syscall.arguments = arguments;
    return self;
}

struct tir_expr*
tir_expr_new_call(
    struct source_location const* location,
    struct tir_expr const* function,
    struct tir_expr const* const* arguments)
{
    assert(location != NULL);
    assert(function != NULL);
    assert(function->type->kind == TYPE_FUNCTION);

    struct type const* const type = function->type->data.function.return_type;
    struct tir_expr* const self = tir_expr_new(location, type, TIR_EXPR_CALL);
    self->data.call.function = function;
    self->data.call.arguments = arguments;
    return self;
}

struct tir_expr*
tir_expr_new_index(
    struct source_location const* location,
    struct tir_expr const* lhs,
    struct tir_expr const* idx)
{
    assert(location != NULL);
    assert(lhs != NULL);
    assert(lhs->type->kind == TYPE_ARRAY || lhs->type->kind == TYPE_SLICE);
    assert(idx != NULL);

    if (lhs->type->kind == TYPE_ARRAY) {
        struct type const* const type = lhs->type->data.array.base;
        struct tir_expr* const self =
            tir_expr_new(location, type, TIR_EXPR_INDEX);
        self->data.index.lhs = lhs;
        self->data.index.idx = idx;
        return self;
    }

    if (lhs->type->kind == TYPE_SLICE) {
        struct type const* const type = lhs->type->data.slice.base;
        struct tir_expr* const self =
            tir_expr_new(location, type, TIR_EXPR_INDEX);
        self->data.index.lhs = lhs;
        self->data.index.idx = idx;
        return self;
    }

    UNREACHABLE();
}

struct tir_expr*
tir_expr_new_slice(
    struct source_location const* location,
    struct tir_expr const* lhs,
    struct tir_expr const* begin,
    struct tir_expr const* end)
{
    assert(location != NULL);
    assert(lhs != NULL);
    assert(lhs->type->kind == TYPE_ARRAY || lhs->type->kind == TYPE_SLICE);
    assert(begin != NULL);
    assert(end != NULL);

    if (lhs->type->kind == TYPE_ARRAY) {
        struct type const* const type =
            type_unique_slice(lhs->type->data.array.base);
        struct tir_expr* const self =
            tir_expr_new(location, type, TIR_EXPR_SLICE);
        self->data.slice.lhs = lhs;
        self->data.slice.begin = begin;
        self->data.slice.end = end;
        return self;
    }

    if (lhs->type->kind == TYPE_SLICE) {
        struct type const* const type =
            type_unique_slice(lhs->type->data.slice.base);
        struct tir_expr* const self =
            tir_expr_new(location, type, TIR_EXPR_SLICE);
        self->data.slice.lhs = lhs;
        self->data.slice.begin = begin;
        self->data.slice.end = end;
        return self;
    }

    UNREACHABLE();
}

struct tir_expr*
tir_expr_new_sizeof(
    struct source_location const* location, struct type const* rhs)
{
    assert(location != NULL);
    assert(rhs != NULL);

    struct tir_expr* const self =
        tir_expr_new(location, context()->builtin.usize, TIR_EXPR_SIZEOF);
    self->data.sizeof_.rhs = rhs;
    return self;
}

struct tir_expr*
tir_expr_new_unary(
    struct source_location const* location,
    struct type const* type,
    enum uop_kind op,
    struct tir_expr const* rhs)
{
    assert(location != NULL);
    assert(type != NULL);
    assert(rhs != NULL);

    struct tir_expr* const self = tir_expr_new(location, type, TIR_EXPR_UNARY);
    self->data.unary.op = op;
    self->data.unary.rhs = rhs;
    return self;
}

struct tir_expr*
tir_expr_new_binary(
    struct source_location const* location,
    struct type const* type,
    enum bop_kind op,
    struct tir_expr const* lhs,
    struct tir_expr const* rhs)
{
    assert(location != NULL);
    assert(type != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);

    struct tir_expr* const self = tir_expr_new(location, type, TIR_EXPR_BINARY);
    self->data.binary.op = op;
    self->data.binary.lhs = lhs;
    self->data.binary.rhs = rhs;
    return self;
}

bool
tir_expr_is_lvalue(struct tir_expr const* self)
{
    assert(self != NULL);

    switch (self->kind) {
    case TIR_EXPR_IDENTIFIER: {
        switch (self->data.identifier->kind) {
        case SYMBOL_TYPE: /* fallthrough */
        case SYMBOL_NAMESPACE:
            UNREACHABLE();
        case SYMBOL_VARIABLE: /* fallthrough */
        case SYMBOL_CONSTANT:
            return true;
        case SYMBOL_FUNCTION:
            return false;
        }
        UNREACHABLE();
    }
    case TIR_EXPR_INDEX: {
        return self->data.index.lhs->type->kind == TYPE_SLICE
            || tir_expr_is_lvalue(self->data.index.lhs);
    }
    case TIR_EXPR_UNARY: {
        return self->data.unary.op == UOP_DEREFERENCE;
    }
    case TIR_EXPR_BOOLEAN: /* fallthrough */
    case TIR_EXPR_INTEGER: /* fallthrough */
    case TIR_EXPR_BYTES: /* fallthrough */
    case TIR_EXPR_LITERAL_ARRAY: /* fallthrough */
    case TIR_EXPR_LITERAL_SLICE: /* fallthrough */
    case TIR_EXPR_CAST: /* fallthrough */
    case TIR_EXPR_SYSCALL: /* fallthrough */
    case TIR_EXPR_CALL: /* fallthrough */
    case TIR_EXPR_SLICE: /* fallthrough */
    case TIR_EXPR_SIZEOF: /* fallthrough */
    case TIR_EXPR_BINARY: {
        return false;
    }
    }

    UNREACHABLE();
    return false;
}

struct tir_function*
tir_function_new(char const* name, struct type const* type)
{
    assert(name != NULL);
    assert(type != NULL);
    assert(type->kind == TYPE_FUNCTION);

    struct tir_function* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->name = name;
    self->type = type;
    return self;
}

struct tir_conditional*
tir_conditional_new(
    struct source_location const* location,
    struct tir_expr const* condition,
    struct tir_block const* body)
{
    struct tir_conditional* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = location;
    self->condition = condition;
    self->body = body;
    return self;
}

struct tir_block*
tir_block_new(
    struct source_location const* location,
    struct symbol_table* symbol_table,
    struct tir_stmt const* const* stmts)
{
    assert(location != NULL);
    assert(symbol_table != NULL);

    struct tir_block* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->location = location;
    self->symbol_table = symbol_table;
    self->stmts = stmts;
    return self;
}

static struct value*
value_new(struct type const* type)
{
    assert(type != NULL);

    struct value* const self = autil_xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->type = type;
    return self;
}

struct value*
value_new_boolean(bool boolean)
{
    struct type const* const type = context()->builtin.bool_;
    struct value* self = value_new(type);
    self->data.boolean = boolean;
    return self;
}

struct value*
value_new_byte(uint8_t byte)
{
    struct type const* const type = context()->builtin.byte;
    struct value* self = value_new(type);
    self->data.byte = byte;
    return self;
}

struct value*
value_new_integer(struct type const* type, struct autil_bigint* integer)
{
    assert(type != NULL);
    assert(type->kind == TYPE_BYTE || type_is_integer(type));
    assert(integer != NULL);
    assert(autil_bigint_cmp(integer, type->data.integer.min) >= 0);
    assert(autil_bigint_cmp(integer, type->data.integer.max) <= 0);

    struct value* self = value_new(type);
    self->data.integer = integer;
    return self;
}

struct value*
value_new_function(struct tir_function const* function)
{
    assert(function != NULL);

    struct value* self = value_new(function->type);
    self->data.function = function;
    return self;
}

struct value*
value_new_pointer(struct type const* type, struct address address)
{
    assert(type != NULL);
    assert(type->kind == TYPE_POINTER);

    struct value* self = value_new(type);
    self->data.pointer = address;
    return self;
}

struct value*
value_new_array(struct type const* type, struct value** elements)
{
    assert(type != NULL);
    assert(type->kind == TYPE_ARRAY);
    assert(type->data.array.count == autil_sbuf_count(elements));

    struct value* self = value_new(type);
    self->data.array.elements = elements;
    return self;
}

struct value*
value_new_slice(
    struct type const* type, struct value* pointer, struct value* count)
{
    assert(type != NULL);
    assert(type->kind == TYPE_SLICE);
    assert(pointer != NULL);
    assert(pointer->type->kind == TYPE_POINTER);
    assert(count != NULL);
    assert(count->type->kind == TYPE_USIZE);
    assert(autil_bigint_cmp(count->data.integer, AUTIL_BIGINT_ZERO) >= 0);

    assert(type->data.slice.base == pointer->type->data.pointer.base);

    struct value* self = value_new(type);
    self->data.slice.pointer = pointer;
    self->data.slice.count = count;
    return self;
}

void
value_del(struct value* self)
{
    assert(self != NULL);

    switch (self->type->kind) {
    case TYPE_VOID: {
        UNREACHABLE();
    }
    case TYPE_BOOL: {
        break;
    }
    case TYPE_BYTE: {
        break;
    }
    case TYPE_U8: /* fallthrough */
    case TYPE_S8: /* fallthrough */
    case TYPE_U16: /* fallthrough */
    case TYPE_S16: /* fallthrough */
    case TYPE_U32: /* fallthrough */
    case TYPE_S32: /* fallthrough */
    case TYPE_U64: /* fallthrough */
    case TYPE_S64: /* fallthrough */
    case TYPE_USIZE: /* fallthrough */
    case TYPE_SSIZE: {
        autil_bigint_del(self->data.integer);
        break;
    }
    case TYPE_FUNCTION: {
        break;
    }
    case TYPE_POINTER: {
        break;
    }
    case TYPE_ARRAY: {
        autil_sbuf(struct value*) const elements = self->data.array.elements;
        for (size_t i = 0; i < autil_sbuf_count(elements); ++i) {
            value_del(elements[i]);
        }
        autil_sbuf_fini(elements);
        break;
    }
    case TYPE_SLICE: {
        value_del(self->data.slice.pointer);
        value_del(self->data.slice.count);
        break;
    }
    }

    memset(self, 0x00, sizeof(*self));
    autil_xalloc(self, AUTIL_XALLOC_FREE);
}

void
value_freeze(struct value* self, struct autil_freezer* freezer)
{
    assert(self != NULL);
    assert(freezer != NULL);

    autil_freezer_register(freezer, self);
    switch (self->type->kind) {
    case TYPE_VOID: {
        UNREACHABLE();
    }
    case TYPE_BOOL: {
        return;
    }
    case TYPE_BYTE: {
        return;
    }
    case TYPE_U8: /* fallthrough */
    case TYPE_S8: /* fallthrough */
    case TYPE_U16: /* fallthrough */
    case TYPE_S16: /* fallthrough */
    case TYPE_U32: /* fallthrough */
    case TYPE_S32: /* fallthrough */
    case TYPE_U64: /* fallthrough */
    case TYPE_S64: /* fallthrough */
    case TYPE_USIZE: /* fallthrough */
    case TYPE_SSIZE: {
        autil_bigint_freeze(self->data.integer, freezer);
        return;
    }
    case TYPE_FUNCTION: {
        return;
    }
    case TYPE_POINTER: {
        return;
    }
    case TYPE_ARRAY: {
        autil_sbuf(struct value*) const elements = self->data.array.elements;
        autil_sbuf_freeze(elements, freezer);
        for (size_t i = 0; i < autil_sbuf_count(elements); ++i) {
            value_freeze(elements[i], freezer);
        }
        return;
    }
    case TYPE_SLICE: {
        value_freeze(self->data.slice.pointer, freezer);
        value_freeze(self->data.slice.count, freezer);
        return;
    }
    }

    UNREACHABLE();
}

struct value*
value_clone(struct value const* self)
{
    assert(self != NULL);

    switch (self->type->kind) {
    case TYPE_VOID: {
        UNREACHABLE();
    }
    case TYPE_BOOL: {
        return value_new_boolean(self->data.boolean);
    }
    case TYPE_BYTE: {
        return value_new_byte(self->data.byte);
    }
    case TYPE_U8: /* fallthrough */
    case TYPE_S8: /* fallthrough */
    case TYPE_U16: /* fallthrough */
    case TYPE_S16: /* fallthrough */
    case TYPE_U32: /* fallthrough */
    case TYPE_S32: /* fallthrough */
    case TYPE_U64: /* fallthrough */
    case TYPE_S64: /* fallthrough */
    case TYPE_USIZE: /* fallthrough */
    case TYPE_SSIZE: {
        return value_new_integer(
            self->type, autil_bigint_new(self->data.integer));
    }
    case TYPE_FUNCTION: {
        return value_new_function(self->data.function);
    }
    case TYPE_POINTER: {
        return value_new_pointer(self->type, self->data.pointer);
    }
    case TYPE_ARRAY: {
        autil_sbuf(struct value*) const elements = self->data.array.elements;
        autil_sbuf(struct value*) cloned_elements = NULL;
        for (size_t i = 0; i < autil_sbuf_count(elements); ++i) {
            autil_sbuf_push(cloned_elements, value_clone(elements[i]));
        }
        return value_new_array(self->type, cloned_elements);
    }
    case TYPE_SLICE: {
        return value_new_slice(
            self->type,
            value_clone(self->data.slice.pointer),
            value_clone(self->data.slice.count));
    }
    }

    UNREACHABLE();
    return NULL;
}

bool
value_eq(struct value const* lhs, struct value const* rhs)
{
    assert(lhs != NULL);
    assert(rhs != NULL);
    assert(lhs->type == rhs->type);

    struct type const* const type = lhs->type; // Arbitrarily use lhs.
    switch (type->kind) {
    case TYPE_VOID: {
        return true;
    }
    case TYPE_BOOL: {
        return lhs->data.boolean == rhs->data.boolean;
    }
    case TYPE_BYTE: {
        return lhs->data.byte == rhs->data.byte;
    }
    case TYPE_U8: /* fallthrough */
    case TYPE_S8: /* fallthrough */
    case TYPE_U16: /* fallthrough */
    case TYPE_S16: /* fallthrough */
    case TYPE_U32: /* fallthrough */
    case TYPE_S32: /* fallthrough */
    case TYPE_U64: /* fallthrough */
    case TYPE_S64: /* fallthrough */
    case TYPE_USIZE: /* fallthrough */
    case TYPE_SSIZE: {
        return autil_bigint_cmp(lhs->data.integer, rhs->data.integer) == 0;
    }
    case TYPE_FUNCTION: {
        return lhs->data.function == rhs->data.function;
    }
    case TYPE_POINTER: {
        // Pointer comparisons are tricky and have many edge cases to think
        // about (dangling pointers, absolute vs stack vs global addressing,
        // etc.). For now the ordering of pointers is undefined during
        // compile-time computations. In the future an easy first pass could
        // include allowing ordering operators on global pointers with the same
        // base address so that comparisons between pointers to elements in the
        // same global array would be allowed.
        UNREACHABLE(); // illegal
    }
    case TYPE_ARRAY: /* fallthrough */
    case TYPE_SLICE: {
        UNREACHABLE(); // illegal
    }
    }

    UNREACHABLE();
    return false;
}

bool
value_lt(struct value const* lhs, struct value const* rhs)
{
    assert(lhs != NULL);
    assert(rhs != NULL);
    assert(lhs->type == rhs->type);

    struct type const* const type = lhs->type; // Arbitrarily use lhs.
    switch (type->kind) {
    case TYPE_VOID: {
        return true;
    }
    case TYPE_BOOL: {
        return lhs->data.boolean < rhs->data.boolean;
    }
    case TYPE_BYTE: {
        return lhs->data.byte < rhs->data.byte;
    }
    case TYPE_U8: /* fallthrough */
    case TYPE_S8: /* fallthrough */
    case TYPE_U16: /* fallthrough */
    case TYPE_S16: /* fallthrough */
    case TYPE_U32: /* fallthrough */
    case TYPE_S32: /* fallthrough */
    case TYPE_U64: /* fallthrough */
    case TYPE_S64: /* fallthrough */
    case TYPE_USIZE: /* fallthrough */
    case TYPE_SSIZE: {
        return autil_bigint_cmp(lhs->data.integer, rhs->data.integer) < 0;
    }
    case TYPE_POINTER: {
        UNREACHABLE(); // illegal (see comment in value_eq)
    }
    case TYPE_FUNCTION: /* fallthrough */
    case TYPE_ARRAY: /* fallthrough */
    case TYPE_SLICE: {
        UNREACHABLE(); // illegal
    }
    }

    UNREACHABLE();
    return false;
}

bool
value_gt(struct value const* lhs, struct value const* rhs)
{
    assert(lhs != NULL);
    assert(rhs != NULL);
    assert(lhs->type == rhs->type);

    struct type const* const type = lhs->type; // Arbitrarily use lhs.
    switch (type->kind) {
    case TYPE_VOID: {
        return true;
    }
    case TYPE_BOOL: {
        return lhs->data.boolean > rhs->data.boolean;
    }
    case TYPE_BYTE: {
        return lhs->data.byte > rhs->data.byte;
    }
    case TYPE_U8: /* fallthrough */
    case TYPE_S8: /* fallthrough */
    case TYPE_U16: /* fallthrough */
    case TYPE_S16: /* fallthrough */
    case TYPE_U32: /* fallthrough */
    case TYPE_S32: /* fallthrough */
    case TYPE_U64: /* fallthrough */
    case TYPE_S64: /* fallthrough */
    case TYPE_USIZE: /* fallthrough */
    case TYPE_SSIZE: {
        return autil_bigint_cmp(lhs->data.integer, rhs->data.integer) > 0;
    }
    case TYPE_POINTER: {
        UNREACHABLE(); // illegal (see comment in value_eq)
    }
    case TYPE_FUNCTION: /* fallthrough */
    case TYPE_ARRAY: /* fallthrough */
    case TYPE_SLICE: {
        UNREACHABLE(); // illegal
    }
    }

    UNREACHABLE();
    return false;
}

uint8_t*
value_to_new_bytes(struct value const* value)
{
    assert(value != NULL);

    autil_sbuf(uint8_t) bytes = NULL;
    autil_sbuf_resize(bytes, value->type->size);
    autil_memset(bytes, 0x00, value->type->size);

    switch (value->type->kind) {
    case TYPE_VOID: {
        assert(autil_sbuf_count(bytes) == 0);
        return bytes;
    }
    case TYPE_BOOL: {
        assert(autil_sbuf_count(bytes) == 1);
        bytes[0] = value->data.boolean;
        return bytes;
    }
    case TYPE_BYTE: {
        assert(autil_sbuf_count(bytes) == 1);
        bytes[0] = value->data.byte;
        return bytes;
    }
    case TYPE_U8: /* fallthrough */
    case TYPE_S8: /* fallthrough */
    case TYPE_U16: /* fallthrough */
    case TYPE_S16: /* fallthrough */
    case TYPE_U32: /* fallthrough */
    case TYPE_S32: /* fallthrough */
    case TYPE_U64: /* fallthrough */
    case TYPE_S64: /* fallthrough */
    case TYPE_USIZE: /* fallthrough */
    case TYPE_SSIZE: {
        // Convert the bigint into a bit array.
        size_t const bit_count = value->type->size * 8u;
        struct autil_bitarr* const bits = autil_bitarr_new(bit_count);
        if (bigint_to_bitarr(bits, value->data.integer)) {
            // Internal compiler error. Integer is out of range.
            UNREACHABLE();
        }

        // Convert the bit array into a byte array via bit shifting and masking.
        for (size_t i = 0; i < bit_count; ++i) {
            uint8_t const mask =
                (uint8_t)(autil_bitarr_get(bits, i) << (i % 8u));
            bytes[i / 8u] |= mask;
        }

        autil_bitarr_del(bits);
        return bytes;
    }
    case TYPE_FUNCTION: {
        // Functions are an abstract concept with an address that is chosen by
        // the assembler/linker. There is no meaningful representation of a
        // function's address at compile time.
        UNREACHABLE();
    }
    case TYPE_POINTER: {
        // The representation of a non-absolute address is chosen by the
        // assembler/linker and has no meaningful representation at
        // compile-time.
        UNREACHABLE();
    }
    case TYPE_ARRAY: {
        autil_sbuf(struct value*) const elements = value->data.array.elements;
        size_t const element_size = value->type->data.array.base->size;
        size_t offset = 0;
        for (size_t i = 0; i < autil_sbuf_count(elements); ++i) {
            autil_sbuf(uint8_t) const element_bytes =
                value_to_new_bytes(elements[i]);
            autil_memmove(bytes + offset, element_bytes, element_size);
            autil_sbuf_fini(element_bytes);
            offset += element_size;
        }
        return bytes;
    }
    case TYPE_SLICE: {
        // The representation of a non-absolute address is chosen by the
        // assembler/linker and has no meaningful representation at
        // compile-time.
        UNREACHABLE();
    }
    }

    UNREACHABLE();
    return NULL;
}
