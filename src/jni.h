// Fake Java VM: a JNIEnv/JavaVM in guest memory backed by host-side
// reimplementations of the Java classes the game calls.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common.h"

struct Module;

namespace java {

// Raw jvalue bits: ints/objects/floats in the low 32 bits, longs/doubles in all 64.
using Value = u64;
inline Value F(float f) { return bitcast<u32>(f); }
inline float asF(Value v) { return bitcast<float>((u32)v); }

struct Object {
    std::string cls;
    virtual ~Object() = default;
};
struct String : Object {
    std::string utf8;
};
struct Array : Object {
    char elem = 'B';
    u32 len = 0, data = 0;  // guest buffer
    ~Array() override;
};

// Object handles (jobject) as seen by the guest.
u32 new_ref(std::shared_ptr<Object> o);
std::shared_ptr<Object> get(u32 handle);
template <class T> std::shared_ptr<T> get_as(u32 h) { return std::dynamic_pointer_cast<T>(get(h)); }

u32 new_string(const std::string& s);
std::string string_of(u32 handle);  // "" for null
u32 new_byte_array(const void* data, u32 len);

// Host implementation of a Java method. `self` is the jobject (or jclass
// for static methods).
using Method = std::function<Value(u32 self, const std::vector<Value>& args)>;
void def(const std::string& cls, const std::string& name, const std::string& sig, Method fn);

void init(const Module& game);
u32 vm();
u32 env();

// Call a native method of the game (Java_<pkg>_<cls>_<name> or RegisterNatives).
u64 call_native(const char* cls, const char* name, std::initializer_list<u32> args = {});
// Whether the loaded library has that native method (builds differ).
bool has_native(const char* cls, const char* name);

void register_classes();  // java_classes.cpp

}  // namespace java
