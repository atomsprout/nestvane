import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Looper;

import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/**
 * Tells Nestvane the names of the apps the person installed, for its root prompt and Settings:
 * "package<TAB>name" lines, written to /dev/nestvane-labels. Run as shell by
 * fixtures/android17-text.sh when the app asks.
 */
public final class AppLabels {
    public static void main(String[] args) throws Exception {
        Looper.prepareMainLooper();
        Class<?> thread = Class.forName("android.app.ActivityThread");
        Object main = thread.getMethod("systemMain").invoke(null);
        Context context = (Context) thread.getMethod("getSystemContext").invoke(main);
        PackageManager packages = context.getPackageManager();
        StringBuilder lines = new StringBuilder();
        for (ApplicationInfo app : packages.getInstalledApplications(0)) {
            if ((app.flags & ApplicationInfo.FLAG_SYSTEM) != 0) continue;
            String name = String.valueOf(packages.getApplicationLabel(app)).replaceAll("[\\t\\n\\r]", " ").trim();
            if (name.isEmpty() || name.length() > 100) continue;
            lines.append(app.packageName).append('\t').append(name).append('\n');
        }
        try (FileOutputStream out = new FileOutputStream("/dev/nestvane-labels")) {
            out.write(lines.toString().getBytes(StandardCharsets.UTF_8));
        }
        // The system context keeps threads alive: leave at once.
        System.exit(0);
    }
}
