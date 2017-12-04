#include "typetree.h"
#include <lacc/array.h>
#include <lacc/context.h>
#include <lacc/symbol.h>

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const Type
    basic_type__void           = { T_VOID },
    basic_type__bool           = { T_BOOL },
    basic_type__char           = { T_CHAR },
    basic_type__short          = { T_SHORT },
    basic_type__int            = { T_INT },
    basic_type__long           = { T_LONG },
    basic_type__unsigned_char  = { T_CHAR, 1 },
    basic_type__unsigned_short = { T_SHORT, 1 },
    basic_type__unsigned_int   = { T_INT, 1 },
    basic_type__unsigned_long  = { T_LONG, 1 },
    basic_type__float          = { T_FLOAT },
    basic_type__double         = { T_DOUBLE },
    basic_type__long_double    = { T_LDOUBLE };

/*
 * Hidden full representation of types. Type objects reference one of
 * these for aggregate and nested structures.
 */
struct typetree {
    enum type type;
    unsigned int is_unsigned : 1;
    unsigned int is_const : 1;
    unsigned int is_volatile : 1;
    unsigned int is_restrict : 1;
    unsigned int is_vararg : 1;
    unsigned int is_flexible : 1;
    unsigned int is_vla : 1;

    /*
     * Total storage size in bytes for struct, union and basic types,
     * equal to what is returned for sizeof. Number of elements in case
     * of array type.
     */
    size_t size;

    /*
     * Symbol holding size of variable length array. Type of variable is
     * always size_t (unsigned long).
     *
     * Special value NULL means any length '*', if is_vla is set. The
     * flag is always set if vlen is not NULL.
     *
     * Normal size member is zero for VLA types, and size_of still
     * returns 0 without error.
     */
    const struct symbol *vlen;

    /* Function parameters, or struct/union members. */
    array_of(struct member) members;

    /*
     * Function return value, pointer target, array base, or pointer to
     * tagged struct or union type. Tag indirections are used to avoid
     * loops in type trees.
     */
    Type next;

    /*
     * Reference to typedef, or struct, union or enum tag. Stored in
     * order to be able to print self-referential types.
     */
    const struct symbol *tag;
};

/*
 * All types have a number, indexing into a global list. This list only
 * grows.
 */
static array_of(struct typetree) types;

static void deallocate_typetrees(void)
{
    int i;
    struct typetree *t;

    for (i = 0; i < array_len(&types); ++i) {
        t = &array_get(&types, i);
        array_clear(&t->members);
    }

    array_clear(&types);
}

static struct typetree *get_typetree_handle(int ref)
{
    assert(ref > 0);
    assert(ref <= array_len(&types));

    return &array_get(&types, ref - 1);
}

static Type get_type_handle(int ref)
{
    Type type = {0};
    struct typetree *t;

    t = get_typetree_handle(ref);
    switch (t->type) {
    case T_POINTER:
        if (t->next.is_pointer) {
            assert(0);
        } else {
            type.type = t->next.type;
            type.is_unsigned = t->next.is_unsigned;
            type.is_const = t->next.is_const;
            type.is_volatile = t->next.is_volatile;
            type.is_restrict = t->next.is_restrict;
            type.ref = t->next.ref;
            type.is_pointer = 1;
            type.is_pointer_const = t->is_const;
            type.is_pointer_volatile = t->is_volatile;
            type.is_pointer_restrict = t->is_restrict;
        }
        break;
    case T_FUNCTION:
    case T_ARRAY:
    case T_STRUCT:
    case T_UNION:
        type.ref = ref;
    default:
        type.type = t->type;
        type.is_unsigned = t->is_unsigned;
        type.is_volatile = t->is_volatile;
        type.is_const = t->is_const;
        type.is_restrict = t->is_restrict;
        break;
    }

    return type;
}

