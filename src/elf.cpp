#include "elf.h"

#include "cpu.h"
#include "hle.h"
#include "mem.h"

namespace {

struct Ehdr {
    u8 ident[16];
    u16 type, machine;
    u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr { u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; };
struct Dyn { s32 tag; u32 val; };
struct Sym { u32 name, value, size; u8 info, other; u16 shndx; };
struct Rel { u32 offset, info; };

enum : u32 { PT_LOAD = 1, PT_DYNAMIC = 2 };
enum : s32 {
    DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_INIT = 12,
    DT_REL = 17, DT_RELSZ = 18, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27,
};
enum : u32 { R_ARM_NONE = 0, R_ARM_ABS32 = 2, R_ARM_REL32 = 3, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23 };

}  // namespace

bool load_elf(const std::vector<u8>& f, u32 base, Module& m) {
    if (f.size() < sizeof(Ehdr) || memcmp(f.data(), "\x7f" "ELF", 4) != 0) return false;
    auto& eh = *(const Ehdr*)f.data();
    if (eh.ident[4] != 1 || eh.machine != 40) {
        LOGE("not a 32-bit ARM ELF");
        return false;
    }
    auto ph = (const Phdr*)(f.data() + eh.phoff);

    u32 lo = ~0u, hi = 0, dyn_vaddr = 0;
    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type == PT_LOAD) {
            lo = std::min(lo, ph[i].vaddr & ~0xFFFu);
            hi = std::max(hi, ph[i].vaddr + ph[i].memsz);
        } else if (ph[i].type == PT_DYNAMIC) {
            dyn_vaddr = ph[i].vaddr;
        }
    }
    if (lo != 0) LOGW("ELF lowest vaddr is %08x", lo);
    m.base = base - lo;
    m.size = align_up(hi - lo, 0x1000);
    mem::commit(base, m.size);
    std::memset(mem::H(base), 0, m.size);
    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        m.segments.push_back({ph[i].vaddr, ph[i].memsz, ph[i].flags});
        std::memcpy(mem::H(m.base + ph[i].vaddr), f.data() + ph[i].offset, ph[i].filesz);
    }
    LOGI("loaded ELF at %08x-%08x", m.base + lo, m.base + hi);

    // dynamic section
    u32 symtab = 0, strtab = 0, hash = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    u32 init = 0, init_array = 0, init_arraysz = 0;
    for (auto d = &mem::ref<Dyn>(m.base + dyn_vaddr); d->tag != DT_NULL; d++) {
        switch (d->tag) {
        case DT_SYMTAB: symtab = m.base + d->val; break;
        case DT_STRTAB: strtab = m.base + d->val; break;
        case DT_HASH: hash = m.base + d->val; break;
        case DT_REL: rel = m.base + d->val; break;
        case DT_RELSZ: relsz = d->val; break;
        case DT_JMPREL: jmprel = m.base + d->val; break;
        case DT_PLTRELSZ: pltrelsz = d->val; break;
        case DT_INIT: init = m.base + d->val; break;
        case DT_INIT_ARRAY: init_array = m.base + d->val; break;
        case DT_INIT_ARRAYSZ: init_arraysz = d->val; break;
        }
    }
    if (!symtab || !strtab || !hash) {
        LOGE("missing dynamic symbol info");
        return false;
    }
    u32 nsyms = mem::rd32(hash + 4);  // nchain
    auto syms = &mem::ref<Sym>(symtab);
    auto name_of = [&](u32 i) { return mem::str(strtab + syms[i].name); };

    for (u32 i = 1; i < nsyms; i++)
        if (syms[i].shndx != 0 && syms[i].name) m.exports[name_of(i)] = m.base + syms[i].value;

    // resolve each symbol at most once
    std::vector<u32> sym_addr(nsyms, ~0u);
    auto resolve = [&](u32 i) -> u32 {
        if (sym_addr[i] != ~0u) return sym_addr[i];
        u32 a = syms[i].shndx != 0 ? m.base + syms[i].value : hle::resolve(name_of(i));
        return sym_addr[i] = a;
    };

    auto apply = [&](u32 table, u32 size) {
        for (u32 off = 0; off < size; off += sizeof(Rel)) {
            Rel r = mem::ref<Rel>(table + off);
            u32 type = r.info & 0xFF, si = r.info >> 8;
            u32 where = m.base + r.offset;
            switch (type) {
            case R_ARM_NONE: break;
            case R_ARM_RELATIVE: mem::wr32(where, mem::rd32(where) + m.base); break;
            case R_ARM_ABS32: mem::wr32(where, mem::rd32(where) + resolve(si)); break;
            case R_ARM_REL32: mem::wr32(where, mem::rd32(where) + resolve(si) - where); break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: mem::wr32(where, resolve(si)); break;
            default: LOGE("unsupported relocation type %u at %08x", type, r.offset); return false;
            }
        }
        return true;
    };
    if (!apply(rel, relsz) || !apply(jmprel, pltrelsz)) return false;

    if (init) m.init_funcs.push_back(init);
    for (u32 i = 0; i < init_arraysz / 4; i++) {
        u32 fn = mem::rd32(init_array + i * 4);
        if (fn != 0 && fn != ~0u) m.init_funcs.push_back(fn);
    }
    LOGI("%u exports, %u constructors", (u32)m.exports.size(), (u32)m.init_funcs.size());
    return true;
}

void run_init(const Module& m) {
    for (u32 fn : m.init_funcs) cpu::call(fn);
}
