// Copyright 2021-2022 The Sunder Project Authors
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "sunder.h"

struct incomplete_function {
    struct cst_decl const* decl;
    struct function* function;
    struct symbol_table* symbol_table;
};

struct resolver {
    struct module* module;
    char const* current_static_addr_prefix; // Optional (NULL => no prefix).
    struct function* current_function; // NULL if not in a function.
    struct symbol_table* current_symbol_table;
    struct symbol_table* current_export_table;
    // Current offset of rbp for stack allocated data. Initialized to zero at
    // the start of function completion.
    int current_rbp_offset;
    // True if the statements being processed are inside of a constant
    // declaration. Currently this is only used to tell whether array-slice
    // backing arrays should be declared as variables or constants.
    //
    // TODO: See if there is a better way to determine how we declare the
    // backing array. This feels like a hack, but it might actually be okay.
    bool is_within_const_decl;
    // True if the statements being processed are inside a loop. Set to true
    // when a loop body is being resolved, and set to false once the loop body
    // is finished resolving.
    bool is_within_loop;
    // Current defer evaluated within the current loop. Used to manage defers
    // for break and continue statements.
    struct stmt const* current_loop_defer; // optional (NULL => no defer)

    // Pointer to the head of the current defer statement list node to be
    // evaluated.
    struct stmt const* current_defer; // optional (NULL => no defer)

    // Functions to be completed at the end of the resolve phase after all
    // top-level declarations have been resolved. Incomplete functions defer
    // having their body's resolved so that mutually recursive functions (e.g. f
    // calls g and g calls f) have access to each others' symbols in the global
    // symbol table without requiring one function to be fully defined before
    // the other.
    //
    // NOTE: This member must *NOT* be cached because template function
    // instantiations may resize the stretchy buffer.
    sbuf(struct incomplete_function const*) incomplete_functions;

    // List of symbol tables that need to be frozen after the module has been
    // fully resolved, used for namespaces that may have many symbols added to
    // them over the course of the resolve phase.
    sbuf(struct symbol_table*) chilling_symbol_tables;
};
static struct resolver*
resolver_new(struct module* module);
static void
resolver_del(struct resolver* self);
// Returns true if resolution being performed in the global scope.
static bool
resolver_is_global(struct resolver const* self);
// Reserve static storage space for an object with the provided name.
static struct address const*
resolver_reserve_storage_static(struct resolver* self, char const* name);
// Reserve local storage space space for an object of the provided type.
static struct address const*
resolver_reserve_storage_local(struct resolver* self, struct type const* type);

// Produce the fully qualified name (e.g. prefix::name).
// Providing a NULL prefix parameter implies no prefix.
// Returns the qualified name as an interned string.
static char const* // interned
qualified_name(char const* prefix, char const* name);
// Produce the fully qualified address/elf-symbol (e.g. prefix.name).
// Providing a NULL prefix parameter implies no prefix.
// Returns the qualified address as an interned string.
static char const* // interned
qualified_addr(char const* prefix, char const* name);
// Normalize the provided name with the provided prefix.
// Providing a NULL prefix parameter implies no prefix.
// Providing a zero unique_id parameter implies the symbol is the first and
// potentially only symbol with the given name and should not have the unique
// identifier appended to the normalized symbol (matches gcc behavior for
// multiple local static symbols defined with the same name within the same
// function).
// If the provided name contains template information (e.g. foo[[u64]]) then
// the template information will be discarded (e.g. foo[[u64]] is truncated to
// foo in the example above).
// Returns the normalized name as an interned string.
static char const* // interned
normalize(char const* prefix, char const* name, unsigned unique_id);
// Returns the normalization of the provided name within the provided prefix
// via the normalize function. Linearly increments unique IDs starting at zero
// until a unique ID is found that does not cause a name collision.
static char const* // interned
normalize_unique(char const* prefix, char const* name);
// Add the provided static symbol to the list of static symbols within the
// compilation context.
static void
register_static_symbol(struct symbol const* symbol);

// Finds the symbol or fatally exits. Returns the symbol associated with the
// target concrete syntax tree.
static struct symbol const*
xget_symbol(struct resolver* resolver, struct cst_symbol const* target);
// Finds and/or instantiates the template symbol with the provided template
// arguments or fatally exits. Returns the symbol associated with the
// instantiated type.
static struct symbol const*
xget_template_instance(
    struct resolver* resolver,
    struct source_location const* location,
    struct symbol const* symbol,
    struct cst_typespec const* const* const template_arguments);

static void
check_type_compatibility(
    struct source_location const* location,
    struct type const* actual,
    struct type const* expected);

// Returns a newly created and registered expression node of expr implicitly
// casted to type if such an implicit cast is valid. If expr cannot be
// implicitly casted to type then expr is returned unchanged.
//
// The attempted implicit cast is "shallow" in the sense that it will not
// recursively traverse the expression tree when casting, so currently
// immediate values (literals), casts from `*T` to `*any`, and casts of
// function types with parameter and/or a return type casts  from `*T` to
// `*any` are the only valid expr targets.
//
// This function is intended for use when casting untyped literals to an
// expression that would require a typed literal (e.g. integer->usize), or for
// casting from a typed pointer to a generic pointer (e.g. *foo->*any).
// Sub-expressions with integer literal constants are constant folded during
// the resolve phase, so the expression `123 + 456 * 2` *should* be folded to
// the integer literal constant `615` long before this function would be called
// on it, so for most cases the sequence:
// ```
// struct expr const* expr = resolve_expr(some_cst_expr);
// expr = shallow_implicit_cast(type, expr);
// ```
// will correctly perform a tree-rewrite of the integer constant sub-expression
// `some_cst_expr` casted to `type`.
static struct expr const*
shallow_implicit_cast(struct type const* type, struct expr const* expr);

static void
resolve_import(struct resolver* resolver, struct cst_import const* import);

static struct symbol const*
resolve_decl(struct resolver* resolver, struct cst_decl const* decl);
static struct symbol const*
resolve_decl_variable(
    struct resolver* resolver,
    struct cst_decl const* decl,
    struct expr const** /*optional*/ lhs,
    struct expr const** /*optional*/ rhs);
static struct symbol const*
resolve_decl_constant(struct resolver* resolver, struct cst_decl const* decl);
static struct symbol const*
resolve_decl_function(struct resolver* resolver, struct cst_decl const* decl);
static struct symbol const*
resolve_decl_struct(struct resolver* resolver, struct cst_decl const* decl);
static struct symbol const*
resolve_decl_extend(struct resolver* resolver, struct cst_decl const* decl);
static struct symbol const*
resolve_decl_alias(struct resolver* resolver, struct cst_decl const* decl);
static struct symbol const*
resolve_decl_extern_variable(
    struct resolver* resolver, struct cst_decl const* decl);
static struct symbol const*
resolve_decl_extern_function(
    struct resolver* resolver, struct cst_decl const* decl);

static void
complete_struct(
    struct resolver* resolver,
    struct symbol const* symbol,
    struct cst_decl const* decl);
static void
complete_function(
    struct resolver* resolver, struct incomplete_function const* incomplete);

static struct stmt const* // optional
resolve_stmt(struct resolver* resolver, struct cst_stmt const* stmt);
static struct stmt const* // optional
resolve_stmt_decl(struct resolver* resolver, struct cst_stmt const* stmt);
static struct stmt const*
resolve_stmt_defer(struct resolver* resolver, struct cst_stmt const* stmt);
static struct stmt const*
resolve_stmt_if(struct resolver* resolver, struct cst_stmt const* stmt);
static struct stmt const*
resolve_stmt_for_range(struct resolver* resolver, struct cst_stmt const* stmt);
static struct stmt const*
resolve_stmt_for_expr(struct resolver* resolver, struct cst_stmt const* stmt);
static struct stmt const*
resolve_stmt_break(struct resolver* resolver, struct cst_stmt const* stmt);
static struct stmt const*
resolve_stmt_continue(struct resolver* resolver, struct cst_stmt const* stmt);
static struct stmt const*
resolve_stmt_dump(struct resolver* resolver, struct cst_stmt const* stmt);
static struct stmt const*
resolve_stmt_return(struct resolver* resolver, struct cst_stmt const* stmt);
static struct stmt const*
resolve_stmt_assign(struct resolver* resolver, struct cst_stmt const* stmt);
static struct stmt const*
resolve_stmt_expr(struct resolver* resolver, struct cst_stmt const* stmt);