static Type alloc_type(enum type tt)
{
    static int init;
    Type type = {0};
    struct typetree t = {0};

    if (!init) {
        init = 1;
        atexit(deallocate_typetrees);
    }

    t.type = tt;
    array_push_back(&types, t);
    type.type = tt;
    type.ref = array_len(&types);
    return type;
}

static Type remove_qualifiers(Type type)
{
    if (type.is_pointer) {
        type.is_pointer_const = 0;
        type.is_pointer_volatile = 0;
        type.is_pointer_restrict = 0;
    } else {
        type.is_const = 0;
        type.is_volatile = 0;
        type.is_restrict = 0;
    }

    return type;
}

/*
 * Add member to type signature list. Create the list if this is the
 * first member added. Calculate new size of parent type based on the
 * type added.
 *
 * Verify that a named struct or union member does not already exist.
 */
static struct member *add_member(Type parent, struct member m)
{
    struct typetree *t;

    assert(is_struct_or_union(parent) || is_function(parent));
    t = get_typetree_handle(parent.ref);
    if (!str_cmp(m.name, str_init("..."))) {
        assert(!t->is_vararg);
        assert(is_function(parent));
        t->is_vararg = 1;
        return NULL;
    }

    if (m.name.len && find_type_member(parent, m.name, NULL)) {
        error("Member '%s' already exists.", str_raw(m.name));
        exit(1);
    }

    array_push_back(&t->members, m);
    if (is_struct_or_union(parent)) {
        if (size_of(m.type) == 0) {
            if (is_array(m.type) && !t->is_flexible) {
                t->is_flexible = 1;
            } else {
                error("Member '%s' has incomplete type.", str_raw(m.name));
                exit(1);
            }
        }
        if (is_flexible(m.type)) {
            if (is_struct(parent)) {
                error("Cannot add flexible struct member.");
                exit(1);
            }
            t->is_flexible = 1;
        }
        if (LONG_MAX - m.offset < size_of(m.type)) {
            error("Object is too large.");
            exit(1);
        }
        if (t->size < m.offset + size_of(m.type)) {
            t->size = m.offset + size_of(m.type);
        }
    }

    return &array_back(&t->members);
}

/*
 * Adjust alignment to next integer width after adding an unnamed zero-
 * width field member.
 *
 * Insert a new field member with the appropriate padding bits. If the
 * previous member is not a field, insert padding to align the next
 * member to integer width.
 *
 * If there are no members, nothing to do. Starting with a zero width
 * field does not put the next member at offset 4.
 */
static void reset_field_alignment(Type type)
{
    struct typetree *t;
    int n, d;
    const struct member *m;

    assert(is_struct(type));
    n = nmembers(type);
    if (n) {
        m = get_member(type, n - 1);
        t = get_typetree_handle(type.ref);
        if (m->field_width) {
            d = m->field_offset + m->field_width;
            if (d < 32) {
                type_add_field(type, str_init(""), basic_type__int, 32 - d);
            }
        } else if (t->size % 4 != 0) {
            t->size += t->size % 4;
        }
    }
}

/*
 * Add necessary padding to parent struct such that new member type can
 * be added. Union types need no padding.
 */
static size_t adjust_member_alignment(Type parent, Type type)
{
    struct typetree *t;
    size_t align = 0;

    assert(is_struct_or_union(parent));
    if (is_struct(parent)) {
        t = get_typetree_handle(parent.ref);
        align = type_alignment(type);
        if (t->size % align) {
            t->size += align - (t->size % align);
            assert(t->size % align == 0);
        }

        align = t->size;
    }

    return align;
}

