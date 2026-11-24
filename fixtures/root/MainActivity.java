package dev.nestvane.root;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;

// Runs "su -c id" and "su -c ls /data/data/com.android.settings" on a thread of its own, and shows and
// logs (tag NestvaneRoot) what came back and how long it took. Without root, su is missing or refused.
public class MainActivity extends Activity {
    private static final String TAG = "NestvaneRoot";
    private TextView text;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        text = new TextView(this);
        text.setTextSize(22);
        text.setPadding(48, 96, 48, 48);
        setContentView(text);
        show("su present: " + (new File("/system/bin/su").exists() || new File("/system/xbin/su").exists()));
        new Thread(() -> {
            exec(new String[] {"sh", "-c", "echo plain"});
            run("echo builtin");
            run("id");
            run("ls /data/data/com.android.settings");
        }).start();
    }

    private void run(String command) {
        exec(new String[] {"su", "-c", command});
    }

    private void exec(String[] arguments) {
        final String command = String.join(" ", arguments);
        final long started = System.currentTimeMillis();
        StringBuilder output = new StringBuilder();
        int status;
        try {
            Process process = Runtime.getRuntime().exec(arguments);
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream()))) {
                for (String line; (line = reader.readLine()) != null; ) output.append(line).append(' ');
            }
            status = process.waitFor();
        } catch (Exception e) {
            output.append(e.getMessage());
            status = -1;
        }
        show(command + " -> " + status + " in " + (System.currentTimeMillis() - started) + " ms: " + output.toString().trim());
    }

    private void show(String line) {
        Log.i(TAG, line);
        runOnUiThread(() -> text.append(line + "\n\n"));
    }
}