static struct expr const*
resolve_expr(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_symbol(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_boolean(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_integer(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_character(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_bytes(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_list(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_slice(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_struct(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_cast(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_call(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_access_index(
    struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_access_slice(
    struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_access_member(
    struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_access_dereference(
    struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_sizeof(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_alignof(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_unary(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_unary_logical(
    struct resolver* resolver,
    struct token const* op,
    enum uop_kind uop,
    struct expr const* rhs);
static struct expr const*
resolve_expr_unary_arithmetic(
    struct resolver* resolver,
    struct token const* op,
    enum uop_kind uop,
    struct expr const* rhs);
static struct expr const*
resolve_expr_unary_bitwise(
    struct resolver* resolver,
    struct token const* op,
    enum uop_kind uop,
    struct expr const* rhs);
static struct expr const*
resolve_expr_unary_dereference(
    struct resolver* resolver, struct token const* op, struct expr const* rhs);
static struct expr const*
resolve_expr_unary_addressof(
    struct resolver* resolver, struct token const* op, struct expr const* rhs);
static struct expr const*
resolve_expr_unary_countof(
    struct resolver* resolver, struct token const* op, struct expr const* rhs);
static struct expr const*
resolve_expr_binary(struct resolver* resolver, struct cst_expr const* expr);
static struct expr const*
resolve_expr_binary_logical(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct expr const* lhs,
    struct expr const* rhs);
static struct expr const*
resolve_expr_binary_shift(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct expr const* lhs,
    struct expr const* rhs);
static struct expr const*
resolve_expr_binary_compare_equality(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct expr const* lhs,
    struct expr const* rhs);
static struct expr const*
resolve_expr_binary_compare_order(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct expr const* lhs,
    struct expr const* rhs);
static struct expr const*
resolve_expr_binary_arithmetic(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct expr const* lhs,
    struct expr const* rhs);
static struct expr const*
resolve_expr_binary_bitwise(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct expr const* lhs,
    struct expr const* rhs);

static struct block const*
resolve_block(
    struct resolver* resolver,
    struct symbol_table* symbol_table,
    struct cst_block const* block);

static struct type const*
resolve_typespec(
    struct resolver* resolver, struct cst_typespec const* typespec);
static struct type const*
resolve_typespec_symbol(
    struct resolver* resolver, struct cst_typespec const* typespec);
static struct type const*
resolve_typespec_function(
    struct resolver* resolver, struct cst_typespec const* typespec);
static struct type const*
resolve_typespec_pointer(
    struct resolver* resolver, struct cst_typespec const* typespec);
static struct type const*
resolve_typespec_array(
    struct resolver* resolver, struct cst_typespec const* typespec);
static struct type const*
resolve_typespec_slice(
    struct resolver* resolver, struct cst_typespec const* typespec);
static struct type const*
resolve_typespec_typeof(
    struct resolver* resolver, struct cst_typespec const* typespec);

static struct resolver*
resolver_new(struct module* module)
{
    assert(module != NULL);

    struct resolver* const self = xalloc(NULL, sizeof(*self));
    memset(self, 0x00, sizeof(*self));
    self->module = module;
    self->current_static_addr_prefix = NULL;
    self->current_function = NULL;
    self->current_symbol_table = module->symbols;
    self->current_export_table = module->exports;
    self->current_rbp_offset = 0x0;
    self->is_within_loop = false;
    return self;
}

static void
resolver_del(struct resolver* self)
{
    assert(self != NULL);

    sbuf_fini(self->incomplete_functions);

    memset(self, 0x00, sizeof(*self));
    xalloc(self, XALLOC_FREE);
}

static bool
resolver_is_global(struct resolver const* self)
{
    assert(self != NULL);

    return self->current_function == NULL;
}

static struct address const*
resolver_reserve_storage_static(struct resolver* self, char const* name)
{
    assert(self != NULL);
    assert(name != NULL);

    char const* const name_normalized =
        normalize_unique(self->current_static_addr_prefix, name);
    struct address* const address =
        address_new(address_init_static(name_normalized, 0u));
    freeze(address);
    return address;
}

static struct address const*
resolver_reserve_storage_local(struct resolver* self, struct type const* type)
{
    assert(self != NULL);
    assert(self->current_function != NULL);
    assert(type != NULL);

    self->current_rbp_offset -= (int)ceil8zu(type->size);
    if (self->current_rbp_offset < self->current_function->local_stack_offset) {
        self->current_function->local_stack_offset = self->current_rbp_offset;
    }

    struct address* const address =
        address_new(address_init_local(self->current_rbp_offset));
    freeze(address);
    return address;
}

static char const*
qualified_name(char const* prefix, char const* name)
{
    assert(name != NULL);

    struct string* const s = string_new(NULL, 0);

    // <prefix>::
    if (prefix != NULL) {
        string_append_fmt(s, "%s::", prefix);
    }
    // <prefix>::<name>
    string_append_cstr(s, name);

    char const* const interned = intern(string_start(s), string_count(s));

    string_del(s);
    return interned;
}

static char const*
qualified_addr(char const* prefix, char const* name)
{
    assert(name != NULL);

    struct string* const s = string_new(NULL, 0);

    // <prefix>.
    if (prefix != NULL) {
        string_append_fmt(s, "%s.", prefix);
    }
    // <prefix>.<name>
    string_append_cstr(s, name);

    char const* const interned = intern(string_start(s), string_count(s));

    string_del(s);
    return interned;
}

static char const*
normalize(char const* prefix, char const* name, unsigned unique_id)
{
    assert(name != NULL);

    // Substitute invalid assembly character symbols with replacement
    // characters within the provided name.
    struct string* const name_string = string_new(NULL, 0);
    for (char const* search = name; *search != '\0'; ++search) {
        if (safe_isalnum(*search) || *search == '_') {
            string_append_fmt(name_string, "%c", *search);
            continue;
        }

        // Replace all non valid identifier characters with an underscore.
        string_append_cstr(name_string, "_");
    }
    assert(string_count(name_string) != 0);

    struct string* const s = string_new(NULL, 0);

    // <prefix>.
    if (prefix != NULL) {
        string_append_fmt(s, "%s.", prefix);
    }
    // <prefix>.<name>
    string_append_fmt(
        s, "%.*s", (int)string_count(name_string), string_start(name_string));
    // <prefix>.<name>.<unique-id>
    if (unique_id != 0) {
        string_append_fmt(s, ".%u", unique_id);
    }

    char const* const interned = intern(string_start(s), string_count(s));

    string_del(s);
    string_del(name_string);
    return interned;
}

static char const*
normalize_unique(char const* prefix, char const* name)
{
    assert(name != NULL);

    unsigned unique_id = 0u;
    char const* normalized = normalize(prefix, name, unique_id);
    while (true) {
        bool name_collision = false;
        for (size_t i = 0; i < sbuf_count(context()->static_symbols); ++i) {
            struct symbol const* const symbol = context()->static_symbols[i];
            struct address const* const address = symbol_xget_address(symbol);
            assert(address->kind == ADDRESS_STATIC);
            assert(address->data.static_.offset == 0);

            if (address->data.static_.name == normalized) {
                name_collision = true;
                break;
            }
        }

        if (!name_collision) {
            break; // Found a unique normalized name.
        }

        // Name collision was found. Try a different name with the next
        // sequential unique ID.
        normalized = normalize(prefix, name, ++unique_id);
    }

    return normalized;
}

static void
register_static_symbol(struct symbol const* symbol)
{
    assert(symbol != NULL);
    assert(symbol_xget_address(symbol) != NULL);
    assert(symbol_xget_address(symbol)->kind == ADDRESS_STATIC);

    // Verify that a static symbol with the provided address does not already
    // exists. This should never happen in practice, so this is a sanity check.
    for (size_t i = 0; i < sbuf_count(context()->static_symbols); ++i) {
        bool const exists =
            symbol_xget_address(context()->static_symbols[i])->data.static_.name
            == symbol_xget_address(symbol)->data.static_.name;
        if (exists) {
            fatal(
                symbol->location,
                "[%s] normalized symbol name `%s` already exists",
                __func__,
                symbol_xget_address(symbol)->data.static_.name);
        }
    }

    sbuf_push(context()->static_symbols, symbol);
}

static struct symbol const*
xget_symbol(struct resolver* resolver, struct cst_symbol const* target)
{
    assert(resolver != NULL);
    assert(target != NULL);

    assert(sbuf_count(target->elements) != 0);
    struct cst_symbol_element const* const element = target->elements[0];
    char const* const name = element->identifier->name;
    struct symbol_table const* const symbol_table = target->is_from_root
        ? resolver->module->symbols
        : resolver->current_symbol_table;
    struct symbol const* lhs = symbol_table_lookup(symbol_table, name);
    if (lhs == NULL) {
        fatal(target->location, "use of undeclared identifier `%s`", name);
    }
    if (sbuf_count(element->template_arguments) != 0) {
        lhs = xget_template_instance(
            resolver, element->location, lhs, element->template_arguments);
    }

    // Single symbol element:
    //      foo
    //      foo[[u16]]
    size_t const element_count = sbuf_count(target->elements);
    if (element_count == 1) {
        return lhs;
    }

    // Qualified symbol:
    //      foo::bar
    //      foo::bar[[u16]]
    //      foo::bar[[u16]]::baz
    //      foo::bar[[u16]]::baz::qux[[u32]]
    struct symbol const* symbol = NULL;
    for (size_t i = 1; i < element_count; ++i) {
        struct cst_symbol_element const* const element = target->elements[i];
        char const* const name = element->identifier->name;

        if (lhs->kind == SYMBOL_NAMESPACE) {
            struct symbol_table const* const symbols =
                lhs->data.namespace.symbols;
            symbol = symbol_table_lookup_local(symbols, name);
            if (symbol == NULL) {
                fatal(
                    element->location,
                    "use of undeclared identifier `%s` within `%s`",
                    name,
                    lhs->name);
            }
            if (sbuf_count(element->template_arguments) != 0) {
                symbol = xget_template_instance(
                    resolver,
                    element->location,
                    symbol,
                    element->template_arguments);
            }
            lhs = symbol;
            continue;
        }

        if (lhs->kind == SYMBOL_TYPE) {
            struct type const* const type = symbol_xget_type(lhs);
            symbol = symbol_table_lookup_local(type->symbols, name);
            if (symbol == NULL) {
                fatal(
                    element->location,
                    "use of undeclared identifier `%s` within `%s`",
                    name,
                    lhs->name);
            }
            if (sbuf_count(element->template_arguments) != 0) {
                symbol = xget_template_instance(
                    resolver,
                    element->location,
                    symbol,
                    element->template_arguments);
            }
            lhs = symbol;
            continue;
        }

        if (lhs->kind == SYMBOL_TEMPLATE) {
            if (sbuf_count(element->template_arguments) == 0) {
                fatal(
                    element->location,
                    "template `%s` must be instantiated",
                    lhs->name);
            }
            lhs = xget_template_instance(
                resolver, element->location, lhs, element->template_arguments);
            continue;
        }

        fatal(element->location, "`%s` is not a namespace or type", lhs->name);
    }

    assert(symbol != NULL);
    return symbol;
}

static struct symbol const*
xget_template_instance(
    struct resolver* resolver,
    struct source_location const* location,
    struct symbol const* symbol,
    struct cst_typespec const* const* const template_arguments)
{
    assert(resolver != NULL);
    assert(location != NULL);
    assert(symbol != NULL);

    switch (symbol->kind) {
    case SYMBOL_TYPE: {
        fatal(
            location,
            "attempted template instantiation of non-template type `%s`",
            symbol->name);
    }
    case SYMBOL_VARIABLE: {
        fatal(
            location,
            "attempted template instantiation of variable `%s`",
            symbol->name);
    }
    case SYMBOL_CONSTANT: {
        fatal(
            location,
            "attempted template instantiation of constant `%s`",
            symbol->name);
    }
    case SYMBOL_FUNCTION: {
        fatal(
            location,
            "attempted template instantiation of function `%s`",
            symbol->name);
    }
    case SYMBOL_TEMPLATE: {
        break;
    }
    case SYMBOL_NAMESPACE: {
        fatal(
            location,
            "attempted template instantiation of namespace `%s`",
            symbol->name);
    }
    }

    // Here we *know* that this should be a template instantiation, because
    // parsing a template list as `[[]]` will produce a parse error with the
    // message "template argument list contains zero template arguments".
    if (sbuf_count(template_arguments) == 0) {
        fatal(
            location,
            "template instantiation of `%s` requires a template argument list",
            symbol->name);
    }

    // To instantiate the function template we replace the template parameters
    // of the template declaration with the template arguments from the current
    // instantiation.
    struct cst_decl const* const decl = symbol->data.template.decl;
    assert(decl != NULL);

    // Currently, functions and structs are the only declarations that can be
    // templated, so the rest off this function will only cater to these cases.
    //
    // TODO: Go over the common code in these two condition branches and remove
    // the duplicate and/or redundant logic.
    assert(decl->kind == CST_DECL_FUNCTION || decl->kind == CST_DECL_STRUCT);
    if (decl->kind == CST_DECL_FUNCTION) {
        sbuf(struct cst_identifier const* const) const template_parameters =
            decl->data.function.template_parameters;
        size_t const template_parameters_count =
            sbuf_count(template_parameters);
        size_t const template_arguments_count = sbuf_count(template_arguments);

        if (template_parameters_count != template_arguments_count) {
            fatal(
                location,
                "expected %zu template argument(s) for template `%s` (received %zu)",
                template_parameters_count,
                symbol->name,
                template_arguments_count);
        }

        sbuf(struct type const*) template_types = NULL;
        for (size_t i = 0; i < template_arguments_count; ++i) {
            sbuf_push(
                template_types,
                resolve_typespec(resolver, template_arguments[i]));
        }
        sbuf_freeze(template_types);

        // Replace function identifier (i.e. name).
        struct string* const name_string = string_new_cstr(symbol->name);
        string_append_cstr(name_string, "[[");
        for (size_t i = 0; i < template_arguments_count; ++i) {
            if (i != 0) {
                string_append_cstr(name_string, ", ");
            }
            string_append_fmt(name_string, "%s", template_types[i]->name);
        }
        string_append_cstr(name_string, "]]");
        char const* const name_interned =
            intern_cstr(string_start(name_string));
        string_del(name_string);
        struct cst_identifier* const instance_identifier =
            cst_identifier_new(location, name_interned);
        freeze(instance_identifier);
        // Replace template parameters. Zero template parameters means this
        // function is no longer a template.
        sbuf(struct cst_identifier const* const)
            const instance_template_parameters = NULL;
        // Function parameters do not change. When the actual function is
        // resolved it will do so inside a symbol table where a template
        // parameter's name maps to the template instance's chosen type symbol.
        sbuf(struct cst_function_parameter const* const)
            const instance_function_parameters =
                decl->data.function.function_parameters;
        // Same goes for the return type specification.
        struct cst_typespec const* const instance_return_typespec =
            decl->data.function.return_typespec;
        // And the body is also unchanged.
        struct cst_block const* const instance_body = decl->data.function.body;

        // Check if a symbol corresponding to these template arguments has
        // already been created. If so then we reuse the cached symbol.
        struct symbol const* const existing_instance =
            symbol_table_lookup(symbol->data.template.symbols, name_interned);
        if (existing_instance != NULL) {
            return existing_instance;
        }

        // Create a symbol table to hold the template arguments for this
        // instance. Then add each template argument type to the symbol table,
        // mapping from the template type name to the argument type.
        struct symbol_table* const instance_symbol_table =
            symbol_table_new(symbol->data.template.parent_symbol_table);
        for (size_t i = 0; i < template_parameters_count; ++i) {
            // TODO: Should we use the template parameter location or the
            // template argument location for the type symbol location?
            struct symbol* const symbol = symbol_new_type(
                template_parameters[i]->location, template_types[i]);
            freeze(symbol);
            symbol_table_insert(
                instance_symbol_table,
                template_parameters[i]->name,
                symbol,
                false);
        }
        // Store the template function itself in addition to the template
        // arguments so that self referential functions (e.g. fibonacci) do not
        // have to fully qualify the function name.
        symbol_table_insert(instance_symbol_table, symbol->name, symbol, false);
        symbol_table_freeze(instance_symbol_table);

        // Generate the template instance concrete syntax tree.
        struct cst_decl* const instance_decl = cst_decl_new_function(
            location,
            instance_identifier,
            instance_template_parameters,
            instance_function_parameters,
            instance_return_typespec,
            instance_body);
        freeze(instance_decl);

        // Resolve the actual template instance.
        char const* const save_static_addr_prefix =
            resolver->current_static_addr_prefix;
        struct symbol_table* const save_symbol_table =
            resolver->current_symbol_table;

        resolver->current_static_addr_prefix =
            symbol->data.template.symbol_addr_prefix;
        resolver->current_symbol_table = instance_symbol_table;
        struct symbol const* resolved_symbol =
            resolve_decl_function(resolver, instance_decl);

        resolver->current_static_addr_prefix = save_static_addr_prefix;
        resolver->current_symbol_table = save_symbol_table;

        // Add the unique instance to the cache of instances for the template.
        assert(resolved_symbol->kind == SYMBOL_FUNCTION);
        symbol_table_insert(
            symbol->data.template.symbols,
            name_interned,
            resolved_symbol,
            false);

        return resolved_symbol;
    }
    if (decl->kind == CST_DECL_STRUCT) {
        sbuf(struct cst_identifier const* const) const template_parameters =
            decl->data.struct_.template_parameters;
        size_t const template_parameters_count =
            sbuf_count(template_parameters);
        size_t const template_arguments_count = sbuf_count(template_arguments);

        if (template_parameters_count != template_arguments_count) {
            fatal(
                location,
                "expected %zu template argument(s) for template `%s` (received %zu)",
                template_parameters_count,
                symbol->name,
                template_arguments_count);
        }

        sbuf(struct type const*) template_types = NULL;
        for (size_t i = 0; i < template_arguments_count; ++i) {
            sbuf_push(
                template_types,
                resolve_typespec(resolver, template_arguments[i]));
        }
        sbuf_freeze(template_types);

        // Replace struct identifier (i.e. name).
        struct string* const name_string = string_new_cstr(symbol->name);
        string_append_cstr(name_string, "[[");
        for (size_t i = 0; i < template_arguments_count; ++i) {
            if (i != 0) {
                string_append_cstr(name_string, ", ");
            }
            string_append_fmt(name_string, "%s", template_types[i]->name);
        }
        string_append_cstr(name_string, "]]");
        char const* const name_interned =
            intern_cstr(string_start(name_string));
        string_del(name_string);
        struct cst_identifier* const instance_identifier =
            cst_identifier_new(location, name_interned);
        freeze(instance_identifier);
        // Replace template parameters. Zero template parameters means this
        // struct is no longer a template.
        sbuf(struct cst_identifier const* const)
            const instance_template_parameters = NULL;
        // Struct members do not change. When the actual struct is resolved it
        // will do so inside a symbol table where a template parameter's name
        // maps to the template instance's chosen type symbol.
        sbuf(struct cst_member const* const) const instance_members =
            decl->data.struct_.members;

        // Check if a symbol corresponding to these template arguments has
        // already been created. If so then we reuse the cached symbol.
        struct symbol const* const existing_instance =
            symbol_table_lookup(symbol->data.template.symbols, name_interned);
        if (existing_instance != NULL) {
            return existing_instance;
        }

        // Create a symbol table to hold the template arguments for this
        // instance. Then add each template argument type to the symbol table,
        // mapping from the template type name to the argument type.
        struct symbol_table* const instance_symbol_table =
            symbol_table_new(symbol->data.template.parent_symbol_table);
        for (size_t i = 0; i < template_parameters_count; ++i) {
            // TODO: Should we use the template parameter location or the
            // template argument location for the type symbol location?
            struct symbol* const symbol = symbol_new_type(
                template_parameters[i]->location, template_types[i]);
            freeze(symbol);
            symbol_table_insert(
                instance_symbol_table,
                template_parameters[i]->name,
                symbol,
                false);
        }
        // Store the template struct itself in addition to the template
        // arguments so that self referential structs (e.g. return values of
        // init functions) do not have to fully qualify the struct type.
        symbol_table_insert(instance_symbol_table, symbol->name, symbol, false);
        symbol_table_freeze(instance_symbol_table);

        // Generate the template instance concrete syntax tree.
        struct cst_decl* const instance_decl = cst_decl_new_struct(
            location,
            instance_identifier,
            instance_template_parameters,
            instance_members);
        freeze(instance_decl);

        // Resolve the actual template instance.
        char const* const save_static_addr_prefix =
            resolver->current_static_addr_prefix;
        struct symbol_table* const save_symbol_table =
            resolver->current_symbol_table;

        resolver->current_static_addr_prefix =
            symbol->data.template.symbol_addr_prefix;
        resolver->current_symbol_table = instance_symbol_table;
        struct symbol const* resolved_symbol =
            resolve_decl_struct(resolver, instance_decl);

        resolver->current_static_addr_prefix = save_static_addr_prefix;
        resolver->current_symbol_table = save_symbol_table;

        // Add the unique instance to the cache of instances for the template.
        assert(resolved_symbol->kind == SYMBOL_TYPE);
        symbol_table_insert(
            symbol->data.template.symbols,
            name_interned,
            resolved_symbol,
            false);

        // Now that the instance is in the cache we can complete the struct. If
        // we did not add the instance to the cache first then any self
        // referential template instances would cause instance resolution to
        // enter an infinite loop.
        complete_struct(resolver, resolved_symbol, instance_decl);

        return resolved_symbol;
    }

    UNREACHABLE();
    return NULL;
}

static void
check_type_compatibility(
    struct source_location const* location,
    struct type const* actual,
    struct type const* expected)
{
    if (actual != expected) {
        fatal(
            location,
            "incompatible type `%s` (expected `%s`)",
            actual->name,
            expected->name);
    }
}

static struct expr const*
shallow_implicit_cast(struct type const* type, struct expr const* expr)
{
    assert(type != NULL);
    assert(expr != NULL);

    // FROM type TO type (same type).
    if (type == expr->type) {
        return expr;
    }

    // FROM untyped integer TO byte.
    if (type->kind == TYPE_BYTE && expr->type->kind == TYPE_INTEGER) {
        assert(expr->kind == EXPR_INTEGER);
        struct bigint const* const min = context()->u8_min;
        struct bigint const* const max = context()->u8_max;

        if (bigint_cmp(expr->data.integer, min) < 0) {
            fatal(
                expr->location,
                "out-of-range conversion from `%s` to `%s` (%s < %s)",
                expr->type->name,
                type->name,
                bigint_to_new_cstr(expr->data.integer, NULL),
                bigint_to_new_cstr(min, NULL));
        }
        if (bigint_cmp(expr->data.integer, max) > 0) {
            fatal(
                expr->location,
                "out-of-range conversion from `%s` to `%s` (%s > %s)",
                expr->type->name,
                type->name,
                bigint_to_new_cstr(expr->data.integer, NULL),
                bigint_to_new_cstr(max, NULL));
        }

        struct expr* const result =
            expr_new_integer(expr->location, type, expr->data.integer);

        freeze(result);
        return result;
    }

    // FROM untyped integer TO typed integer.
    if (type_is_any_integer(type) && type->kind != TYPE_INTEGER
        && expr->type->kind == TYPE_INTEGER) {
        assert(expr->kind == EXPR_INTEGER);
        assert(type->data.integer.min != NULL);
        assert(type->data.integer.max != NULL);
        struct bigint const* const min = type->data.integer.min;
        struct bigint const* const max = type->data.integer.max;

        if (bigint_cmp(expr->data.integer, min) < 0) {
            fatal(
                expr->location,
                "out-of-range conversion from `%s` to `%s` (%s < %s)",
                expr->type->name,
                type->name,
                bigint_to_new_cstr(expr->data.integer, NULL),
                bigint_to_new_cstr(min, NULL));
        }
        if (bigint_cmp(expr->data.integer, max) > 0) {
            fatal(
                expr->location,
                "out-of-range conversion from `%s` to `%s` (%s > %s)",
                expr->type->name,
                type->name,
                bigint_to_new_cstr(expr->data.integer, NULL),
                bigint_to_new_cstr(max, NULL));
        }

        struct expr* const result =
            expr_new_integer(expr->location, type, expr->data.integer);

        freeze(result);
        return result;
    }

    // FROM non-any pointer TO any pointer.
    if (type->kind == TYPE_POINTER && type->data.pointer.base->kind == TYPE_ANY
        && expr->type->kind == TYPE_POINTER
        && expr->type->data.pointer.base->kind != TYPE_ANY) {
        struct expr* const result = expr_new_cast(
            expr->location, type_unique_pointer(context()->builtin.any), expr);

        freeze(result);
        return result;
    }

    // FROM function with typed pointers TO function with any pointers.
    if (type->kind == TYPE_FUNCTION && expr->type->kind == TYPE_FUNCTION) {
        struct type const* const from = expr->type;
        if (sbuf_count(type->data.function.parameter_types)
            != sbuf_count(from->data.function.parameter_types)) {
            // Mismatched parameter count. Cannot make an implicit conversion.
        }

        size_t const param_count =
            sbuf_count(type->data.function.parameter_types);
        for (size_t i = 0; i < param_count; ++i) {
            struct type const* const type_param =
                type->data.function.parameter_types[i];
            struct type const* const from_param =
                from->data.function.parameter_types[i];

            bool const same = type_param == from_param;
            bool const non_any_ptr_to_any_ptr = type_param->kind == TYPE_POINTER
                && type_param->data.pointer.base->kind == TYPE_ANY
                && from_param->kind == TYPE_POINTER
                && from_param->data.pointer.base->kind != TYPE_ANY;
            if (!same && !non_any_ptr_to_any_ptr) {
                // Invalid implicit parameter cast.
                return expr;
            }
        }

        struct type const* const type_return = type->data.function.return_type;
        struct type const* const from_return = from->data.function.return_type;
        bool const same = type_return == from_return;
        bool const non_any_ptr_to_any_ptr = type_return->kind == TYPE_POINTER
            && type_return->data.pointer.base->kind == TYPE_ANY
            && from_return->kind == TYPE_POINTER
            && from_return->data.pointer.base->kind != TYPE_ANY;
        if (!same && !non_any_ptr_to_any_ptr) {
            // Invalid implicit return type cast.
            return expr;
        }

        struct expr* const result = expr_new_cast(expr->location, type, expr);

        freeze(result);
        return result;
    }

    // No implicit cast could be performed.
    return expr;
}

static void
merge_symbol_table(
    struct resolver* resolver,
    struct symbol_table* self,
    struct symbol_table const* othr)
{
    assert(resolver != NULL);
    assert(self != NULL);
    assert(othr != NULL);

    for (size_t i = 0; i < sbuf_count(othr->elements); ++i) {
        char const* const name = othr->elements[i].name;
        struct symbol const* const symbol = othr->elements[i].symbol;

        if (symbol->kind == SYMBOL_NAMESPACE) {
            // Add all symbols from the namespace in the other symbol table to
            // the namespace in self's symbol table.
            struct symbol const* existing =
                symbol_table_lookup_local(self, name);
            if (existing == NULL) {
                // There is currently no symbol associated for the namespace in
                // self. Create a new namespace symbol for this purpose and
                // perform the merge.
                struct symbol_table* const table = symbol_table_new(self);
                sbuf_push(context()->chilling_symbol_tables, table);

                struct symbol* const namespace =
                    symbol_new_namespace(symbol->location, symbol->name, table);
                freeze(namespace);
                symbol_table_insert(self, name, namespace, false);
                existing = namespace;
            }

            if (existing->kind != SYMBOL_NAMESPACE) {
                // Actual name collision! Attempt to insert the symbol from the
                // other symbol table into self so that a redeclaration error
                // is generated.
                symbol_table_insert(self, name, symbol, false);
            }

            merge_symbol_table(
                resolver,
                existing->data.namespace.symbols,
                symbol->data.namespace.symbols);
            continue;
        }

        // Add the symbol if it has not been added by a previous import.
        // Perform a pointer inequality comparison so that symbols with the
        // same name that do not refer to the same symbol definition cause a
        // redeclaration error.
        struct symbol const* const existing =
            symbol_table_lookup_local(self, name);
        if (existing == NULL || existing != symbol) {
            symbol_table_insert(self, name, symbol, false);
        }
    }
}

// Returns the canonical representation of the provided import path or NULL.
static char const* // interned
canonical_import_path(char const* module_path, char const* import_path)
{
    assert(module_path != NULL);
    assert(import_path != NULL);

    char const* result = NULL;

    // Path relative to the current module.
    char const* const module_dir = directory_path(module_path);
    struct string* const tmp = string_new_fmt("%s/%s", module_dir, import_path);
    if (file_exists(string_start(tmp))) {
        result = canonical_path(string_start(tmp));
        string_del(tmp);
        return result;
    }

    // Path relative to environment-defined import path-list.
    char const* SUNDER_IMPORT_PATH = getenv("SUNDER_IMPORT_PATH");
    if (SUNDER_IMPORT_PATH == NULL) {
        string_del(tmp);
        return NULL;
    }
    string_resize(tmp, 0u);
    string_append_cstr(tmp, SUNDER_IMPORT_PATH);

    sbuf(struct string*) const imp =
        string_split_on(tmp, ":", STR_LITERAL_COUNT(":"));
    for (size_t i = 0; i < sbuf_count(imp); ++i) {
        struct string* const s = imp[i];

        string_resize(tmp, 0u);
        string_append_fmt(tmp, "%s/%s", string_start(s), import_path);

        if (!file_exists(string_start(tmp))) {
            continue;
        }

        result = canonical_path(string_start(tmp));
        break; // Found the module.
    }

    string_del(tmp);
    for (size_t i = 0; i < sbuf_count(imp); ++i) {
        string_del(imp[i]);
    }
    sbuf_fini(imp);
    return result;
}

static void
resolve_import_file(
    struct resolver* resolver,
    struct source_location const* location,
    char const* file_name,
    bool from_directory)
{
    assert(resolver != NULL);
    assert(file_name != NULL);

    char const* const path =
        canonical_import_path(resolver->module->path, file_name);
    if (path == NULL) {
        fatal(location, "failed to resolve import `%s`", file_name);
    }

    if (file_is_directory(path)) {
        sbuf(char const*) dir_contents = directory_files(path);
        for (size_t i = 0; i < sbuf_count(dir_contents); ++i) {
            struct string* const string =
                string_new_fmt("%s/%s", file_name, dir_contents[i]);
            char const* const interned = intern_cstr(string_start(string));
            string_del(string);
            resolve_import_file(resolver, location, interned, true);
        }
        sbuf_fini(dir_contents);
        return;
    }

    if (!cstr_ends_with(file_name, ".sunder") && from_directory) {
        // Ignore files imported via a directory import if they do not end in a
        // `.sunder` extension. This will allow directories containing
        // non-sunder files to be imported without the compiler producing an
        // error from trying to load something like `.txt` file as a sunder
        // module.
        return;
    }

    struct module const* module = lookup_module(path);
    if (module == NULL) {
        module = load_module(file_name, path);
    }
    if (!module->loaded) {
        fatal(location, "circular dependency when importing `%s`", file_name);
    }
    merge_symbol_table(resolver, resolver->module->symbols, module->exports);
}

static void
resolve_import(struct resolver* resolver, struct cst_import const* import)
{
    assert(resolver != NULL);
    assert(import != NULL);

    resolve_import_file(resolver, import->location, import->path, false);
}

static struct symbol const*
resolve_decl(struct resolver* resolver, struct cst_decl const* decl)
{
    assert(resolver != NULL);
    assert(decl != NULL);

    switch (decl->kind) {
    case CST_DECL_VARIABLE: {
        return resolve_decl_variable(resolver, decl, NULL, NULL);
    }
    case CST_DECL_CONSTANT: {
        return resolve_decl_constant(resolver, decl);
    }
    case CST_DECL_FUNCTION: {
        return resolve_decl_function(resolver, decl);
    }
    case CST_DECL_STRUCT: {
        // Should have already been resolved in the initial pre-declaration of
        // all top-level structs.
        UNREACHABLE();
    }
    case CST_DECL_EXTEND: {
        return resolve_decl_extend(resolver, decl);
    }
    case CST_DECL_ALIAS: {
        return resolve_decl_alias(resolver, decl);
    }
    case CST_DECL_EXTERN_VARIABLE: {
        return resolve_decl_extern_variable(resolver, decl);
    }
    case CST_DECL_EXTERN_FUNCTION: {
        return resolve_decl_extern_function(resolver, decl);
    }
    }

    UNREACHABLE();
    return NULL;
}

static struct symbol const*
resolve_decl_variable(
    struct resolver* resolver,
    struct cst_decl const* decl,
    struct expr const** lhs,
    struct expr const** rhs)
{
    assert(resolver != NULL);
    assert(decl != NULL);
    assert(decl->kind == CST_DECL_VARIABLE);

    struct expr const* expr = resolve_expr(resolver, decl->data.variable.expr);

    struct type const* const type = decl->data.variable.typespec != NULL
        ? resolve_typespec(resolver, decl->data.variable.typespec)
        : expr->type;
    if (type->size == SIZEOF_UNSIZED) {
        fatal(
            decl->location,
            "declaration of variable with unsized type `%s`",
            type->name);
    }

    expr = shallow_implicit_cast(type, expr);
    check_type_compatibility(expr->location, expr->type, type);

    // Global/static variables have their initial values computed at
    // compile-time, but local/non-static variables have their value
    // calculated/assigned at runtime when the value is placed on the stack.
    bool const is_static = resolver_is_global(resolver);
    struct value* value = NULL;
    if (is_static) {
        value = eval_rvalue(expr);
        value_freeze(value);
    }

    struct address const* const address = is_static
        ? resolver_reserve_storage_static(resolver, decl->name)
        : resolver_reserve_storage_local(resolver, type);

    struct symbol* const symbol =
        symbol_new_variable(decl->location, decl->name, type, address, value);
    freeze(symbol);

    symbol_table_insert(
        resolver->current_symbol_table,
        symbol->name,
        symbol,
        !resolver_is_global(resolver));
    if (is_static) {
        register_static_symbol(symbol);
    }

    if (lhs != NULL) {
        struct expr* const identifier =
            expr_new_symbol(decl->data.variable.identifier->location, symbol);
        freeze(identifier);
        *lhs = identifier;
    }
    if (rhs != NULL) {
        *rhs = expr;
    }
    return symbol;
}

static struct symbol const*
resolve_decl_constant(struct resolver* resolver, struct cst_decl const* decl)
{
    assert(resolver != NULL);
    assert(decl != NULL);
    assert(decl->kind == CST_DECL_CONSTANT);

    resolver->is_within_const_decl = true;

    struct expr const* expr = resolve_expr(resolver, decl->data.constant.expr);

    struct type const* const type = decl->data.variable.typespec != NULL
        ? resolve_typespec(resolver, decl->data.constant.typespec)
        : expr->type;
    if (type->size == SIZEOF_UNSIZED) {
        fatal(
            decl->location,
            "declaration of constant with unsized type `%s`",
            type->name);
    }

    expr = shallow_implicit_cast(type, expr);
    check_type_compatibility(expr->location, expr->type, type);

    // Constants (globals and locals) have their values computed at
    // compile-time and therefore must always be added to the symbol table with
    // an evaluated value.
    struct value* const value = eval_rvalue(expr);
    value_freeze(value);

    struct address const* const address =
        resolver_reserve_storage_static(resolver, decl->name);

    struct symbol* const symbol =
        symbol_new_constant(decl->location, decl->name, type, address, value);
    freeze(symbol);

    symbol_table_insert(
        resolver->current_symbol_table,
        symbol->name,
        symbol,
        !resolver_is_global(resolver));
    register_static_symbol(symbol);

    resolver->is_within_const_decl = false;

    return symbol;
}

static struct symbol const*
resolve_decl_function(struct resolver* resolver, struct cst_decl const* decl)
{
    assert(resolver != NULL);
    assert(decl != NULL);
    assert(decl->kind == CST_DECL_FUNCTION);

    // Check for declaration of a template function.
    sbuf(struct cst_identifier const* const) const template_parameters =
        decl->data.function.template_parameters;
    if (sbuf_count(template_parameters) != 0) {
        struct symbol_table* const symbols =
            symbol_table_new(resolver->current_symbol_table);
        struct symbol* const template_symbol = symbol_new_template(
            decl->location,
            decl->name,
            decl,
            resolver->current_static_addr_prefix,
            resolver->current_symbol_table,
            symbols);

        freeze(template_symbol);
        sbuf_push(context()->chilling_symbol_tables, symbols);
        symbol_table_insert(
            resolver->current_symbol_table,
            template_symbol->name,
            template_symbol,
            false);
        return template_symbol;
    }

    sbuf(struct cst_function_parameter const* const) const function_parameters =
        decl->data.function.function_parameters;

    // Create the type corresponding to the function.
    struct type const** parameter_types = NULL;
    sbuf_resize(parameter_types, sbuf_count(function_parameters));
    for (size_t i = 0; i < sbuf_count(function_parameters); ++i) {
        parameter_types[i] =
            resolve_typespec(resolver, function_parameters[i]->typespec);
        if (parameter_types[i]->size == SIZEOF_UNSIZED) {
            fatal(
                function_parameters[i]->typespec->location,
                "declaration of function parameter with unsized type `%s`",
                parameter_types[i]->name);
        }
    }
    sbuf_freeze(parameter_types);

    struct type const* const return_type =
        resolve_typespec(resolver, decl->data.function.return_typespec);
    if (return_type->size == SIZEOF_UNSIZED) {
        fatal(
            decl->data.function.return_typespec->location,
            "declaration of function with unsized return type `%s`",
            return_type->name);
    }

    struct type const* function_type =
        type_unique_function(parameter_types, return_type);

    struct address const* const address =
        resolver_reserve_storage_static(resolver, decl->name);

    // Create a new incomplete function, a value that evaluates to that
    // function, and the address of that function/value.
    struct function* const function = function_new(
        decl->data.function.identifier->name, function_type, address);
    freeze(function);

    struct value* const value = value_new_function(function);
    value_freeze(value);
    function->value = value;

    // Add the function/value to the symbol table now so that recursive
    // functions may reference themselves.
    struct symbol* const function_symbol =
        symbol_new_function(decl->location, function);
    freeze(function_symbol);
    symbol_table_insert(
        resolver->current_symbol_table,
        function_symbol->name,
        function_symbol,
        false);
    register_static_symbol(function_symbol);

    // Executing a call instruction pushes the return address (0x8 bytes) onto
    // the stack. Inside the function the prelude saves the previous value of
    // rbp (0x8 bytes) by pushing it on the stack. So in total there are 0x8 +
    // 0x8 = 0x10 bytes between the current rbp (saved from the stack pointer)
    // and the region of the stack containing function parameters.
    // XXX: Currently the compiler assumes 0x8 byte stack alignment and does
    // *NOT* pad the stack to be 0x10 byte-aligned as required by some ABIs.
    int rbp_offset = 0x10; // Saved rbp + return address.
    // Resolve the function's parameters in order from lowest->highest on the
    // stack (i.e. right to left), adjusting the rbp_offset for each parameter
    // along the way.
    sbuf(struct symbol const*) symbol_parameters = NULL;
    sbuf_resize(symbol_parameters, sbuf_count(function_parameters));
    for (size_t i = sbuf_count(function_parameters); i--;) {
        struct source_location const* const location =
            function_parameters[i]->location;
        char const* const name = function_parameters[i]->identifier->name;
        struct type const* const type = parameter_types[i];
        struct address* const address =
            address_new(address_init_local(rbp_offset));
        freeze(address);

        rbp_offset += (int)ceil8zu(type->size);
        struct symbol* const symbol =
            symbol_new_variable(location, name, type, address, NULL);
        freeze(symbol);

        symbol_parameters[i] = symbol;
    }
    sbuf_freeze(symbol_parameters);
    function->symbol_parameters = symbol_parameters;

    // Add the function's parameters to its outermost symbol table in order
    // from left to right so that any error message about duplicate parameter
    // symbols will list the left-most symbol as the first of the two symbols
    // added to the table.
    struct symbol_table* const symbol_table =
        symbol_table_new(resolver->current_symbol_table);
    sbuf_push(context()->chilling_symbol_tables, symbol_table);
    // The function references, but does not own, its outermost symbol table.
    function->symbol_table = symbol_table;
    for (size_t i = 0; i < sbuf_count(function_parameters); ++i) {
        symbol_table_insert(
            symbol_table,
            function->symbol_parameters[i]->name,
            function->symbol_parameters[i],
            false);
    }

    // Add the function's return value to its outermost symbol table.
    struct address* const return_value_address =
        address_new(address_init_local(rbp_offset));
    freeze(return_value_address);
    struct symbol* const return_value_symbol = symbol_new_variable(
        decl->data.function.return_typespec->location,
        context()->interned.return_,
        return_type,
        return_value_address,
        NULL);
    freeze(return_value_symbol);
    symbol_table_insert(
        symbol_table, return_value_symbol->name, return_value_symbol, false);
    function->symbol_return = return_value_symbol;

    struct incomplete_function* const incomplete =
        xalloc(NULL, sizeof(*incomplete));
    *incomplete = (struct incomplete_function){
        decl,
        function,
        symbol_table,
    };
    freeze(incomplete);
    sbuf_push(resolver->incomplete_functions, incomplete);

    return function_symbol;
}

static struct symbol const*
resolve_decl_struct(struct resolver* resolver, struct cst_decl const* decl)
{
    assert(resolver != NULL);
    assert(decl != NULL);
    assert(decl->kind == CST_DECL_STRUCT);

    // Check for declaration of a template function.
    sbuf(struct cst_identifier const* const) const template_parameters =
        decl->data.struct_.template_parameters;
    if (sbuf_count(template_parameters) != 0) {
        struct symbol_table* const symbols =
            symbol_table_new(resolver->current_symbol_table);
        struct symbol* const template_symbol = symbol_new_template(
            decl->location,
            decl->name,
            decl,
            resolver->current_static_addr_prefix,
            resolver->current_symbol_table,
            symbols);

        freeze(template_symbol);
        sbuf_push(context()->chilling_symbol_tables, symbols);
        symbol_table_insert(
            resolver->current_symbol_table,
            template_symbol->name,
            template_symbol,
            false);
        return template_symbol;
    }

    struct symbol_table* const struct_symbols =
        symbol_table_new(resolver->current_symbol_table);
    sbuf_push(context()->chilling_symbol_tables, struct_symbols);
    struct type* const type = type_new_struct(decl->name, struct_symbols);
    freeze(type);

    struct symbol* const symbol = symbol_new_type(decl->location, type);
    freeze(symbol);

    // Add the symbol to the current symbol table so that structs with
    // self-referential pointer and slice members may reference the type.
    symbol_table_insert(
        resolver->current_symbol_table, symbol->name, symbol, false);

    sbuf(struct cst_member const* const) const members =
        decl->data.struct_.members;
    size_t const members_count = sbuf_count(members);

    // Check for duplicate member definitions.
    for (size_t i = 0; i < members_count; ++i) {
        for (size_t j = i + 1; j < members_count; ++j) {
            if (members[i]->name == members[j]->name) {
                // XXX: Call to sbuf_fini here because GCC 8.3 w/ ASAN
                // complains about a memory leak even though we hold a valid
                // path to the buffer.
                //
                // TODO: See if maybe we can trick ASAN by holding a pointer to
                // the head of the stretchy buffer here???
                sbuf_fini(type->data.struct_.member_variables);

                fatal(
                    members[j]->location,
                    "duplicate definition of member `%s`",
                    members[j]->name);
            }
        }
    }

    return symbol;
}

static struct symbol const*
resolve_decl_extend(struct resolver* resolver, struct cst_decl const* decl)
{
    assert(resolver != NULL);
    assert(decl != NULL);
    assert(decl->kind == CST_DECL_EXTEND);

    if (decl->data.extend.decl->kind != CST_DECL_CONSTANT
        && decl->data.extend.decl->kind != CST_DECL_FUNCTION) {
        fatal(
            decl->location,
            "type extension declaration must be a constant or function");
    }

    struct type const* const type =
        resolve_typespec(resolver, decl->data.extend.typespec);

    // PLAN: Create the decl in a sub-symbol table of the module namespace that
    // is created specifically for this one symbol so that name collisions
    // don't happen. Then add the symbol to the type.

    // Create a symbol table for this declaration only in order to prevent name
    // collisions and hide the created symbol from the rest of the module.
    struct symbol_table* const symbol_table =
        symbol_table_new(resolver->current_symbol_table);

    // TODO: This current symbol name prefix and addr prefix do not take into
    // account the namespace prefix! Adjust this so that the prefixes use
    // <namespace>.<typename> instead just <typename>.
    //
    // Look for a similar comment under complete_struct which also shares this
    // problem.
    char const* const save_static_addr_prefix =
        resolver->current_static_addr_prefix;
    struct symbol_table* const save_symbol_table =
        resolver->current_symbol_table;
    resolver->current_static_addr_prefix = normalize(NULL, type->name, 0);
    resolver->current_symbol_table = symbol_table;

    struct symbol const* const symbol =
        resolve_decl(resolver, decl->data.extend.decl);
    symbol_table_insert(type->symbols, decl->name, symbol, false);

    resolver->current_static_addr_prefix = save_static_addr_prefix;
    resolver->current_symbol_table = save_symbol_table;

    symbol_table_freeze(symbol_table);
    return symbol;
}

static struct symbol const*
resolve_decl_alias(struct resolver* resolver, struct cst_decl const* decl)
{
    assert(resolver != NULL);
    assert(decl != NULL);
    assert(decl->kind == CST_DECL_ALIAS);

    struct type const* const type =
        resolve_typespec(resolver, decl->data.alias.typespec);
    struct symbol* const symbol = symbol_new_type(decl->location, type);
    freeze(symbol);
    symbol_table_insert(
        resolver->current_symbol_table,
        decl->name,
        symbol,
        !resolver_is_global(resolver));

    return symbol;
}

static struct symbol const*
resolve_decl_extern_variable(
    struct resolver* resolver, struct cst_decl const* decl)
{
    assert(resolver != NULL);
    assert(decl != NULL);
    assert(decl->kind == CST_DECL_EXTERN_VARIABLE);
    assert(resolver_is_global(resolver));

    struct type const* const type =
        resolve_typespec(resolver, decl->data.variable.typespec);
    if (type->size == SIZEOF_UNSIZED) {
        fatal(
            decl->data.variable.typespec->location,
            "declaration of extern variable with unsized type `%s`",
            type->name);
    }

    struct address const* const address =
        resolver_reserve_storage_static(resolver, decl->name);

    struct symbol* const symbol =
        symbol_new_variable(decl->location, decl->name, type, address, NULL);
    freeze(symbol);

    symbol_table_insert(
        resolver->current_symbol_table, symbol->name, symbol, false);
    register_static_symbol(symbol); // Extern variables are always static.

    return symbol;
}

static struct symbol const*
resolve_decl_extern_function(
    struct resolver* resolver, struct cst_decl const* decl)
{
    assert(resolver != NULL);
    assert(decl != NULL);
    assert(decl->kind == CST_DECL_EXTERN_FUNCTION);
    assert(resolver_is_global(resolver));

    sbuf(struct cst_function_parameter const* const) const function_parameters =
        decl->data.extern_function.function_parameters;

    // Create the type corresponding to the function.
    struct type const** parameter_types = NULL;
    sbuf_resize(parameter_types, sbuf_count(function_parameters));
    for (size_t i = 0; i < sbuf_count(function_parameters); ++i) {
        parameter_types[i] =
            resolve_typespec(resolver, function_parameters[i]->typespec);
        if (parameter_types[i]->size == SIZEOF_UNSIZED) {
            fatal(
                function_parameters[i]->typespec->location,
                "declaration of function parameter with unsized type `%s`",
                parameter_types[i]->name);
        }
    }
    sbuf_freeze(parameter_types);

    struct type const* const return_type =
        resolve_typespec(resolver, decl->data.extern_function.return_typespec);
    if (return_type->size == SIZEOF_UNSIZED) {
        fatal(
            decl->data.extern_function.return_typespec->location,
            "declaration of function with unsized return type `%s`",
            return_type->name);
    }

    struct type const* function_type =
        type_unique_function(parameter_types, return_type);

    struct address const* const address =
        resolver_reserve_storage_static(resolver, decl->name);

    // Create a new incomplete function, a value that evaluates to that
    // function, and the address of that function/value.
    struct function* const function = function_new(
        decl->data.extern_function.identifier->name, function_type, address);
    freeze(function);

    struct value* const value = value_new_function(function);
    value_freeze(value);
    function->value = value;

    struct symbol* const symbol = symbol_new_function(decl->location, function);
    freeze(symbol);

    symbol_table_insert(
        resolver->current_symbol_table, symbol->name, symbol, false);
    register_static_symbol(symbol); // Extern functions are always static.

    return symbol;
}

static void
complete_struct(
    struct resolver* resolver,
    struct symbol const* symbol,
    struct cst_decl const* decl)
{
    assert(resolver != NULL);
    assert(symbol != NULL);
    assert(symbol->kind == SYMBOL_TYPE);
    assert(symbol_xget_type(symbol)->kind == TYPE_STRUCT);
    assert(decl != NULL);
    assert(decl->kind == CST_DECL_STRUCT);
    assert(symbol->name == decl->name);

    sbuf(struct cst_member const* const) const members =
        decl->data.struct_.members;
    size_t const members_count = sbuf_count(members);

    // XXX: Evil const cast.
    struct type* type = (struct type*)symbol_xget_type(symbol);
    // XXX: Evil const cast.
    struct symbol_table* const struct_symbols =
        (struct symbol_table*)symbol_xget_type(symbol)->symbols;

    // XXX: A "direct leak" is detected if we encounter a fatal error while
    // resolving the member constants and declarations below even though we
    // hold a valid path to the buffer through the `type` pointer. Always keep
    // a reference to the the stretchy buffer so that ASAN does not complain.
    sbuf(struct member_variable const) xxx_member_variables_ref = NULL;
    (void)xxx_member_variables_ref;
    // Add all member definitions to the struct in the order that they were
    // defined in.
    char const* const save_static_addr_prefix =
        resolver->current_static_addr_prefix;
    struct symbol_table* const save_symbol_table =
        resolver->current_symbol_table;
    // TODO: This current symbol name prefix and addr prefix do not take into
    // account the namespace prefix! Adjust this so that the prefixes use
    // <namespace>.<typename> instead just <typename>.
    //
    // Look for a similar comment under resolve_decl_extend which also shares
    // this problem.
    resolver->current_static_addr_prefix =
        normalize(NULL, symbol_xget_type(symbol)->name, 0);
    resolver->current_symbol_table = struct_symbols;
    for (size_t i = 0; i < members_count; ++i) {
        struct cst_member const* const member = members[i];
        switch (member->kind) {
        case CST_MEMBER_VARIABLE: {
            struct type const* const member_type =
                resolve_typespec(resolver, member->data.variable.typespec);
            type_struct_add_member_variable(type, member->name, member_type);
            xxx_member_variables_ref = type->data.struct_.member_variables;
            continue;
        }
        case CST_MEMBER_CONSTANT: {
            resolve_decl_constant(resolver, member->data.constant.decl);
            continue;
        }
        case CST_MEMBER_FUNCTION: {
            resolve_decl_function(resolver, member->data.function.decl);
            continue;
        }
        }
        UNREACHABLE();
    }
    resolver->current_static_addr_prefix = save_static_addr_prefix;
    resolver->current_symbol_table = save_symbol_table;

    sbuf_freeze(type->data.struct_.member_variables);
}

static void
complete_function(
    struct resolver* resolver, struct incomplete_function const* incomplete)
{
    assert(resolver != NULL);
    assert(incomplete != NULL);

    struct function* const function = incomplete->function;

    // Complete the function.
    assert(resolver->current_function == NULL);
    assert(resolver->current_rbp_offset == 0x0);
    assert(!resolver->is_within_loop);
    char const* const save_static_addr_prefix =
        resolver->current_static_addr_prefix;
    resolver->current_static_addr_prefix = function->address->data.static_.name;
    resolver->current_function = function;
    function->body = resolve_block(
        resolver,
        incomplete->symbol_table,
        incomplete->decl->data.function.body);
    resolver->current_static_addr_prefix = save_static_addr_prefix;
    resolver->current_function = NULL;
    assert(resolver->current_rbp_offset == 0x0);

    // Produce an error if the last statement of a non-void returning function
    // is *not* a return statement. Even if the last statement is an if-else
    // block with a return in each arm of the statement we should still produce
    // an error as idiomatic Sunder code should use
    //
    //      if condition {
    //          return early_return_value;
    //      }
    //      return other_return_value;
    //
    // instead of
    //
    //      if condition {
    //          return first_return_value;
    //      }
    //      else {
    //          return other_return_value;
    //      }
    assert(function->type->kind == TYPE_FUNCTION);
    bool func_has_void_return =
        function->type->data.function.return_type->kind == TYPE_VOID;
    sbuf(struct stmt const* const) const stmts = function->body->stmts;
    bool const non_void_returning_func_does_not_end_with_return =
        !func_has_void_return
        && (sbuf_count(stmts) == 0
            || stmts[sbuf_count(stmts) - 1]->kind != STMT_RETURN);
    if (non_void_returning_func_does_not_end_with_return) {
        fatal(
            incomplete->decl->location,
            "Non-void-returning function does not end with a return statement");
    }
}

static struct stmt const*
resolve_stmt(struct resolver* resolver, struct cst_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);

    switch (stmt->kind) {
    case CST_STMT_DECL: {
        return resolve_stmt_decl(resolver, stmt);
    }
    case CST_STMT_DEFER: {
        return resolve_stmt_defer(resolver, stmt);
    }
    case CST_STMT_IF: {
        return resolve_stmt_if(resolver, stmt);
    }
    case CST_STMT_FOR_RANGE: {
        return resolve_stmt_for_range(resolver, stmt);
    }
    case CST_STMT_FOR_EXPR: {
        return resolve_stmt_for_expr(resolver, stmt);
    }
    case CST_STMT_BREAK: {
        return resolve_stmt_break(resolver, stmt);
    }
    case CST_STMT_CONTINUE: {
        return resolve_stmt_continue(resolver, stmt);
    }
    case CST_STMT_DUMP: {
        return resolve_stmt_dump(resolver, stmt);
    }
    case CST_STMT_RETURN: {
        return resolve_stmt_return(resolver, stmt);
    }
    case CST_STMT_ASSIGN: {
        return resolve_stmt_assign(resolver, stmt);
    }
    case CST_STMT_EXPR: {
        return resolve_stmt_expr(resolver, stmt);
    }
    }

    UNREACHABLE();
    return NULL;
}

static struct stmt const*
resolve_stmt_decl(struct resolver* resolver, struct cst_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == CST_STMT_DECL);

    struct cst_decl const* const decl = stmt->data.decl;
    switch (decl->kind) {
    case CST_DECL_VARIABLE: {
        struct expr const* lhs = NULL;
        struct expr const* rhs = NULL;
        resolve_decl_variable(resolver, decl, &lhs, &rhs);
        struct stmt* const resolved = stmt_new_assign(stmt->location, lhs, rhs);

        freeze(resolved);
        return resolved;
    }
    case CST_DECL_CONSTANT: {
        resolve_decl_constant(resolver, decl);
        return NULL;
    }
    case CST_DECL_FUNCTION: {
        fatal(stmt->location, "nested function declaration");
        return NULL;
    }
    case CST_DECL_STRUCT: {
        fatal(decl->location, "local declaration of struct `%s`", decl->name);
        return NULL;
    }
    case CST_DECL_EXTEND: {
        fatal(
            decl->location,
            "local declaration of type extension `%s`",
            decl->name);
        return NULL;
    }
    case CST_DECL_ALIAS: {
        resolve_decl_alias(resolver, stmt->data.decl);
        return NULL;
    }
    case CST_DECL_EXTERN_VARIABLE: {
        fatal(
            decl->location,
            "local declaration of extern variable `%s`",
            decl->name);
        return NULL;
    }
    case CST_DECL_EXTERN_FUNCTION: {
        fatal(
            decl->location,
            "local declaration of extern function `%s`",
            decl->name);
        return NULL;
    }
    }

    UNREACHABLE();
    return NULL;
}

static struct stmt const*
resolve_stmt_defer(struct resolver* resolver, struct cst_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == CST_STMT_DEFER);

    struct symbol_table* const symbol_table =
        symbol_table_new(resolver->current_symbol_table);
    struct block const* const body =
        resolve_block(resolver, symbol_table, stmt->data.defer);
    symbol_table_freeze(symbol_table);

    struct stmt* const resolved =
        stmt_new_defer(stmt->location, resolver->current_defer, body);
    resolver->current_defer = resolved;

    freeze(resolved);
    return resolved;
}

static struct stmt const*
resolve_stmt_if(struct resolver* resolver, struct cst_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == CST_STMT_IF);

    sbuf(struct cst_conditional const* const) const conditionals =
        stmt->data.if_.conditionals;
    sbuf(struct conditional const*) resolved_conditionals = NULL;
    sbuf_resize(resolved_conditionals, sbuf_count(conditionals));
    for (size_t i = 0; i < sbuf_count(conditionals); ++i) {
        assert(
            (conditionals[i]->condition != NULL)
            || (i == (sbuf_count(conditionals) - 1)));

        struct expr const* condition = NULL;
        if (conditionals[i]->condition != NULL) {
            condition = resolve_expr(resolver, conditionals[i]->condition);
            if (condition->type->kind != TYPE_BOOL) {
                fatal(
                    condition->location,
                    "illegal condition with non-boolean type `%s`",
                    condition->type->name);
            }
        }

        struct symbol_table* const symbol_table =
            symbol_table_new(resolver->current_symbol_table);
        struct block const* const block =
            resolve_block(resolver, symbol_table, conditionals[i]->body);
        // Freeze the symbol table now that the block has been resolved and no
        // new symbols will be added.
        symbol_table_freeze(symbol_table);

        struct conditional* const resolved_conditional =
            conditional_new(conditionals[i]->location, condition, block);
        freeze(resolved_conditional);
        resolved_conditionals[i] = resolved_conditional;
    }

    sbuf_freeze(resolved_conditionals);
    struct stmt* const resolved = stmt_new_if(resolved_conditionals);

    freeze(resolved);
    return resolved;
}

static struct stmt const*
resolve_stmt_for_range(struct resolver* resolver, struct cst_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == CST_STMT_FOR_RANGE);

    struct expr const* begin = NULL;
    if (stmt->data.for_range.begin != NULL) {
        begin = resolve_expr(resolver, stmt->data.for_range.begin);
        begin = shallow_implicit_cast(context()->builtin.usize, begin);
        if (begin->type != context()->builtin.usize) {
            fatal(
                begin->location,
                "illegal range-begin-expression with non-usize type `%s`",
                begin->type->name);
        }
    }
    else {
        struct expr* zero = expr_new_integer(
            stmt->location, context()->builtin.usize, context()->zero);
        freeze(zero);
        begin = zero;
    }

    struct expr const* end = resolve_expr(resolver, stmt->data.for_range.end);
    end = shallow_implicit_cast(context()->builtin.usize, end);
    if (end->type != context()->builtin.usize) {
        fatal(
            end->location,
            "illegal range-end-expression with non-usize type `%s`",
            end->type->name);
    }

    int const save_rbp_offset = resolver->current_rbp_offset;
    struct source_location const* const loop_var_location =
        stmt->data.for_range.identifier->location;
    char const* const loop_var_name = stmt->data.for_range.identifier->name;
    struct type const* const loop_var_type = context()->builtin.usize;
    struct address const* const loop_var_address =
        resolver_reserve_storage_local(resolver, loop_var_type);
    struct symbol* const loop_var_symbol = symbol_new_variable(
        loop_var_location,
        loop_var_name,
        loop_var_type,
        loop_var_address,
        NULL);
    freeze(loop_var_symbol);

    struct symbol_table* const symbol_table =
        symbol_table_new(resolver->current_symbol_table);
    symbol_table_insert(
        symbol_table, loop_var_symbol->name, loop_var_symbol, false);

    bool const save_is_within_loop = resolver->is_within_loop;
    struct stmt const* const save_current_loop_defer =
        resolver->current_loop_defer;

    resolver->is_within_loop = true;
    resolver->current_loop_defer = resolver->current_defer;
    struct block const* const body =
        resolve_block(resolver, symbol_table, stmt->data.for_range.body);
    resolver->current_rbp_offset = save_rbp_offset;
    resolver->is_within_loop = save_is_within_loop;
    resolver->current_loop_defer = save_current_loop_defer;

    // Freeze the symbol table now that the block has been resolved and no new
    // symbols will be added.
    symbol_table_freeze(symbol_table);

    struct stmt* const resolved =
        stmt_new_for_range(stmt->location, loop_var_symbol, begin, end, body);
    freeze(resolved);
    return resolved;
}

static struct stmt const*
resolve_stmt_for_expr(struct resolver* resolver, struct cst_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == CST_STMT_FOR_EXPR);

    struct expr const* const expr =
        resolve_expr(resolver, stmt->data.for_expr.expr);
    if (expr->type->kind != TYPE_BOOL) {
        fatal(
            expr->location,
            "illegal condition with non-boolean type `%s`",
            expr->type->name);
    }

    struct symbol_table* const symbol_table =
        symbol_table_new(resolver->current_symbol_table);

    bool const save_is_within_loop = resolver->is_within_loop;
    struct stmt const* const save_current_loop_defer =
        resolver->current_loop_defer;

    resolver->is_within_loop = true;
    resolver->current_loop_defer = resolver->current_defer;
    struct block const* const body =
        resolve_block(resolver, symbol_table, stmt->data.for_expr.body);
    resolver->is_within_loop = save_is_within_loop;
    resolver->current_loop_defer = save_current_loop_defer;

    // Freeze the symbol table now that the block has been resolved and no new
    // symbols will be added.
    symbol_table_freeze(symbol_table);

    struct stmt* const resolved = stmt_new_for_expr(stmt->location, expr, body);

    freeze(resolved);
    return resolved;
}

static struct stmt const*
resolve_stmt_break(struct resolver* resolver, struct cst_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == CST_STMT_BREAK);

    if (!resolver->is_within_loop) {
        fatal(stmt->location, "break statement outside of loop");
    }

    struct stmt* const resolved = stmt_new_break(
        stmt->location, resolver->current_defer, resolver->current_loop_defer);

    freeze(resolved);
    return resolved;
}

static struct stmt const*
resolve_stmt_continue(struct resolver* resolver, struct cst_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == CST_STMT_CONTINUE);

    if (!resolver->is_within_loop) {
        fatal(stmt->location, "continue statement outside of loop");
    }

    struct stmt* const resolved = stmt_new_continue(
        stmt->location, resolver->current_defer, resolver->current_loop_defer);

    freeze(resolved);
    return resolved;
}

static struct stmt const*
resolve_stmt_dump(struct resolver* resolver, struct cst_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == CST_STMT_DUMP);

    struct expr const* expr = resolve_expr(resolver, stmt->data.dump.expr);
    if (expr->type->size == SIZEOF_UNSIZED) {
        fatal(
            stmt->location, "type `%s` has no defined size", expr->type->name);
    }

    struct stmt* const resolved = stmt_new_dump(stmt->location, expr);

    freeze(resolved);
    return resolved;
}

static struct stmt const*
resolve_stmt_return(struct resolver* resolver, struct cst_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == CST_STMT_RETURN);

    struct type const* const return_type =
        resolver->current_function->type->data.function.return_type;
    struct expr const* expr = NULL;
    if (stmt->data.return_.expr != NULL) {
        expr = resolve_expr(resolver, stmt->data.return_.expr);
        expr = shallow_implicit_cast(return_type, expr);
        check_type_compatibility(expr->location, expr->type, return_type);
    }
    else {
        if (context()->builtin.void_ != return_type) {
            fatal(
                stmt->location,
                "illegal return statement in function with non-void return type");
        }
    }

    struct stmt* const resolved =
        stmt_new_return(stmt->location, expr, resolver->current_defer);

    freeze(resolved);
    return resolved;
}

static struct stmt const*
resolve_stmt_assign(struct resolver* resolver, struct cst_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == CST_STMT_ASSIGN);

    struct expr const* const lhs =
        resolve_expr(resolver, stmt->data.assign.lhs);
    struct expr const* rhs = resolve_expr(resolver, stmt->data.assign.rhs);

    // TODO: Rather than query if lhs is an lvalue, perhaps there could
    // function `validate_expr_is_lvalue` in resolve.c which traverses the
    // expression tree and emits an error with more context about *why* a
    // specific expression is not an lvalue. Currently it's up to the user to
    // figure out *why* lhs is not an lvalue, and better information could ease
    // debugging.
    if (!expr_is_lvalue(lhs)) {
        fatal(
            lhs->location,
            "left hand side of assignment statement is not an lvalue");
    }

    rhs = shallow_implicit_cast(lhs->type, rhs);
    check_type_compatibility(stmt->location, rhs->type, lhs->type);

    struct stmt* const resolved = stmt_new_assign(stmt->location, lhs, rhs);

    freeze(resolved);
    return resolved;
}

static struct stmt const*
resolve_stmt_expr(struct resolver* resolver, struct cst_stmt const* stmt)
{
    assert(resolver != NULL);
    assert(!resolver_is_global(resolver));
    assert(stmt != NULL);
    assert(stmt->kind == CST_STMT_EXPR);

    struct expr const* const expr = resolve_expr(resolver, stmt->data.expr);

    if (expr->type->size == SIZEOF_UNSIZED) {
        fatal(
            expr->location,
            "statement-expression produces result of unsized type `%s`",
            expr->type->name);
    }
    struct stmt* const resolved = stmt_new_expr(stmt->location, expr);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);

    switch (expr->kind) {
    case CST_EXPR_SYMBOL: {
        return resolve_expr_symbol(resolver, expr);
    }
    case CST_EXPR_BOOLEAN: {
        return resolve_expr_boolean(resolver, expr);
    }
    case CST_EXPR_INTEGER: {
        return resolve_expr_integer(resolver, expr);
    }
    case CST_EXPR_CHARACTER: {
        return resolve_expr_character(resolver, expr);
    }
    case CST_EXPR_BYTES: {
        return resolve_expr_bytes(resolver, expr);
    }
    case CST_EXPR_LIST: {
        return resolve_expr_list(resolver, expr);
    }
    case CST_EXPR_SLICE: {
        return resolve_expr_slice(resolver, expr);
    }
    case CST_EXPR_STRUCT: {
        return resolve_expr_struct(resolver, expr);
    }
    case CST_EXPR_CAST: {
        return resolve_expr_cast(resolver, expr);
    }
    case CST_EXPR_GROUPED: {
        return resolve_expr(resolver, expr->data.grouped.expr);
    }
    case CST_EXPR_CALL: {
        return resolve_expr_call(resolver, expr);
    }
    case CST_EXPR_ACCESS_INDEX: {
        return resolve_expr_access_index(resolver, expr);
    }
    case CST_EXPR_ACCESS_SLICE: {
        return resolve_expr_access_slice(resolver, expr);
    }
    case CST_EXPR_ACCESS_MEMBER: {
        return resolve_expr_access_member(resolver, expr);
    }
    case CST_EXPR_ACCESS_DEREFERENCE: {
        return resolve_expr_access_dereference(resolver, expr);
    }
    case CST_EXPR_SIZEOF: {
        return resolve_expr_sizeof(resolver, expr);
    }
    case CST_EXPR_ALIGNOF: {
        return resolve_expr_alignof(resolver, expr);
    }
    case CST_EXPR_UNARY: {
        return resolve_expr_unary(resolver, expr);
    }
    case CST_EXPR_BINARY: {
        return resolve_expr_binary(resolver, expr);
    }
    }

    UNREACHABLE();
    return NULL;
}

static struct expr const*
resolve_expr_symbol(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_SYMBOL);

    struct symbol const* const symbol =
        xget_symbol(resolver, expr->data.symbol);
    switch (symbol->kind) {
    case SYMBOL_TYPE: {
        fatal(
            expr->location, "use of type `%s` as an expression", symbol->name);
    }
    case SYMBOL_TEMPLATE: {
        fatal(
            expr->location,
            "use of template `%s` as an expression",
            symbol->name);
    }
    case SYMBOL_NAMESPACE: {
        fatal(
            expr->location,
            "use of namespace `%s` as an expression",
            symbol->name);
    }
    case SYMBOL_VARIABLE: /* fallthrough */
    case SYMBOL_CONSTANT: /* fallthrough */
    case SYMBOL_FUNCTION: {
        // Variables, constants, and functions may be used in an identifier
        // expression.
        break;
    }
    }

    struct expr* const resolved = expr_new_symbol(expr->location, symbol);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_boolean(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_BOOLEAN);
    (void)resolver;

    bool const value = expr->data.boolean->value;
    struct expr* const resolved = expr_new_boolean(expr->location, value);

    freeze(resolved);
    return resolved;
}

static struct type const*
integer_literal_suffix_to_type(
    struct source_location const* location, char const* suffix)
{
    assert(suffix != NULL);

    if (suffix == context()->interned.empty) {
        return context()->builtin.integer;
    }
    if (suffix == context()->interned.y) {
        return context()->builtin.byte;
    }
    if (suffix == context()->interned.u8) {
        return context()->builtin.u8;
    }
    if (suffix == context()->interned.s8) {
        return context()->builtin.s8;
    }
    if (suffix == context()->interned.u16) {
        return context()->builtin.u16;
    }
    if (suffix == context()->interned.s16) {
        return context()->builtin.s16;
    }
    if (suffix == context()->interned.u32) {
        return context()->builtin.u32;
    }
    if (suffix == context()->interned.s32) {
        return context()->builtin.s32;
    }
    if (suffix == context()->interned.u64) {
        return context()->builtin.u64;
    }
    if (suffix == context()->interned.s64) {
        return context()->builtin.s64;
    }
    if (suffix == context()->interned.u) {
        return context()->builtin.usize;
    }
    if (suffix == context()->interned.s) {
        return context()->builtin.ssize;
    }

    fatal(location, "unknown integer literal suffix `%s`", suffix);
    return NULL;
}

static struct expr const*
resolve_expr_integer(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_INTEGER);
    (void)resolver;

    struct cst_integer const* const cst_integer = expr->data.integer;
    struct bigint const* const value = cst_integer->value;
    struct type const* const type = integer_literal_suffix_to_type(
        cst_integer->location, cst_integer->suffix);

    struct expr* const resolved = expr_new_integer(expr->location, type, value);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_character(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_CHARACTER);
    (void)resolver;

    // XXX: Hack to get around the bigint API not having a constructor
    // function that creates a bigint based of of an int input value.
    char buf[255] = {0};
    int const written = snprintf(buf, sizeof(buf), "%d", expr->data.character);
    assert(written < (int)sizeof(buf));
    (void)written;

    struct type const* const type = context()->builtin.integer;
    struct bigint* const value = bigint_new_text(buf, strlen(buf));
    bigint_freeze(value);
    struct expr* const resolved = expr_new_integer(expr->location, type, value);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_bytes(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_BYTES);
    (void)resolver;

    struct address const* const address =
        resolver_reserve_storage_static(resolver, "__bytes");

    size_t const count = string_count(expr->data.bytes);
    struct type const* const type =
        type_unique_array(count + 1 /*NUL*/, context()->builtin.byte);
    // TODO: Allocating a value for each and every byte in the bytes literal
    // feels wasteful. It may be worth investigating some specific ascii or
    // asciiz static object that would use the expr's string directly and then
    // generate a readable string in the output assembly during the codegen
    // phase.
    sbuf(struct value*) elements = NULL;
    for (size_t i = 0; i < count; ++i) {
        uint8_t const byte = (uint8_t)string_start(expr->data.bytes)[i];
        sbuf_push(elements, value_new_byte(byte));
    }
    // Append a NUL byte to the end of every bytes literal. This NUL byte is
    // not included in the slice length, but will allow bytes literals to be
    // accessed as NUL-terminated arrays when interfacing with C code.
    sbuf_push(elements, value_new_byte(0x00));
    struct value* const value = value_new_array(type, elements, NULL);
    value_freeze(value);

    struct symbol* const symbol = symbol_new_constant(
        expr->location, address->data.static_.name, type, address, value);
    freeze(symbol);
    register_static_symbol(symbol);

    struct expr* const resolved =
        expr_new_bytes(expr->location, address, count);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_list(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_LIST);

    struct type const* const type =
        resolve_typespec(resolver, expr->data.list.typespec);
    if (type->kind != TYPE_ARRAY && type->kind != TYPE_SLICE) {
        fatal(
            expr->data.list.typespec->location,
            "expected array or slice type (received `%s`)",
            type->name);
    }
    struct type const* const base = type->kind == TYPE_ARRAY
        ? type->data.array.base
        : type->data.slice.base;

    if (type->kind == TYPE_ARRAY) {
        sbuf(struct cst_expr const* const) elements = expr->data.list.elements;
        sbuf(struct expr const*) resolved_elements = NULL;
        for (size_t i = 0; i < sbuf_count(elements); ++i) {
            struct expr const* resolved_element =
                resolve_expr(resolver, elements[i]);
            resolved_element = shallow_implicit_cast(base, resolved_element);
            check_type_compatibility(
                resolved_element->location, resolved_element->type, base);
            sbuf_push(resolved_elements, resolved_element);
        }
        sbuf_freeze(resolved_elements);

        struct expr const* resolved_ellipsis = NULL;
        if (expr->data.list.ellipsis != NULL) {
            resolved_ellipsis =
                resolve_expr(resolver, expr->data.list.ellipsis);
            resolved_ellipsis = shallow_implicit_cast(base, resolved_ellipsis);
            check_type_compatibility(
                resolved_ellipsis->location, resolved_ellipsis->type, base);
        }

        if ((type->data.array.count != sbuf_count(resolved_elements))
            && resolved_ellipsis == NULL) {
            fatal(
                expr->location,
                "array of type `%s` created with %zu elements (expected %zu)",
                type->name,
                sbuf_count(resolved_elements),
                type->data.array.count);
        }

        struct expr* const resolved = expr_new_array_list(
            expr->location, type, resolved_elements, resolved_ellipsis);

        freeze(resolved);
        return resolved;
    }

    assert(type->kind == TYPE_SLICE);
    if (expr->data.list.ellipsis != NULL) {
        fatal(
            expr->data.list.ellipsis->location,
            "ellipsis element is not allowed in slice lists",
            type->name);
    }

    sbuf(struct cst_expr const* const) elements = expr->data.list.elements;
    size_t const elements_count = sbuf_count(elements);

    static size_t id = 0;
    struct string* const array_name_string =
        string_new_fmt("__slice_list_elements_%zu", id++);
    char const* const array_name = intern_cstr(string_start(array_name_string));
    string_del(array_name_string);

    struct type const* const array_type =
        type_unique_array(elements_count, type->data.slice.base);

    bool const is_global = resolver_is_global(resolver);
    bool const is_static = is_global || resolver->is_within_const_decl;
    struct address const* const array_address = is_static
        ? resolver_reserve_storage_static(resolver, array_name)
        : resolver_reserve_storage_local(resolver, array_type);

    struct value* array_value = NULL;
    if (is_static) {
        sbuf(struct expr const*) resolved_elements = NULL;
        sbuf(struct value*) resolved_values = NULL;
        for (size_t i = 0; i < sbuf_count(elements); ++i) {
            struct expr const* resolved_element =
                resolve_expr(resolver, elements[i]);
            resolved_element = shallow_implicit_cast(
                array_type->data.array.base, resolved_element);
            check_type_compatibility(
                resolved_element->location,
                resolved_element->type,
                array_type->data.array.base);
            sbuf_push(resolved_elements, resolved_element);
            sbuf_push(resolved_values, eval_rvalue(resolved_element));
        }
        sbuf_freeze(resolved_elements);
        array_value = value_new_array(array_type, resolved_values, NULL);
        value_freeze(array_value);
    }

    struct symbol* const array_symbol =
        (resolver->is_within_const_decl ? symbol_new_constant
                                        : symbol_new_variable)(
            expr->location, array_name, array_type, array_address, array_value);
    if (is_static) {
        register_static_symbol(array_symbol);
    }
    freeze(array_symbol);

    symbol_table_insert(
        resolver->current_symbol_table,
        array_symbol->name,
        array_symbol,
        false);

    sbuf(struct expr const*) resolved_elements = NULL;
    for (size_t i = 0; i < elements_count; ++i) {
        struct expr const* resolved_element =
            resolve_expr(resolver, elements[i]);
        resolved_element =
            shallow_implicit_cast(type->data.slice.base, resolved_element);
        check_type_compatibility(
            resolved_element->location,
            resolved_element->type,
            type->data.slice.base);
        sbuf_push(resolved_elements, resolved_element);
    }
    sbuf_freeze(resolved_elements);

    struct expr* const resolved = expr_new_slice_list(
        expr->location, type, array_symbol, resolved_elements);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_slice(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_SLICE);

    struct type const* const type =
        resolve_typespec(resolver, expr->data.slice.typespec);
    if (type->kind != TYPE_SLICE) {
        fatal(
            expr->data.slice.typespec->location,
            "expected slice type (received `%s`)",
            type->name);
    }

    struct expr const* const pointer =
        resolve_expr(resolver, expr->data.slice.pointer);
    if (pointer->type->kind != TYPE_POINTER) {
        fatal(
            pointer->location,
            "expression of type `%s` is not a pointer",
            pointer->type->name);
    }
    struct type const* const slice_pointer_type =
        type_unique_pointer(type->data.slice.base);
    check_type_compatibility(
        pointer->location, pointer->type, slice_pointer_type);

    struct expr const* count = resolve_expr(resolver, expr->data.slice.count);
    count = shallow_implicit_cast(context()->builtin.usize, count);
    check_type_compatibility(
        count->location, count->type, context()->builtin.usize);

    struct expr* const resolved =
        expr_new_slice(expr->location, type, pointer, count);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_struct(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_STRUCT);

    struct type const* const type =
        resolve_typespec(resolver, expr->data.struct_.typespec);
    if (type->kind != TYPE_STRUCT) {
        fatal(
            expr->location, "expected struct type (received `%s`)", type->name);
    }

    sbuf(struct member_variable) const member_variable_defs =
        type->data.struct_.member_variables;

    sbuf(struct cst_member_initializer const* const) const initializers =
        expr->data.struct_.initializers;

    // Resolve the expressions associated with each initializer element.
    // Expressions are resolved before the checks for duplicate elements,
    // missing elements, or extra elements not corresponding to a struct member
    // variable, all so that the user can receive feedback about any malformed
    // expressions *before* feedback on how the initializer list does not match
    // the struct definition.
    sbuf(struct expr const*) initializer_exprs = NULL;
    for (size_t i = 0; i < sbuf_count(initializers); ++i) {
        sbuf_push(
            initializer_exprs, resolve_expr(resolver, initializers[i]->expr));
    }

    // Ordered list of member variables corresponding to the member variables
    // defined by the struct type. The list is initialized to the length of
    // struct type's member variable list with all NULLs. As the initializer
    // list is processed the NULLs are replaced with expr pointers so that
    // duplicate initializers can be detected when a non-NULL value would be
    // overwritten, and missing initializers can be detected by looking for
    // remaining NULLs after all initializer elements have been processed.
    sbuf(struct expr const*) member_variable_exprs = NULL;
    for (size_t i = 0; i < sbuf_count(member_variable_defs); ++i) {
        sbuf_push(member_variable_exprs, NULL);
    }

    for (size_t i = 0; i < sbuf_count(initializers); ++i) {
        char const* const initializer_name = initializers[i]->identifier->name;
        bool found = false; // Did we find the member for this initializer?

        for (size_t j = 0; j < sbuf_count(member_variable_defs); ++j) {
            if (initializer_name != member_variable_defs[j].name) {
                continue;
            }

            if (member_variable_exprs[j] != NULL) {
                fatal(
                    initializers[i]->location,
                    "duplicate initializer for member variable `%s`",
                    member_variable_defs[j].name);
            }

            struct expr const* const initializer_expr = shallow_implicit_cast(
                member_variable_defs[j].type, initializer_exprs[i]);
            check_type_compatibility(
                initializer_expr->location,
                initializer_expr->type,
                member_variable_defs[j].type);
            member_variable_exprs[j] = initializer_expr;
            found = true;
            break;
        }

        if (!found) {
            fatal(
                initializers[i]->location,
                "struct `%s` does not have a member variable `%s`",
                type->name,
                initializer_name);
        }
    }

    for (size_t i = 0; i < sbuf_count(member_variable_defs); ++i) {
        if (member_variable_exprs[i] == NULL) {
            fatal(
                expr->location,
                "missing initializer for member variable `%s`",
                member_variable_defs[i].name);
        }
    }

    sbuf_fini(initializer_exprs);
    sbuf_freeze(member_variable_exprs);
    struct expr* const resolved =
        expr_new_struct(expr->location, type, member_variable_exprs);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_cast(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_CAST);

    struct type const* const type =
        resolve_typespec(resolver, expr->data.cast.typespec);
    struct expr const* const rhs = resolve_expr(resolver, expr->data.cast.expr);

    // TODO: Casts to and from unsized integers are not permitted because it is
    // unclear how we should handle modulo operations when a casted-from value
    // is narrowed by the cast. Investigate what the reasonable behavior should
    // be in this situation before the operation is solidified in the language
    // for this and other (future?) unsized types.
    if (rhs->type->size == SIZEOF_UNSIZED) {
        fatal(
            rhs->location,
            "invalid cast from unsized type `%s` to `%s`",
            rhs->type->name,
            type->name);
    }
    if (type->size == SIZEOF_UNSIZED) {
        fatal(
            rhs->location,
            "invalid cast to unsized type `%s` from `%s`",
            type->name,
            rhs->type->name);
    }

    bool const valid =
        (type_is_any_integer(type) && type_is_any_integer(rhs->type))
        || (type->kind == TYPE_BOOL && rhs->type->kind == TYPE_BYTE)
        || (type->kind == TYPE_BYTE && rhs->type->kind == TYPE_BOOL)
        || (type->kind == TYPE_BOOL && type_is_any_integer(rhs->type))
        || (type_is_any_integer(type) && rhs->type->kind == TYPE_BOOL)
        || (type->kind == TYPE_BYTE && type_is_any_integer(rhs->type))
        || (type_is_any_integer(type) && rhs->type->kind == TYPE_BYTE)
        || (type->kind == TYPE_POINTER && rhs->type->kind == TYPE_USIZE)
        || (type->kind == TYPE_USIZE && rhs->type->kind == TYPE_POINTER)
        || (type->kind == TYPE_POINTER && rhs->type->kind == TYPE_POINTER)
        // TODO, XXX: We allow casts between function pointers so that structs
        // can implement interfaces. In the future we should perform type
        // checking on the parameters and return type of the two functions and
        // only allow the cast if the difference between the two functions is
        // the first pointer-to-self paramter.
        || (type->kind == TYPE_FUNCTION && rhs->type->kind == TYPE_FUNCTION);
    if (!valid) {
        fatal(
            rhs->location,
            "invalid cast from `%s` to `%s`",
            rhs->type->name,
            type->name);
    }

    struct expr* const resolved = expr_new_cast(expr->location, type, rhs);

    freeze(resolved);
    return resolved;
}

// TODO: Remove redundant logic in this function that is the same for member
// and non-member functions calls (such as the argument type checking code).
static struct expr const*
resolve_expr_call(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_CALL);

    // Member function call.
    if (expr->data.call.func->kind == CST_EXPR_ACCESS_MEMBER) {
        struct cst_expr const* const dot = expr->data.call.func;
        struct cst_expr const* const lhs = dot->data.access_member.lhs;
        char const* const name =
            dot->data.access_member.member->identifier->name;
        sbuf(struct cst_typespec const* const) const template_arguments =
            dot->data.access_member.member->template_arguments;

        struct expr const* const instance = resolve_expr(resolver, lhs);
        if (!expr_is_lvalue(instance)) {
            fatal(
                instance->location,
                "attempted to call member function `%s` on non-lvalue instance of type `%s`",
                name,
                instance->type->name);
        }

        if (instance->type->kind == TYPE_STRUCT) {
            struct member_variable const* const variable =
                type_struct_member_variable(instance->type, name);
            if (variable != NULL) {
                // Actually this is *not* a member function call - this is a
                // normal function invocation of a member variable that just
                // happens to have a function type.
                goto regular_function_call;
            }
        }

        struct symbol const* symbol = type_member_symbol(instance->type, name);
        if (symbol == NULL) {
            fatal(
                instance->location,
                "type `%s` has no member function `%s`",
                instance->type->name,
                name);
        }
        if (symbol->kind == SYMBOL_TEMPLATE) {
            symbol = xget_template_instance(
                resolver,
                dot->data.access_member.member->location,
                symbol,
                template_arguments);
        }

        if (symbol->kind != SYMBOL_FUNCTION) {
            fatal(
                instance->location,
                "type `%s` has no member function `%s`",
                instance->type->name,
                name);
        }
        struct function const* const function = symbol->data.function;
        struct type const* function_type = function->type;

        struct type const* const selfptr_type =
            type_unique_pointer(instance->type);

        sbuf(struct type const* const) const parameter_types =
            function_type->data.function.parameter_types;
        if (sbuf_count(parameter_types) == 0) {
            fatal(
                instance->location,
                "expected type `%s` for the first parameter of member function `%s` of type `%s`",
                selfptr_type->name,
                name,
                instance->type->name);
        }
        if (parameter_types[0] != selfptr_type) {
            fatal(
                instance->location,
                "expected type `%s` for the first parameter of member function `%s` of type `%s` (found `%s`)",
                selfptr_type->name,
                name,
                instance->type->name,
                parameter_types[0]->name);
        }
        size_t const arg_count = sbuf_count(expr->data.call.arguments);
        // Number of parameters minus one for the implicit pointer to self.
        size_t const expected_arg_count = sbuf_count(parameter_types) - 1;
        if (arg_count != expected_arg_count) {
            fatal(
                expr->location,
                "member function with type `%s` expects %zu argument(s) (%zu provided)",
                function_type->name,
                expected_arg_count,
                arg_count);
        }

        sbuf(struct expr const*) arguments = NULL;
        // Add the implicit pointer to self as the first argument.
        assert(expr_is_lvalue(instance));
        struct expr* const selfptr = expr_new_unary(
            expr->location, selfptr_type, UOP_ADDRESSOF, instance);
        freeze(selfptr);
        sbuf_push(arguments, selfptr);
        for (size_t i = 0; i < arg_count; ++i) {
            struct expr const* arg =
                resolve_expr(resolver, expr->data.call.arguments[i]);
            sbuf_push(arguments, arg);
        }
        sbuf_freeze(arguments);

        // Type-check function arguments.
        for (size_t i = 0; i < sbuf_count(arguments); ++i) {
            arguments[i] =
                shallow_implicit_cast(parameter_types[i], arguments[i]);
        }
        for (size_t i = 0; i < sbuf_count(parameter_types); ++i) {
            struct type const* const expected = parameter_types[i];
            struct type const* const received = arguments[i]->type;
            if (received != expected) {
                fatal(
                    arguments[i]->location,
                    "incompatible argument type `%s` (expected `%s`)",
                    received->name,
                    expected->name);
            }
        }

        assert(symbol != NULL);
        assert(symbol->kind == SYMBOL_FUNCTION);
        struct expr* member_expr = expr_new_symbol(
            dot->data.access_member.member->identifier->location, symbol);
        freeze(member_expr);

        struct expr* const resolved =
            expr_new_call(expr->location, member_expr, arguments);

        freeze(resolved);
        return resolved;
    }

    // Regular function call.
regular_function_call:;
    struct expr const* const function =
        resolve_expr(resolver, expr->data.call.func);
    if (function->type->kind != TYPE_FUNCTION) {
        fatal(
            expr->location,
            "non-callable type `%s` used in function call expression",
            function->type->name);
    }

    if (sbuf_count(expr->data.call.arguments)
        != sbuf_count(function->type->data.function.parameter_types)) {
        fatal(
            expr->location,
            "function with type `%s` expects %zu argument(s) (%zu provided)",
            function->type->name,
            sbuf_count(function->type->data.function.parameter_types),
            sbuf_count(expr->data.call.arguments));
    }

    sbuf(struct expr const*) arguments = NULL;
    for (size_t i = 0; i < sbuf_count(expr->data.call.arguments); ++i) {
        struct expr const* arg =
            resolve_expr(resolver, expr->data.call.arguments[i]);
        sbuf_push(arguments, arg);
    }
    sbuf_freeze(arguments);

    // Type-check function arguments.
    sbuf(struct type const* const) const parameter_types =
        function->type->data.function.parameter_types;
    for (size_t i = 0; i < sbuf_count(arguments); ++i) {
        arguments[i] = shallow_implicit_cast(parameter_types[i], arguments[i]);
    }
    for (size_t i = 0; i < sbuf_count(parameter_types); ++i) {
        struct type const* const expected = parameter_types[i];
        struct type const* const received = arguments[i]->type;
        if (received != expected) {
            fatal(
                arguments[i]->location,
                "incompatible argument type `%s` (expected `%s`)",
                received->name,
                expected->name);
        }
    }

    struct expr* const resolved =
        expr_new_call(expr->location, function, arguments);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_access_index(
    struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_ACCESS_INDEX);

    struct expr const* const lhs =
        resolve_expr(resolver, expr->data.access_index.lhs);
    if (lhs->type->kind != TYPE_ARRAY && lhs->type->kind != TYPE_SLICE) {
        fatal(
            lhs->location,
            "illegal index operation with left-hand-side of type `%s`",
            lhs->type->name);
    }

    struct expr const* idx =
        resolve_expr(resolver, expr->data.access_index.idx);
    idx = shallow_implicit_cast(context()->builtin.usize, idx);
    if (idx->type->kind != TYPE_USIZE) {
        fatal(
            idx->location,
            "illegal index operation with index of non-usize type `%s`",
            idx->type->name);
    }

    struct expr* const resolved =
        expr_new_access_index(expr->location, lhs, idx);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_access_slice(
    struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_ACCESS_SLICE);

    struct expr const* const lhs =
        resolve_expr(resolver, expr->data.access_slice.lhs);
    if (lhs->type->kind != TYPE_ARRAY && lhs->type->kind != TYPE_SLICE) {
        fatal(
            lhs->location,
            "illegal slice operation with left-hand-side of type `%s`",
            lhs->type->name);
    }
    if (lhs->type->kind == TYPE_ARRAY && !expr_is_lvalue(lhs)) {
        fatal(
            lhs->location,
            "left hand side of slice operation is an rvalue array");
    }

    struct expr const* begin =
        resolve_expr(resolver, expr->data.access_slice.begin);
    begin = shallow_implicit_cast(context()->builtin.usize, begin);
    if (begin->type->kind != TYPE_USIZE) {
        fatal(
            begin->location,
            "illegal slice operation with index of non-usize type `%s`",
            begin->type->name);
    }

    struct expr const* end =
        resolve_expr(resolver, expr->data.access_slice.end);
    end = shallow_implicit_cast(context()->builtin.usize, end);
    if (end->type->kind != TYPE_USIZE) {
        fatal(
            end->location,
            "illegal slice operation with index of non-usize type `%s`",
            end->type->name);
    }

    struct expr* const resolved =
        expr_new_access_slice(expr->location, lhs, begin, end);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_access_member(
    struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_ACCESS_MEMBER);

    struct expr const* const lhs =
        resolve_expr(resolver, expr->data.access_member.lhs);
    if (lhs->type->kind != TYPE_STRUCT) {
        fatal(
            lhs->location,
            "attempted member access on non-struct type `%s`",
            lhs->type->name);
    }

    char const* const member_name =
        expr->data.access_member.member->identifier->name;

    struct member_variable const* const member_variable_def =
        type_struct_member_variable(lhs->type, member_name);
    if (member_variable_def != NULL) {
        if (sbuf_count(expr->data.access_member.member->template_arguments)
            != 0) {
            fatal(
                expr->location,
                "attempted template instantiation of member variable `%s` on type `%s`",
                member_name,
                lhs->type->name);
        }
        struct expr* const resolved = expr_new_access_member_variable(
            expr->location, lhs, member_variable_def);

        freeze(resolved);
        return resolved;
    }

    struct symbol const* const member_symbol =
        type_member_symbol(lhs->type, member_name);

    if (member_symbol != NULL && member_symbol->kind == SYMBOL_CONSTANT) {
        fatal(
            expr->location,
            "attempted to take the value of member constant `%s` on type `%s`",
            member_symbol->name,
            lhs->type->name);
    }

    if (member_symbol != NULL && member_symbol->kind == SYMBOL_FUNCTION) {
        fatal(
            expr->location,
            "attempted to take the value of member function `%s` on type `%s`",
            member_symbol->name,
            lhs->type->name);
    }

    if (member_symbol != NULL && member_symbol->kind == SYMBOL_TEMPLATE) {
        fatal(
            expr->location,
            "attempted to take the value of member template `%s` on type `%s`",
            member_symbol->name,
            lhs->type->name);
    }

    assert(member_symbol == NULL);
    fatal(
        lhs->location,
        "struct `%s` has no member `%s`",
        lhs->type->name,
        member_name);
    return NULL;
}

// Basically a copy-paste of the logic from resolve_expr_unary and
// resolve_expr_unary_dereference with the unary operator fields exchanged for
// the access dereference fields in the cst_expr.
static struct expr const*
resolve_expr_access_dereference(
    struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_ACCESS_DEREFERENCE);

    struct expr const* const lhs =
        resolve_expr(resolver, expr->data.access_dereference.lhs);

    if (lhs->type->kind != TYPE_POINTER) {
        fatal(
            lhs->location,
            "cannot dereference non-pointer type `%s`",
            lhs->type->name);
    }
    struct expr* const resolved = expr_new_unary(
        expr->location, lhs->type->data.pointer.base, UOP_DEREFERENCE, lhs);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_sizeof(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);

    struct type const* const rhs =
        resolve_typespec(resolver, expr->data.sizeof_.rhs);
    if (rhs->size == SIZEOF_UNSIZED) {
        fatal(expr->location, "type `%s` has no defined size", rhs->name);
    }

    struct expr* const resolved = expr_new_sizeof(expr->location, rhs);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_alignof(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);

    struct type const* const rhs =
        resolve_typespec(resolver, expr->data.alignof_.rhs);
    if (rhs->align == ALIGNOF_UNSIZED) {
        fatal(expr->location, "type `%s` has no defined alignment", rhs->name);
    }

    struct expr* const resolved = expr_new_alignof(expr->location, rhs);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_unary(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_UNARY);

    // While a human would identify the integer expression -128s8 as the hex
    // byte 0x80, the parser identifies the integer expression -128s8 as the
    // unary negation (via the unary - operator) of the integer literal 128s8.
    // Positive 128 is an out-of-range value for an integer of type s8 (the max
    // being  positive 127) even though the intended value of -128 *is* within
    // the range of an s8. Here we identify the special case where a + or -
    // token is immediately followed by an integer token and combine the two
    // into a single integer expression.
    struct token const* const op = expr->data.unary.op;
    bool const is_sign = (op->kind == TOKEN_PLUS) || (op->kind == TOKEN_DASH);
    struct cst_expr const* const cst_rhs = expr->data.unary.rhs;
    if (is_sign && cst_rhs->kind == CST_EXPR_INTEGER) {
        struct cst_integer const* const cst_integer = cst_rhs->data.integer;
        struct bigint const* value = cst_integer->value;
        if (op->kind == TOKEN_DASH) {
            struct bigint* const tmp = bigint_new(value);
            bigint_neg(tmp, value);
            bigint_freeze(tmp);
            value = tmp;
        }
        struct type const* const type = integer_literal_suffix_to_type(
            cst_integer->location, cst_integer->suffix);

        struct expr* const resolved =
            expr_new_integer(&op->location, type, value);

        freeze(resolved);
        return resolved;
    }

    struct expr const* const rhs = resolve_expr(resolver, cst_rhs);
    switch (op->kind) {
    case TOKEN_NOT: {
        return resolve_expr_unary_logical(resolver, op, UOP_NOT, rhs);
    }
    case TOKEN_COUNTOF: {
        return resolve_expr_unary_countof(resolver, op, rhs);
    }
    case TOKEN_PLUS: {
        return resolve_expr_unary_arithmetic(resolver, op, UOP_POS, rhs);
    }
    case TOKEN_DASH: {
        if (type_is_unsigned_integer(rhs->type)) {
            fatal(
                &op->location,
                "invalid argument of type `%s` in unary `%s` expression",
                rhs->type->name,
                token_kind_to_cstr(op->kind));
        }
        return resolve_expr_unary_arithmetic(resolver, op, UOP_NEG, rhs);
    }
    case TOKEN_TILDE: {
        return resolve_expr_unary_bitwise(resolver, op, UOP_BITNOT, rhs);
    }
    case TOKEN_STAR: {
        return resolve_expr_unary_dereference(resolver, op, rhs);
    }
    case TOKEN_AMPERSAND: {
        return resolve_expr_unary_addressof(resolver, op, rhs);
    }
    default: {
        UNREACHABLE();
    }
    }

    UNREACHABLE();
    return NULL;
}

static struct expr const*
resolve_expr_unary_logical(
    struct resolver* resolver,
    struct token const* op,
    enum uop_kind uop,
    struct expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(rhs != NULL);
    (void)resolver;

    if (rhs->type->kind != TYPE_BOOL) {
        fatal(
            &op->location,
            "invalid argument of type `%s` in unary `%s` expression",
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }

    struct expr* const resolved =
        expr_new_unary(&op->location, rhs->type, uop, rhs);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_unary_arithmetic(
    struct resolver* resolver,
    struct token const* op,
    enum uop_kind uop,
    struct expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(rhs != NULL);
    (void)resolver;

    if (!type_is_any_integer(rhs->type)) {
        fatal(
            &op->location,
            "invalid argument of type `%s` in unary `%s` expression",
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }

    struct expr* const resolved =
        expr_new_unary(&op->location, rhs->type, uop, rhs);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_unary_bitwise(
    struct resolver* resolver,
    struct token const* op,
    enum uop_kind uop,
    struct expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(rhs != NULL);
    (void)resolver;

    if (rhs->type->size == SIZEOF_UNSIZED) {
        fatal(
            &op->location,
            "unsized type `%s` in unary `%s` expression has no bit-representation",
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }
    if (!(rhs->type->kind == TYPE_BYTE || type_is_any_integer(rhs->type))) {
        fatal(
            rhs->location,
            "cannot apply bitwise NOT to type `%s`",
            rhs->type->name);
    }
    struct expr* const resolved =
        expr_new_unary(&op->location, rhs->type, uop, rhs);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_unary_dereference(
    struct resolver* resolver, struct token const* op, struct expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(op->kind == TOKEN_STAR);
    assert(rhs != NULL);
    (void)resolver;

    if (rhs->type->kind != TYPE_POINTER) {
        fatal(
            rhs->location,
            "cannot dereference non-pointer type `%s`",
            rhs->type->name);
    }
    struct expr* const resolved = expr_new_unary(
        &op->location, rhs->type->data.pointer.base, UOP_DEREFERENCE, rhs);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_unary_addressof(
    struct resolver* resolver, struct token const* op, struct expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(op->kind == TOKEN_AMPERSAND);
    assert(rhs != NULL);
    (void)resolver;

    if (!expr_is_lvalue(rhs)) {
        fatal(rhs->location, "cannot take the address of a non-lvalue");
    }

    struct expr* const resolved = expr_new_unary(
        &op->location, type_unique_pointer(rhs->type), UOP_ADDRESSOF, rhs);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_unary_countof(
    struct resolver* resolver, struct token const* op, struct expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(op->kind == TOKEN_COUNTOF);
    assert(rhs != NULL);
    (void)resolver;

    if (rhs->type->kind != TYPE_ARRAY && rhs->type->kind != TYPE_SLICE) {
        fatal(
            rhs->location,
            "expected array or slice type (received `%s`)",
            rhs->type->name);
    }

    struct expr* const resolved = expr_new_unary(
        &op->location, context()->builtin.usize, UOP_COUNTOF, rhs);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_binary(struct resolver* resolver, struct cst_expr const* expr)
{
    assert(resolver != NULL);
    assert(expr != NULL);
    assert(expr->kind == CST_EXPR_BINARY);

    struct expr const* const lhs =
        resolve_expr(resolver, expr->data.binary.lhs);
    struct expr const* const rhs =
        resolve_expr(resolver, expr->data.binary.rhs);
    struct token const* const op = expr->data.binary.op;
    switch (op->kind) {
    case TOKEN_OR: {
        return resolve_expr_binary_logical(resolver, op, BOP_OR, lhs, rhs);
    }
    case TOKEN_AND: {
        return resolve_expr_binary_logical(resolver, op, BOP_AND, lhs, rhs);
    }
    case TOKEN_SHL: {
        return resolve_expr_binary_shift(resolver, op, BOP_SHL, lhs, rhs);
    }
    case TOKEN_SHR: {
        return resolve_expr_binary_shift(resolver, op, BOP_SHR, lhs, rhs);
    }
    case TOKEN_EQ: {
        return resolve_expr_binary_compare_equality(
            resolver, op, BOP_EQ, lhs, rhs);
    }
    case TOKEN_NE: {
        return resolve_expr_binary_compare_equality(
            resolver, op, BOP_NE, lhs, rhs);
    }
    case TOKEN_LE: {
        return resolve_expr_binary_compare_order(
            resolver, op, BOP_LE, lhs, rhs);
    }
    case TOKEN_LT: {
        return resolve_expr_binary_compare_order(
            resolver, op, BOP_LT, lhs, rhs);
    }
    case TOKEN_GE: {
        return resolve_expr_binary_compare_order(
            resolver, op, BOP_GE, lhs, rhs);
    }
    case TOKEN_GT: {
        return resolve_expr_binary_compare_order(
            resolver, op, BOP_GT, lhs, rhs);
    }
    case TOKEN_PLUS: {
        return resolve_expr_binary_arithmetic(resolver, op, BOP_ADD, lhs, rhs);
    }
    case TOKEN_DASH: {
        return resolve_expr_binary_arithmetic(resolver, op, BOP_SUB, lhs, rhs);
    }
    case TOKEN_STAR: {
        return resolve_expr_binary_arithmetic(resolver, op, BOP_MUL, lhs, rhs);
    }
    case TOKEN_FSLASH: {
        return resolve_expr_binary_arithmetic(resolver, op, BOP_DIV, lhs, rhs);
    }
    case TOKEN_PIPE: {
        return resolve_expr_binary_bitwise(resolver, op, BOP_BITOR, lhs, rhs);
    }
    case TOKEN_CARET: {
        return resolve_expr_binary_bitwise(resolver, op, BOP_BITXOR, lhs, rhs);
    }
    case TOKEN_AMPERSAND: {
        return resolve_expr_binary_bitwise(resolver, op, BOP_BITAND, lhs, rhs);
    }
    default: {
        UNREACHABLE();
    }
    }

    UNREACHABLE();
    return NULL;
}

static struct expr const*
resolve_expr_binary_logical(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct expr const* lhs,
    struct expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);
    (void)resolver;

    bool const valid = lhs->type == rhs->type && lhs->type->kind == TYPE_BOOL
        && rhs->type->kind == TYPE_BOOL;
    if (!valid) {
        fatal(
            &op->location,
            "invalid arguments of types `%s` and `%s` in binary `%s` expression",
            lhs->type->name,
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }

    struct type const* const type = context()->builtin.bool_;
    struct expr* const resolved =
        expr_new_binary(&op->location, type, bop, lhs, rhs);

    freeze(resolved);
    return resolved;
}

static struct expr const*
resolve_expr_binary_shift(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct expr const* lhs,
    struct expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);
    (void)resolver;

    if (!type_is_any_integer(lhs->type)) {
        fatal(
            &op->location,
            "invalid left-hand argument of type `%s` in binary `%s` expression",
            lhs->type->name,
            token_kind_to_cstr(op->kind));
    }
    if (lhs->type->size == SIZEOF_UNSIZED) {
        fatal(
            &op->location,
            "unsized type `%s` in binary `%s` expression has no bit-representation",
            lhs->type->name,
            token_kind_to_cstr(op->kind));
    }

    rhs = shallow_implicit_cast(context()->builtin.usize, rhs);
    if (rhs->type != context()->builtin.usize) {
        fatal(
            &op->location,
            "invalid non-usize right-hand argument of type `%s` in binary `%s` expression",
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }

    struct type const* const type = lhs->type;

    struct expr* const resolved =
        expr_new_binary(&op->location, type, bop, lhs, rhs);
    freeze(resolved);

    return resolved;
}

static struct expr const*
resolve_expr_binary_compare_equality(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct expr const* lhs,
    struct expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);
    (void)resolver;

    lhs = shallow_implicit_cast(rhs->type, lhs);
    rhs = shallow_implicit_cast(lhs->type, rhs);

    if (lhs->type != rhs->type) {
        fatal(
            &op->location,
            "invalid arguments of types `%s` and `%s` in binary `%s` expression",
            lhs->type->name,
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }
    struct type const* const xhs_type = lhs->type;
    if (!type_can_compare_equality(xhs_type)) {
        fatal(
            &op->location,
            "invalid arguments of type `%s` in binary `%s` expression",
            xhs_type->name,
            token_kind_to_cstr(op->kind));
    }

    struct expr* resolved =
        expr_new_binary(&op->location, context()->builtin.bool_, bop, lhs, rhs);
    freeze(resolved);

    // Constant fold integer literal constant expression.
    if (lhs->kind == EXPR_INTEGER && rhs->kind == EXPR_INTEGER) {
        lhs = shallow_implicit_cast(rhs->type, lhs);
        rhs = shallow_implicit_cast(lhs->type, rhs);

        struct value* const value = eval_rvalue(resolved);
        value_freeze(value);

        assert(value->type->kind == TYPE_BOOL);
        resolved = expr_new_boolean(resolved->location, value->data.boolean);
        freeze(resolved);
    }

    return resolved;
}

static struct expr const*
resolve_expr_binary_compare_order(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct expr const* lhs,
    struct expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);
    (void)resolver;

    lhs = shallow_implicit_cast(rhs->type, lhs);
    rhs = shallow_implicit_cast(lhs->type, rhs);

    if (lhs->type != rhs->type) {
        fatal(
            &op->location,
            "invalid arguments of types `%s` and `%s` in binary `%s` expression",
            lhs->type->name,
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }

    struct type const* const xhs_type = lhs->type;
    if (!type_can_compare_order(xhs_type)) {
        fatal(
            &op->location,
            "invalid arguments of type `%s` in binary `%s` expression",
            xhs_type->name,
            token_kind_to_cstr(op->kind));
    }

    struct expr* resolved =
        expr_new_binary(&op->location, context()->builtin.bool_, bop, lhs, rhs);
    freeze(resolved);

    // Constant fold integer literal constant expression.
    if (lhs->kind == EXPR_INTEGER && rhs->kind == EXPR_INTEGER) {
        lhs = shallow_implicit_cast(rhs->type, lhs);
        rhs = shallow_implicit_cast(lhs->type, rhs);

        struct value* const value = eval_rvalue(resolved);
        value_freeze(value);

        assert(value->type->kind == TYPE_BOOL);
        resolved = expr_new_boolean(resolved->location, value->data.boolean);
        freeze(resolved);
    }

    return resolved;
}

static struct expr const*
resolve_expr_binary_arithmetic(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct expr const* lhs,
    struct expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);
    (void)resolver;

    lhs = shallow_implicit_cast(rhs->type, lhs);
    rhs = shallow_implicit_cast(lhs->type, rhs);

    bool const valid = lhs->type == rhs->type && type_is_any_integer(lhs->type)
        && type_is_any_integer(rhs->type);
    if (!valid) {
        fatal(
            &op->location,
            "invalid arguments of types `%s` and `%s` in binary `%s` expression",
            lhs->type->name,
            rhs->type->name,
            token_kind_to_cstr(op->kind));
    }

    struct type const* const type = lhs->type; // Arbitrarily use lhs.
    struct expr* resolved = expr_new_binary(&op->location, type, bop, lhs, rhs);
    freeze(resolved);

    // Constant fold integer literal constant expression.
    if (lhs->kind == EXPR_INTEGER && rhs->kind == EXPR_INTEGER) {
        lhs = shallow_implicit_cast(rhs->type, lhs);
        rhs = shallow_implicit_cast(lhs->type, rhs);

        struct value* const value = eval_rvalue(resolved);
        value_freeze(value);

        assert(type_is_any_integer(value->type));
        resolved = expr_new_integer(
            resolved->location, resolved->type, value->data.integer);
        freeze(resolved);
    }

    return resolved;
}

static struct expr const*
resolve_expr_binary_bitwise(
    struct resolver* resolver,
    struct token const* op,
    enum bop_kind bop,
    struct expr const* lhs,
    struct expr const* rhs)
{
    assert(resolver != NULL);
    assert(op != NULL);
    assert(lhs != NULL);
    assert(rhs != NULL);
    (void)resolver;

    lhs = shallow_implicit_cast(rhs->type, lhs);
    rhs = shallow_implicit_cast(lhs->type, rhs);

    if (lhs->type != rhs->type) {
        goto invalid_operand_types;
    }
    struct type const* type = lhs->type; // Arbitrarily use lhs.
    if (type->size == SIZEOF_UNSIZED) {
        fatal(
            &op->location,
            "unsized types `%s` in binary `%s` expression have no bit-representation",
            type->name,
            token_kind_to_cstr(op->kind));
    }

    bool const valid = type->kind == TYPE_BOOL || type->kind == TYPE_BYTE
        || type_is_any_integer(type);
    if (!valid) {
        goto invalid_operand_types;
    }

    struct expr* resolved = expr_new_binary(&op->location, type, bop, lhs, rhs);
    freeze(resolved);

    // Constant fold integer literal constant expression.
    if (lhs->kind == EXPR_INTEGER && rhs->kind == EXPR_INTEGER) {
        lhs = shallow_implicit_cast(rhs->type, lhs);
        rhs = shallow_implicit_cast(lhs->type, rhs);

        struct value* const value = eval_rvalue(resolved);
        value_freeze(value);

        assert(type_is_any_integer(value->type));
        resolved = expr_new_integer(
            resolved->location, resolved->type, value->data.integer);
        freeze(resolved);
    }

    return resolved;

invalid_operand_types:
    fatal(
        &op->location,
        "invalid arguments of types `%s` and `%s` in binary `%s` expression",
        lhs->type->name,
        rhs->type->name,
        token_kind_to_cstr(op->kind));
}

static struct block const*
resolve_block(
    struct resolver* resolver,
    struct symbol_table* symbol_table,
    struct cst_block const* block)
{
    assert(resolver->current_function != NULL);
    assert(symbol_table != NULL);

    struct symbol_table* const save_symbol_table =
        resolver->current_symbol_table;
    resolver->current_symbol_table = symbol_table;
    int const save_rbp_offset = resolver->current_rbp_offset;
    struct stmt const* save_current_defer = resolver->current_defer;

    sbuf(struct stmt const*) stmts = NULL;
    for (size_t i = 0; i < sbuf_count(block->stmts); ++i) {
        struct stmt const* const resolved_stmt =
            resolve_stmt(resolver, block->stmts[i]);
        if (resolved_stmt != NULL) {
            sbuf_push(stmts, resolved_stmt);
        }
    }
    sbuf_freeze(stmts);

    struct block* const resolved = block_new(
        block->location,
        symbol_table,
        stmts,
        resolver->current_defer,
        save_current_defer);

    freeze(resolved);
    resolver->current_symbol_table = save_symbol_table;
    resolver->current_rbp_offset = save_rbp_offset;
    resolver->current_defer = save_current_defer;
    return resolved;
}

static struct type const*
resolve_typespec(struct resolver* resolver, struct cst_typespec const* typespec)
{
    assert(resolver != NULL);
    assert(typespec != NULL);

    switch (typespec->kind) {
    case TYPESPEC_SYMBOL: {
        return resolve_typespec_symbol(resolver, typespec);
    }
    case TYPESPEC_FUNCTION: {
        return resolve_typespec_function(resolver, typespec);
    }
    case TYPESPEC_POINTER: {
        return resolve_typespec_pointer(resolver, typespec);
    }
    case TYPESPEC_ARRAY: {
        return resolve_typespec_array(resolver, typespec);
    }
    case TYPESPEC_SLICE: {
        return resolve_typespec_slice(resolver, typespec);
    }
    case TYPESPEC_TYPEOF: {
        return resolve_typespec_typeof(resolver, typespec);
    }
    }

    UNREACHABLE();
    return NULL;
}

static struct type const*
resolve_typespec_symbol(
    struct resolver* resolver, struct cst_typespec const* typespec)
{
    assert(resolver != NULL);
    assert(typespec != NULL);
    assert(typespec->kind == TYPESPEC_SYMBOL);

    struct symbol const* const symbol =
        xget_symbol(resolver, typespec->data.symbol);
    if (symbol->kind == SYMBOL_TEMPLATE) {
        fatal(
            typespec->location,
            "template `%s` must be instantiated",
            symbol->name);
    }
    if (symbol->kind != SYMBOL_TYPE) {
        fatal(
            typespec->location, "identifier `%s` is not a type", symbol->name);
    }

    return symbol_xget_type(symbol);
}

static struct type const*
resolve_typespec_function(
    struct resolver* resolver, struct cst_typespec const* typespec)
{
    assert(resolver != NULL);
    assert(typespec != NULL);
    assert(typespec->kind == TYPESPEC_FUNCTION);

    sbuf(struct cst_typespec const* const) const parameter_typespecs =
        typespec->data.function.parameter_typespecs;

    sbuf(struct type const*) parameter_types = NULL;
    sbuf_resize(parameter_types, sbuf_count(parameter_typespecs));
    for (size_t i = 0; i < sbuf_count(parameter_typespecs); ++i) {
        parameter_types[i] = resolve_typespec(resolver, parameter_typespecs[i]);
    }
    sbuf_freeze(parameter_types);

    struct type const* const return_type =
        resolve_typespec(resolver, typespec->data.function.return_typespec);

    return type_unique_function(parameter_types, return_type);
}

static struct type const*
resolve_typespec_pointer(
    struct resolver* resolver, struct cst_typespec const* typespec)
{
    assert(resolver != NULL);
    assert(typespec != NULL);
    assert(typespec->kind == TYPESPEC_POINTER);

    struct type const* const base =
        resolve_typespec(resolver, typespec->data.pointer.base);
    return type_unique_pointer(base);
}

static struct type const*
resolve_typespec_array(
    struct resolver* resolver, struct cst_typespec const* typespec)
{
    struct expr const* count_expr =
        resolve_expr(resolver, typespec->data.array.count);
    count_expr = shallow_implicit_cast(context()->builtin.usize, count_expr);

    if (count_expr->type != context()->builtin.usize) {
        fatal(
            count_expr->location,
            "illegal array count with non-usize type `%s`",
            count_expr->type->name);
    }

    struct value* const count_value = eval_rvalue(count_expr);

    assert(count_value->type == context()->builtin.usize);
    size_t count = 0u;
    if (bigint_to_uz(&count, count_value->data.integer)) {
        fatal(
            count_expr->location,
            "array count too large (received %s)",
            bigint_to_new_cstr(count_value->data.integer, NULL));
    }
    value_del(count_value);

    struct type const* const base =
        resolve_typespec(resolver, typespec->data.array.base);
    return type_unique_array(count, base);
}

static struct type const*
resolve_typespec_slice(
    struct resolver* resolver, struct cst_typespec const* typespec)
{
    assert(resolver != NULL);
    assert(typespec != NULL);
    assert(typespec->kind == TYPESPEC_SLICE);

    struct type const* const base =
        resolve_typespec(resolver, typespec->data.slice.base);
    return type_unique_slice(base);
}

static struct type const*
resolve_typespec_typeof(
    struct resolver* resolver, struct cst_typespec const* typespec)
{
    assert(resolver != NULL);
    assert(typespec != NULL);
    assert(typespec->kind == TYPESPEC_TYPEOF);

    struct expr const* const expr =
        resolve_expr(resolver, typespec->data.typeof_.expr);
    return expr->type;
}

void
resolve(struct module* module)
{
    assert(module != NULL);

    struct resolver* const resolver = resolver_new(module);

    // Module namespace.
    if (module->cst->namespace != NULL) {
        sbuf(struct cst_identifier const* const) const identifiers =
            module->cst->namespace->identifiers;

        char const* nsname = NULL;
        char const* nsaddr = NULL;
        for (size_t i = 0; i < sbuf_count(identifiers); ++i) {
            char const* const name = identifiers[i]->name;
            struct source_location const* const location =
                identifiers[i]->location;

            nsname = qualified_name(nsname, name);
            nsaddr = qualified_addr(nsaddr, name);

            struct symbol_table* const module_table =
                symbol_table_new(resolver->current_symbol_table);
            struct symbol_table* const export_table =
                symbol_table_new(resolver->current_export_table);
            sbuf_push(context()->chilling_symbol_tables, module_table);
            sbuf_push(context()->chilling_symbol_tables, export_table);

            struct symbol* const module_nssymbol =
                symbol_new_namespace(location, nsname, module_table);
            struct symbol* const export_nssymbol =
                symbol_new_namespace(location, nsname, module_table);
            freeze(module_nssymbol);
            freeze(export_nssymbol);

            symbol_table_insert(
                resolver->current_symbol_table, name, module_nssymbol, false);
            symbol_table_insert(
                resolver->current_export_table, name, export_nssymbol, false);
            resolver->current_symbol_table = module_table;
            resolver->current_export_table = export_table;
        }

        resolver->current_static_addr_prefix = nsaddr;
    }

    // Resolve imports.
    for (size_t i = 0; i < sbuf_count(module->cst->imports); ++i) {
        resolve_import(resolver, module->cst->imports[i]);
    }

    // Resolve top-level declarations.
    sbuf(struct cst_decl const* const) const ordered = module->ordered;
    for (size_t i = 0; i < sbuf_count(ordered); ++i) {
        // Structs have their symbols created before all other declarations to
        // allow for self referential and cross referential struct
        // declarations. These structs are then completed as needed based on
        // their topological order. This is roughly equivalent to forward
        // declaring structs in C.
        struct cst_decl const* const decl = module->ordered[i];
        if (decl->kind != CST_DECL_STRUCT) {
            continue;
        }

        struct symbol const* const symbol = resolve_decl_struct(resolver, decl);
        // If this module declares a namespace then top-level declarations will
        // have been added under the (exported) module namespace and should
        // *not* be added to the module export table or global symbol table
        // using their unqualified names.
        if (module->cst->namespace == NULL) {
            symbol_table_insert(
                resolver->current_export_table, symbol->name, symbol, false);
            symbol_table_insert(
                context()->global_symbol_table, symbol->name, symbol, false);
        }
    }
    for (size_t i = 0; i < sbuf_count(ordered); ++i) {
        struct cst_decl const* const decl = module->ordered[i];

        // If the declaration was a non-template struct then it has already been
        // resolved and must now be completed.
        if (decl->kind == CST_DECL_STRUCT) {
            struct symbol const* const symbol = symbol_table_lookup_local(
                resolver->current_symbol_table, decl->name);
            assert(symbol != NULL);
            if (symbol->kind != SYMBOL_TYPE) {
                assert(symbol->kind == SYMBOL_TEMPLATE);
                continue;
            }

            complete_struct(resolver, symbol, decl);
            continue;
        }

        struct symbol const* const symbol =
            resolve_decl(resolver, module->ordered[i]);
        // If this module declares a namespace then top-level declarations will
        // have been added under the (exported) module namespace and should
        // *not* be added to the module export table or global symbol table
        // using their unqualified names.
        if (module->cst->namespace == NULL) {
            symbol_table_insert(
                resolver->current_export_table, decl->name, symbol, false);
            symbol_table_insert(
                context()->global_symbol_table, decl->name, symbol, false);
        }
    }

    for (size_t i = 0; i < sbuf_count(resolver->incomplete_functions); ++i) {
        complete_function(resolver, resolver->incomplete_functions[i]);
    }

    resolver_del(resolver);
}
