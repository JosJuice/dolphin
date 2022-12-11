package org.dolphinemu.dolphinemu.features.verify.model;

import androidx.annotation.IntDef;
import androidx.annotation.Keep;
import androidx.annotation.NonNull;

import java.io.IOException;
import java.lang.annotation.Retention;

import static java.lang.annotation.RetentionPolicy.SOURCE;

/**
 * Checks a game file for errors.
 *
 * To be used as follows:
 *
 * <pre>
 * {@code
 * VolumeVerifier verifier = new VolumeVerifier(path, redumpVerification, calculateCrc32,
 *                                              calculateMd5, calculateSha1);
 * verifier.start();
 * while (verifier.getBytesProcessed() != verifier.getTotalBytes())
 *   verifier.process();
 * verifier.finish();
 * }
 * </pre>
 *
 * start, process and finish may take some time to run.
 *
 * The result methods can be called before the processing is finished,
 * but the result will be incomplete.
 */
public final class VolumeVerifier
{
  @Retention(SOURCE)
  @IntDef({SEVERITY_LOW, SEVERITY_MEDIUM, SEVERITY_HIGH})
  public @interface Severity {}
  public static final int SEVERITY_LOW = 1;
  public static final int SEVERITY_MEDIUM = 2;
  public static final int SEVERITY_HIGH = 3;

  @Retention(SOURCE)
  @IntDef({REDUMP_UNKNOWN, REDUMP_GOOD_DUMP, REDUMP_BAD_DUMP, REDUMP_ERROR})
  public @interface RedumpStatus {}
  public static final int REDUMP_UNKNOWN = 0;
  public static final int REDUMP_GOOD_DUMP = 1;
  public static final int REDUMP_BAD_DUMP = 2;
  public static final int REDUMP_ERROR = 3;

  @Keep
  private final long mPointer;

  public VolumeVerifier(String path, boolean redumpVerification, boolean calculateCrc32,
                        boolean calculateMd5, boolean calculateSha1) throws IOException
  {
    mPointer = createNew(path, redumpVerification, calculateCrc32, calculateMd5, calculateSha1);

    if (mPointer == 0)
      throw new IOException("Failed to open volume");
  }

  @Override
  public native void finalize();

  private native long createNew(String path, boolean redumpVerification, boolean calculateCrc32,
          boolean calculateMd5, boolean calculateSha1);

  public native void start();

  public native void process();

  public native long getBytesProcessed();

  public native long getTotalBytes();

  public native void finish();

  @NonNull
  public native String getResultSummaryText();

  @RedumpStatus
  public native int getResultRedumpStatus();

  @NonNull
  public native String getResultRedumpMessage();

  public native byte[] getResultCrc32();

  public native byte[] getResultMd5();

  public native byte[] getResultSha1();

  public native int getResultProblemCount();

  @Severity
  public native int getResultProblemSeverity(int i);

  @NonNull
  public native String getResultProblemText(int i);

  public static native boolean shouldCalculateCrc32ByDefault();

  public static native boolean shouldCalculateMd5ByDefault();

  public static native boolean shouldCalculateSha1ByDefault();
}
