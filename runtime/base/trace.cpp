// SPDX-License-Identifier: MIT
// Per-thread event rings and interned call stacks for CSAN_TRACE, and the
// TSan-style finding blocks printed from them.
#include "base/trace.h"

#include "base/atomics.h"
#include "base/report.h"
#include "base/spinlock.h"
#include "repr/vectorclock.h"
#include "repr/slotmap.h"
#include "repr/shadow.h"

#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace csan {

namespace {

// ---- modules ---------------------------------------------------------------
//
// A frame is (module index, file offset), not a runtime address: each machine
// has its own load bias and stacks are symbolized after the fact. dladdr gives
// both but walks the link map, so its answers are cached per process.

struct ModuleRange {
    uintptr_t lo;
    uintptr_t hi;
    uintptr_t bias; // subtract to get what the symbolizer wants: see pack_frame
    uint32_t index;
};

// Process-local: the shared table is the authority on names and indices, this
// is only a cache of where those modules landed in THIS process.
constexpr uint32_t kModuleCacheSize = 8;
ModuleRange t_module_cache[kModuleCacheSize];
uint32_t t_module_cached = 0;

// The shared index for this path, adding it if it is new. Caller holds the
// global lock.
uint32_t module_index_locked(const char* path) {
    for (uint32_t i = 0; i < g_header->module_count; ++i) {
        if (std::strcmp(g_header->modules[i].path, path) == 0) {
            return i;
        }
    }
    if (g_header->module_count >= kMaxModules) {
        return 0;
    }
    uint32_t i = g_header->module_count++;
    std::snprintf(g_header->modules[i].path, sizeof(g_header->modules[i].path), "%s", path);
    return i;
}

// Whether the module mapped here is a non-PIE executable (ET_EXEC).
bool elf_is_exec(const void* map) {
    const auto* eh = static_cast<const Elf64_Ehdr*>(map);
    return std::memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 && eh->e_type == ET_EXEC;
}

// A return address as (module, file offset -- or virtual address, above), or 0
// when it cannot be placed.
uint64_t pack_frame(const void* pc) {
    uintptr_t a = reinterpret_cast<uintptr_t>(pc);
    for (uint32_t i = 0; i < t_module_cached; ++i) {
        if (a >= t_module_cache[i].lo && a < t_module_cache[i].hi) {
            return frame_pack(t_module_cache[i].index, a - t_module_cache[i].bias);
        }
    }
    Dl_info info;
    if (dladdr(const_cast<void*>(pc), &info) == 0 || info.dli_fname == nullptr ||
        info.dli_fbase == nullptr) {
        return 0;
    }
    uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    // llvm-symbolizer wants a file offset for a shared object but a VIRTUAL
    // ADDRESS for a non-PIE executable, which is mapped at its own link
    // address; subtracting the base there yields an address it rejects.
    uintptr_t bias = elf_is_exec(info.dli_fbase) ? 0 : base;
    spin_lock(&g_header->global_lock);
    uint32_t idx = module_index_locked(info.dli_fname);
    spin_unlock(&g_header->global_lock);
    if (t_module_cached < kModuleCacheSize) {
        // One page past the last seen address is a guess at the module's
        // extent; a miss only costs another dladdr, never a wrong answer,
        // because the next lookup re-checks and widens.
        t_module_cache[t_module_cached] = {base, a + 4096, bias, idx};
        ++t_module_cached;
    }
    return frame_pack(idx, a - bias);
}

// ---- stack interning -------------------------------------------------------

uint32_t mix(uint64_t v, uint32_t h) {
    h ^= static_cast<uint32_t>(v) + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= static_cast<uint32_t>(v >> 32) + 0x85ebca6bu + (h << 6) + (h >> 2);
    return h;
}

// The id of this stack, interning it if new. 0 when the table is full, which
// prints as unavailable rather than as some other stack.
uint32_t intern_stack(const uint64_t* frames) {
    uint32_t h = 1;
    for (uint32_t i = 0; i < kStackFrames; ++i) {
        h = mix(frames[i], h);
    }
    if (h == 0) {
        h = 1; // 0 marks a free slot
    }
    uint32_t mask = g_trace_stacks - 1;
    for (uint32_t probe = 0; probe < 64; ++probe) {
        uint32_t i = (h + probe) & mask;
        StackEntry& e = g_stacks[i];
        uint32_t seen = atomic_load_u32_acq(&e.hash);
        if (seen == 0) {
            uint32_t expect = 0;
            if (!__atomic_compare_exchange_n(&e.hash, &expect, h, false, __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE)) {
                --probe; // someone else took it; re-read this slot
                continue;
            }
            std::memcpy(e.frame, frames, sizeof(e.frame));
            atomic_store_u32_rel(&e.pad, 1); // frames are readable now
            return i + 1;
        }
        if (seen == h) {
            while (atomic_load_u32_acq(&e.pad) == 0) {
                __builtin_ia32_pause();
            }
            if (std::memcmp(e.frame, frames, sizeof(e.frame)) == 0) {
                return i + 1;
            }
        }
    }
    return 0;
}

// ---- capturing a stack ------------------------------------------------------
//
// The frame chain, walked by hand. __builtin_return_address(N) for N > 0 is
// undefined past the current function and faults on a stack shorter than N.
//
// Every candidate is checked before it is followed -- inside the stack, above
// the previous frame, aligned -- so a short stack comes back short.
struct Frame {
    Frame* next;
    void* ret;
};

// No thread stack is larger, so a chain reaching past this is not a chain.
constexpr uintptr_t kMaxFrameSpan = 8u << 20;

__attribute__((noinline)) uint32_t capture_frames(const void** out, uint32_t max, uint32_t skip) {
    Frame* fp = static_cast<Frame*>(__builtin_frame_address(0));
    uintptr_t lo = reinterpret_cast<uintptr_t>(fp);
    uintptr_t hi = lo + kMaxFrameSpan;
    uint32_t n = 0;
    while (n < max + skip && fp != nullptr) {
        uintptr_t a = reinterpret_cast<uintptr_t>(fp);
        if (a < lo || a >= hi || (a & 7u) != 0 || fp->ret == nullptr) {
            break;
        }
        if (n >= skip) {
            out[n - skip] = fp->ret;
        }
        ++n;
        lo = a + 1; // strictly increasing, so a cycle cannot spin here
        fp = fp->next;
    }
    return n > skip ? n - skip : 0;
}

// ---- symbolization ----------------------------------------------------------
//
// Frames go to llvm-symbolizer as "<module> 0x<offset>" lines. Without one, the
// module and offset print as they are.

// The symbolizer matching the compiler that built this runtime. Derived rather
// than hardcoded: only that version can read the debug info that compiler
// emits.
#define CSAN_STR2(x) #x
#define CSAN_STR(x) CSAN_STR2(x)
#if defined(__clang_major__)
constexpr const char* kVersionedSymbolizer = "llvm-symbolizer-" CSAN_STR(__clang_major__);
#else
constexpr const char* kVersionedSymbolizer = "llvm-symbolizer";
#endif

// One child per stack: all its frames are written, stdin is CLOSED, then the
// answers are read to EOF. Closing is what avoids a deadlock -- the child's
// stdout is a pipe and so fully buffered, and a query-at-a-time reader would
// wait for a flush that only comes when the buffer fills.
bool symbolize_stack(const char* const* modules, const uint64_t* offs, uint32_t n,
                     char funcs[kStackFrames][256], char locs[kStackFrames][512]) {
    for (uint32_t i = 0; i < n; ++i) {
        funcs[i][0] = '\0';
        locs[i][0] = '\0';
    }
    int to_child[2];
    int from_child[2];
    if (n == 0 || pipe(to_child) != 0) {
        return false;
    }
    if (pipe(from_child) != 0) {
        close(to_child[0]);
        close(to_child[1]);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        return false;
    }
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[1]);
        close(from_child[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
        }
        const char* env = getenv("CSAN_SYMBOLIZER");
        if (env != nullptr && *env != '\0') {
            execlp(env, env, nullptr);
        }
        // The version-matched name first: an older symbolizer silently answers
        // "??:0:0" for DWARF it cannot read.
        execlp(kVersionedSymbolizer, kVersionedSymbolizer, nullptr);
        execlp("llvm-symbolizer", "llvm-symbolizer", nullptr);
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    for (uint32_t i = 0; i < n; ++i) {
        char q[512];
        int qn = std::snprintf(q, sizeof(q), "%s 0x%" PRIx64 "\n", modules[i], offs[i]);
        if (qn > 0) {
            ssize_t ignored = write(to_child[1], q, static_cast<size_t>(qn));
            (void)ignored;
        }
    }
    close(to_child[1]); // the child now sees EOF, finishes, and flushes

    char buf[8192];
    size_t used = 0;
    ssize_t r = 0;
    while (used < sizeof(buf) - 1 &&
           (r = read(from_child[0], buf + used, sizeof(buf) - 1 - used)) > 0) {
        used += static_cast<size_t>(r);
    }
    buf[used] = '\0';
    close(from_child[0]);
    if (getenv("CSAN_TRACE_DEBUG") != nullptr) {
        std::fprintf(stderr, "[symbolizer raw, %u queries]\n%s[end]\n", n, buf);
    }
    int status = 0;
    waitpid(pid, &status, 0);

    // Three lines per frame: function, file:line:col, blank.
    uint32_t frame = 0;
    uint32_t line_in_frame = 0;
    char* p = buf;
    while (frame < n && *p != '\0') {
        char* nl = std::strchr(p, '\n');
        if (nl == nullptr) {
            break;
        }
        *nl = '\0';
        if (line_in_frame == 0) {
            std::snprintf(funcs[frame], 256, "%s", p);
        } else if (line_in_frame == 1) {
            std::snprintf(locs[frame], 512, "%s", p);
        }
        ++line_in_frame;
        if (line_in_frame == 3) {
            line_in_frame = 0;
            ++frame;
        }
        p = nl + 1;
    }
    return frame > 0;
}

const char* const kEventNames[kEvKindCount] = {
    "none",   "read",   "write",  "writeback", "flush",  "sfence",
    "mfence", "lock",   "unlock", "release",   "acquire",
};

} // namespace