Type type_create(enum type tt, ...)
{
    Type type = {0}, next;
    const struct symbol *sym;
    struct typetree *t;
    size_t elem;
    va_list args;

    va_start(args, tt);
    switch (tt) {
    case T_POINTER:
        next = va_arg(args, Type);
        if (next.is_pointer) {
            type = alloc_type(T_POINTER);
            t = get_typetree_handle(type.ref);
            t->is_const = is_const(next);
            t->is_volatile = is_volatile(next);
            next = remove_qualifiers(next);
            next.is_pointer = 0;
            t->next = next;
        } else {
            type = next;
            type.is_pointer = 1;
        }
        break;
    case T_ARRAY:
        next = va_arg(args, Type);
        elem = va_arg(args, size_t);
        sym = va_arg(args, const struct symbol *);
        if (elem * size_of(next) > LONG_MAX) {
            error("Array is too large (%lu elements).", elem);
            exit(1);
        }
        type = alloc_type(T_ARRAY);
        t = get_typetree_handle(type.ref);
        t->size = elem;
        t->next = next;
        if (sym) {
            t->vlen = sym;
            t->is_vla = 1;
        }
        break;
    case T_FUNCTION:
        next = va_arg(args, Type);
        type = alloc_type(T_FUNCTION);
        t = get_typetree_handle(type.ref);
        t->next = next;
        break;
    case T_STRUCT:
    case T_UNION:
        type = alloc_type(tt);
        break;
    default: assert(0);
    }

    va_end(args);
    return type;
}

Type type_set_const(Type type)
{
    if (type.is_pointer) {
        type.is_pointer_const = 1;
    } else {
        type.is_const = 1;
    }

    return type;
}

Type type_set_volatile(Type type)
{
    if (type.is_pointer) {
        type.is_pointer_volatile = 1;
    } else {
        type.is_volatile = 1;
    }

    return type;
}

Type type_set_restrict(Type type)
{
    if (!is_pointer(type)) {
        error("Cannot apply 'restrict' qualifier to non-pointer types.");
        exit(1);
    }

    if (type.is_pointer) {
        type.is_pointer_restrict = 1;
    } else {
        type.is_restrict = 1;
    }

    return type;
}

Type type_apply_qualifiers(Type type, Type other)
{
    if (is_const(other))
        type = type_set_const(type);
    if (is_volatile(other))
        type = type_set_volatile(type);
    if (is_restrict(other))
        type = type_set_restrict(type);
    return type;
}

Type type_patch_declarator(Type head, Type target)
{
    struct typetree *t;
    Type next;

    assert(is_function(target) || is_array(target));
    if (is_void(head)) {
        next = target;
    } else {
        if (is_pointer(head)) {
            next = type_next(head);
            next = type_patch_declarator(next, target);
            next = type_create(T_POINTER, next);
            next = type_apply_qualifiers(next, head);
        } else {
            assert(is_function(head) || is_array(head));
            t = get_typetree_handle(head.ref);
            t->next = type_patch_declarator(t->next, target);
            next = head;
        }
    }

    return next;
}

void type_clean_prototype(Type type)
{
    int i;
    struct member *m;
    struct typetree *t;

    switch (type_of(type)) {
    case T_POINTER:
        type_clean_prototype(type_next(type));
        break;
    case T_ARRAY:
        t = get_typetree_handle(type.ref);
        if (t->is_vla) {
            t->vlen = NULL;
        }
        type_clean_prototype(t->next);
        break;
    case T_STRUCT:
    case T_UNION:
        t = get_typetree_handle(type.ref);
        if (t->tag)
            break;
    case T_FUNCTION:
        for (i = 0; i < nmembers(type); ++i) {
            m = get_member(type, i);
            m->sym = NULL;
            type_clean_prototype(m->type);
        }
        break;
    }
}

/*
 * Associate type with a tag symbol, which can be used to print self-
 * referential struct or union types, or typedef'ed objects.
 *
 * This is only relevant for diagnostics, and since basic types do not
 * have room to store a tag, just ignore that case. This means that if
 * you have following, the verbose output of a will be int, not i32.
 *
 *  typedef int i32;
 *  i32 a;
 * 
 */
