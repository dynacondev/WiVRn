package org.meumeu.wivrn;

import android.app.NativeActivity;
import android.content.Context;

public class EmbeddedPlugin extends NativeActivity {
    static {
        System.loadLibrary("wivrn");
    }

    public static native void nativeLog(String message);

    public static void runMain(Context context, long xrInstance, long xrSystemId, long xrSession) {
        // Log the received OpenXR handles
        nativeLog("Received OpenXR handles in Java:");
        nativeLog("XrInstance: " + xrInstance);
        nativeLog("XrSystemId: " + xrSystemId);
        nativeLog("XrSession: " + xrSession);

        // Obtain the paths in Java
        String configPath = context.getFilesDir().getAbsolutePath(); // Internal storage
        String cachePath = context.getCacheDir().getAbsolutePath(); // Cache storage

        // Pass the paths to C++ via JNI
        androidLibMain(configPath + "/wivrn", cachePath + "/wivrn", xrInstance, xrSystemId, xrSession);
    }

    public static native void androidLibMain(String configPath, String cachePath, long xrInstance, long xrSystemId, long xrSession);

    // private static final String TAG = "MyPlugin";

    // public static void runMain(Activity activity, long xrInstance, long
    // xrSystemId, long xrSession) {
    // nativeLog("Received OpenXR handles in Java:");
    // nativeLog("XrInstance: " + xrInstance);
    // nativeLog("XrSystemId: " + xrSystemId);
    // nativeLog("XrSession: " + xrSession);

    // // Pass these references to the native C++ layer if needed
    // updateNativeOpenXRHandles(xrInstance, xrSystemId, xrSession);

    // // Continue with your main setup logic
    // nativeLog("Running main setup...");
    // // Add additional setup logic here
    // }
}