const char* event_kind_name(uint8_t kind) {
    return kind < kEvKindCount ? kEventNames[kind] : "?";
}

const char* event_src_name(uint16_t src, bool is_wb) {
    bool asm_form = (src & CSAN_SRC_ASM) != 0;
    switch (src & CSAN_SRC_KIND_MASK) {
    case CSAN_SRC_CLWB:
        return asm_form ? "clwb (inline asm)" : "clwb";
    case CSAN_SRC_CLFLUSHOPT:
        return asm_form ? "clflushopt (inline asm)" : "clflushopt";
    case CSAN_SRC_CLFLUSH:
        return asm_form ? "clflush (inline asm, self-fencing)" : "clflush (self-fencing)";
    case CSAN_SRC_NT_STORE:
        return "non-temporal store";
    default:
        return is_wb ? "write-back" : "flush";
    }
}

namespace {
thread_local uint32_t t_cur_pos = 0;
} // namespace

uint32_t trace_cur_pos() {
    return t_cur_pos;
}

uint32_t trace_record(uint8_t kind, uintptr_t addr, uint32_t len, uint32_t tid, uint32_t from,
                      uint32_t src) {
    if (!tracing() || tid >= kMaxThreads) {
        return 0;
    }
    // Drop capture_frames' own frame and this one, so #0 is the hook's caller.
    const void* pcs[kStackFrames] = {};
    uint32_t got = capture_frames(pcs, kStackFrames, 2);
    uint64_t frames[kStackFrames] = {};
    for (uint32_t i = 0; i < got; ++i) {
        frames[i] = pack_frame(pcs[i]);
    }

    // The kernel tid, so a report can name the thread the way top and gdb do.
    // Keyed by the checker tid rather than a flag: a forked child inherits the
    // parent's thread-locals, and would skip this having never recorded its own.
    static thread_local uint32_t noted_for = UINT32_MAX;
    if (noted_for != tid) {
        noted_for = tid;
        g_header->os_tid[tid] = static_cast<uint32_t>(syscall(SYS_gettid));
    }

    uint32_t seq = ++g_header->trace_seq[tid]; // seq 0 means "no event"
    Event& e = g_events[static_cast<uint64_t>(tid) * g_trace_depth + (seq % g_trace_depth)];
    e.addr = addr;
    e.stack = intern_stack(frames);
    t_cur_pos = e.stack;
    e.from = from;
    e.kind = kind;
    e.len = static_cast<uint8_t>(len > 255 ? 255 : len);
    e.src = static_cast<uint16_t>(src);
    // Last, and released: a walker that sees this seq can read the rest.
    atomic_store_u32_rel(&e.seq, seq);
    return event_ref(tid, seq);
}

