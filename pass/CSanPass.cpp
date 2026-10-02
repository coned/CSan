// SPDX-License-Identifier: MIT
// LLVM pass: inserts csan runtime hooks around memory accesses, cache
// maintenance, fences, atomics, locks and thread joins.
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
// LLVM 23 moved PassPlugin.h from Passes/ to Plugins/.
#if LLVM_VERSION_MAJOR >= 23
#include "llvm/Plugins/PassPlugin.h"
#else
#include "llvm/Passes/PassPlugin.h"
#endif
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace llvm;

// Supports LLVM 15 through 23 (opaque pointers). LLVM 16 renamed
// StringRef::startswith to starts_with.
#if LLVM_VERSION_MAJOR < 16
static inline bool startsWith(StringRef name, StringRef prefix) {
    return name.startswith(prefix);
}
#else
static inline bool startsWith(StringRef name, StringRef prefix) {
    return name.starts_with(prefix);
}
#endif

// Environment the pass reads at COMPILE time (a build system does not track it):
//
//   CSAN_SCOPE=sub,str    instrument only functions whose name contains one of
//                         these. Empty (the default) instruments everything.

namespace {

FunctionCallee getRuntimeFn(Module& M, StringRef name, ArrayRef<Type*> args, Type* ret = nullptr) {
    if (ret == nullptr) {
        ret = Type::getVoidTy(M.getContext());
    }
    FunctionType* FT = FunctionType::get(ret, args, /*isVarArg=*/false);
    return M.getOrInsertFunction(name, FT);
}

void insertCallBefore(Instruction* I, FunctionCallee F, ArrayRef<Value*> args) {
    IRBuilder<> builder(I);
    builder.CreateCall(F, args);
}

void insertCallAfter(Instruction* I, FunctionCallee F, ArrayRef<Value*> args) {
    IRBuilder<> builder(I->getNextNode());
    builder.CreateCall(F, args);
}

Value* toI64(Value* V, Instruction& I, Type* I64Ty) {
    if (V->getType() == I64Ty) {
        return V;
    }
    IRBuilder<> builder(&I);
    return builder.CreateZExtOrTrunc(V, I64Ty);
}

// Which construct produced a write-back or flush, for reporting. Shared with
// the runtime (CSAN_SRC_* in csan_runtime.h), mirrored rather than included.
enum : uint32_t {
    CSAN_SRC_CLWB = 1,
    CSAN_SRC_CLFLUSHOPT = 2,
    CSAN_SRC_CLFLUSH = 3,
    CSAN_SRC_NT_STORE = 4,
    CSAN_SRC_ASM = 0x100,
};

// Flag bits shared with the runtime (see AtomicFlags there).
enum : uint32_t {
    kAtomicAcquire = 1u << 0,
    kAtomicRelease = 1u << 1,
    kAtomicFailAcquire = 1u << 4, // cmpxchg's failure ordering
    kAtomicMFenced = 1u << 5,     // the lowering is a full machine barrier
};

// The value type an atomic instruction operates on.
Type* atomicValueType(Instruction& I) {
    if (auto* LI = dyn_cast<LoadInst>(&I)) return LI->getType();
    if (auto* SI = dyn_cast<StoreInst>(&I)) return SI->getValueOperand()->getType();
    if (auto* RMW = dyn_cast<AtomicRMWInst>(&I)) return RMW->getValOperand()->getType();
    if (auto* CX = dyn_cast<AtomicCmpXchgInst>(&I)) return CX->getCompareOperand()->getType();
    return nullptr;
}

// Only integer/pointer atomics that fit in the runtime's uint64_t ABI are
// replaced; floats, vectors and >8-byte types get plain read/write hooks.
bool canReplaceAtomic(Instruction& I, Module& M) {
    Type* ty = atomicValueType(I);
    if (ty == nullptr || (!ty->isIntegerTy() && !ty->isPointerTy())) return false;
    if (M.getDataLayout().getTypeStoreSize(ty) > 8) return false;
    if (auto* RMW = dyn_cast<AtomicRMWInst>(&I)) {
        switch (RMW->getOperation()) {
        case AtomicRMWInst::Xchg:
        case AtomicRMWInst::Add:
        case AtomicRMWInst::Sub:
        case AtomicRMWInst::And:
        case AtomicRMWInst::Or:
        case AtomicRMWInst::Xor:
            return true;
        default:
            return false; // Nand/min/max: leave alone rather than mis-model
        }
    }
    return true;
}

uint32_t atomicFlagsFor(AtomicOrdering ord, bool isLoadSide) {
    uint32_t f = 0;
    if (isLoadSide && (ord == AtomicOrdering::Acquire || ord == AtomicOrdering::AcquireRelease ||
                       ord == AtomicOrdering::SequentiallyConsistent)) {
        f |= kAtomicAcquire;
    }
    if (!isLoadSide && (ord == AtomicOrdering::Release || ord == AtomicOrdering::AcquireRelease ||
                        ord == AtomicOrdering::SequentiallyConsistent)) {
        f |= kAtomicRelease;
    }
    return f;
}

// MFENCED: the lowering is `lock`-prefixed or `xchg`, a full barrier that the
// SDM lists among the operations ordering CLWB and CLFLUSHOPT, so it does to
// the relays what an mfence does.
//
// A property of the LOWERING, not the C++ order, so it cannot fold into
// atomicFlagsFor: every RMW is locked at every ordering, a seq_cst store is
// `xchg` or `mov; mfence`, everything else is a plain `mov`.
bool isMFenced(Instruction& I) {
    if (isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I)) {
        return true; // lock xadd / lock xchg / lock cmpxchg, whatever the order
    }
    if (auto* SI = dyn_cast<StoreInst>(&I)) {
        return SI->getOrdering() == AtomicOrdering::SequentiallyConsistent;
    }
    return false;
}

