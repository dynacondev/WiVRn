package org.meumeu.wivrn;

import android.app.NativeActivity;
import android.content.Context;
import android.content.res.AssetManager;

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
        AssetManager assetManager = context.getAssets(); // Asset manager

        // Pass the paths to C++ via JNI
        androidLibMain(assetManager, configPath + "/wivrn", cachePath + "/wivrn", xrInstance, xrSystemId, xrSession);
    }

    public static native void androidLibMain(AssetManager assetManager, String configPath, String cachePath, long xrInstance, long xrSystemId, long xrSession);
}