void trace_set_from(uint32_t ref, uint32_t from) {
    if (!tracing() || ref == 0 || event_ref_tid(ref) >= kMaxThreads) {
        return;
    }
    uint32_t seq = event_ref_seq(ref);
    Event& e = g_events[static_cast<uint64_t>(event_ref_tid(ref)) * g_trace_depth +
                        (seq % g_trace_depth)];
    if (atomic_load_u32_acq(&e.seq) == seq) {
        e.from = from;
    }
}

const Event* trace_event_at(uint32_t ref) {
    if (!tracing() || ref == 0) {
        return nullptr;
    }
    uint32_t tid = event_ref_tid(ref);
    uint32_t seq = event_ref_seq(ref);
    if (tid >= kMaxThreads || seq == 0) {
        return nullptr;
    }
    const Event* e = &g_events[static_cast<uint64_t>(tid) * g_trace_depth + (seq % g_trace_depth)];
    // The slot is reused every trace_depth events, so a seq that no longer
    // matches means this event has aged out.
    return atomic_load_u32_acq(&e->seq) == seq ? e : nullptr;
}

namespace {

bool covers(const Event& e, uintptr_t addr, uint32_t len) {
    if (addr == 0) {
        return true;
    }
    uintptr_t lo = e.addr;
    uintptr_t hi = e.addr + (e.len != 0 ? e.len : 1);
    return addr < hi && lo < addr + (len != 0 ? len : 1);
}

} // namespace

