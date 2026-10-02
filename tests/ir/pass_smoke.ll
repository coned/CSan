; SPDX-License-Identifier: MIT
; Input for the pass_inserts_hooks test: one site for every hook family.
; Kept as .ll so no compiler version decides which intrinsic is emitted.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare void @llvm.x86.sse2.mfence()
declare void @llvm.x86.sse.sfence()
declare void @llvm.x86.clflushopt(ptr)
declare void @llvm.x86.clwb(ptr)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

define i32 @main() {
entry:
  %p = alloca i32, align 4
  %q = alloca i32, align 4

  ; plain load and store -> __csan_write / __csan_read
  store i32 7, ptr %p, align 4
  %v = load i32, ptr %p, align 4

  ; atomics -> __csan_atomic_store / _load / _rmw / _cas
  store atomic i32 1, ptr %p seq_cst, align 4
  %a = load atomic i32, ptr %p acquire, align 4
  %r = atomicrmw add ptr %p, i32 1 release
  %c = cmpxchg ptr %p, i32 1, i32 2 acq_rel monotonic

  ; the IR fences -> __csan_fence_acquire / _release
  fence acquire
  fence release

  ; the x86 fences -> __csan_sfence / __csan_mfence
  call void @llvm.x86.sse.sfence()
  call void @llvm.x86.sse2.mfence()

  ; cache maintenance -> __csan_wb / __csan_flush
  call void @llvm.x86.clwb(ptr %p)
  call void @llvm.x86.clflushopt(ptr %p)

  ; memcpy -> __csan_memcpy
  call void @llvm.memcpy.p0.p0.i64(ptr %q, ptr %p, i64 4, i1 false)

  ret i32 0
}