void type_set_tag(Type type, const struct symbol *tag)
{
    struct typetree *t;

    assert(tag->symtype == SYM_TAG || tag->symtype == SYM_TYPEDEF);
    if (type.ref) {
        t = get_typetree_handle(type.ref);
        if (!t->tag || tag->symtype != SYM_TYPEDEF) {
            t->tag = tag;
        }
    }
}

size_t type_alignment(Type type)
{
    int i;
    size_t m = 0, d;
    assert(is_object(type));

    switch (type_of(type)) {
    case T_ARRAY:
        return type_alignment(type_next(type));
    case T_STRUCT:
    case T_UNION:
        for (i = 0; i < nmembers(type); ++i) {
            d = type_alignment(get_member(type, i)->type);
            if (d > m) m = d;
        }
        assert(m);
        return m;
    default:
        return size_of(type);
    }
}

int nmembers(Type type)
{
    struct typetree *t = get_typetree_handle(type.ref);
    return array_len(&t->members);
}

struct member *get_member(Type type, int n)
{
    struct typetree *t;

    assert(n >= 0);
    t = get_typetree_handle(type.ref);
    assert(array_len(&t->members) > n);
    return &array_get(&t->members, n);
}

struct member *type_add_member(Type parent, String name, Type type)
{
    struct member m = {0};

    assert(is_struct_or_union(parent) || is_function(parent));
    if (!is_function(parent)) {
        m.offset = adjust_member_alignment(parent, type);
    }

    m.name = name;
    m.type = type;
    return add_member(parent, m);
}

static int pack_field(const struct member *prev, struct member *m)
{
    int bits;

    assert(prev);
    bits = prev->field_offset + prev->field_width;
    if (bits + m->field_width <= size_of(basic_type__int) * 8) {
        m->offset = prev->offset;
        m->field_offset = bits;
        return 1;
    }

    return 0;
}

static const struct member *get_last_field_member(struct typetree *t)
{
    const struct member *prev;

    if (array_len(&t->members)) {
        prev = &array_back(&t->members);
        if (prev->field_width) {
            return prev;
        }
    }

    return NULL;
}

/*
 * Add struct or union field member to typetree member list, updating
 * total size and alignment accordingly.
 *
 * Anonymous union fields are ignored, not needed for alignment.
 */
void type_add_field(Type parent, String name, Type type, size_t width)
{
    struct member m = {0};
    struct typetree *t;
    const struct member *prev;

    assert(is_struct_or_union(parent));
    assert(type_equal(type, basic_type__int)
        || type_equal(type, basic_type__unsigned_int)
        || is_bool(type));

    if (width > size_of(type) * 8 || (is_bool(type) && width > 1)) {
        error("Width of bit-field (%lu bits) exceeds width of type %t.",
            width, type);
        exit(1);
    }

    if (name.len && !width) {
        error("Zero length field %s.", str_raw(name));
        exit(1);
    }

    if (is_union(parent) && name.len == 0) {
        return;
    }

    m.name = name;
    m.type = type;
    m.field_width = width;
    if (is_struct(parent)) {
        t = get_typetree_handle(parent.ref);
        prev = get_last_field_member(t);
        if (!prev || !pack_field(prev, &m)) {
            m.field_offset = 0;
            m.offset = adjust_member_alignment(parent, type);
        }
    }

    if (!width) {
        reset_field_alignment(parent);
    } else {
        add_member(parent, m);
    }
}

void type_add_anonymous_member(Type parent, Type type)
{
    int i;
    size_t offset;
    struct member m;
    struct typetree *t;

    assert(is_struct_or_union(parent));
    assert(is_struct_or_union(type));
    t = get_typetree_handle(type.ref);
    if (is_struct(parent) && is_union(type)) {
        offset = adjust_member_alignment(parent, type);
        for (i = 0; i < nmembers(type); ++i) {
            m = array_get(&t->members, i);
            m.offset += offset;
            add_member(parent, m);
        }
    } else if (is_union(parent) && is_struct(type)) {
        for (i = 0; i < nmembers(type); ++i) {
            m = array_get(&t->members, i);
            add_member(parent, m);
        }
    } else {
        for (i = 0; i < nmembers(type); ++i) {
            m = array_get(&t->members, i);
            type_add_member(parent, m.name, m.type);
        }
    }
}