const Event* trace_find_back(uint32_t tid, uint8_t kind, uintptr_t addr, uint32_t len) {
    if (!tracing() || tid >= kMaxThreads) {
        return nullptr;
    }
    uint32_t newest = g_header->trace_seq[tid];
    uint32_t oldest = newest > g_trace_depth ? newest - g_trace_depth + 1 : 1;
    for (uint32_t seq = newest; seq >= oldest && seq != 0; --seq) {
        const Event* e = trace_event_at(event_ref(tid, seq));
        if (e != nullptr && e->kind == kind && covers(*e, addr, len)) {
            return e;
        }
    }
    return nullptr;
}

const Event* trace_find_after(uint32_t tid, uint32_t after_seq, uint8_t kind, uintptr_t addr,
                              uint32_t len) {
    if (!tracing() || tid >= kMaxThreads) {
        return nullptr;
    }
    uint32_t newest = g_header->trace_seq[tid];
    for (uint32_t seq = after_seq + 1; seq <= newest; ++seq) {
        const Event* e = trace_event_at(event_ref(tid, seq));
        if (e != nullptr && e->kind == kind && covers(*e, addr, len)) {
            return e;
        }
    }
    return nullptr;
}

// The text of one source line, or empty when the file is not readable. Cached,
// since a report prints the same lines repeatedly.
const std::string& source_line(const std::string& path, uint32_t line) {
    static std::unordered_map<std::string, std::string> cache;
    static const std::string empty;
    if (path.empty() || line == 0) {
        return empty;
    }
    std::string key = path + ":" + std::to_string(line);
    auto hit = cache.find(key);
    if (hit != cache.end()) {
        return hit->second;
    }
    std::string text;
    if (std::FILE* f = std::fopen(path.c_str(), "r")) {
        char buf[1024];
        for (uint32_t n = 1; n <= line && std::fgets(buf, sizeof(buf), f) != nullptr; ++n) {
            if (n == line) {
                text = buf;
            }
        }
        std::fclose(f);
    }
    // Trim leading indentation and the trailing newline.
    size_t b = text.find_first_not_of(" \t");
    size_t e = text.find_last_not_of(" \t\r\n");
    text = (b == std::string::npos) ? std::string() : text.substr(b, e - b + 1);
    return cache.emplace(std::move(key), std::move(text)).first->second;
}

// A frame nobody wants to read: the standard library's own plumbing between the
// program's call and where it lands. TSan drops these too. Frame 0 is always
// kept, whatever it is.
bool boring_frame(const char* func, const char* loc) {
    if (std::strcmp(func, "??") == 0) {
        return true; // no debug info for it, so it names nothing
    }
    static const char* const kPaths[] = {"/include/c++/", "/usr/lib/gcc/", "/bits/",
                                         "/sysdeps/", "/libc/", "csu/"};
    static const char* const kFuncs[] = {"std::", "__gnu_cxx::", "__libc_", "start_thread"};
    for (const char* p : kPaths) {
        if (std::strstr(loc, p) != nullptr) {
            return true;
        }
    }
    for (const char* f : kFuncs) {
        if (std::strncmp(func, f, std::strlen(f)) == 0) {
            return true;
        }
    }
    return false;
}