void replaceAtomic(Instruction& I, Module& M, Type* i32Ty, Type* i64Ty,
                   FunctionCallee atomicLoadFn, FunctionCallee atomicStoreFn,
                   FunctionCallee atomicRmwFn, FunctionCallee atomicCasFn) {
    IRBuilder<> b(&I);
    Type* ty = atomicValueType(I);
    uint64_t bytes = M.getDataLayout().getTypeStoreSize(ty);
    Value* size = ConstantInt::get(i32Ty, bytes);

    auto toI64 = [&](Value* v) -> Value* {
        if (v->getType()->isPointerTy()) return b.CreatePtrToInt(v, i64Ty);
        return b.CreateZExtOrTrunc(v, i64Ty);
    };
    auto fromI64 = [&](Value* v) -> Value* {
        if (ty->isPointerTy()) return b.CreateIntToPtr(v, ty);
        return b.CreateZExtOrTrunc(v, ty);
    };

    if (auto* LI = dyn_cast<LoadInst>(&I)) {
        uint32_t flags = atomicFlagsFor(LI->getOrdering(), true);
        Value* r = b.CreateCall(atomicLoadFn, {LI->getPointerOperand(), size,
                                               ConstantInt::get(i32Ty, flags)});
        I.replaceAllUsesWith(fromI64(r));
        I.eraseFromParent();
        return;
    }
    if (auto* SI = dyn_cast<StoreInst>(&I)) {
        uint32_t flags = atomicFlagsFor(SI->getOrdering(), false);
        if (isMFenced(I)) flags |= kAtomicMFenced;
        b.CreateCall(atomicStoreFn, {SI->getPointerOperand(), toI64(SI->getValueOperand()), size,
                                     ConstantInt::get(i32Ty, flags)});
        I.eraseFromParent();
        return;
    }
    if (auto* RMW = dyn_cast<AtomicRMWInst>(&I)) {
        uint32_t opcode = 0;
        switch (RMW->getOperation()) {
        case AtomicRMWInst::Xchg: opcode = 0; break;
        case AtomicRMWInst::Add: opcode = 1; break;
        case AtomicRMWInst::Sub: opcode = 2; break;
        case AtomicRMWInst::And: opcode = 3; break;
        case AtomicRMWInst::Or: opcode = 4; break;
        default: opcode = 5; break; // Xor
        }
        AtomicOrdering ord = RMW->getOrdering();
        uint32_t flags = atomicFlagsFor(ord, true) | atomicFlagsFor(ord, false);
        if (isMFenced(I)) flags |= kAtomicMFenced;
        Value* r = b.CreateCall(atomicRmwFn, {RMW->getPointerOperand(),
                                              toI64(RMW->getValOperand()), size,
                                              ConstantInt::get(i32Ty, opcode),
                                              ConstantInt::get(i32Ty, flags)});
        I.replaceAllUsesWith(fromI64(r));
        I.eraseFromParent();
        return;
    }
    auto* CX = cast<AtomicCmpXchgInst>(&I);
    // Bits 0-1 describe the SUCCESS ordering; the failure ordering's acquire
    // has its own bit, because a failing compare-exchange is a load at the
    // failure ordering and must not inherit the success ordering's effects.
    uint32_t flags = atomicFlagsFor(CX->getSuccessOrdering(), true) |
                     atomicFlagsFor(CX->getSuccessOrdering(), false);
    if ((atomicFlagsFor(CX->getFailureOrdering(), true) & kAtomicAcquire) != 0) {
        flags |= kAtomicFailAcquire;
    }
    // Set whether or not the comparison will succeed: `lock cmpxchg` asserts the
    // prefix and fences either way, unlike the release half, which needs a write.
    if (isMFenced(I)) flags |= kAtomicMFenced;
    Value* cmp = CX->getCompareOperand();
    Value* r = b.CreateCall(atomicCasFn, {CX->getPointerOperand(), toI64(cmp),
                                          toI64(CX->getNewValOperand()), size,
                                          ConstantInt::get(i32Ty, flags)});
    // cmpxchg yields { oldValue, didSwap }; the runtime returns the value that
    // was in memory, which equals the comparand exactly when the swap happened.
    Value* oldVal = fromI64(r);
    Value* ok = ty->isPointerTy() ? b.CreateICmpEQ(b.CreatePtrToInt(oldVal, i64Ty), toI64(cmp))
                                  : b.CreateICmpEQ(oldVal, cmp);
    Value* agg = UndefValue::get(CX->getType());
    agg = b.CreateInsertValue(agg, oldVal, 0);
    agg = b.CreateInsertValue(agg, ok, 1);
    I.replaceAllUsesWith(agg);
    I.eraseFromParent();
}

