#include "jni.h"

#include <mutex>
#include <unordered_map>

#include "abi.h"
#include "elf.h"

namespace java {

static const char* const PKG = "com/gameloft/android/ANMP/GloftR6HP/";

// ---------------------------------------------------------------------------
// handles

struct Ref {
    std::shared_ptr<Object> obj;
    int count = 0;
};
static std::recursive_mutex ref_lock;
static std::unordered_map<u32, Ref> refs;
static u32 next_handle = 0x1000;
static thread_local std::vector<std::vector<u32>> local_frames;

Array::~Array() { mem::free(data); }

u32 new_ref(std::shared_ptr<Object> o) {
    if (!o) return 0;
    std::lock_guard lk(ref_lock);
    u32 h = next_handle += 4;
    refs[h] = {std::move(o), 1};
    if (!local_frames.empty()) local_frames.back().push_back(h);
    return h;
}

static void release(u32 h) {
    std::lock_guard lk(ref_lock);
    auto it = refs.find(h);
    if (it != refs.end() && --it->second.count <= 0) refs.erase(it);
}

static void retain(u32 h) {
    std::lock_guard lk(ref_lock);
    auto it = refs.find(h);
    if (it != refs.end()) it->second.count++;
}

std::shared_ptr<Object> get(u32 h) {
    std::lock_guard lk(ref_lock);
    auto it = refs.find(h);
    return it == refs.end() ? nullptr : it->second.obj;
}

u32 new_string(const std::string& s) {
    auto o = std::make_shared<String>();
    o->cls = "java/lang/String";
    o->utf8 = s;
    return new_ref(o);
}

std::string string_of(u32 h) {
    auto s = get_as<String>(h);
    return s ? s->utf8 : std::string();
}

static u32 elem_size(char t) { return t == 'J' || t == 'D' ? 8 : t == 'I' || t == 'F' ? 4 : t == 'S' || t == 'C' ? 2 : t == 'L' ? 4 : 1; }

static u32 new_array(char elem, u32 len) {
    auto a = std::make_shared<Array>();
    a->cls = std::string("[") + elem;
    a->elem = elem;
    a->len = len;
    a->data = mem::calloc(len ? len : 1, elem_size(elem));
    return new_ref(a);
}

u32 new_byte_array(const void* data, u32 len) {
    u32 h = new_array('B', len);
    if (data && len) memcpy(mem::H(get_as<Array>(h)->data), data, len);
    return h;
}

// classes are interned so the same class name always yields the same handle
static std::unordered_map<std::string, u32> class_handles;
static u32 class_ref(const std::string& name) {
    std::lock_guard lk(ref_lock);
    auto& h = class_handles[name];
    if (!h) {
        auto o = std::make_shared<Object>();
        o->cls = name;
        h = next_handle += 4;
        refs[h] = {o, 1 << 30};  // permanent, never part of a local frame
    }
    return h;
}
static std::string class_name_of(u32 h) {
    auto o = get(h);
    return o ? o->cls : std::string();
}

// ---------------------------------------------------------------------------
// methods and fields

struct MethodInfo {
    std::string cls, name, sig;
    std::vector<char> params;
    char ret = 'V';
    Method fn;
};
static std::vector<MethodInfo> methods;  // method id = index + 1
static std::unordered_map<std::string, u32> method_index;
static std::unordered_map<std::string, Value> fields;  // "cls.name" -> value (static and instance)
static std::vector<std::string> field_names;            // field id = index + 1

static void parse_sig(MethodInfo& m) {
    const char* p = m.sig.c_str();
    if (*p == '(') p++;
    while (*p && *p != ')') {
        char t = *p;
        while (*p == '[') p++;
        if (*p == 'L') while (*p && *p != ';') p++;
        p++;
        m.params.push_back(t == '[' ? 'L' : t);
    }
    if (*p == ')') p++;
    m.ret = *p == '[' ? 'L' : *p;
}

static u32 find_method(const std::string& cls, const std::string& name, const std::string& sig) {
    std::string key = cls + "." + name + sig;
    auto it = method_index.find(key);
    if (it != method_index.end()) return it->second;
    MethodInfo m{cls, name, sig};
    parse_sig(m);
    LOGW("JNI: no implementation for %s, using stub", key.c_str());
    methods.push_back(std::move(m));
    return method_index[key] = (u32)methods.size();
}

void def(const std::string& cls, const std::string& name, const std::string& sig, Method fn) {
    std::string key = cls + "." + name + sig;
    MethodInfo m{cls, name, sig};
    parse_sig(m);
    m.fn = std::move(fn);
    methods.push_back(std::move(m));
    method_index[key] = (u32)methods.size();
}

// read call arguments according to the method's signature
enum class ArgMode { Vararg, VaList, Array };
static std::vector<Value> read_args(Cpu& c, const MethodInfo& m, ArgMode mode, int first) {
    std::vector<Value> out;
    if (mode == ArgMode::Array) {
        u32 p = c.arg(first);
        for (size_t i = 0; i < m.params.size(); i++) out.push_back(mem::ref<u64>(p + 8 * (u32)i));
        return out;
    }
    ArgReader rd = mode == ArgMode::VaList ? ArgReader::from_va_list(c, c.arg(first)) : ArgReader(c, first);
    for (char t : m.params) {
        switch (t) {
        case 'J': case 'D': out.push_back(rd.w64()); break;
        case 'F': out.push_back(F((float)bitcast<double>(rd.w64()))); break;  // promoted to double
        default: out.push_back(rd.w32()); break;
        }
    }
    return out;
}

static void set_ret(Cpu& c, char t, Value v) {
    if (t == 'J' || t == 'D') c.ret64(v);
    else c.ret((u32)v);
}

static Value invoke(u32 mid, u32 self, const std::vector<Value>& args) {
    if (!mid || mid > methods.size()) {
        LOGE("JNI: call with bad method id %u", mid);
        return 0;
    }
    auto& m = methods[mid - 1];
    if (!m.fn) {
        static std::unordered_map<u32, int> seen;
        if (seen[mid]++ < 3) LOGW("JNI stub called: %s.%s%s", m.cls.c_str(), m.name.c_str(), m.sig.c_str());
        return 0;
    }
    return m.fn(self, args);
}

// ---------------------------------------------------------------------------
// JNIEnv functions (indices follow JNINativeInterface)

static u32 g_vm, g_env;
static const Module* game;
static std::unordered_map<std::string, u32> registered_natives;

enum : int {
    GetVersion = 4, FindClass = 6, Throw = 13, ThrowNew = 14, ExceptionOccurred = 15, ExceptionDescribe = 16,
    ExceptionClear = 17, PushLocalFrame = 19, PopLocalFrame = 20, NewGlobalRef = 21, DeleteGlobalRef = 22,
    DeleteLocalRef = 23, IsSameObject = 24, NewLocalRef = 25, EnsureLocalCapacity = 26, AllocObject = 27,
    NewObject = 28, NewObjectV = 29, NewObjectA = 30, GetObjectClass = 31, IsInstanceOf = 32, GetMethodID = 33,
    CallFirst = 34, CallLast = 63, NonvirtualFirst = 64, NonvirtualLast = 93, GetFieldID = 94,
    GetFieldFirst = 95, GetFieldLast = 103, SetFieldFirst = 104, SetFieldLast = 112, GetStaticMethodID = 113,
    CallStaticFirst = 114, CallStaticLast = 143, GetStaticFieldID = 144, GetStaticFieldFirst = 145,
    GetStaticFieldLast = 153, SetStaticFieldFirst = 154, SetStaticFieldLast = 162, NewString = 163,
    GetStringLength = 164, GetStringChars = 165, ReleaseStringChars = 166, NewStringUTF = 167,
    GetStringUTFLength = 168, GetStringUTFChars = 169, ReleaseStringUTFChars = 170, GetArrayLength = 171,
    NewObjectArray = 172, GetObjectArrayElement = 173, SetObjectArrayElement = 174, NewArrayFirst = 175,
    NewArrayLast = 182, GetElementsFirst = 183, GetElementsLast = 190, ReleaseElementsFirst = 191,
    ReleaseElementsLast = 198, GetRegionFirst = 199, GetRegionLast = 206, SetRegionFirst = 207,
    SetRegionLast = 214, RegisterNatives = 215, UnregisterNatives = 216, MonitorEnter = 217, MonitorExit = 218,
    GetJavaVM = 219, GetPrimitiveArrayCritical = 222, ReleasePrimitiveArrayCritical = 223,
    NewWeakGlobalRef = 226, DeleteWeakGlobalRef = 227, ExceptionCheck = 228, NUM_JNI = 233,
};
static const char PRIM[] = "ZBCSIJFD";  // order used by array/field function groups

static thread_local int jni_index;
static void jni_dispatch(Cpu& c);

static std::string field_key(u32 fid) { return fid && fid <= field_names.size() ? field_names[fid - 1] : "?"; }

static u32 field_id(const std::string& cls, const std::string& name) {
    std::string key = cls + "." + name;
    for (size_t i = 0; i < field_names.size(); i++)
        if (field_names[i] == key) return (u32)i + 1;
    LOGI("JNI: field %s", key.c_str());
    field_names.push_back(key);
    return (u32)field_names.size();
}

static void jni_dispatch(Cpu& c) {
    int i = jni_index;
    u32 a1 = c.r(1), a2 = c.r(2), a3 = c.r(3);

    if (i >= CallFirst && i <= CallLast) {
        auto mode = (ArgMode)((i - CallFirst) % 3);
        auto& m = methods[a2 - 1 < methods.size() ? a2 - 1 : 0];
        set_ret(c, m.ret, invoke(a2, a1, read_args(c, m, mode, 3)));
        return;
    }
    if (i >= CallStaticFirst && i <= CallStaticLast) {
        auto mode = (ArgMode)((i - CallStaticFirst) % 3);
        auto& m = methods[a2 - 1 < methods.size() ? a2 - 1 : 0];
        set_ret(c, m.ret, invoke(a2, a1, read_args(c, m, mode, 3)));
        return;
    }
    if (i >= NonvirtualFirst && i <= NonvirtualLast) {  // (env, obj, cls, mid, ...)
        auto mode = (ArgMode)((i - NonvirtualFirst) % 3);
        auto& m = methods[a3 - 1 < methods.size() ? a3 - 1 : 0];
        set_ret(c, m.ret, invoke(a3, a1, read_args(c, m, mode, 4)));
        return;
    }
    auto field_get = [&](u32 fid, bool wide) {
        std::lock_guard lk(ref_lock);
        Value v = fields[field_key(fid)];
        if (wide) c.ret64(v); else c.ret((u32)v);
    };
    auto field_set = [&](u32 fid, bool wide) {
        // env, obj, fid take r0-r2; a 64-bit value can't use r3 alone and goes on the stack
        Value v = wide ? mem::ref<u64>(c.sp()) : c.r(3);
        std::lock_guard lk(ref_lock);
        fields[field_key(fid)] = v;
    };
    if (i >= GetFieldFirst && i <= GetFieldLast) { field_get(a2, i == GetFieldFirst + 6 || i == GetFieldFirst + 8); return; }
    if (i >= GetStaticFieldFirst && i <= GetStaticFieldLast) { field_get(a2, i == GetStaticFieldFirst + 6 || i == GetStaticFieldFirst + 8); return; }
    if (i >= SetFieldFirst && i <= SetFieldLast) { field_set(a2, i == SetFieldFirst + 6 || i == SetFieldFirst + 8); return; }
    if (i >= SetStaticFieldFirst && i <= SetStaticFieldLast) { field_set(a2, i == SetStaticFieldFirst + 6 || i == SetStaticFieldFirst + 8); return; }

    if (i >= NewArrayFirst && i <= NewArrayLast) { c.ret(new_array(PRIM[i - NewArrayFirst], a1)); return; }
    if (i >= GetElementsFirst && i <= GetElementsLast) {
        auto a = get_as<Array>(a1);
        if (a2) mem::wr32(a2, 0);  // isCopy = false
        c.ret(a ? a->data : 0);
        return;
    }
    if (i >= ReleaseElementsFirst && i <= ReleaseElementsLast) return;  // elements are never copied
    if (i >= GetRegionFirst && i <= SetRegionLast) {
        // (env, array, start, len, buf)
        auto a = get_as<Array>(a1);
        u32 len = a3, buf = c.arg(4);
        if (!a) return;
        u32 es = elem_size(a->elem);
        u8* arr = (u8*)mem::H(a->data) + a2 * es;
        if (i <= GetRegionLast) memcpy(mem::H(buf), arr, len * es);
        else memcpy(arr, mem::H(buf), len * es);
        return;
    }

    switch (i) {
    case GetVersion: c.ret(0x00010006); return;
    case FindClass: {
        std::string name = mem::str(a1);
        LOGI("JNI FindClass(%s)", name.c_str());
        c.ret(class_ref(name));
        return;
    }
    case Throw: case ThrowNew:
        LOGW("JNI: guest threw %s", i == ThrowNew ? mem::str(a2) : "an exception");
        c.ret(0);
        return;
    case ExceptionOccurred: case ExceptionCheck: c.ret(0); return;
    case ExceptionDescribe: case ExceptionClear: return;
    case PushLocalFrame: case EnsureLocalCapacity: c.ret(0); return;
    case PopLocalFrame: c.ret(a1); return;
    case NewGlobalRef: case NewLocalRef: case NewWeakGlobalRef: retain(a1); c.ret(a1); return;
    case DeleteGlobalRef: case DeleteLocalRef: case DeleteWeakGlobalRef: release(a1); return;
    case IsSameObject: c.ret(a1 == a2); return;
    case AllocObject: {
        auto o = std::make_shared<Object>();
        o->cls = class_name_of(a1);
        c.ret(new_ref(o));
        return;
    }
    case NewObject: case NewObjectV: case NewObjectA: {
        auto o = std::make_shared<Object>();
        o->cls = class_name_of(a1);
        u32 h = new_ref(o);
        auto& m = methods[a2 - 1 < methods.size() ? a2 - 1 : 0];
        invoke(a2, h, read_args(c, m, (ArgMode)(i - NewObject), 3));
        c.ret(h);
        return;
    }
    case GetObjectClass: c.ret(class_ref(class_name_of(a1))); return;
    case IsInstanceOf: c.ret(class_name_of(a1) == class_name_of(a2)); return;
    case GetMethodID: case GetStaticMethodID:
        c.ret(find_method(class_name_of(a1), mem::str(a2), mem::str(a3)));
        return;
    case GetFieldID: case GetStaticFieldID:
        c.ret(field_id(class_name_of(a1), mem::str(a2)));
        return;
    case NewStringUTF: c.ret(a1 ? new_string(mem::str(a1)) : 0); return;
    case NewString: {  // UTF-16 -> UTF-8 (BMP only)
        std::string s;
        for (u32 k = 0; k < a2; k++) {
            u16 ch = mem::ref<u16>(a1 + 2 * k);
            if (ch < 0x80) s += (char)ch;
            else if (ch < 0x800) { s += (char)(0xC0 | (ch >> 6)); s += (char)(0x80 | (ch & 0x3F)); }
            else { s += (char)(0xE0 | (ch >> 12)); s += (char)(0x80 | ((ch >> 6) & 0x3F)); s += (char)(0x80 | (ch & 0x3F)); }
        }
        c.ret(new_string(s));
        return;
    }
    case GetStringUTFLength: c.ret((u32)string_of(a1).size()); return;
    case GetStringLength: {
        u32 n = 0;
        for (u8 ch : string_of(a1)) if ((ch & 0xC0) != 0x80) n++;
        c.ret(n);
        return;
    }
    case GetStringUTFChars:
        if (a2) mem::wr32(a2, 1);
        c.ret(mem::strdup(string_of(a1).c_str()));
        return;
    case ReleaseStringUTFChars: mem::free(a2); return;
    case GetStringChars: {
        std::string s = string_of(a1);
        u32 g = mem::alloc((u32)s.size() * 2 + 2);
        u32 n = 0;
        for (size_t k = 0; k < s.size(); k++) mem::ref<u16>(g + 2 * n++) = (u8)s[k];  // ASCII is all we see
        mem::ref<u16>(g + 2 * n) = 0;
        if (a2) mem::wr32(a2, 1);
        c.ret(g);
        return;
    }
    case ReleaseStringChars: mem::free(a2); return;
    case GetArrayLength: {
        auto a = get_as<Array>(a1);
        c.ret(a ? a->len : 0);
        return;
    }
    case NewObjectArray: c.ret(new_array('L', a1)); return;
    case GetObjectArrayElement: {
        auto a = get_as<Array>(a1);
        c.ret(a && a2 < a->len ? mem::rd32(a->data + 4 * a2) : 0);
        return;
    }
    case SetObjectArrayElement: {
        auto a = get_as<Array>(a1);
        if (a && a2 < a->len) mem::wr32(a->data + 4 * a2, a3);
        return;
    }
    case GetPrimitiveArrayCritical: {
        auto a = get_as<Array>(a1);
        if (a2) mem::wr32(a2, 0);
        c.ret(a ? a->data : 0);
        return;
    }
    case ReleasePrimitiveArrayCritical: return;
    case RegisterNatives: {  // (env, cls, const JNINativeMethod*, n)
        std::string cls = class_name_of(a1);
        for (u32 k = 0; k < a3; k++) {
            u32 e = a2 + 12 * k;
            std::string key = cls + "." + mem::str(mem::rd32(e));
            registered_natives[key] = mem::rd32(e + 8);
            LOGI("JNI RegisterNatives %s%s -> %08x", key.c_str(), mem::str(mem::rd32(e + 4)), mem::rd32(e + 8));
        }
        c.ret(0);
        return;
    }
    case UnregisterNatives: case MonitorEnter: case MonitorExit: c.ret(0); return;
    case GetJavaVM: mem::wr32(a1, g_vm); c.ret(0); return;
    }
    fatal("JNI function #%d is not implemented (lr=%08x)", i, c.r(14));
}

template <int I> static void jni_entry(Cpu& c) {
    int saved = jni_index;
    jni_index = I;
    jni_dispatch(c);
    jni_index = saved;
}
template <int... Is> static void make_jni_table(u32 table, std::integer_sequence<int, Is...>) {
    ((mem::wr32(table + 4 * Is, cpu::make_thunk(jni_entry<Is>, "JNIEnv"))), ...);
}

// JavaVM: GetEnv / AttachCurrentThread just hand out the single env
static void vm_ok(Cpu& c) { c.ret(0); }
static void vm_get_env(Cpu& c) {
    mem::wr32(c.r(1), g_env);
    c.ret(0);
}

void init(const Module& m) {
    game = &m;
    u32 table = mem::data_alloc(4 * NUM_JNI);
    make_jni_table(table, std::make_integer_sequence<int, NUM_JNI>{});
    g_env = mem::data_alloc(4);
    mem::wr32(g_env, table);

    u32 vtable = mem::data_alloc(4 * 8);
    for (int k = 0; k < 8; k++) mem::wr32(vtable + 4 * k, cpu::make_thunk(vm_ok, "JavaVM"));
    mem::wr32(vtable + 4 * 4, cpu::make_thunk(vm_get_env, "AttachCurrentThread"));
    mem::wr32(vtable + 4 * 6, cpu::make_thunk(vm_get_env, "GetEnv"));
    mem::wr32(vtable + 4 * 7, cpu::make_thunk(vm_get_env, "AttachCurrentThreadAsDaemon"));
    g_vm = mem::data_alloc(4);
    mem::wr32(g_vm, vtable);

    register_classes();
}

u32 vm() { return g_vm; }
u32 env() { return g_env; }

static u32 find_native(const std::string& full, const char* name) {
    auto it = registered_natives.find(full + "." + name);
    if (it != registered_natives.end()) return it->second;
    std::string sym = "Java_" + full + "_" + name;
    for (char& ch : sym) if (ch == '/') ch = '_';
    return game->sym(sym);
}

bool has_native(const char* cls, const char* name) { return find_native(std::string(PKG) + cls, name) != 0; }

u64 call_native(const char* cls, const char* name, std::initializer_list<u32> args) {
    std::string full = std::string(PKG) + cls;
    u32 fn = find_native(full, name);
    if (!fn) {
        LOGW("native method %s.%s not found", cls, name);
        return 0;
    }
    std::vector<u32> a = {g_env, class_ref(full)};
    a.insert(a.end(), args.begin(), args.end());
    local_frames.emplace_back();
    u64 r;
    switch (a.size()) {  // call() takes an initializer_list
    case 2: r = cpu::call64(fn, {a[0], a[1]}); break;
    case 3: r = cpu::call64(fn, {a[0], a[1], a[2]}); break;
    case 4: r = cpu::call64(fn, {a[0], a[1], a[2], a[3]}); break;
    case 5: r = cpu::call64(fn, {a[0], a[1], a[2], a[3], a[4]}); break;
    case 6: r = cpu::call64(fn, {a[0], a[1], a[2], a[3], a[4], a[5]}); break;
    default: fatal("too many native args");
    }
    for (u32 h : local_frames.back()) release(h);
    local_frames.pop_back();
    return r;
}

}  // namespace java
