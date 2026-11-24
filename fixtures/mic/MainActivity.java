package dev.nestvane.mic;

import android.app.Activity;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

public class MainActivity extends Activity {
    private static final String TAG = "NestvaneMic";
    private TextView text;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        text = new TextView(this);
        text.setTextSize(22);
        text.setPadding(48, 96, 48, 48);
        setContentView(text);
        new Thread(this::record).start();
    }

    private void record() {
        try {
            if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) != 0) {
                Process grant = Runtime.getRuntime().exec(new String[] {"su", "-c",
                        "cmd package grant dev.nestvane.mic android.permission.RECORD_AUDIO 2>&1"});
                StringBuilder said = new StringBuilder();
                try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.InputStreamReader(grant.getInputStream()))) {
                    for (String line; (line = reader.readLine()) != null; ) said.append(line).append(' ');
                }
                show("grant -> " + grant.waitFor() + " " + said.toString().trim() + "; now " +
                     (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) == 0 ? "granted" : "not granted"));
            }
            final int rate = 16000;
            final int minimum = AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
            AudioRecord recorder = new AudioRecord(MediaRecorder.AudioSource.MIC, rate, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, Math.max(minimum, rate));
            short[] samples = new short[rate * 3];
            recorder.startRecording();
            int read = 0;
            while (read < samples.length) {
                int got = recorder.read(samples, read, samples.length - read);
                if (got <= 0) break;
                read += got;
            }
            recorder.stop();
            recorder.release();
            long sum = 0;
            int peak = 0;
            // The first half second may be the device settling: measure the rest.
            for (int i = rate / 2; i < read; ++i) {
                sum += (long) samples[i] * samples[i];
                peak = Math.max(peak, Math.abs(samples[i]));
            }
            double rms = Math.sqrt(sum / (double) Math.max(1, read - rate / 2));
            show("recorded " + read + " samples, peak " + peak + ", rms " + String.format("%.1f", rms));
        } catch (Exception e) {
            show("failed: " + e);
        }
    }

    private void show(String line) {
        Log.i(TAG, line);
        runOnUiThread(() -> text.append(line + "\n\n"));
    }
}
