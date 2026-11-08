// SPDX-License-Identifier: GPL-2.0-only
/* libSceJson2: the sce::Json C++ classes the game imports, ported from AnyPS5's implementation
 * (https://github.com/boykopovar/AnyPS5, core/libs/prx/libSceJson2/Export.cpp at c547deaa, GPL-2.0-only).
 * Value, String and the containers are one pointer to a host-heap node inside the game's storage (the game's Value is
 * 0x20 bytes and its String 8), so the game only ever passes them back to these functions. The InitParameter
 * allocator is not used. Behaviour AnyPS5 leaves unverified stops instead of guessing. */
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
extern "C" {
#include "runtime.h"
}

namespace {

enum ValueType : std::int32_t {
    TypeNull = 0,
    TypeBoolean = 1,
    TypeInteger = 2,
    TypeUInteger = 3,
    TypeReal = 4,
    TypeString = 5,
    TypeArray = 6,
    TypeObject = 7,
};

constexpr int JsonErrorParse = static_cast<int>(0x80848101);

struct Node;
struct Value { Node* node; };
struct String { std::string* text; };
struct Array { std::list<Value>* items; };
struct Pair;
struct Object { std::list<Pair>* items; };
struct Pair {
    String key;
    std::uint64_t reserved;
    Value value;
};
static_assert(sizeof(Value) == 8 && sizeof(String) == 8 && sizeof(Array) == 8 && sizeof(Object) == 8);

struct Node {
    ValueType type = TypeNull;
    bool boolean = false;
    std::int64_t integer = 0;
    std::uint64_t uinteger = 0;
    double real = 0;
    String string{};
    Array array{};
    Object object{};
};

using NullAccessCallback = const Value& (ABI*)(ValueType, const Value*, void*);

struct Globals {
    std::mutex mutex;
    NullAccessCallback callback = nullptr;
    void* context = nullptr;
    std::atomic<std::size_t> parses{0}, failed{0};
};

/* Constant-initialized: the crash report reads the counters without taking a lock. */
constinit Globals globals;

Globals& State() { return globals; }

[[noreturn]] void Stop(const char* what) {
    std::fprintf(stderr, "STOP: %s\n", what);
    runtime_report();
    runtime_exit(21);
}

void Construct(String& s, const std::string& text) { s.text = new std::string(text); }
void Construct(Array& a) { a.items = new std::list<Value>(); }
void Construct(Object& o) { o.items = new std::list<Pair>(); }
void Destroy(Value& v);

void Destroy(String& s) { delete s.text; s.text = nullptr; }
void Destroy(Array& a) {
    if (!a.items) return;
    for (auto& item : *a.items) Destroy(item);
    delete a.items;
    a.items = nullptr;
}
void Destroy(Object& o) {
    if (!o.items) return;
    for (auto& pair : *o.items) {
        Destroy(pair.key);
        Destroy(pair.value);
    }
    delete o.items;
    o.items = nullptr;
}

void Clear(Node& n) {
    if (n.type == TypeString) Destroy(n.string);
    if (n.type == TypeArray) Destroy(n.array);
    if (n.type == TypeObject) Destroy(n.object);
    n = Node{};
}

void Destroy(Value& v) {
    if (!v.node) return;
    Clear(*v.node);
    delete v.node;
    v.node = nullptr;
}

Node& NodeOf(const Value& v) {
    if (!v.node) Stop("sce::Json::Value used before construction");
    return *v.node;
}

void Construct(Value& v) { v.node = new Node(); }

void SetType(Node& n, ValueType type) {
    if (type < TypeNull || type > TypeObject) Stop("sce::Json::Value::set: invalid value type");
    Node pending{};
    pending.type = type;
    if (type == TypeString) Construct(pending.string, std::string());
    if (type == TypeArray) Construct(pending.array);
    if (type == TypeObject) Construct(pending.object);
    Clear(n);
    n = pending;
}

const Value& Defaults(ValueType type) {
    static Value values[8]{};
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < 8; ++i) {
            Construct(values[i]);
            SetType(*values[i].node, static_cast<ValueType>(i));
        }
    });
    return values[type];
}