// Split "path:line:col" into its path and line.
void split_loc(const char* loc, std::string* path, uint32_t* line) {
    path->clear();
    *line = 0;
    const char* colon = std::strrchr(loc, ':');
    if (colon == nullptr) {
        return;
    }
    const char* colon2 = colon;
    while (colon2 > loc && *(colon2 - 1) != ':') {
        --colon2;
    }
    if (colon2 <= loc) {
        return;
    }
    *line = static_cast<uint32_t>(std::strtoul(colon2, nullptr, 10));
    path->assign(loc, static_cast<size_t>(colon2 - 1 - loc));
}

void trace_print_stack(std::FILE* out, uint32_t stack_id) {
    if (!tracing() || stack_id == 0 || stack_id > g_trace_stacks) {
        std::fprintf(out, "    #0 <no stack captured>\n");
        return;
    }
    // Formatted once per distinct (interned) stack, so the symbolizer runs once
    // per place a finding comes from, not once per finding.
    static std::unordered_map<uint32_t, std::string> cache;
    auto hit = cache.find(stack_id);
    if (hit != cache.end()) {
        std::fputs(hit->second.c_str(), out);
        return;
    }

    const StackEntry& e = g_stacks[stack_id - 1];
    const char* mods[kStackFrames] = {};
    uint64_t offs[kStackFrames] = {};
    uint32_t n = 0;
    for (uint32_t i = 0; i < kStackFrames && e.frame[i] != 0; ++i) {
        uint32_t mod = frame_module(e.frame[i]);
        mods[n] = mod < g_header->module_count ? g_header->modules[mod].path : "?";
        offs[n] = frame_offset(e.frame[i]);
        ++n;
    }
    if (n == 0) {
        std::fprintf(out, "    #0 <no stack captured>\n");
        return;
    }
    // Every frame holds a RETURN address, which points just past the call. Ask
    // the symbolizer about the byte before it so the answer is the call site
    // and not the next statement -- otherwise a frame reads as `}` or as the
    // line after the one that did the work.
    uint64_t query[kStackFrames] = {};
    for (uint32_t i = 0; i < n; ++i) {
        query[i] = offs[i] > 0 ? offs[i] - 1 : 0;
    }
    char funcs[kStackFrames][256];
    char locs[kStackFrames][512];
    bool named = symbolize_stack(mods, query, n, funcs, locs);
    std::string text;
    uint32_t shown = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (i > 0 && named && boring_frame(funcs[i], locs[i])) {
            continue;
        }
        // "#N func file:line (module+0xoffset)", as TSan prints it. The module
        // and offset stay on the line even when the name resolved: they are what
        // makes a frame checkable by hand afterwards.
        char line[1024];
        if (named && funcs[i][0] != '\0') {
            std::snprintf(line, sizeof(line), "    #%u %s %s (%s+0x%" PRIx64 ")\n", shown,
                          funcs[i], locs[i], mods[i], offs[i]);
            text += line;
            ++shown;
            // The source line itself, under the frame that names it.
            std::string path;
            uint32_t num = 0;
            split_loc(locs[i], &path, &num);
            const std::string& src = source_line(path, num);
            if (!src.empty()) {
                text += "          ";
                text += src;
                text += "\n";
            }
        } else {
            std::snprintf(line, sizeof(line), "    #%u %s+0x%" PRIx64 "\n", shown, mods[i],
                          offs[i]);
            text += line;
            ++shown;
        }
    }
    std::fputs(text.c_str(), out);
    cache.emplace(stack_id, std::move(text));
}