// The checker's own code, which must not be instrumented: the runtime's entry
// points (__csan_*), its public API (csan_*), and the test harness helpers
// (csan_test_*) that call them.
//
// Anchored, not `contains`: a substring test makes every symbol with "csan"
// anywhere in it invisible to the pass, which quietly turns a program into a
// clean bill of health rather than into a build error.
bool isCheckerName(StringRef name) {
    return startsWith(name, "__csan_") || startsWith(name, "csan_");
}

bool isLockCall(StringRef name) {
    return name == "pthread_spin_lock" || name == "pthread_mutex_lock" ||
           name == "pthread_rwlock_rdlock" || name == "pthread_rwlock_wrlock" ||
           name == "_ZNSt5mutex4lockEv";
}

bool isUnlockCall(StringRef name) {
    return name == "pthread_spin_unlock" || name == "pthread_mutex_unlock" ||
           name == "pthread_rwlock_unlock" || name == "_ZNSt5mutex6unlockEv";
}

// CLFLUSHOPT: a Flush event on its own. clang lowers _mm_clflushopt to a static
// wrapper at -O3 (e.g. _ZL14_mm_clflushoptPKv), so match the wrapper too.
bool isFlushOptIntrinsic(StringRef name) {
    return name == "llvm.x86.clflushopt" || name.contains("_mm_clflushopt");
}

// CLFLUSH: Flush AND SFence. Unlike CLFLUSHOPT it is ordered with respect to
// later stores on its own, so it completes line acquisition without a fence.
// Both forms also write back a dirty line; that is part of the flush rule
// (runtime/rules/rules_visibility.cpp), so no __csan_wb is emitted here.
bool isFlushIntrinsic(StringRef name) {
    if (isFlushOptIntrinsic(name)) {
        return false;
    }
    return name == "llvm.x86.sse2.clflush" || name == "llvm.x86.clflush" ||
           name.contains("_mm_clflush");
}

