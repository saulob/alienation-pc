/* Focused tests of libSceJson2's constructors, containers and serialization, resolved by NID as the loader binds the game's
 * imports. Guest objects are canary-filled buffers of the game's sizes: the runtime may only touch their first 8 bytes. */
#include "runtime.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

enum { TypeNull, TypeBoolean, TypeInteger, TypeUInteger, TypeReal, TypeString, TypeArray, TypeObject };
#define VALUE_SIZE 0x20
#define CANARY 0xAA

typedef struct { unsigned char bytes[VALUE_SIZE]; } Guest;
typedef void (ABI *Ctor)(void *self);
typedef void (ABI *CtorArg)(void *self, const void *arg);
typedef void (ABI *CtorType)(void *self, int type);
typedef void (ABI *CtorBool)(void *self, int value);
typedef void (ABI *CtorInt)(void *self, int64_t value);
typedef void (ABI *CtorReal)(void *self, double value);
typedef void (ABI *CtorText)(void *self, const char *text);
typedef void *(ABI *Member)(void *self, const void *key);
typedef const char *(ABI *CStr)(const void *self);
typedef const int64_t *(ABI *GetInteger)(const void *self);
typedef size_t (ABI *Count)(const void *self);
typedef const void *(ABI *Index)(const void *self, size_t index);
typedef const void *(ABI *Key)(const void *self, const char *key);
typedef int (ABI *Parse)(void *out, const char *text, size_t size);
typedef int (ABI *Serialize)(void *self, void *out);
typedef void *(ABI *Refer)(void *self);
typedef void *(ABI *Assign)(void *self, const void *other);
typedef void (ABI *SetInt)(void *self, int64_t value);

static void *resolve(const char *nid, const char *name) {
    void *function = (void *)runtime_resolve(nid, 0);
    CHECK(function != NULL && !strcmp(runtime_import_name(nid), name));
    return function;
}

static Guest *fresh(void) {
    Guest *g = malloc(sizeof(*g));
    memset(g, CANARY, sizeof(*g));
    return g;
}

static int canary_intact(const Guest *g) {
    for (size_t i = 8; i < sizeof(g->bytes); ++i)
        if (g->bytes[i] != CANARY) return 0;
    return 1;
}