const Value& NullAccess(ValueType requested, const Value* parent) {
    auto& state = State();
    NullAccessCallback callback;
    void* context;
    {
        std::lock_guard lock(state.mutex);
        callback = state.callback;
        context = state.context;
    }
    if (callback) {
        const Value& result = callback(requested, parent, context);
        if (result.node && (requested == TypeNull || result.node->type == requested)) return result;
    }
    return Defaults(requested);
}

const Node& Typed(const Value* self, ValueType requested) {
    const Node& n = NodeOf(*self);
    if (n.type == requested) return n;
    return NodeOf(NullAccess(requested, self));
}

void AppendEscaped(std::string& out, const std::string& text) {
    out += '"';
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else out += static_cast<char>(c);
        }
    }
    out += '"';
}

std::string RealText(double value) {
    if (!std::isfinite(value)) Stop("sce::Json: cannot serialize a non-finite number");
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 17);
    if (result.ec != std::errc{}) Stop("sce::Json: real number formatting failed");
    return std::string(buffer, result.ptr);
}

void Serialize(std::string& out, const Node& n) {
    switch (n.type) {
        case TypeNull: out += "null"; return;
        case TypeBoolean: out += n.boolean ? "true" : "false"; return;
        case TypeInteger: out += std::to_string(n.integer); return;
        case TypeUInteger: out += std::to_string(n.uinteger); return;
        case TypeReal: out += RealText(n.real); return;
        case TypeString: AppendEscaped(out, *n.string.text); return;
        case TypeArray: {
            out += '[';
            bool first = true;
            for (const auto& item : *n.array.items) {
                if (!first) out += ',';
                first = false;
                Serialize(out, NodeOf(item));
            }
            out += ']';
            return;
        }
        case TypeObject: {
            out += '{';
            bool first = true;
            for (const auto& pair : *n.object.items) {
                if (!first) out += ',';
                first = false;
                AppendEscaped(out, *pair.key.text);
                out += ':';
                Serialize(out, NodeOf(pair.value));
            }
            out += '}';
            return;
        }
    }
    Stop("sce::Json: invalid value type");
}

int CompareText(const char* left, const char* right) {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return static_cast<unsigned char>(*left) - static_cast<unsigned char>(*right);
}

/* Members stay sorted by key, as the console orders them; a repeated key replaces the earlier value. */
Value& ObjectEntry(Object& object, const std::string& key) {
    auto position = object.items->begin();
    while (position != object.items->end() && std::strcmp(position->key.text->c_str(), key.c_str()) < 0) ++position;
    if (position != object.items->end() && std::strcmp(position->key.text->c_str(), key.c_str()) == 0) return position->value;
    Pair pair{};
    Construct(pair.key, key);
    Construct(pair.value);
    return object.items->insert(position, pair)->value;
}

void CopyNode(Node& out, const Node& in);

void CopyArray(Array& out, const Array& in) {
    Construct(out);
    for (const auto& item : *in.items) {
        Value copy{};
        Construct(copy);
        CopyNode(*copy.node, NodeOf(item));
        out.items->push_back(copy);
    }
}

void CopyObject(Object& out, const Object& in) {
    Construct(out);
    for (const auto& source : *in.items) {
        Pair pair{};
        Construct(pair.key, *source.key.text);
        Construct(pair.value);
        CopyNode(*pair.value.node, NodeOf(source.value));
        out.items->push_back(pair);
    }
}

/* `out` holds no heap data. */
void CopyNode(Node& out, const Node& in) {
    out = in;
    out.string = String{};
    out.array = Array{};
    out.object = Object{};
    if (in.type == TypeString) Construct(out.string, *in.string.text);
    else if (in.type == TypeArray) CopyArray(out.array, in.array);
    else if (in.type == TypeObject) CopyObject(out.object, in.object);
}

void AssignNode(Node& destination, const Node& source) {
    if (&destination == &source) return;
    Node copy{};
    CopyNode(copy, source);
    Clear(destination);
    destination = copy;
}

class Parser {
public:
    Parser(const char* text, std::size_t size) : text(text), end(text + size) {}