bool isSfenceIntrinsic(StringRef name) {
    return name == "llvm.x86.sse.sfence" || name == "llvm.x86.sse2.sfence" ||
           name.contains("_mm_sfence");
}

bool isWbIntrinsic(StringRef name) {
    return name == "llvm.x86.clwb" || name.contains("_mm_clwb");
}

bool isMemTransfer(StringRef name) {
    return name == "llvm.memcpy.p0.p0.i32" || name == "llvm.memcpy.p0.p0.i64" ||
           name == "llvm.memmove.p0.p0.i32" || name == "llvm.memmove.p0.p0.i64" ||
           name == "memcpy" || name == "memmove";
}

bool isMemSet(StringRef name) {
    return name == "llvm.memset.p0.i32" || name == "llvm.memset.p0.i64" || name == "memset";
}

// Cache maintenance written as inline asm rather than an intrinsic, e.g.
// `asm volatile("clwb (%0)")` or `asm volatile("sfence")` in a program. Such a
// call has no callee, so the name matching above never sees it; classify the
// asm text instead. The same instruction is the same event whichever way it
// was spelled.
enum class AsmKind { None, Flush, FlushOpt, Wb, Sfence, Mfence };

AsmKind classifyInlineAsm(const CallBase* CB) {
    const auto* IA = dyn_cast<InlineAsm>(CB->getCalledOperand());
    if (IA == nullptr) {
        return AsmKind::None;
    }
    StringRef text = IA->getAsmString();
    // ".byte 0x66; clflush" is how PMDK encodes CLFLUSHOPT where the assembler
    // does not know the mnemonic, so the 0x66 prefix decides which this is.
    if (text.contains("clflushopt") || (text.contains("clflush") && text.contains("0x66"))) {
        return AsmKind::FlushOpt;
    }
    if (text.contains("clflush")) {
        return AsmKind::Flush;
    }
    // ".byte 0x66; xsaveopt" is how PMDK, and the code that copies it, encodes
    // CLWB for assemblers that do not know the mnemonic.
    if (text.contains("clwb") || text.contains("xsaveopt")) {
        return AsmKind::Wb;
    }
    if (text.contains("mfence")) {
        return AsmKind::Mfence;
    }
    if (text.contains("sfence")) {
        return AsmKind::Sfence;
    }
    return AsmKind::None;
}

// The address a flush or write-back asm operates on: its first operand, which
// an "r" or "m" constraint gives as a pointer or an integer.
Value* asmTargetAddress(CallBase* CB, Type* i8PtrTy) {
    if (CB->arg_size() == 0) {
        return nullptr;
    }
    Value* arg = CB->getArgOperand(0);
    if (arg->getType()->isPointerTy()) {
        return arg;
    }
    if (arg->getType()->isIntegerTy()) {
        return IRBuilder<>(CB).CreateIntToPtr(arg, i8PtrTy);
    }
    return nullptr;
}

// Comma-separated list from an environment variable.
std::vector<std::string> parseListEnv(const char* name) {
    std::vector<std::string> out;
    const char* env = std::getenv(name);
    if (env == nullptr) {
        return out;
    }
    StringRef rest(env);
    while (!rest.empty()) {
        auto split = rest.split(',');
        StringRef tok = split.first.trim();
        if (!tok.empty()) {
            out.emplace_back(tok.str());
        }
        rest = split.second;
    }
    return out;
}

bool nameContainsAny(StringRef name, const std::vector<std::string>& list) {
    for (const std::string& s : list) {
        if (name.contains(s)) {
            return true;
        }
    }
    return false;
}

bool shouldInstrumentFunction(const Function& F, const std::vector<std::string>& scope) {
    if (F.isDeclaration() || F.empty()) {
        return false;
    }
    if (startsWith(F.getName(), "__cxx_global_var_init") ||
        startsWith(F.getName(), "_GLOBAL__sub_I_") || F.getName().contains("global_var_init")) {
        return false;
    }
    if (isCheckerName(F.getName())) {
        return false;
    }
    // An empty scope is the whole module. A program that narrows it must list
    // every layer its happens-before flows through, not only the layer it
    // suspects: std::atomic is emitted as libstdc++ __atomic_base helpers, so
    // a scope that omits those sees the data and none of the synchronization
    // ordering it, and every correctly released write becomes a report.
    return scope.empty() || nameContainsAny(F.getName(), scope);
}

