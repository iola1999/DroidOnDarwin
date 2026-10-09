/* SPDX-License-Identifier: Apache-2.0 */
#include "art_method-inl.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jni/jni_internal.h"
#include "scoped_thread_state_change-inl.h"

/* The regression invokes compilation synchronously so that success requires
 * an installed JIT entrypoint, independent of background hotness sampling. */
extern "C" JNIEXPORT bool nce_art_compile_method(JNIEnv* env, jmethodID id)
{
    art::ScopedObjectAccess soa(env);
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
