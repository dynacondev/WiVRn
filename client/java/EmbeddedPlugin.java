package org.meumeu.wivrn;

import android.app.NativeActivity;

public class EmbeddedPlugin extends NativeActivity {
    static {
        System.loadLibrary("wivrn");
    }

    public static native void nativeLog(String message);
}
