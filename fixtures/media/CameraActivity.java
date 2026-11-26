package dev.nestvane.media;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaMetadataRetriever;
import android.media.MediaRecorder;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.util.Log;
import android.util.Size;
import android.view.Surface;

import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * Uses the cameras as a camera app does and says in the log how it went. Started with the extra
 * "what": "list" names every camera with the way it faces; "photo" takes a JPEG with the camera
 * given by the extra "facing" ("back", the default, or "front") and saves it into DCIM/Camera of the
 * media library; "video" records five seconds from that camera, with sound, into Movies the way a
 * camera app does (a pending item, then published), and reads back its length and frames.
 */
public final class CameraActivity extends Activity {
    private static final String TAG = "NestvaneMedia";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        final String what = getIntent().getStringExtra("what");
        final String facing = getIntent().getStringExtra("facing");
        new Thread(() -> {
            try {
                if ("list".equals(what)) list();
                else if ("photo".equals(what)) photo("front".equals(facing));
                else if ("video".equals(what)) video("front".equals(facing));
                else Log.i(TAG, "camera: no such test " + what);
            } catch (Throwable error) {
                Log.w(TAG, "camera " + what + " failed", error);
            }
            runOnUiThread(this::finish);
        }, "camera").start();
    }

    private CameraManager manager() { return (CameraManager) getSystemService(Context.CAMERA_SERVICE); }

    private void list() throws Exception {
        String[] ids = manager().getCameraIdList();
        Log.i(TAG, "camera list: " + ids.length + " camera(s)");
        for (String id : ids) {
            CameraCharacteristics about = manager().getCameraCharacteristics(id);
            Integer lens = about.get(CameraCharacteristics.LENS_FACING);
            StreamConfigurationMap map = about.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Log.i(TAG, "camera list: id " + id + " facing " + (lens == null ? "?" : lens == CameraCharacteristics.LENS_FACING_FRONT ? "front" : lens == CameraCharacteristics.LENS_FACING_BACK ? "back" : "external")
                + " orientation " + about.get(CameraCharacteristics.SENSOR_ORIENTATION)
                + " jpeg " + (map == null ? "none" : Arrays.toString(map.getOutputSizes(ImageFormat.JPEG))));
        }
    }

    private String find(boolean front) throws Exception {
        for (String id : manager().getCameraIdList()) {
            Integer lens = manager().getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
            if (lens != null && (lens == CameraCharacteristics.LENS_FACING_FRONT) == front) return id;
        }
        throw new IllegalStateException("no " + (front ? "front" : "back") + " camera");
    }

    private static final class Opened {
        final HandlerThread thread = new HandlerThread("camera-callbacks");
        Handler handler;
        CameraDevice device;
        CameraCaptureSession session;

        void close() {
            if (session != null) session.close();
            if (device != null) device.close();
            thread.quitSafely();
        }
    }

    private Opened open(String id, Surface... outputs) throws Exception {
        Opened opened = new Opened();
        opened.thread.start();
        opened.handler = new Handler(opened.thread.getLooper());
        BlockingQueue<Object> result = new ArrayBlockingQueue<>(1);
        manager().openCamera(id, new CameraDevice.StateCallback() {
            @Override public void onOpened(CameraDevice device) { result.offer(device); }
            @Override public void onDisconnected(CameraDevice device) { result.offer("disconnected"); device.close(); }
            @Override public void onError(CameraDevice device, int error) { result.offer("error " + error); device.close(); }
        }, opened.handler);
        Object device = result.poll(20, TimeUnit.SECONDS);
        if (!(device instanceof CameraDevice)) throw new IllegalStateException("open camera " + id + ": " + device);
        opened.device = (CameraDevice) device;
        opened.device.createCaptureSession(Arrays.asList(outputs), new CameraCaptureSession.StateCallback() {
            @Override public void onConfigured(CameraCaptureSession session) { result.offer(session); }
            @Override public void onConfigureFailed(CameraCaptureSession session) { result.offer("configure failed"); }
        }, opened.handler);
        Object session = result.poll(20, TimeUnit.SECONDS);
        if (!(session instanceof CameraCaptureSession)) {
            opened.close();
            throw new IllegalStateException("session for camera " + id + ": " + session);
        }
        opened.session = (CameraCaptureSession) session;
        return opened;
    }

    private void photo(boolean front) throws Exception {
        String id = find(front);
        long began = System.nanoTime();
        ImageReader preview = ImageReader.newInstance(640, 480, ImageFormat.YUV_420_888, 4);
        ImageReader still = ImageReader.newInstance(640, 480, ImageFormat.JPEG, 2);
        HandlerThread readers = new HandlerThread("camera-readers");
        readers.start();
        Handler handler = new Handler(readers.getLooper());
        final int[] previewFrames = {0};
        preview.setOnImageAvailableListener(reader -> {
            Image image = reader.acquireLatestImage();
            if (image != null) { ++previewFrames[0]; image.close(); }
        }, handler);
        BlockingQueue<byte[]> jpegs = new ArrayBlockingQueue<>(2);
        still.setOnImageAvailableListener(reader -> {
            Image image = reader.acquireNextImage();
            if (image == null) return;
            ByteBuffer buffer = image.getPlanes()[0].getBuffer();
            byte[] bytes = new byte[buffer.remaining()];
            buffer.get(bytes);
            image.close();
            jpegs.offer(bytes);
        }, handler);
        Opened opened = open(id, preview.getSurface(), still.getSurface());
        try {
            CaptureRequest.Builder repeating = opened.device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            repeating.addTarget(preview.getSurface());
            opened.session.setRepeatingRequest(repeating.build(), null, opened.handler);
            Thread.sleep(2000);
            Log.i(TAG, "camera photo " + (front ? "front" : "back") + ": id " + id + ", " + previewFrames[0] + " preview frames in 2 s");
            CaptureRequest.Builder capture = opened.device.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
            capture.addTarget(still.getSurface());
            capture.set(CaptureRequest.JPEG_ORIENTATION, front ? 270 : 90);
            opened.session.capture(capture.build(), null, opened.handler);
            byte[] jpeg = jpegs.poll(20, TimeUnit.SECONDS);
            if (jpeg == null) throw new IllegalStateException("no picture within 20 s");
            Bitmap bitmap = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
            long sum = 0;
            int samples = 0;
            for (int y = 0; y < bitmap.getHeight(); y += 8) {
                for (int x = 0; x < bitmap.getWidth(); x += 8) {
                    int pixel = bitmap.getPixel(x, y);
                    sum += ((pixel >> 16) & 255) + ((pixel >> 8) & 255) + (pixel & 255);
                    ++samples;
                }
            }
            ContentResolver resolver = getContentResolver();
            ContentValues values = new ContentValues();
            String name = "NESTVANE_" + (front ? "FRONT_" : "BACK_") + System.currentTimeMillis() + ".jpg";
            values.put(MediaStore.Images.Media.DISPLAY_NAME, name);
            values.put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg");
            values.put(MediaStore.Images.Media.RELATIVE_PATH, "DCIM/Camera");
            values.put(MediaStore.Images.Media.IS_PENDING, 1);
            Uri item = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values);
            try (OutputStream out = resolver.openOutputStream(item)) { out.write(jpeg); }
            values.clear();
            values.put(MediaStore.Images.Media.IS_PENDING, 0);
            resolver.update(item, values, null, null);
            Log.i(TAG, "camera photo " + (front ? "front" : "back") + ": " + jpeg.length + " bytes, "
                + bitmap.getWidth() + "x" + bitmap.getHeight() + ", mean level " + (samples == 0 ? 0 : sum / (3 * samples))
                + ", saved as DCIM/Camera/" + name + " (" + item + ") in " + (System.nanoTime() - began) / 1_000_000 + " ms");
        } finally {
            opened.close();
            preview.close();
            still.close();
            readers.quitSafely();
        }
    }

    private void video(boolean front) throws Exception {
        String id = find(front);
        ContentResolver resolver = getContentResolver();
        ContentValues values = new ContentValues();
        String name = "NESTVANE_" + (front ? "FRONT_" : "BACK_") + System.currentTimeMillis() + ".mp4";
        values.put(MediaStore.Video.Media.DISPLAY_NAME, name);
        values.put(MediaStore.Video.Media.MIME_TYPE, "video/mp4");
        values.put(MediaStore.Video.Media.RELATIVE_PATH, "DCIM/Camera");
        values.put(MediaStore.Video.Media.IS_PENDING, 1);
        Uri item = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values);
        ParcelFileDescriptor file = resolver.openFileDescriptor(item, "rw");
        MediaRecorder recorder = new MediaRecorder(this);
        recorder.setOnErrorListener((r, what, extra) -> Log.w(TAG, "camera video: error " + what + " " + extra));
        recorder.setAudioSource(MediaRecorder.AudioSource.CAMCORDER);
        recorder.setVideoSource(MediaRecorder.VideoSource.SURFACE);
        recorder.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
        recorder.setVideoEncoder(MediaRecorder.VideoEncoder.H264);
        recorder.setAudioEncoder(MediaRecorder.AudioEncoder.AAC);
        recorder.setVideoSize(640, 480);
        recorder.setVideoFrameRate(30);
        recorder.setVideoEncodingBitRate(2_000_000);
        recorder.setOrientationHint(front ? 270 : 90);
        recorder.setOutputFile(file.getFileDescriptor());
        recorder.prepare();
        Surface input = recorder.getSurface();
        Opened opened = open(id, input);
        long began = System.nanoTime();
        try {
            CaptureRequest.Builder request = opened.device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
            request.addTarget(input);
            opened.session.setRepeatingRequest(request.build(), null, opened.handler);
            recorder.start();
            Thread.sleep(5000);
            recorder.stop();
        } finally {
            opened.close();
            recorder.release();
            file.close();
        }
        long recorded = (System.nanoTime() - began) / 1_000_000;
        values.clear();
        values.put(MediaStore.Video.Media.IS_PENDING, 0);
        resolver.update(item, values, null, null);
        MediaMetadataRetriever retriever = new MediaMetadataRetriever();
        retriever.setDataSource(this, item);
        String size;
        try (ParcelFileDescriptor read = resolver.openFileDescriptor(item, "r")) { size = String.valueOf(read.getStatSize()); }
        Log.i(TAG, "camera video " + (front ? "front" : "back") + ": " + size + " bytes in " + recorded + " ms, duration "
            + retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION) + " ms, frames "
            + retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_FRAME_COUNT) + ", "
            + retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH) + "x"
            + retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT) + ", rotation "
            + retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION) + ", sound "
            + retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_HAS_AUDIO)
            + ", saved as DCIM/Camera/" + name);
        retriever.release();
    }
}