    bool Parse(Node& out) {
        skip();
        return value(out);
    }

private:
    /* Bounds the recursion on the calling guest thread's stack. */
    static constexpr std::size_t MaxNestingDepth = 512;

    const char* text;
    const char* end;
    std::size_t depth = 0;

    int peek() const { return text == end ? -1 : static_cast<unsigned char>(*text); }
    int take() { return text == end ? -1 : static_cast<unsigned char>(*text++); }
    void skip() { while (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r') take(); }
    bool literal(const char* word) {
        for (; *word != '\0'; ++word) {
            if (peek() != static_cast<unsigned char>(*word)) return false;
            take();
        }
        return true;
    }
    static void appendUtf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) out += static_cast<char>(code);
        else if (code < 0x800) { out += static_cast<char>(0xc0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3f)); }
        else if (code < 0x10000) { out += static_cast<char>(0xe0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3f)); out += static_cast<char>(0x80 | (code & 0x3f)); }
        else { out += static_cast<char>(0xf0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 0x3f)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3f)); out += static_cast<char>(0x80 | (code & 0x3f)); }
    }
    bool hex4(std::uint32_t& code) {
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const int c = take();
            code <<= 4;
            if (c >= '0' && c <= '9') code |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (peek() != '"') return false;
        take();
        while (peek() >= 0 && peek() != '"') {
            const int c = take();
            if (c < 0x20) return false;
            if (c != '\\') { out += static_cast<char>(c); continue; }
            switch (take()) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t code = 0;
                    if (!hex4(code)) return false;
                    if (code >= 0xd800 && code < 0xdc00) {
                        std::uint32_t low = 0;
                        if (!literal("\\u") || !hex4(low) || low < 0xdc00 || low >= 0xe000) return false;
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    } else if (code >= 0xdc00 && code < 0xe000) return false;
                    appendUtf8(out, code);
                    break;
                }
                default: return false;
            }
        }
        return take() == '"';
    }
    bool digits(std::string& token) {
        if (peek() < '0' || peek() > '9') return false;
        while (peek() >= '0' && peek() <= '9') token.push_back(static_cast<char>(take()));
        return true;
    }
    bool number(Node& out) {
        std::string token;
        if (peek() == '-') token.push_back(static_cast<char>(take()));
        if (!digits(token)) return false;
        bool integral = true;
        if (peek() == '.') {
            integral = false;
            token.push_back(static_cast<char>(take()));
            if (!digits(token)) return false;
        }
        if (peek() == 'e' || peek() == 'E') {
            integral = false;
            token.push_back(static_cast<char>(take()));
            if (peek() == '+' || peek() == '-') token.push_back(static_cast<char>(take()));
            if (!digits(token)) return false;
        }
        if (integral) {
            errno = 0;
            if (token[0] == '-') {
                const long long value = std::strtoll(token.c_str(), nullptr, 10);
                if (errno == 0) { out.type = TypeInteger; out.integer = value; return true; }
            } else {
                const unsigned long long value = std::strtoull(token.c_str(), nullptr, 10);
                if (errno == 0) { out.type = TypeUInteger; out.uinteger = value; return true; }
            }
        }
        double real = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), real);
        bool converted = parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
        if (parsed.ec == std::errc::result_out_of_range) {
            std::istringstream stream(token);
            stream.imbue(std::locale::classic());
            stream >> real;
            converted = !stream.fail() && stream.eof();
        }
        if (!converted || !std::isfinite(real)) Stop("sce::Json::Parser::parse: a number overflows a double; the console's behaviour is unverified");
        out.type = TypeReal;
        out.real = real;
        return true;
    }
    bool value(Node& out) {
        switch (peek()) {
            case -1: return false;
            case 'n': if (!literal("null")) return false; SetType(out, TypeNull); return true;
            case 't': if (!literal("true")) return false; SetType(out, TypeBoolean); out.boolean = true; return true;
            case 'f': if (!literal("false")) return false; SetType(out, TypeBoolean); return true;
            case '"': {
                std::string text;
                if (!string(text)) return false;
                SetType(out, TypeString);
                *out.string.text = std::move(text);
                return true;
            }
            case '[': {
                if (depth == MaxNestingDepth) return false;
                take();
                ++depth;
                SetType(out, TypeArray);
                skip();
                if (peek() == ']') { take(); --depth; return true; }
                for (;;) {
                    auto& item = out.array.items->emplace_back();
                    Construct(item);
                    skip();
                    if (!value(*item.node)) return false;
                    skip();
                    if (peek() == ',') { take(); continue; }
                    if (peek() == ']') { take(); --depth; return true; }
                    return false;
                }
            }
            case '{': {
                if (depth == MaxNestingDepth) return false;
                take();
                ++depth;
                SetType(out, TypeObject);
                skip();
                if (peek() == '}') { take(); --depth; return true; }
                for (;;) {
                    skip();
                    std::string key;
                    if (!string(key)) return false;
                    skip();
                    if (take() != ':') return false;
                    skip();
                    Value& entry = ObjectEntry(out.object, key);
                    Clear(*entry.node);
                    if (!value(*entry.node)) return false;
                    skip();
                    if (peek() == ',') { take(); continue; }
                    if (peek() == '}') { take(); --depth; return true; }
                    return false;
                }
            }
            default:
                return number(out);
        }
    }
};