/*
 * Remove anonymous field members, which are only kept for alignment
 * during type construction. Return largest remaining member alignment.
 */
static size_t remove_anonymous_fields(struct typetree *t)
{
    int i;
    size_t align, maxalign;
    struct member *m;

    maxalign = 0;
    for (i = array_len(&t->members) - 1; i >= 0; --i) {
        m = &array_get(&t->members, i);
        if (m->name.len == 0) {
            array_erase(&t->members, i);
        } else {
            align = type_alignment(m->type);
            if (align > maxalign) {
                maxalign = align;
            }
        }
    }

    return maxalign;
}

/*
 * Adjust aggregate type size to be a multiple of strongest member
 * alignment.
 *
 * This function should only be called only once all members have been
 * added.
 */
void type_seal(Type type)
{
    struct typetree *t;
    size_t align;
    assert(is_struct_or_union(type));

    t = get_typetree_handle(type.ref);
    align = remove_anonymous_fields(t);
    if (align == 0) {
        error("%s has no named members.", is_struct(type) ? "Struct" : "Union");
        exit(1);
    }

    if (t->size % align) {
        t->size += align - (t->size % align);
    }
}

int is_vararg(Type type)
{
    struct typetree *t;

    assert(is_function(type));
    t = get_typetree_handle(type.ref);
    return t->is_vararg;
}

int is_vla(Type type)
{
    struct typetree *t;

    if (is_array(type)) {
        t = get_typetree_handle(type.ref);
        return t->is_vla || is_vla(t->next);
    }

    return 0;
}

int is_flexible(Type type)
{
    struct typetree *t;

    if (is_struct_or_union(type)) {
        t = get_typetree_handle(type.ref);
        return t->is_flexible;
    }

    return 0;
}

int is_variably_modified(Type type)
{
    switch (type_of(type)) {
    case T_POINTER:
        return is_variably_modified(type_next(type));
    case T_ARRAY:
        return is_vla(type);
    default: return 0;
    }
}

static int typetree_equal(const struct typetree *a, const struct typetree *b)
{
    int i, len;
    struct member *ma, *mb;

    if (a->type != b->type
        || a->size != b->size
        || a->is_unsigned != b->is_unsigned
        || a->is_vararg != b->is_vararg)
    {
        return 0;
    }

    len = array_len(&a->members);
    if (len != array_len(&b->members)) {
        return 0;
    }

    for (i = 0; i < len; ++i) {
        ma = &array_get(&a->members, i);
        mb = &array_get(&b->members, i);
        if (!type_equal(ma->type, mb->type)) {
            return 0;
        } else if (a->type != T_FUNCTION) {
            assert(ma->offset == mb->offset);
            if (str_cmp(ma->name, mb->name)) {
                return 0;
            }
        }
    }

    return 1;
}

/*
 * Determine whether two types are the same. Disregard qualifiers, and
 * names of function parameters.
 */
int type_equal(Type a, Type b)
{
    struct typetree *ta, *tb;

    if (!memcmp(&a, &b, sizeof(a)))
        return 1;

    if (a.type != b.type || a.is_unsigned != b.is_unsigned)
        return 0;

    if ((a.ref == 0) != (b.ref == 0))
        return 0;

    if (a.ref != 0 && b.ref != 0) {
        ta = get_typetree_handle(a.ref);
        tb = get_typetree_handle(b.ref);
        return typetree_equal(ta, tb);
    }

    return 1;
}

Type promote_integer(Type type)
{
    assert(is_integer(type));
    if (size_of(type) < 4) {
        type = basic_type__int;
    }

    return type;
}

