// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <jni.h>

#include "DiscIO/VolumeVerifier.h"
#include "jni/AndroidCommon/AndroidCommon.h"
#include "jni/AndroidCommon/IDCache.h"

static DiscIO::VolumeVerifier* GetPointer(JNIEnv* env, jobject obj)
{
  return reinterpret_cast<DiscIO::VolumeVerifier*>(
      env->GetLongField(obj, IDCache::GetVolumeVerifierPointer()));
}

extern "C" {

JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_finalize(
    JNIEnv* env, jobject obj)
{
  delete GetPointer(env, obj);
}

JNIEXPORT jlong JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_createNew(
    JNIEnv* env, jobject obj, jstring path, jboolean redump_verification, jboolean calculate_crc32,
    jboolean calculate_md5, jboolean calculate_sha1)
{
  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(GetJString(env, path));
  if (!volume)
    return 0;

  auto* verifier = new DiscIO::VolumeVerifier(
      *volume, redump_verification, {!!calculate_crc32, !!calculate_md5, !!calculate_sha1});

  return reinterpret_cast<jlong>(verifier);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_start(JNIEnv* env, jobject obj)
{
  GetPointer(env, obj)->Start();
}

JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_process(
    JNIEnv* env, jobject obj)
{
  GetPointer(env, obj)->Process();
}

JNIEXPORT jlong JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getBytesProcessed(JNIEnv* env,
                                                                                      jobject obj)
{
  return static_cast<jlong>(GetPointer(env, obj)->GetBytesProcessed());
}

JNIEXPORT jlong JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getTotalBytes(JNIEnv* env,
                                                                                  jobject obj)
{
  return static_cast<jlong>(GetPointer(env, obj)->GetTotalBytes());
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_finish(JNIEnv* env, jobject obj)
{
  GetPointer(env, obj)->Process();
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultSummaryText(
    JNIEnv* env, jobject obj)
{
  return ToJString(env, GetPointer(env, obj)->GetResult().summary_text);
}

JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultRedumpStatus(
    JNIEnv* env, jobject obj)
{
  return static_cast<jint>(GetPointer(env, obj)->GetResult().redump.status);
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultRedumpMessage(
    JNIEnv* env, jobject obj)
{
  return ToJString(env, GetPointer(env, obj)->GetResult().redump.message);
}

JNIEXPORT jbyteArray JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultCrc32(JNIEnv* env,
                                                                                   jobject obj)
{
  return VectorToJByteArray(env, GetPointer(env, obj)->GetResult().hashes.crc32);
}

JNIEXPORT jbyteArray JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultMd5(JNIEnv* env,
                                                                                 jobject obj)
{
  return VectorToJByteArray(env, GetPointer(env, obj)->GetResult().hashes.md5);
}

JNIEXPORT jbyteArray JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultSha1(JNIEnv* env,
                                                                                  jobject obj)
{
  return VectorToJByteArray(env, GetPointer(env, obj)->GetResult().hashes.sha1);
}

JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultProblemCount(
    JNIEnv* env, jobject obj)
{
  return static_cast<jint>(GetPointer(env, obj)->GetResult().problems.size());
}

JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultProblemSeverity(
    JNIEnv* env, jobject obj, jint i)
{
  return static_cast<jint>(GetPointer(env, obj)->GetResult().problems[i].severity);
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_verify_model_VolumeVerifier_getResultProblemText(
    JNIEnv* env, jobject obj, jint i)
{
  return ToJString(env, GetPointer(env, obj)->GetResult().problems[i].text);
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