void ABI NotifyError(void* self, int error, std::size_t size, void* user_data) {
    (void)self;
    (void)error;
    (void)size;
    (void)user_data;
}

[[noreturn]] void ABI PureVirtual() { Stop("sce::Json::MemAllocator: pure virtual function called"); }

/* {offset to top, RTTI, ~MemAllocator, deleting ~MemAllocator, allocate, deallocate, notifyError}: the slots of the
 * game's own MemAllocator subclass, which inherits notifyError. */
void* const MemAllocatorVtable[] = {
    nullptr, nullptr,
    reinterpret_cast<void*>(&PureVirtual), reinterpret_cast<void*>(&PureVirtual),
    reinterpret_cast<void*>(&PureVirtual), reinterpret_cast<void*>(&PureVirtual),
    reinterpret_cast<void*>(&NotifyError),
};

int ABI InitializerConstruct(void* self) {
    (void)self;
    return 0;
}
int ABI InitializerDestroy(void* self) {
    (void)self;
    return 0;
}
int ABI InitializerInitialize(void* self, const void* parameter) {
    (void)self;
    if (parameter == nullptr) Stop("sce::Json::Initializer::initialize: null parameter");
    return 0;
}

void ABI MemAllocatorConstruct(void* self) { *static_cast<void* const**>(self) = &MemAllocatorVtable[2]; }
void ABI MemAllocatorDestroy(void* self) { *static_cast<void* const**>(self) = &MemAllocatorVtable[2]; }

void ABI StringConstruct(String* self) { Construct(*self, std::string()); }
void ABI StringConstructText(String* self, const char* text) {
    if (text == nullptr) Stop("sce::Json::String: null text");
    Construct(*self, text);
}
void ABI StringDestroy(String* self) { Destroy(*self); }
const char* ABI StringCStr(const String* self) { return self->text->c_str(); }
std::size_t ABI StringLength(const String* self) { return self->text->size(); }
char ABI StringAt(const String* self, std::size_t position) {
    if (position >= self->text->size()) Stop("sce::Json::String::at: position past the end");
    return (*self->text)[position];
}
bool ABI StringEquals(const String* self, const char* text) { return text != nullptr && CompareText(self->text->c_str(), text) == 0; }

void ABI ValueConstruct(Value* self) { Construct(*self); }
void ABI ValueDestroy(Value* self) { Destroy(*self); }
int ABI ValueSetType(Value* self, ValueType type) { SetType(NodeOf(*self), type); return 0; }
int ABI ValueSetBoolean(Value* self, bool value) { SetType(NodeOf(*self), TypeBoolean); self->node->boolean = value; return 0; }
int ABI ValueSetInteger(Value* self, std::int64_t value) { SetType(NodeOf(*self), TypeInteger); self->node->integer = value; return 0; }
int ABI ValueSetUInteger(Value* self, std::uint64_t value) { SetType(NodeOf(*self), TypeUInteger); self->node->uinteger = value; return 0; }
int ABI ValueSetReal(Value* self, double value) { SetType(NodeOf(*self), TypeReal); self->node->real = value; return 0; }
int ABI ValueSetString(Value* self, const String* text) {
    const std::string copy(*text->text);
    SetType(NodeOf(*self), TypeString);
    *self->node->string.text = copy;
    return 0;
}
void ABI ValueClear(Value* self) {
    if (self != nullptr) Clear(NodeOf(*self));
}
/* A member function here (the game passes the Value first); the callback serves every Value, as in AnyPS5. */
void ABI ValueSetNullAccessCallback(Value* self, NullAccessCallback callback, void* context) {
    (void)self;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    state.callback = callback;
    state.context = context;
}

