/* SPDX-License-Identifier: Apache-2.0 */
#include "art_method-inl.h"
#include "class_linker.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jit/small_pattern_matcher.h"
#include "jni/jni_internal.h"
#include "scoped_thread_state_change-inl.h"

/* The regression invokes compilation synchronously so that success requires
 * an installed JIT entrypoint, independent of background hotness sampling. */
extern "C" JNIEXPORT bool nce_art_compile_method(JNIEnv* env, jmethodID id)
{
    art::ScopedObjectAccess soa(env);
    // JNI lookup may leave a newly initialized class behind its resolution entrypoint.
    // Publish initialization before requesting a synchronous test compilation.
    {
        art::ScopedThreadSuspension suspended(soa.Self(), art::ThreadState::kNative);
        art::Runtime::Current()->GetClassLinker()->MakeInitializedClassesVisiblyInitialized(
            soa.Self(), true);
    }
    art::ArtMethod* method = art::jni::DecodeArtMethod(id);
    art::jit::Jit* jit = art::Runtime::Current()->GetJit();
    if (!jit || !method) return false;
    if (!jit->CompileMethod(method, soa.Self(), art::CompilationKind::kOptimized, false)) return false;
    return jit->GetCodeCache()->ContainsPc(method->GetEntryPointFromQuickCompiledCode());
}

extern "C" JNIEXPORT void nce_art_keep_interpreted(JNIEnv* env, jmethodID id)
{
    art::ScopedObjectAccess soa(env);
    art::jni::DecodeArtMethod(id)->SetDontCompile();
}

extern "C" JNIEXPORT bool nce_art_wait_for_compilation(JNIEnv* env, jmethodID id)
{
    art::ScopedObjectAccess soa(env);
    art::jit::Jit* jit = art::Runtime::Current()->GetJit();
    if (!jit) return false;
    jit->WaitForCompilationToFinish(soa.Self());
    return jit->GetCodeCache()->ContainsPc(art::jni::DecodeArtMethod(id)->GetEntryPointFromQuickCompiledCode());
}

extern "C" JNIEXPORT bool nce_art_compile_pattern(JNIEnv* env, jmethodID id)
{
    art::ScopedObjectAccess soa(env);
    // Static pattern entrypoints require the class initialization checkpoint.
    {
        art::ScopedThreadSuspension suspended(soa.Self(), art::ThreadState::kNative);
        art::Runtime::Current()->GetClassLinker()->MakeInitializedClassesVisiblyInitialized(
            soa.Self(), true);
    }
    art::ArtMethod* method = art::jni::DecodeArtMethod(id);
    art::jit::Jit* jit = art::Runtime::Current()->GetJit();
    if (!jit || !method) return false;
    const void* expected = art::jit::SmallPatternMatcher::TryMatch(method);
    if (!expected) {
        fprintf(stderr, "ART pattern did not match %s\n", method->PrettyMethod().c_str());
        return false;
    }
    bool installed = jit->TryPatternMatch(method, art::CompilationKind::kBaseline);
    bool matched = method->GetEntryPointFromQuickCompiledCode() == expected;
    if (!matched) fprintf(stderr, "ART pattern was not installed %s installed=%d clinit=%d compilable=%d\n",
                         method->PrettyMethod().c_str(), installed, method->StillNeedsClinitCheck(),
                         method->IsCompilable());
    return matched;
}