struct CSanPass : public PassInfoMixin<CSanPass> {
    PreservedAnalyses run(Module& M, ModuleAnalysisManager&) {
        LLVMContext& Ctx = M.getContext();
        std::vector<std::string> scope = parseListEnv("CSAN_SCOPE");
        Type* i8PtrTy = PointerType::getUnqual(Ctx);
        Type* i64Ty = Type::getInt64Ty(Ctx);
        Type* i32Ty = Type::getInt32Ty(Ctx);
        FunctionCallee initFn = getRuntimeFn(M, "__csan_init", {});
        FunctionCallee readFn = getRuntimeFn(M, "__csan_read", {i8PtrTy, i64Ty});
        FunctionCallee writeFn = getRuntimeFn(M, "__csan_write", {i8PtrTy, i64Ty});
        FunctionCallee memcpyFn =
            getRuntimeFn(M, "__csan_memcpy", {i8PtrTy, i8PtrTy, i64Ty});
        FunctionCallee flushFn = getRuntimeFn(M, "__csan_flush", {i8PtrTy, i64Ty, i32Ty});
        FunctionCallee wbFn = getRuntimeFn(M, "__csan_wb", {i8PtrTy, i64Ty, i32Ty});
        FunctionCallee sfenceFn = getRuntimeFn(M, "__csan_sfence", {});
        FunctionCallee mfenceFn = getRuntimeFn(M, "__csan_mfence", {});
        FunctionCallee fenceAcqFn = getRuntimeFn(M, "__csan_fence_acquire", {});
        FunctionCallee fenceRelFn = getRuntimeFn(M, "__csan_fence_release", {});
        FunctionCallee fenceArFn = getRuntimeFn(M, "__csan_fence_acqrel", {});
        FunctionCallee lockFn = getRuntimeFn(M, "__csan_lock", {i8PtrTy});
        FunctionCallee unlockFn = getRuntimeFn(M, "__csan_unlock", {i8PtrTy});
        FunctionCallee atomicLoadFn =
            getRuntimeFn(M, "__csan_atomic_load", {i8PtrTy, i32Ty, i32Ty}, i64Ty);
        FunctionCallee atomicStoreFn =
            getRuntimeFn(M, "__csan_atomic_store", {i8PtrTy, i64Ty, i32Ty, i32Ty});
        FunctionCallee atomicRmwFn = getRuntimeFn(
            M, "__csan_atomic_rmw", {i8PtrTy, i64Ty, i32Ty, i32Ty, i32Ty}, i64Ty);
        FunctionCallee atomicCasFn = getRuntimeFn(
            M, "__csan_atomic_cas", {i8PtrTy, i64Ty, i64Ty, i32Ty, i32Ty}, i64Ty);
        FunctionCallee pthreadJoinFn = getRuntimeFn(M, "__csan_pthread_join", {i64Ty});

        std::vector<std::pair<Instruction*, std::function<void()>>> work;

        // Thread create/join is hooked in every module, whatever CSAN_SCOPE
        // says: threads are started outside the layer whose data is under test,
        // and skipping them loses the fork/join edges.

        // Insert a hook after a call or invoke. For an invoke, at the entry of
        // the normal-destination block.
        auto insertHookAfter = [&](CallBase* CB, FunctionCallee F, ArrayRef<Value*> args) {
            if (auto* II = dyn_cast<InvokeInst>(CB)) {
                IRBuilder<> builder(&*II->getNormalDest()->getFirstInsertionPt());
                builder.CreateCall(F, args);
            } else {
                insertCallAfter(static_cast<CallInst*>(CB), F, args);
            }
        };

        auto handleSpecialCall = [&](CallBase* CB) {
            Function* called = CB->getCalledFunction();
            if (called == nullptr) {
                return;
            }
            StringRef name = called->getName();

            if (name == "pthread_join") {
                Value* pthRaw = CB->getArgOperand(0);
                work.emplace_back(CB, [&, CB, pthRaw]() {
                    Value* pth = toI64(pthRaw, *CB, i64Ty);
                    insertHookAfter(CB, pthreadJoinFn, {pth});
                });
            } else if (name == "_ZNSt6thread4joinEv") {
                // std::thread::join: load the native handle from `this`
                // (offset 0) BEFORE the call -- libstdc++ resets the handle
                // to 0 once the thread is joined -- then merge the child's
                // clock into the caller after the call returns.
                Value* thisPtr = CB->getArgOperand(0);
                work.emplace_back(CB, [&, CB, thisPtr]() {
                    Instruction* insPt = nullptr;
                    if (auto* II = dyn_cast<InvokeInst>(CB)) {
                        insPt = &*II->getNormalDest()->getFirstInsertionPt();
                    } else {
                        insPt = CB->getNextNode();
                    }
                    IRBuilder<> preBuilder(CB);
                    Value* pth = preBuilder.CreateLoad(i64Ty, thisPtr);
                    IRBuilder<> postBuilder(insPt);
                    postBuilder.CreateCall(pthreadJoinFn, {pth});
                });
            }
        };

        for (Function& F : M) {
            bool instrument = shouldInstrumentFunction(F, scope);
            for (BasicBlock& BB : F) {
                for (Instruction& I : BB) {
                    if (auto* CB = dyn_cast<CallBase>(&I)) {
                        handleSpecialCall(CB);
                    }
                    if (!instrument) {
                        continue;
                    }

                    // ---- atomics ------------------------------------------------
                    // REPLACED by a runtime call that performs the access and
                    // its happens-before effect in one critical section, as
                    // ThreadSanitizer's __tsan_atomic_* do. Hooks placed around
                    // the instruction would be separate operations, letting a
                    // thread observe a released value before its clock.
                    //
                    // Only integer and pointer atomics up to 8 bytes; anything
                    // else keeps the hook treatment.
                    {
                        Instruction* atomicI = nullptr;
                        if (auto* LI = dyn_cast<LoadInst>(&I)) {
                            if (LI->isAtomic()) atomicI = &I;
                        } else if (auto* SI = dyn_cast<StoreInst>(&I)) {
                            if (SI->isAtomic()) atomicI = &I;
                        } else if (isa<AtomicRMWInst>(&I) || isa<AtomicCmpXchgInst>(&I)) {
                            atomicI = &I;
                        }
                        if (atomicI != nullptr && canReplaceAtomic(*atomicI, M)) {
                            work.emplace_back(&I, [&]() {
                                replaceAtomic(I, M, i32Ty, i64Ty, atomicLoadFn, atomicStoreFn,
                                              atomicRmwFn, atomicCasFn);
                            });
                            continue;
                        }
                    }

                    if (auto* LI = dyn_cast<LoadInst>(&I)) {
                        Value* ptr = LI->getPointerOperand();
                        uint64_t bytes = M.getDataLayout().getTypeStoreSize(LI->getType());
                        Value* size = ConstantInt::get(i64Ty, bytes);
                        work.emplace_back(&I, [&, ptr, size]() {
                            insertCallBefore(&I, readFn, {ptr, size});
                        });
                        continue;
                    }

                    if (auto* SI = dyn_cast<StoreInst>(&I)) {
                        Value* ptr = SI->getPointerOperand();
                        uint64_t bytes =
                            M.getDataLayout().getTypeStoreSize(SI->getValueOperand()->getType());
                        Value* size = ConstantInt::get(i64Ty, bytes);
                        // A non-temporal store (_mm_stream_*, which clang emits
                        // as a store with !nontemporal) reaches memory without
                        // a clwb, so it is a Write followed by a WriteBack of
                        // its line; an sfence still publishes it.
                        bool nonTemporal =
                            SI->getMetadata(LLVMContext::MD_nontemporal) != nullptr;
                        work.emplace_back(&I, [&, ptr, size]() {
                            insertCallBefore(&I, writeFn, {ptr, size});
                        });
                        if (nonTemporal) {
                            work.emplace_back(&I, [&, ptr, size]() {
                                insertCallAfter(&I, wbFn,
                                                {ptr, size,
                                                 ConstantInt::get(i32Ty, CSAN_SRC_NT_STORE)});
                            });
                        }
                        continue;
                    }

                    // An LLVM `fence` is a C++ atomic_thread_fence: CLOCKS
                    // ONLY. On x86 acquire, release and acquire-release fences
                    // emit no instruction, so they order no clwb. A seq_cst
                    // fence emits mfence and gets the visibility effect too.
                    if (auto* FI = dyn_cast<FenceInst>(&I)) {
                        AtomicOrdering ord = FI->getOrdering();
                        if (ord == AtomicOrdering::Acquire) {
                            work.emplace_back(&I,
                                              [&]() { insertCallBefore(&I, fenceAcqFn, {}); });
                        } else if (ord == AtomicOrdering::Release) {
                            work.emplace_back(&I,
                                              [&]() { insertCallBefore(&I, fenceRelFn, {}); });
                        } else if (ord == AtomicOrdering::AcquireRelease) {
                            work.emplace_back(&I, [&]() { insertCallBefore(&I, fenceArFn, {}); });
                        } else if (ord == AtomicOrdering::SequentiallyConsistent) {
                            work.emplace_back(&I, [&]() {
                                insertCallBefore(&I, mfenceFn, {});
                                insertCallBefore(&I, fenceArFn, {});
                            });
                        }
                        continue;
                    }

                    // Lock and unlock calls may be `invoke`s: a lock function
                    // that can throw, called where there is something to
                    // unwind, is one. The lock hook must run AFTER the lock is
                    // held and the unlock hook BEFORE it is released, so the
                    // published clock is in place before the next holder can
                    // merge it; for an invoke, "after" is the normal
                    // destination.
                    if (auto* CB = dyn_cast<CallBase>(&I)) {
                        if (Function* called = CB->getCalledFunction()) {
                            StringRef name = called->getName();
                            // The lock's identity is its first argument; a
                            // function that takes none guards one global
                            // lock, and the function itself is its key.
                            auto lockKey = [&]() -> Value* {
                                return CB->arg_size() > 0 ? CB->getArgOperand(0)
                                                          : static_cast<Value*>(called);
                            };
                            if (isLockCall(name)) {
                                Value* ptr = lockKey();
                                work.emplace_back(&I, [&, CB, ptr]() {
                                    insertHookAfter(CB, lockFn, {ptr});
                                });
                            }
                            if (isUnlockCall(name)) {
                                Value* ptr = lockKey();
                                work.emplace_back(
                                    &I, [&, ptr]() { insertCallBefore(&I, unlockFn, {ptr}); });
                            }
                        }
                    }

                    if (auto* CI = dyn_cast<CallInst>(&I)) {
                        if (CI->isInlineAsm()) {
                            AsmKind kind = classifyInlineAsm(CI);
                            if (kind == AsmKind::Sfence) {
                                work.emplace_back(&I, [&]() { insertCallBefore(&I, sfenceFn, {}); });
                            } else if (kind == AsmKind::Mfence) {
                                work.emplace_back(&I, [&]() { insertCallBefore(&I, mfenceFn, {}); });
                            } else if (kind == AsmKind::Flush || kind == AsmKind::FlushOpt ||
                                       kind == AsmKind::Wb) {
                                FunctionCallee target = kind == AsmKind::Wb ? wbFn : flushFn;
                                bool ordered = kind == AsmKind::Flush; // CLFLUSH self-fences
                                Value* size = ConstantInt::get(i64Ty, 1);
                                uint32_t src = CSAN_SRC_ASM | (kind == AsmKind::Wb
                                                                   ? CSAN_SRC_CLWB
                                                                   : kind == AsmKind::Flush
                                                                         ? CSAN_SRC_CLFLUSH
                                                                         : CSAN_SRC_CLFLUSHOPT);
                                Value* srcV = ConstantInt::get(i32Ty, src);
                                work.emplace_back(
                                    &I, [&, CI, target, size, srcV, i8PtrTy, ordered]() {
                                    if (Value* addr = asmTargetAddress(CI, i8PtrTy)) {
                                        insertCallBefore(&I, target, {addr, size, srcV});
                                        if (ordered) {
                                            insertCallBefore(&I, sfenceFn, {});
                                        }
                                    }
                                });
                            }
                            continue;
                        }
                        Function* called = CI->getCalledFunction();
                        if (called == nullptr) {
                            continue;
                        }
                        StringRef name = called->getName();

                        // A memset is a write of its whole range.
                        if (isMemSet(name)) {
                            Value* dst = CI->getArgOperand(0);
                            Value* n = toI64(CI->getArgOperand(2), I, i64Ty);
                            work.emplace_back(&I, [&, dst, n]() {
                                insertCallBefore(&I, writeFn, {dst, n});
                            });
                            continue;
                        }

                        // The _mm_* wrappers are matched by name for the case
                        // where only the call is visible. When the wrapper's
                        // body is in this module and instrumented, the
                        // intrinsic inside it gets the hook, and hooking the
                        // call too would record every flush twice.
                        if (!called->isDeclaration() && name.contains("_mm_") &&
                            shouldInstrumentFunction(*called, scope)) {
                            continue;
                        }

                        // clflushopt and clwb act on the ONE line containing
                        // their operand, which need not be line-aligned. The
                        // hooks take a byte range and cover the lines it
                        // touches, so the range is that single byte: 64 bytes
                        // from a misaligned operand would spill onto the next
                        // line and credit it with a write-back it never had.
                        if (isFlushOptIntrinsic(name) || isFlushIntrinsic(name)) {
                            Value* ptr = CI->getArgOperand(0);
                            Value* size = ConstantInt::get(i64Ty, 1);
                            bool ordered = isFlushIntrinsic(name); // CLFLUSH self-fences
                            uint32_t src = ordered ? CSAN_SRC_CLFLUSH : CSAN_SRC_CLFLUSHOPT;
                            work.emplace_back(&I, [&, ptr, size, ordered, src]() {
                                insertCallBefore(&I, flushFn,
                                                 {ptr, size, ConstantInt::get(i32Ty, src)});
                                if (ordered) {
                                    insertCallBefore(&I, sfenceFn, {});
                                }
                            });
                        }

                        if (isWbIntrinsic(name)) {
                            Value* ptr = CI->getArgOperand(0);
                            Value* size = ConstantInt::get(i64Ty, 1);
                            work.emplace_back(&I, [&, ptr, size]() {
                                insertCallBefore(&I, wbFn,
                                                 {ptr, size,
                                                  ConstantInt::get(i32Ty, CSAN_SRC_CLWB)});
                            });
                        }

                        if (isSfenceIntrinsic(name)) {
                            work.emplace_back(&I, [&]() { insertCallBefore(&I, sfenceFn, {}); });
                        }

                        if (name == "llvm.x86.sse2.mfence" || name.contains("_mm_mfence")) {
                            work.emplace_back(&I, [&]() { insertCallBefore(&I, mfenceFn, {}); });
                        }

                        // A memcpy is a plain read and a plain write.
                        if (isMemTransfer(name)) {
                            Value* dst = CI->getArgOperand(0);
                            Value* src = CI->getArgOperand(1);
                            Value* n = toI64(CI->getArgOperand(2), I, i64Ty);
                            work.emplace_back(&I, [&, dst, src, n]() {
                                insertCallBefore(&I, memcpyFn, {dst, src, n});
                            });
                        }
                    }
                }
            }
        }

        for (auto& entry : work) {
            entry.second();
        }

        Function* mainFn = M.getFunction("main");
        if (mainFn != nullptr && !mainFn->isDeclaration() && !mainFn->empty()) {
            Instruction* first = &*mainFn->getEntryBlock().getFirstInsertionPt();
            IRBuilder<> builder(first);
            builder.CreateCall(initFn);
        }

        return PreservedAnalyses::none();
    }
};

} // namespace

llvm::PassPluginLibraryInfo getCSanPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "CSan", LLVM_VERSION_STRING, [](PassBuilder& PB) {
                PB.registerPipelineStartEPCallback([](ModulePassManager& MPM, OptimizationLevel) {
                    MPM.addPass(CSanPass());
                });
                PB.registerPipelineParsingCallback([](StringRef Name, ModulePassManager& MPM,
                                                      ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "csan") {
                        MPM.addPass(CSanPass());
                        return true;
                    }
                    return false;
                });
            }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return getCSanPluginInfo();
}
