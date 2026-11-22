package dev.nestvane.media;

import android.app.Activity;
import android.media.MediaPlayer;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "NestvaneMedia";
    private final Handler handler = new Handler();
    private MediaPlayer player;
    private TextView status;
    private long started;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        FrameLayout layout = new FrameLayout(this);
        SurfaceView video = new SurfaceView(this);
        layout.addView(video);
        status = new TextView(this);
        status.setTextSize(16);
        status.setTextColor(0xffffffff);
        status.setBackgroundColor(0x80000000);
        layout.addView(status, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL));
        setContentView(layout);
        video.getHolder().addCallback(this);
    }

    private void say(String text) {
        Log.i(TAG, text);
        status.setText(text);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        File file = new File(getExternalFilesDir(null), "test.mp4");
        if (!file.isFile()) {
            say("No " + file);
            return;
        }
        try {
            player = new MediaPlayer();
            player.setDataSource(file.getPath());
            player.setDisplay(holder);
            player.setLooping(true);
            player.setOnErrorListener((mp, what, extra) -> {
                say("Error " + what + " " + extra);
                return true;
            });
            player.setOnInfoListener((mp, what, extra) -> {
                Log.i(TAG, "Info " + what + " " + extra);
                return false;
            });
            player.setOnPreparedListener(mp -> {
                say("Prepared " + mp.getVideoWidth() + "x" + mp.getVideoHeight() + ", " + mp.getDuration() + " ms");
                started = SystemClock.elapsedRealtime();
                mp.start();
                handler.postDelayed(this::report, 1000);
            });
            player.prepareAsync();
            say("Preparing " + file.length() + " bytes");
        } catch (Exception error) {
            say("Failed: " + error);
        }
    }

    // Each second: where the playing is, against the time that has passed. They advance together when it plays smoothly.
    private void report() {
        if (player == null) return;
        say("Position " + player.getCurrentPosition() + " ms after " + (SystemClock.elapsedRealtime() - started) + " ms"
                + (player.isPlaying() ? "" : ", not playing"));
        handler.postDelayed(this::report, 1000);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (player != null) player.release();
        player = null;
    }
}