const bool* ABI ValueGetBoolean(const Value* self) { return &Typed(self, TypeBoolean).boolean; }
const std::int64_t* ABI ValueGetInteger(const Value* self) {
    Node& n = const_cast<Node&>(NodeOf(*self));
    if (n.type == TypeUInteger) {
        n.integer = std::bit_cast<std::int64_t>(n.uinteger);
        return &n.integer;
    }
    return &Typed(self, TypeInteger).integer;
}
const String* ABI ValueGetString(const Value* self) { return &Typed(self, TypeString).string; }
std::size_t ABI ValueCount(const Value* self) {
    const Node& n = NodeOf(*self);
    if (n.type == TypeArray) return n.array.items->size();
    if (n.type == TypeObject) return n.object.items->size();
    return 0;
}
const Value* ABI ValueMember(const Value* self, const char* key) {
    const Node& n = NodeOf(*self);
    if (key != nullptr && n.type == TypeObject)
        for (const auto& pair : *n.object.items)
            if (std::strcmp(pair.key.text->c_str(), key) == 0) return &pair.value;
    return &NullAccess(TypeNull, self);
}
const Value* ABI ValueElement(const Value* self, std::size_t index) {
    const Node& n = NodeOf(*self);
    if (n.type == TypeArray && index < n.array.items->size()) {
        auto it = n.array.items->begin();
        std::advance(it, static_cast<std::ptrdiff_t>(index));
        return &*it;
    }
    return &NullAccess(TypeNull, self);
}
int ABI ValueToString(const Value* self, String* out) {
    const Node& n = NodeOf(*self);
    if (n.type == TypeString) {
        *out->text = *n.string.text;
        return 0;
    }
    std::string text;
    Serialize(text, n);
    *out->text = std::move(text);
    return 0;
}
bool ABI ValueTruth(const Value* self) {
    const Node& n = NodeOf(*self);
    switch (n.type) {
        case TypeNull: return false;
        case TypeBoolean: return n.boolean;
        case TypeInteger: return n.integer != 0;
        case TypeUInteger: return n.uinteger != 0;
        case TypeReal: return n.real != 0.0;
        case TypeString: return !n.string.text->empty();
        case TypeArray:
        case TypeObject: return true;
    }
    Stop("sce::Json::Value: invalid value type");
}

void ABI ValueConstructType(Value* self, ValueType type) { Construct(*self); SetType(*self->node, type); }
void ABI ValueConstructBoolean(Value* self, bool value) { Construct(*self); ValueSetBoolean(self, value); }
void ABI ValueConstructInteger(Value* self, std::int64_t value) { Construct(*self); ValueSetInteger(self, value); }
void ABI ValueConstructUInteger(Value* self, std::uint64_t value) { Construct(*self); ValueSetUInteger(self, value); }
void ABI ValueConstructReal(Value* self, double value) { Construct(*self); ValueSetReal(self, value); }
void ABI ValueConstructString(Value* self, const String* text) { Construct(*self); ValueSetString(self, text); }
void ABI ValueConstructArray(Value* self, const Array* array) {
    Construct(*self);
    if (!array->items) Stop("sce::Json::Value: array used before construction");
    SetType(*self->node, TypeArray);
    Destroy(self->node->array);
    CopyArray(self->node->array, *array);
}
void ABI ValueConstructObject(Value* self, const Object* object) {
    Construct(*self);
    if (!object->items) Stop("sce::Json::Value: object used before construction");
    SetType(*self->node, TypeObject);
    Destroy(self->node->object);
    CopyObject(self->node->object, *object);
}
void ABI ValueConstructCopy(Value* self, const Value* other) {
    Construct(*self);
    AssignNode(*self->node, NodeOf(*other));
}
void ABI ValueSetValue(Value* self, const Value* other) { AssignNode(NodeOf(*self), NodeOf(*other)); }
Value* ABI ValueAssign(Value* self, const Value* other) {
    AssignNode(NodeOf(*self), NodeOf(*other));
    return self;
}
Object* ABI ValueReferObject(Value* self) {
    Node& n = NodeOf(*self);
    if (n.type == TypeNull) SetType(n, TypeObject);
    if (n.type != TypeObject) Stop("sce::Json::Value::referObject: value is not an object");
    return &n.object;
}
int ABI ValueSerialize(Value* self, String* out) {
    if (!out || !out->text) Stop("sce::Json::Value::serialize: string used before construction");
    std::string text;
    Serialize(text, NodeOf(*self));
    *out->text = std::move(text);
    return 0;
}

