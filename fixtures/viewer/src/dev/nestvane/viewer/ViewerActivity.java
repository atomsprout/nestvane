/*
 * Copyright 2026 AtomSprout
 * SPDX-License-Identifier: Apache-2.0
 */
package dev.nestvane.viewer;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.ImageDecoder;
import android.graphics.Matrix;
import android.graphics.RectF;
import android.graphics.drawable.AnimatedImageDrawable;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.GestureDetector;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.MediaController;
import android.widget.TextView;
import android.widget.VideoView;

/**
 * Shows the picture, or plays the video or the sound, that a VIEW intent names. Pictures can be
 * pinched and moved, and a double tap zooms in and out; a video's controls come and go with a tap.
 */
public final class ViewerActivity extends Activity {
    private static final String TAG = "NestvaneViewer";
    private FrameLayout root;
    private VideoView video;
    private int position;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        setContentView(root);
        position = state == null ? 0 : state.getInt("position");
        show(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        position = 0;
        show(intent);
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        if (video != null) state.putInt("position", video.getCurrentPosition());
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (video != null) {
            position = video.getCurrentPosition();
            video.pause();
        }
    }

    private void show(Intent intent) {
        root.removeAllViews();
        video = null;
        final Uri uri = intent.getData();
        String type = intent.getType();
        if (uri == null) {
            message("Nothing to open.");
            return;
        }
        if (type == null) type = getContentResolver().getType(uri);
        final String name = displayName(uri);
        setTitle(name != null ? name : uri.getLastPathSegment());
        Log.i(TAG, "open " + type + " " + name);
        if (type != null && type.startsWith("image/")) showPicture(uri);
        else if (type != null && (type.startsWith("video/") || type.startsWith("audio/")))
            play(uri, type.startsWith("audio/"), name);
        else message("Can’t open this kind of file.");
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) return cursor.getString(0);
        } catch (RuntimeException ignored) {
            // A file: address, or a provider that does not answer queries.
        }
        return null;
    }

    private void message(String text) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextColor(Color.WHITE);
        view.setTextSize(18);
        view.setGravity(Gravity.CENTER);
        root.addView(view, new FrameLayout.LayoutParams(-1, -1));
    }

    private void showPicture(final Uri uri) {
        final ZoomView view = new ZoomView(this);
        root.addView(view, new FrameLayout.LayoutParams(-1, -1));
        // Decoded away from the main thread, no larger than twice the screen: a photo from a camera
        // has some fifty million pixels at full size.
        final int most = 2 * Math.max(getResources().getDisplayMetrics().widthPixels,
                                      getResources().getDisplayMetrics().heightPixels);
        new Thread(() -> {
            Drawable drawable = null;
            try {
                drawable = ImageDecoder.decodeDrawable(ImageDecoder.createSource(getContentResolver(), uri),
                    (decoder, info, source) -> {
                        int size = Math.max(info.getSize().getWidth(), info.getSize().getHeight());
                        int sample = 1;
                        while (size / sample > most) sample *= 2;
                        decoder.setTargetSampleSize(sample);
                    });
            } catch (Exception e) {
                Log.w(TAG, "cannot decode " + uri, e);
            }
            final Drawable decoded = drawable;
            runOnUiThread(() -> {
                if (isDestroyed() || view.getParent() == null) return;
                if (decoded == null) {
                    root.removeView(view);
                    message("Can’t show this picture.");
                    return;
                }
                view.setImageDrawable(decoded);
                if (decoded instanceof AnimatedImageDrawable) ((AnimatedImageDrawable) decoded).start();
            });
        }, "decode").start();
    }

    private void play(Uri uri, boolean sound, String name) {
        video = new VideoView(this);
        FrameLayout.LayoutParams centered = new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER);
        root.addView(video, sound ? new FrameLayout.LayoutParams(1, 1, Gravity.CENTER) : centered);
        if (sound) {
            TextView title = new TextView(this);
            title.setText(name != null ? name : "");
            title.setTextColor(Color.WHITE);
            title.setTextSize(20);
            title.setGravity(Gravity.CENTER);
            title.setPadding(48, 48, 48, 48);
            root.addView(title, new FrameLayout.LayoutParams(-1, -1));
        }
        final MediaController controls = new MediaController(this);
        controls.setAnchorView(root);
        video.setMediaController(controls);
        video.setOnPreparedListener(player -> {
            if (position > 0) video.seekTo(position);
            video.start();
            controls.show(sound ? 0 : 3000);
        });
        video.setOnErrorListener((player, what, extra) -> {
            Log.w(TAG, "cannot play " + uri + ": " + what + " " + extra);
            root.removeAllViews();
            video = null;
            message(sound ? "Can’t play this sound." : "Can’t play this video.");
            return true;
        });
        root.setOnClickListener(v -> {
            if (controls.isShowing()) controls.hide();
            else controls.show(sound ? 0 : 3000);
        });
        video.setVideoURI(uri);
    }

    /** A picture that fits the screen, can be pinched, moved, and zoomed with a double tap. */
    private static final class ZoomView extends ImageView {
        private final Matrix matrix = new Matrix();
        private final ScaleGestureDetector pinch;
        private final GestureDetector gestures;
        private float fit = 1;

        ZoomView(Activity activity) {
            super(activity);
            setScaleType(ScaleType.MATRIX);
            pinch = new ScaleGestureDetector(activity, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                @Override
                public boolean onScale(ScaleGestureDetector detector) {
                    zoom(detector.getScaleFactor(), detector.getFocusX(), detector.getFocusY());
                    return true;
                }
            });
            gestures = new GestureDetector(activity, new GestureDetector.SimpleOnGestureListener() {
                @Override
                public boolean onScroll(MotionEvent first, MotionEvent event, float dx, float dy) {
                    matrix.postTranslate(-dx, -dy);
                    settle();
                    return true;
                }

                @Override
                public boolean onDoubleTap(MotionEvent event) {
                    if (scale() > fit * 1.01f) fitScreen();
                    else zoom(2.5f, event.getX(), event.getY());
                    return true;
                }
            });
        }

        @Override
        public void setImageDrawable(Drawable drawable) {
            super.setImageDrawable(drawable);
            fitScreen();
        }

        @Override
        protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
            super.onSizeChanged(width, height, oldWidth, oldHeight);
            fitScreen();
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            pinch.onTouchEvent(event);
            if (!pinch.isInProgress()) gestures.onTouchEvent(event);
            return true;
        }

        private float scale() {
            float[] values = new float[9];
            matrix.getValues(values);
            return values[Matrix.MSCALE_X];
        }

        private void fitScreen() {
            Drawable drawable = getDrawable();
            if (drawable == null || getWidth() == 0 || drawable.getIntrinsicWidth() <= 0) return;
            RectF picture = new RectF(0, 0, drawable.getIntrinsicWidth(), drawable.getIntrinsicHeight());
            matrix.setRectToRect(picture, new RectF(0, 0, getWidth(), getHeight()), Matrix.ScaleToFit.CENTER);
            fit = scale();
            setImageMatrix(matrix);
        }

        private void zoom(float factor, float x, float y) {
            float wanted = Math.max(fit, Math.min(fit * 8, scale() * factor));
            matrix.postScale(wanted / scale(), wanted / scale(), x, y);
            settle();
        }

        // Keeps the picture on the screen: centred where it is smaller, edge to edge where larger.
        private void settle() {
            Drawable drawable = getDrawable();
            if (drawable == null) return;
            RectF bounds = new RectF(0, 0, drawable.getIntrinsicWidth(), drawable.getIntrinsicHeight());
            matrix.mapRect(bounds);
            float dx = bounds.width() <= getWidth() ? (getWidth() - bounds.width()) / 2 - bounds.left
                : bounds.left > 0 ? -bounds.left : bounds.right < getWidth() ? getWidth() - bounds.right : 0;
            float dy = bounds.height() <= getHeight() ? (getHeight() - bounds.height()) / 2 - bounds.top
                : bounds.top > 0 ? -bounds.top : bounds.bottom < getHeight() ? getHeight() - bounds.bottom : 0;
            matrix.postTranslate(dx, dy);
            setImageMatrix(matrix);
        }
    }
}