Type usual_arithmetic_conversion(Type t1, Type t2)
{
    Type res;

    assert(is_arithmetic(t1));
    assert(is_arithmetic(t2));
    if (is_long_double(t1) || is_long_double(t2)) {
        res = basic_type__long_double;
    } else if (is_double(t1) || is_double(t2)) {
        res = basic_type__double;
    } else if (is_float(t1) || is_float(t2)) {
        res = basic_type__float;
    } else {
        t1 = promote_integer(t1);
        t2 = promote_integer(t2);
        if (size_of(t1) > size_of(t2)) {
            res = t1;
        } else if (size_of(t2) > size_of(t1)) {
            res = t2;
        } else {
            res = is_unsigned(t1) ? t1 : t2;
        }
    }

    return remove_qualifiers(res);
}

int is_compatible(Type l, Type r)
{
    size_t s1, s2;

    if (type_of(l) != type_of(r)
        || is_const(l) != is_const(r)
        || is_volatile(l) != is_volatile(r)
        || is_restrict(l) != is_restrict(r))
    {
        return 0;
    }

    switch (type_of(l)) {
    case T_BOOL:
    case T_CHAR:
    case T_SHORT:
    case T_INT:
    case T_LONG:
    case T_FLOAT:
    case T_DOUBLE:
    case T_LDOUBLE:
        return 1;
    case T_POINTER:
        return is_compatible(type_next(l), type_next(r));
    case T_ARRAY:
        s1 = type_array_len(l);
        s2 = type_array_len(r);
        /* Also accept VLA, which returns 0 length here. */
        if (s1 == 0 || s2 == 0 || s1 == s2) {
            return is_compatible(type_next(l), type_next(r));
        }
        return 0;
    default:
        return type_equal(l, r);
    }
}

int is_compatible_unqualified(Type l, Type r)
{
    l = remove_qualifiers(l);
    r = remove_qualifiers(r);
    return is_compatible(l, r);
}

size_t size_of(Type type)
{
    struct typetree *t;

    switch (type_of(type)) {
    case T_BOOL:
    case T_CHAR:
        return 1;
    case T_SHORT:
        return 2;
    case T_INT:
    case T_FLOAT:
        return 4;
    case T_LONG:
    case T_DOUBLE:
    case T_POINTER:
        return 8;
    case T_LDOUBLE:
        return 16;
    case T_STRUCT:
    case T_UNION:
        t = get_typetree_handle(type.ref);
        return t->size;
    case T_ARRAY:
        t = get_typetree_handle(type.ref);
        return t->size * size_of(t->next);
    default:
        return 0;
    }
}

size_t type_array_len(Type type)
{
    struct typetree *t;
    assert(is_array(type));

    t = get_typetree_handle(type.ref);
    return t->size;
}

const struct symbol *type_vla_length(Type type)
{
    struct typetree *t;
    assert(is_vla(type));

    t = get_typetree_handle(type.ref);
    return t->vlen;
}

Type type_deref(Type type)
{
    assert(is_pointer(type));
    if (type.is_pointer) {
        type = remove_qualifiers(type);
        type.is_pointer = 0;
    } else {
        type = get_type_handle(type.ref);
    }

    return type;
}

Type type_next(Type type)
{
    struct typetree *t;
    assert(is_pointer(type) || is_function(type) || is_array(type));

    if (is_pointer(type)) {
        return type_deref(type);
    }

    t = get_typetree_handle(type.ref);
    return t->next;
}

void set_array_length(Type type, size_t length)
{
    struct typetree *t;
    assert(is_array(type));
    assert(length > 0);

    t = get_typetree_handle(type.ref);
    assert(t->size == 0);
    t->size = length;
}