void ABI ArrayConstruct(Array* self) { Construct(*self); }
void ABI ArrayConstructCopy(Array* self, const Array* other) {
    if (!other->items) Stop("sce::Json::Array: array used before construction");
    CopyArray(*self, *other);
}
void ABI ArrayDestroy(Array* self) { Destroy(*self); }
void ABI ArrayPushBack(Array* self, const Value* value) {
    if (!self->items) Stop("sce::Json::Array: array used before construction");
    Value copy{};
    Construct(copy);
    CopyNode(*copy.node, NodeOf(*value));
    self->items->push_back(copy);
}

void ABI ObjectConstruct(Object* self) { Construct(*self); }
void ABI ObjectConstructCopy(Object* self, const Object* other) {
    if (!other->items) Stop("sce::Json::Object: object used before construction");
    CopyObject(*self, *other);
}
void ABI ObjectDestroy(Object* self) { Destroy(*self); }
Value* ABI ObjectMember(Object* self, const String* key) {
    if (!self->items || !key->text) Stop("sce::Json::Object::operator[]: used before construction");
    return &ObjectEntry(*self, *key->text);
}

int ABI Parse(Value* out, const char* text, std::size_t size) {
    auto& state = State();
    ++state.parses;
    Node parsed{};
    if (size != 0 && text == nullptr) Stop("sce::Json::Parser::parse: null text");
    if (size == 0 || !Parser(text, size).Parse(parsed)) {
        Clear(parsed);
        ++state.failed;
        return JsonErrorParse;
    }
    Clear(NodeOf(*out));
    *out->node = parsed;
    return 0;
}

template <typename F>
void* Function(F* function) { return reinterpret_cast<void*>(function); }

