package org.meumeu.wivrn;

import android.app.NativeActivity;
import android.content.Context;

public class EmbeddedPlugin extends NativeActivity {
    static {
        System.loadLibrary("wivrn");
    }

    public static native void nativeLog(String message);

    public static void runMain(Context context) {
        // Obtain the paths in Java
        String configPath = context.getFilesDir().getAbsolutePath(); // Internal storage
        String cachePath = context.getCacheDir().getAbsolutePath(); // Cache storage

        // Pass the paths to C++ via JNI
        androidLibMain(configPath + "/wivrn", cachePath + "/wivrn");
    }

    public static native void androidLibMain(String configPath, String cachePath);
}