namespace {

// Where the address is, named so a reader can act on it: the region a program
// registered, or the checker's own pool.
void print_location(std::FILE* out, uintptr_t addr) {
    uintptr_t pool = reinterpret_cast<uintptr_t>(g_pool);
    if (g_pool != nullptr && addr >= pool && addr < pool + g_header->cxl_size) {
        std::fprintf(out, "pool+0x%06" PRIx64, static_cast<uint64_t>(addr - pool));
        return;
    }
    for (uint32_t i = 0; i < g_header->watch_count; ++i) {
        uintptr_t base = g_watch_base[i];
        if (g_header->watch[i].in_use && base != 0 && addr >= base &&
            addr < base + g_header->watch[i].size) {
            std::fprintf(out, "region %u+0x%06" PRIx64, g_header->watch[i].id,
                         static_cast<uint64_t>(addr - base));
            return;
        }
    }
    std::fprintf(out, "0x%012" PRIxPTR, addr);
}

// One TSan-style block: a headline, then the stack. A null event means the
// ring no longer holds it, which is said rather than silently skipped -- an
// absent block and an aged-out one mean opposite things.
void print_block(std::FILE* out, const char* headline, const Event* e) {
    std::fprintf(out, "  %s\n", headline);
    if (e == nullptr) {
        std::fprintf(out, "    #0 <history exhausted: raise CSAN_TRACE_DEPTH>\n\n");
        return;
    }
    trace_print_stack(out, e->stack);
    std::fprintf(out, "\n");
}

// A block for a Pos from the state: its headline, then the stack. 0 means the
// state was set where tracing recorded no position.
void print_pos_block(std::FILE* out, const char* headline, uint32_t pos) {
    std::fprintf(out, "  %s\n", headline);
    if (pos == 0) {
        std::fprintf(out, "    #0 <no position recorded>\n\n");
        return;
    }
    trace_print_stack(out, pos);
    std::fprintf(out, "\n");
}

// Every non-empty slot of the map at `off`, for the report of thread `tid`:
// who, whether it is finished, whether the rule could count it -- its epoch in
// tid's clock, and for an acquisition on tid's machine -- and the positions
// that recorded and finished it. Returns how many slots counted.
uint32_t print_slots(std::FILE* out, const char* unit, const char* what, const char* rec_verb,
                     uint32_t off, uint32_t tid, bool same_machine) {
    SlotMap* m = slotmap_at(off);
    uint32_t shown = 0, counted = 0;
    VectorClock c = thread_vc(tid);
    auto one = [&](uint32_t u, SlotState st, Epoch e, uint32_t rec, uint32_t fin) {
        bool final = st == kSlotFinal;
        bool here = !same_machine || machine_of(u) == machine_of(tid);
        bool known = c.covers(e);
        bool counts = final && here && known;
        counted += counts ? 1 : 0;
        ++shown;
        const char* verb = final ? what : (same_machine ? "FLUSHED, NOT FENCED" : "WRITTEN BACK, NOT FENCED");
        const char* why = counts ? ""
                          : !final ? " -- not counted: no fence has finished it"
                          : !here  ? " -- not counted: another machine"
                                   : " -- not counted: not in this thread's clock";
        std::fprintf(out, "  %s %s by thread T%u (tid=%u, machine M%u)%s:\n", verb, unit, u,
                     g_header->os_tid[u], machine_of(u), why);
        char sub[64];
        std::snprintf(sub, sizeof(sub), "  %s at:", rec_verb);
        print_pos_block(out, sub, rec);
        if (final) {
            print_pos_block(out, "  fenced at:", fin);
        }
    };
    if (m != nullptr && m->full_off == 0) {
        SlotProv* prov = slotmap_prov(m);
        for (uint32_t i = 0; i < 4; ++i) {
            SlotState st = slot_state(m, i);
            if (st != kSlotBot) {
                one(epoch_tid(m->inline_e[i]), st, m->inline_e[i],
                    prov != nullptr ? prov->rec[i] : 0, prov != nullptr ? prov->fin[i] : 0);
            }
        }
    } else if (m != nullptr) {
        uint8_t* st = fullmap_states(m->full_off);
        uint32_t* clk = fullmap_clocks(m->full_off);
        uint32_t* rec = fullmap_rec_pos(m->full_off);
        uint32_t* fin = fullmap_fin_pos(m->full_off);
        for (uint32_t u = 0; u < g_threads; ++u) {
            if (st[u] != kSlotBot) {
                one(u, static_cast<SlotState>(st[u]), epoch_make(u, clk[u]),
                    rec != nullptr ? rec[u] : 0, fin != nullptr ? fin[u] : 0);
            }
        }
    }
    if (shown == 0) {
        std::fprintf(out, "  %s %s: none.\n\n", what, unit);
    }
    return counted;
}

// The thread's newest event: the access being reported, recorded just before
// the rule that found the conflict ran.
const Event* newest_of(uint32_t tid) {
    return tid < kMaxThreads ? trace_event_at(event_ref(tid, g_header->trace_seq[tid])) : nullptr;
}

} // namespace