const RuntimeExport exports[] = {
    {"_ZN3sce4Json11InitializerC1Ev", Function(InitializerConstruct)},
    {"_ZN3sce4Json11InitializerD1Ev", Function(InitializerDestroy)},
    {"_ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE", Function(InitializerInitialize)},
    {"_ZN3sce4Json12MemAllocatorC2Ev", Function(MemAllocatorConstruct)},
    {"_ZN3sce4Json12MemAllocatorD2Ev", Function(MemAllocatorDestroy)},
    {"_ZN3sce4Json12MemAllocator11notifyErrorEimPv", Function(NotifyError)},
    {"_ZN3sce4Json6StringC1Ev", Function(StringConstruct)},
    {"_ZN3sce4Json6StringC1EPKc", Function(StringConstructText)},
    {"_ZN3sce4Json6StringD1Ev", Function(StringDestroy)},
    {"_ZNK3sce4Json6String5c_strEv", Function(StringCStr)},
    {"_ZNK3sce4Json6String6lengthEv", Function(StringLength)},
    {"_ZNK3sce4Json6String4sizeEv", Function(StringLength)},
    {"_ZNK3sce4Json6String2atEm", Function(StringAt)},
    {"_ZNK3sce4Json6StringeqEPKc", Function(StringEquals)},
    {"_ZN3sce4Json5ValueC1Ev", Function(ValueConstruct)},
    {"_ZN3sce4Json5ValueD1Ev", Function(ValueDestroy)},
    {"_ZN3sce4Json5Value3setENS0_9ValueTypeE", Function(ValueSetType)},
    {"_ZN3sce4Json5Value3setEb", Function(ValueSetBoolean)},
    {"_ZN3sce4Json5Value3setEl", Function(ValueSetInteger)},
    {"_ZN3sce4Json5Value3setEm", Function(ValueSetUInteger)},
    {"_ZN3sce4Json5Value3setEd", Function(ValueSetReal)},
    {"_ZN3sce4Json5Value3setERKNS0_6StringE", Function(ValueSetString)},
    {"_ZN3sce4Json5Value5clearEv", Function(ValueClear)},
    {"_ZN3sce4Json5Value21setNullAccessCallBackEPFRKS1_NS0_9ValueTypeEPS2_PvES6_", Function(ValueSetNullAccessCallback)},
    {"_ZNK3sce4Json5Value10getBooleanEv", Function(ValueGetBoolean)},
    {"_ZNK3sce4Json5Value10getIntegerEv", Function(ValueGetInteger)},
    {"_ZNK3sce4Json5Value9getStringEv", Function(ValueGetString)},
    {"_ZNK3sce4Json5Value5countEv", Function(ValueCount)},
    {"_ZNK3sce4Json5ValueixEPKc", Function(ValueMember)},
    {"_ZNK3sce4Json5ValueixEm", Function(ValueElement)},
    {"_ZNK3sce4Json5Value8toStringERNS0_6StringE", Function(ValueToString)},
    {"_ZNK3sce4Json5ValuecvbEv", Function(ValueTruth)},
    {"_ZN3sce4Json5ValueC1ENS0_9ValueTypeE", Function(ValueConstructType)},
    {"_ZN3sce4Json5ValueC1Eb", Function(ValueConstructBoolean)},
    {"_ZN3sce4Json5ValueC1El", Function(ValueConstructInteger)},
    {"_ZN3sce4Json5ValueC1Em", Function(ValueConstructUInteger)},
    {"_ZN3sce4Json5ValueC1Ed", Function(ValueConstructReal)},
    {"_ZN3sce4Json5ValueC1ERKNS0_6StringE", Function(ValueConstructString)},
    {"_ZN3sce4Json5ValueC1ERKNS0_5ArrayE", Function(ValueConstructArray)},
    {"_ZN3sce4Json5ValueC1ERKNS0_6ObjectE", Function(ValueConstructObject)},
    {"_ZN3sce4Json5ValueC1ERKS1_", Function(ValueConstructCopy)},
    {"_ZN3sce4Json5Value3setERKS1_", Function(ValueSetValue)},
    {"_ZN3sce4Json5ValueaSERKS1_", Function(ValueAssign)},
    {"_ZN3sce4Json5Value11referObjectEv", Function(ValueReferObject)},
    {"_ZN3sce4Json5Value9serializeERNS0_6StringE", Function(ValueSerialize)},
    {"_ZN3sce4Json5ArrayC1Ev", Function(ArrayConstruct)},
    {"_ZN3sce4Json5ArrayC1ERKS1_", Function(ArrayConstructCopy)},
    {"_ZN3sce4Json5ArrayD1Ev", Function(ArrayDestroy)},
    {"_ZN3sce4Json5Array9push_backERKNS0_5ValueE", Function(ArrayPushBack)},
    {"_ZN3sce4Json6ObjectC1Ev", Function(ObjectConstruct)},
    {"_ZN3sce4Json6ObjectC1ERKS1_", Function(ObjectConstructCopy)},
    {"_ZN3sce4Json6ObjectD1Ev", Function(ObjectDestroy)},
    {"_ZN3sce4Json6ObjectixERKNS0_6StringE", Function(ObjectMember)},
    {"_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm", Function(Parse)},
};

}

extern "C" uintptr_t runtime_json_resolve(const char* name) { return RUNTIME_LOOKUP(exports, name); }
extern "C" void runtime_json_report(void) {
    const auto& state = State();
    if (!state.parses) return;
    std::printf("Runtime: Json2 documents parsed=%zu, rejected=%zu\n", state.parses.load(), state.failed.load());
}
