/* SPDX-License-Identifier: Apache-2.0 */
#include "art_method-inl.h"
#include "class_linker.h"
#include "oat/oat_quick_method_header.h"
#include "oat/stack_map.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jit/small_pattern_matcher.h"
#include "jni/jni_internal.h"
#include "interpreter/mterp/nterp.h"
#include "scoped_thread_state_change-inl.h"

namespace art
{
extern thread_local uint64_t nce_fast_baseline_installed;
}

extern "C" JNIEXPORT bool nce_art_nterp_enabled()
{
    return art::interpreter::IsNterpSupported();
}

extern "C" JNIEXPORT bool nce_art_method_uses_nterp(JNIEnv *env, jmethodID id)
{
    art::ScopedObjectAccess soa(env);
    // JNI 查找可能保留类初始化入口；先完成发布，再检查方法的执行入口。
    {
        art::ScopedThreadSuspension suspended(soa.Self(), art::ThreadState::kNative);
        art::Runtime::Current()->GetClassLinker()->MakeInitializedClassesVisiblyInitialized(
            soa.Self(), true);
    }
    art::ArtMethod *method = art::jni::DecodeArtMethod(id);
    return method && art::Runtime::Current()->GetClassLinker()->IsNterpEntryPoint(
                         method->GetEntryPointFromQuickCompiledCode());
}

/* The regression invokes compilation synchronously so that success requires
 * an installed JIT entrypoint, independent of background hotness sampling. */
static int compile_method(JNIEnv *env, jmethodID id, art::CompilationKind kind)
{
    art::ScopedObjectAccess soa(env);
    // JNI lookup may leave a newly initialized class behind its resolution entrypoint.
    // Publish initialization before requesting a synchronous test compilation.
    {
        art::ScopedThreadSuspension suspended(soa.Self(), art::ThreadState::kNative);
        art::Runtime::Current()->GetClassLinker()->MakeInitializedClassesVisiblyInitialized(
            soa.Self(), true);
    }
    art::ArtMethod *method = art::jni::DecodeArtMethod(id);
    art::jit::Jit *jit = art::Runtime::Current()->GetJit();
    if (!jit || !method)
        return false;
    const bool intrinsic = method->IsIntrinsic();
    const auto identity = intrinsic ? method->GetIntrinsic() : art::Intrinsics::kNone;
    const uint64_t fast_before = art::nce_fast_baseline_installed;
    const bool compiled = jit->CompileMethod(method, soa.Self(), kind, false);
    // Rejection must preserve intrinsic dispatch for later callers and inlining.
    if (intrinsic)
        CHECK(method->IsIntrinsic() && method->GetIntrinsic() == identity);
    if (!compiled || !jit->GetCodeCache()->ContainsPc(method->GetEntryPointFromQuickCompiledCode()))
        return 0;
    return art::nce_fast_baseline_installed != fast_before ? 2 : 1;
}

extern "C" JNIEXPORT bool nce_art_compile_method(JNIEnv *env, jmethodID id)
{
    return compile_method(env, id, art::CompilationKind::kOptimized) != 0;
}

// 0: failed, 1: optimizing compiler, 2: fast baseline compiler.
extern "C" JNIEXPORT int nce_art_compile_baseline(JNIEnv *env, jmethodID id)
{
    return compile_method(env, id, art::CompilationKind::kBaseline);
}

extern "C" JNIEXPORT void nce_art_keep_interpreted(JNIEnv *env, jmethodID id)
{
    art::ScopedObjectAccess soa(env);
    art::jni::DecodeArtMethod(id)->SetDontCompile();
}

extern "C" JNIEXPORT bool nce_art_wait_for_compilation(JNIEnv *env, jmethodID id)
{
    art::ScopedObjectAccess soa(env);
    art::jit::Jit *jit = art::Runtime::Current()->GetJit();
    if (!jit)
        return false;
    jit->WaitForCompilationToFinish(soa.Self());
    return jit->GetCodeCache()->ContainsPc(art::jni::DecodeArtMethod(id)->GetEntryPointFromQuickCompiledCode());
}

extern "C" JNIEXPORT bool nce_art_compile_pattern(JNIEnv *env, jmethodID id)
{
    art::ScopedObjectAccess soa(env);
    // Static pattern entrypoints require the class initialization checkpoint.
    {
        art::ScopedThreadSuspension suspended(soa.Self(), art::ThreadState::kNative);
        art::Runtime::Current()->GetClassLinker()->MakeInitializedClassesVisiblyInitialized(
            soa.Self(), true);
    }
    art::ArtMethod *method = art::jni::DecodeArtMethod(id);
    art::jit::Jit *jit = art::Runtime::Current()->GetJit();
    if (!jit || !method)
        return false;
    const void *expected = art::jit::SmallPatternMatcher::TryMatch(method);
    if (!expected) {
        fprintf(stderr, "ART pattern did not match %s\n", method->PrettyMethod().c_str());
        return false;
    }
    bool installed = jit->TryPatternMatch(method, art::CompilationKind::kBaseline);
    bool matched = method->GetEntryPointFromQuickCompiledCode() == expected;
    if (!matched)
        fprintf(stderr, "ART pattern was not installed %s installed=%d clinit=%d compilable=%d\n",
                method->PrettyMethod().c_str(), installed, method->StillNeedsClinitCheck(),
                method->IsCompilable());
    return matched;
}

extern "C" JNIEXPORT int nce_art_inlined_frames(JNIEnv *env, jmethodID id)
{
    art::ScopedObjectAccess soa(env);
    art::ArtMethod *method = art::jni::DecodeArtMethod(id);
    const void *entry = method->GetEntryPointFromQuickCompiledCode();
    art::jit::Jit *jit = art::Runtime::Current()->GetJit();
    if (!jit || !jit->GetCodeCache()->ContainsPc(entry))
        return -1;
    art::CodeInfo info(art::OatQuickMethodHeader::FromEntryPoint(entry));
    size_t depth = 0;
    for (uint32_t i = 0; i < info.GetNumberOfStackMaps(); ++i) {
        size_t current = info.GetInlineInfosOf(info.GetStackMapAt(i)).size();
        if (current > depth)
            depth = current;
    }
    return static_cast<int>(depth);
}