void trace_print_finding(std::FILE* out, uint32_t race_kind, uintptr_t addr, uint32_t len,
                         uint32_t tid, uint32_t other_tid) {
    if (!tracing()) {
        return;
    }
    const Event* self = newest_of(tid);
    const char* verb = (self != nullptr && self->kind == kEvWrite) ? "WRITE" : "READ";

    std::fprintf(out, "==================\n");
    std::fprintf(out, "WARNING: CSan: %s (pid=%d)\n", race_kind_name(race_kind),
                 static_cast<int>(getpid()));

    std::fprintf(out, "  %s of size %u at ", verb, len);
    print_location(out, addr);
    std::fprintf(out, " by thread T%u (tid=%u, machine M%u):\n", tid, g_header->os_tid[tid],
                 machine_of(tid));
    trace_print_stack(out, self != nullptr ? self->stack : 0);
    std::fprintf(out, "\n");

    // Write-backs and flushes act on whole lines, so they are matched by line.
    uintptr_t line = addr & ~static_cast<uintptr_t>(kLineBytes - 1);
    uint32_t line_len = static_cast<uint32_t>(kLineBytes);
    // A line kind conflicts with a write anywhere on the line, not at this byte,
    // and is cleared by a line acquisition rather than a byte one.
    bool line_kind = race_kind == CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE ||
                     race_kind == CSAN_RACE_ATOMIC_WRITE_WITHOUT_LINE_ACQUIRE ||
                     race_kind == CSAN_RACE_WRITE_BEFORE_HB_LINE_WRITE;

    (void)other_tid;
    // The chain is read from the state the rule just decided on, not searched
    // for in the history: the last write it compared against, and every
    // publication and acquisition slot, each with the code that made it.
    LineCell* lc = line_cell_at(addr);
    ByteCell* x = nullptr;
    if (!line_kind) {
        ByteCell* wc = word_cell_at(addr);
        x = cell_split(wc) ? &split_array(wc)[addr & (kWordBytes - 1)] : wc;
    }
    const char* unit = line_kind ? "line" : "byte";
    Epoch w = line_kind ? lc->wl : x->wb;
    uint32_t* wpos = line_kind ? line_pos_at(addr) : write_pos_at(addr);
    if (w == kEpochBot) {
        std::fprintf(out, "  LAST WRITE of that %s: none.\n\n", unit);
    } else {
        uint32_t wt = epoch_tid(w);
        char head[160];
        std::snprintf(head, sizeof(head), "LAST WRITE of that %s by thread T%u (tid=%u, machine M%u):",
                      unit, wt, g_header->os_tid[wt], machine_of(wt));
        print_pos_block(out, head, wpos != nullptr ? *wpos : 0);
    }
    uint32_t pub_off = line_kind ? lc->pub : cell_pub_off(x);
    uint32_t acq_off = line_kind ? lc->acq : cell_acq_off(x);
    print_slots(out, unit, "PUBLISHED", "write back", pub_off, tid, /*same_machine=*/false);
    uint32_t acquired = print_slots(out, unit, "ACQUIRED", "flush", acq_off, tid,
                                    /*same_machine=*/true);

    // A flush that acquired nothing leaves no slot, so the state cannot show
    // it; its event carries the rule's verdict, which is said here.
    if (acquired == 0) {
        for (uint32_t u = 0; u < kMaxThreads; ++u) {
            if (machine_of(u) != machine_of(tid)) {
                continue;
            }
            const Event* e = trace_find_back(u, kEvFlush, line, line_len);
            if (e == nullptr) {
                continue;
            }
            uint32_t got = line_kind ? flush_verdict_lines(e->from) : flush_verdict_bytes(e->from);
            if (got != 0) {
                continue; // it acquired, and a later write has since cleared that
            }
            char head[160];
            std::snprintf(head, sizeof(head),
                          "FLUSH by thread T%u (tid=%u, machine M%u) ACQUIRED NOTHING: no "
                          "publication was in its clock:",
                          u, g_header->os_tid[u], machine_of(u));
            print_block(out, head, e);
            break;
        }
    }

    std::fprintf(out, "SUMMARY: CSan: %s at ", race_kind_name(race_kind));
    print_location(out, addr);
    std::fprintf(out, "\n==================\n");
    std::fflush(out);
}

} // namespace csan
