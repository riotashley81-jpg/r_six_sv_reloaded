// Loader for 32-bit ARM Android shared objects.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"

struct Module {
    struct Segment { u32 vaddr, memsz, flags; };  // PT_LOAD, flags: 1 = X, 2 = W, 4 = R
    u32 base = 0, size = 0;
    std::vector<Segment> segments;
    std::unordered_map<std::string, u32> exports;
    std::vector<u32> init_funcs;  // DT_INIT then DT_INIT_ARRAY, in order

    u32 sym(const std::string& name) const {
        auto it = exports.find(name);
        return it == exports.end() ? 0 : it->second;
    }
};

bool load_elf(const std::vector<u8>& file, u32 load_base, Module& out);
void run_init(const Module& m);
