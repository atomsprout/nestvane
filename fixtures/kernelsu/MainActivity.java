package dev.nestvane.kernelsu;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.InputStreamReader;

// Runs ksutest (its native library directory has it as libksutest.so, so that Android unpacks it),
// and shows and logs (tag NestvaneKsu) each line it writes.
public class MainActivity extends Activity {
    private static final String TAG = "NestvaneKsu";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        final TextView text = new TextView(this);
        text.setTextSize(16);
        text.setPadding(48, 96, 48, 48);
        setContentView(text);
        final String program = getApplicationInfo().nativeLibraryDir + "/libksutest.so";
        new Thread(() -> {
            StringBuilder shown = new StringBuilder();
            try {
                Process process = new ProcessBuilder(program).redirectErrorStream(true).start();
                try (BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream()))) {
                    for (String line; (line = reader.readLine()) != null; ) {
                        Log.i(TAG, line);
                        shown.append(line).append('\n');
                    }
                }
                Log.i(TAG, "exit " + process.waitFor());
            } catch (Exception e) {
                Log.i(TAG, "failed: " + e);
                shown.append(e);
            }
            runOnUiThread(() -> text.setText(shown));
        }).start();
    }
}
