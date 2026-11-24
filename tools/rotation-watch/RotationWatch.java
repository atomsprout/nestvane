import android.os.IBinder;
import android.os.ServiceManager;
import android.util.Log;
import android.view.IRotationWatcher;
import android.view.IWindowManager;

import java.io.FileOutputStream;

/**
 * Tells the app which way up Android's display is: 0 to 3, a quarter turn each, written as one
 * digit to /dev/nestvane-rotation at the start and each time it changes. The frames the app gets
 * are always the display as it was made, so when Android turns its screens, for an app that asks
 * for landscape or because the app asked it to (text.sh, "rotate"), the app has to turn the
 * picture and the touches back. Run by init (DynamicProbe.cpp) for as long as Android runs.
 */
public final class RotationWatch {
    private static final String TAG = "NestvaneRotation";

    private static void report(int rotation) {
        try (FileOutputStream out = new FileOutputStream("/dev/nestvane-rotation")) {
            out.write('0' + (rotation & 3));
        } catch (Exception e) {
            Log.w(TAG, "could not report rotation " + rotation + ": " + e);
        }
    }

    public static void main(String[] args) throws Exception {
        IBinder binder = null;
        for (int tries = 0; tries < 120 && binder == null; ++tries) {
            binder = ServiceManager.getService("window");
            if (binder == null) Thread.sleep(500);
        }
        if (binder == null) throw new IllegalStateException("no window service");
        IWindowManager windows = IWindowManager.Stub.asInterface(binder);
        IRotationWatcher watcher = new IRotationWatcher.Stub() {
            @Override public void onRotationChanged(int rotation) { report(rotation); }
        };
        report(windows.watchRotation(watcher, 0));
        Log.i(TAG, "watching the display's rotation");
        while (true) Thread.sleep(3600_000);
    }
}