const struct member *find_type_member(Type type, String name, int *index)
{
    int i;
    struct typetree *t;
    const struct member *member;

    assert(is_struct_or_union(type) || is_function(type));
    t = get_typetree_handle(type.ref);
    for (i = 0; i < array_len(&t->members); ++i) {
        member = &array_get(&t->members, i);
        if (!str_cmp(name, member->name)) {
            if (index) {
                *index = i;
            }
            return member;
        }
    }

    if (index) {
        *index = -1;
    }

    return NULL;
}

int fprinttype(FILE *stream, Type type, const struct symbol *expand)
{
    struct typetree *t;
    struct member *m;
    const char *s;
    int i, n = 0;

    if (is_const(type))
        n += fputs("const ", stream);

    if (is_volatile(type))
        n += fputs("volatile ", stream);

    if (is_restrict(type))
        n += fputs("restrict ", stream);

    if (is_unsigned(type) && !is_bool(type))
        n += fputs("unsigned ", stream);

    switch (type_of(type)) {
    case T_VOID:
        n += fputs("void", stream);
        break;
    case T_BOOL:
        n += fputs("_Bool", stream);
        break;
    case T_CHAR:
        n += fputs("char", stream);
        break;
    case T_SHORT:
        n += fputs("short", stream);
        break;
    case T_INT:
        n += fputs("int", stream);
        break;
    case T_LONG:
        n += fputs("long", stream);
        break;
    case T_FLOAT:
        n += fputs("float", stream);
        break;
    case T_DOUBLE:
        n += fputs("double", stream);
        break;
    case T_LDOUBLE:
        n += fputs("long double", stream);
        break;
    case T_POINTER:
        n += fputs("* ", stream);
        n += fprinttype(stream, type_deref(type), NULL);
        break;
    case T_FUNCTION:
        t = get_typetree_handle(type.ref);
        n += fputs("(", stream);
        for (i = 0; i < nmembers(type); ++i) {
            m = get_member(type, i);
            if (m->offset) {
                assert(is_pointer(m->type));
                fprintf(stream, "static(%lu) ", m->offset);
            }
            n += fprinttype(stream, m->type, NULL);
            if (i < nmembers(type) - 1) {
                n += fputs(", ", stream);
            }
        }
        if (is_vararg(type)) {
            n += fputs(", ...", stream);
        }
        n += fputs(") -> ", stream);
        n += fprinttype(stream, t->next, NULL);
        break;
    case T_ARRAY:
        t = get_typetree_handle(type.ref);
        if (t->is_vla) {
            if (t->vlen) {
                n += fprintf(stream, "[%s] ", sym_name(t->vlen));
            } else {
                n += fputs("[*] ", stream);
            }
        } else if (t->size) {
            n += fprintf(stream, "[%lu] ", t->size);
        } else {
            n += fputs("[] ", stream);
        }
        n += fprinttype(stream, t->next, NULL);
        break;
    case T_STRUCT:
    case T_UNION:
        t = get_typetree_handle(type.ref);
        if (t->tag && (t->tag != expand)) {
            if (t->tag->symtype == SYM_TAG) {
                s = is_union(type) ? "union" : "struct";
                n += fprintf(stream, "%s %s", s, sym_name(t->tag));
            } else {
                assert(t->tag->symtype == SYM_TYPEDEF);
                n += fprintf(stream, "%s", sym_name(t->tag));
            }
        } else {
            n += fputc('{', stream);
            for (i = 0; i < nmembers(type); ++i) {
                m = get_member(type, i);
                n += fprintf(stream, ".%s::", str_raw(m->name));
                n += fprinttype(stream, m->type, NULL);
                if (m->field_width) {
                    n += fprintf(stream, " (+%lu:%d:%d)",
                        m->offset, m->field_offset, m->field_width);
                } else {
                    n += fprintf(stream, " (+%lu)", m->offset);
                }
                if (i < nmembers(type) - 1) {
                    n += fputs(", ", stream);
                }
            }
            n += fputc('}', stream);
        }
        break;
    }

    return n;
}