int main(void) {
    runtime_start(1);
#define F(type, var, nid, name) type var = (type)resolve(nid, name)
    F(Ctor, value_new, "qBMjqyBn3OM#libSceJson2", "_ZN3sce4Json5ValueC1Ev");
    F(Ctor, value_free, "WTtYf+cNnXI#libSceJson2", "_ZN3sce4Json5ValueD1Ev");
    F(CtorText, string_new_text, "9KUZFjI1IxA#libSceJson2", "_ZN3sce4Json6StringC1EPKc");
    F(Ctor, string_free, "cG1VE2HMl6c#libSceJson2", "_ZN3sce4Json6StringD1Ev");
    F(Ctor, string_new, "qSmqLXXCPas#libSceJson2", "_ZN3sce4Json6StringC1Ev");
    F(CStr, string_c_str, "L1KAkYWml-M#libSceJson2", "_ZNK3sce4Json6String5c_strEv");
    F(GetInteger, get_integer, "DIxvoy7Ngvk#libSceJson2", "_ZNK3sce4Json5Value10getIntegerEv");
    F(Count, value_count, "RBw+4NukeGQ#libSceJson2", "_ZNK3sce4Json5Value5countEv");
    F(Key, value_member, "HwDt5lD9Bfo#libSceJson2", "_ZNK3sce4Json5ValueixEPKc");
    F(Index, value_element, "XlWbvieLj2M#libSceJson2", "_ZNK3sce4Json5ValueixEm");
    F(Parse, parse, "S5JxQnoGF3E#libSceJson2", "_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm");
    F(SetInt, value_set_integer, "QxVVYhP-mvg#libSceJson2", "_ZN3sce4Json5Value3setEl");
    F(Ctor, array_new, "JP-PtKMiI1E#libSceJson2", "_ZN3sce4Json5ArrayC1Ev");
    F(Ctor, object_new, "OJPTonqdg0I#libSceJson2", "_ZN3sce4Json6ObjectC1Ev");
    F(Member, object_member, "ERuf9y0DY84#libSceJson2", "_ZN3sce4Json6ObjectixERKNS0_6StringE");
    F(CtorArg, value_new_string, "sZIoMRGO+jk#libSceJson2", "_ZN3sce4Json5ValueC1ERKNS0_6StringE");
    F(Assign, value_assign, "4zrm6VrgIAw#libSceJson2", "_ZN3sce4Json5ValueaSERKS1_");
    F(CtorInt, value_new_uinteger, "x4AUdbhpRB0#libSceJson2", "_ZN3sce4Json5ValueC1Em");
    F(CtorBool, value_new_boolean, "UeuWT+yNdCQ#libSceJson2", "_ZN3sce4Json5ValueC1Eb");
    F(CtorArg, value_new_object, "3xUXnmUkXfo#libSceJson2", "_ZN3sce4Json5ValueC1ERKNS0_6ObjectE");
    F(CtorArg, array_push_back, "zQtLRTqceMY#libSceJson2", "_ZN3sce4Json5Array9push_backERKNS0_5ValueE");
    F(CtorInt, value_new_integer, "0lLK8+kDqmE#libSceJson2", "_ZN3sce4Json5ValueC1El");
    F(CtorArg, array_copy, "bI5AGFMydrA#libSceJson2", "_ZN3sce4Json5ArrayC1ERKS1_");
    F(CtorArg, value_new_array, "iZeYfOxtMRg#libSceJson2", "_ZN3sce4Json5ValueC1ERKNS0_5ArrayE");
    F(Ctor, array_free, "HJ8GpRT1aiw#libSceJson2", "_ZN3sce4Json5ArrayD1Ev");
    F(CtorArg, value_set_value, "XL8+BUqjB1w#libSceJson2", "_ZN3sce4Json5Value3setERKS1_");
    F(Serialize, serialize, "R7FDWtcN6f8#libSceJson2", "_ZN3sce4Json5Value9serializeERNS0_6StringE");
    F(Ctor, object_free, "5JmzZt8twAo#libSceJson2", "_ZN3sce4Json6ObjectD1Ev");
    F(CtorType, value_new_type, "CbrT3dwDILo#libSceJson2", "_ZN3sce4Json5ValueC1ENS0_9ValueTypeE");
    F(Refer, value_refer_object, "-NxEk7XLkDY#libSceJson2", "_ZN3sce4Json5Value11referObjectEv");
    F(CtorReal, value_new_real, "sOmU4vnx3s0#libSceJson2", "_ZN3sce4Json5ValueC1Ed");
    F(CtorArg, object_copy, "a+W7HHlwpBs#libSceJson2", "_ZN3sce4Json6ObjectC1ERKS1_");
    F(CtorArg, value_copy, "fSb2oQTNrgA#libSceJson2", "_ZN3sce4Json5ValueC1ERKS1_");
    if (failures) return 1;

    Guest *text = fresh(), *key_a = fresh(), *key_list = fresh(), *out = fresh();
    string_new(out);
    CHECK(canary_intact(out));

    Guest *object = fresh();
    object_new(object);
    CHECK(canary_intact(object));
    string_new_text(key_a, "a");
    string_new_text(key_list, "list");

    Guest *five = fresh();
    value_new_integer(five, 5);
    CHECK(canary_intact(five) && *get_integer(five) == 5);
    value_assign(object_member(object, key_a), five);

    Guest *array = fresh();
    array_new(array);
    Guest *flag = fresh(), *real = fresh(), *unsigned_value = fresh(), *label = fresh();
    value_new_boolean(flag, 1);
    value_new_real(real, 1.5);
    value_new_uinteger(unsigned_value, 7);
    string_new_text(text, "x\"y");
    value_new_string(label, text);
    array_push_back(array, flag);
    array_push_back(array, real);
    array_push_back(array, unsigned_value);
    array_push_back(array, label);

    Guest *list = fresh();
    value_new_array(list, array);
    CHECK(canary_intact(list));
    value_set_value(object_member(object, key_list), list);

    Guest *root = fresh();
    value_new_object(root, object);
    CHECK(canary_intact(root));
    CHECK(value_count(root) == 2);
    CHECK(serialize(root, out) == 0);
    CHECK(!strcmp(string_c_str(out), "{\"a\":5,\"list\":[true,1.5,7,\"x\\\"y\"]}"));

    /* Copies are deep: changing the source afterwards leaves the copy alone. */
    Guest *copy = fresh();
    value_copy(copy, root);
    value_set_integer(object_member(object, key_a), 0);
    value_set_integer((void *)value_member(root, "a"), 99);
    CHECK(*get_integer(value_member(copy, "a")) == 5);
    CHECK(*get_integer(value_member(root, "a")) == 99);
    CHECK(value_count(value_member(copy, "list")) == 4);

    Guest *array_clone = fresh();
    array_copy(array_clone, array);
    array_free(array);
    Guest *clone_value = fresh();
    value_new_array(clone_value, array_clone);
    string_new_text(text, "unused");
    CHECK(serialize(clone_value, out) == 0);
    CHECK(!strcmp(string_c_str(out), "[true,1.5,7,\"x\\\"y\"]"));

    Guest *object_clone = fresh();
    object_copy(object_clone, object);
    Guest *clone_object_value = fresh();
    value_new_object(clone_object_value, object_clone);
    CHECK(value_count(clone_object_value) == 2);

    /* Assignment replaces the target, also when the source lives inside it. */
    value_assign(root, value_member(root, "list"));
    CHECK(serialize(root, out) == 0 && !strcmp(string_c_str(out), "[true,1.5,7,\"x\\\"y\"]"));
    value_assign(root, root);
    CHECK(value_count(root) == 4);

    /* Typed construction and referObject. */
    Guest *typed = fresh();
    value_new_type(typed, TypeString);
    CHECK(serialize(typed, out) == 0 && !strcmp(string_c_str(out), "\"\""));
    value_free(typed);
    value_new_type(typed, TypeNull);
    CHECK(value_refer_object(typed) != NULL);
    CHECK(serialize(typed, out) == 0 && !strcmp(string_c_str(out), "{}"));
    CHECK(value_refer_object(typed) == value_refer_object(typed));

    /* Parsed documents are copyable and re-serialize identically. */
    Guest *parsed = fresh();
    value_new(parsed);
    const char json[] = "{\"b\":[1,2],\"a\":{\"n\":-3}}";
    CHECK(parse(parsed, json, sizeof(json) - 1) == 0);
    Guest *parsed_copy = fresh();
    value_copy(parsed_copy, parsed);
    value_free(parsed);
    CHECK(serialize(parsed_copy, out) == 0 && !strcmp(string_c_str(out), "{\"a\":{\"n\":-3},\"b\":[1,2]}"));
    CHECK(*get_integer(value_element(value_member(parsed_copy, "b"), 1)) == 2);

    value_free(parsed_copy); value_free(typed); value_free(clone_object_value); value_free(clone_value);
    value_free(copy); value_free(root); value_free(list); value_free(five); value_free(flag); value_free(real);
    value_free(unsigned_value); value_free(label);
    object_free(object); object_free(object_clone); array_free(array_clone);
    string_free(out); string_free(text); string_free(key_a); string_free(key_list);
    CHECK(canary_intact(object) && canary_intact(array_clone));
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
