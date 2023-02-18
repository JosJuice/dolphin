// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <utility>

#include <jni.h>

#include "DiscIO/Volume.h"
#include "DiscIO/VolumeVerifier.h"
#include "jni/AndroidCommon/AndroidCommon.h"
#include "jni/AndroidCommon/IDCache.h"

struct VolumeVerifierContext
{
  DiscIO::VolumeVerifier verifier;
  std::unique_ptr<DiscIO::Volume> volume;
};

static VolumeVerifierContext* GetContext(JNIEnv* env, jobject obj)
{
  return reinterpret_cast<VolumeVerifierContext*>(
      env->GetLongField(obj, IDCache::GetVolumeVerifierPointer()));
}

extern "C" {

JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_finalize(
    JNIEnv* env, jobject obj)
{
  delete GetContext(env, obj);
}

JNIEXPORT jlong JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_createNew(
    JNIEnv* env, jobject obj, jstring path, jboolean redump_verification, jboolean calculate_crc32,
    jboolean calculate_md5, jboolean calculate_sha1)
{
  std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(GetJString(env, path));
  if (!volume)
    return 0;

  auto* context = new VolumeVerifierContext{
      DiscIO::VolumeVerifier{
          *volume, !!redump_verification, {!!calculate_crc32, !!calculate_md5, !!calculate_sha1}},
      nullptr};

  context->volume = std::move(volume);

  return reinterpret_cast<jlong>(context);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_start(JNIEnv* env, jobject obj)
{
  GetContext(env, obj)->verifier.Start();
}

JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_process(
    JNIEnv* env, jobject obj)
{
  GetContext(env, obj)->verifier.Process();
}

JNIEXPORT jlong JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getBytesProcessed(JNIEnv* env,
                                                                                      jobject obj)
{
  return static_cast<jlong>(GetContext(env, obj)->verifier.GetBytesProcessed());
}

JNIEXPORT jlong JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getTotalBytes(JNIEnv* env,
                                                                                  jobject obj)
{
  return static_cast<jlong>(GetContext(env, obj)->verifier.GetTotalBytes());
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_finish(JNIEnv* env, jobject obj)
{
  GetContext(env, obj)->verifier.Finish();
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultSummaryText(
    JNIEnv* env, jobject obj)
{
  return ToJString(env, GetContext(env, obj)->verifier.GetResult().summary_text);
}

JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultRedumpStatus(
    JNIEnv* env, jobject obj)
{
  return static_cast<jint>(GetContext(env, obj)->verifier.GetResult().redump.status);
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultRedumpMessage(
    JNIEnv* env, jobject obj)
{
  return ToJString(env, GetContext(env, obj)->verifier.GetResult().redump.message);
}

JNIEXPORT jbyteArray JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultCrc32(JNIEnv* env,
                                                                                   jobject obj)
{
  return VectorToJByteArray(env, GetContext(env, obj)->verifier.GetResult().hashes.crc32);
}

JNIEXPORT jbyteArray JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultMd5(JNIEnv* env,
                                                                                 jobject obj)
{
  return VectorToJByteArray(env, GetContext(env, obj)->verifier.GetResult().hashes.md5);
}

JNIEXPORT jbyteArray JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultSha1(JNIEnv* env,
                                                                                  jobject obj)
{
  return VectorToJByteArray(env, GetContext(env, obj)->verifier.GetResult().hashes.sha1);
}

JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultProblemCount(
    JNIEnv* env, jobject obj)
{
  return static_cast<jint>(GetContext(env, obj)->verifier.GetResult().problems.size());
}

JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultProblemSeverity(
    JNIEnv* env, jobject obj, jint i)
{
  return static_cast<jint>(GetContext(env, obj)->verifier.GetResult().problems[i].severity);
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultProblemText(
    JNIEnv* env, jobject obj, jint i)
{
  return ToJString(env, GetContext(env, obj)->verifier.GetResult().problems[i].text);
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_shouldCalculateCrc32ByDefault(
    JNIEnv* env, jclass)
{
  return static_cast<jboolean>(DiscIO::VolumeVerifier::GetDefaultHashesToCalculate().crc32);
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_shouldCalculateMd5ByDefault(
    JNIEnv* env, jclass)
{
  return static_cast<jboolean>(DiscIO::VolumeVerifier::GetDefaultHashesToCalculate().md5);
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_shouldCalculateSha1ByDefault(
    JNIEnv* env, jclass)
{
  return static_cast<jboolean>(DiscIO::VolumeVerifier::GetDefaultHashesToCalculate().sha1);
}
}
