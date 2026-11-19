package dev.nestvane.smoke;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ImageFormat;
import android.graphics.Insets;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureFailure;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.Size;
import android.view.Surface;
import android.view.TextureView;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.Comparator;

public final class CameraActivity extends Activity implements TextureView.SurfaceTextureListener {
    private static final String TAG = "NestvaneCamera";
    private static final int CAMERA_PERMISSION = 1;

    private final Handler callbacks = new Handler(Looper.getMainLooper());
    private TextureView preview;
    private TextView status;
    private Button capture;
    private CameraDevice camera;
    private CameraCaptureSession session;
    private Surface previewSurface;
    private ImageReader jpegReader;
    private int generation;
    private boolean resumed;
    private boolean opening;
    private boolean sawFrame;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        if (Build.VERSION.SDK_INT >= 30) {
            layout.setOnApplyWindowInsetsListener((view, insets) -> {
                Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
                return insets;
            });
        } else {
            layout.setFitsSystemWindows(true);
        }
        preview = new TextureView(this);
        preview.setSurfaceTextureListener(this);
        layout.addView(preview, new LinearLayout.LayoutParams(-1, 0, 1));
        status = new TextView(this);
        layout.addView(status);
        capture = new Button(this);
        capture.setText("Capture photo");
        capture.setEnabled(false);
        capture.setOnClickListener(view -> capture());
        layout.addView(capture);
        setContentView(layout);

        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            status.setText("Waiting for Android camera permission");
            requestPermissions(new String[]{Manifest.permission.CAMERA}, CAMERA_PERMISSION);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        openCamera();
    }

    @Override
    protected void onPause() {
        resumed = false;
        closeCamera();
        super.onPause();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode != CAMERA_PERMISSION) return;
        if (results.length == 1 && results[0] == PackageManager.PERMISSION_GRANTED) {
            openCamera();
        } else {
            report("Android camera permission denied");
        }
    }

    private static Size chooseSize(Size[] sizes) {
        return Arrays.stream(sizes)
            .min(Comparator.comparingLong(size -> Math.abs((long) size.getWidth() * size.getHeight() - 640L * 480)))
            .orElseThrow(() -> new IllegalStateException("No supported stream sizes"));
    }

    private void openCamera() {
        if (!resumed || !preview.isAvailable() || opening || camera != null ||
            checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            return;
        }
        CameraManager manager = getSystemService(CameraManager.class);
        try {
            for (String id : manager.getCameraIdList()) {
                CameraCharacteristics info = manager.getCameraCharacteristics(id);
                Integer facing = info.get(CameraCharacteristics.LENS_FACING);
                if (facing == null || facing != CameraCharacteristics.LENS_FACING_BACK) continue;

                StreamConfigurationMap streams = info.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                if (streams == null) continue;
                Size size = chooseSize(streams.getOutputSizes(SurfaceTexture.class));
                Size photo = chooseSize(streams.getOutputSizes(ImageFormat.JPEG));
                SurfaceTexture texture = preview.getSurfaceTexture();
                texture.setDefaultBufferSize(size.getWidth(), size.getHeight());
                previewSurface = new Surface(texture);
                jpegReader = ImageReader.newInstance(photo.getWidth(), photo.getHeight(), ImageFormat.JPEG, 2);
                jpegReader.setOnImageAvailableListener(this::readPhoto, callbacks);
                opening = true;
                status.setText("Opening rear camera " + id);
                int attempt = ++generation;
                manager.openCamera(id, new CameraDevice.StateCallback() {
                    @Override
                    public void onOpened(CameraDevice device) {
                        if (attempt != generation) {
                            device.close();
                            return;
                        }
                        opening = false;
                        camera = device;
                        startPreview(attempt);
                    }

                    @Override
                    public void onDisconnected(CameraDevice device) {
                        device.close();
                        if (attempt == generation) fail("Rear camera disconnected");
                    }

                    @Override
                    public void onError(CameraDevice device, int error) {
                        device.close();
                        if (attempt == generation) fail("Rear camera error " + error);
                    }
                }, callbacks);
                return;
            }
            report("No rear camera reported by Android");
        } catch (CameraAccessException | RuntimeException error) {
            fail("Could not open rear camera: " + error.getMessage());
        }
    }

    private void startPreview(int attempt) {
        try {
            camera.createCaptureSession(Arrays.asList(previewSurface, jpegReader.getSurface()),
                new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(CameraCaptureSession configured) {
                        if (attempt != generation) {
                            configured.close();
                            return;
                        }
                        session = configured;
                        try {
                            CaptureRequest.Builder request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                            request.addTarget(previewSurface);
                            session.setRepeatingRequest(request.build(), null, callbacks);
                        } catch (CameraAccessException | IllegalStateException error) {
                            fail("Preview request failed: " + error.getMessage());
                        }
                    }

                    @Override
                    public void onConfigureFailed(CameraCaptureSession configured) {
                        configured.close();
                        if (attempt == generation) fail("Camera could not configure preview and JPEG streams");
                    }
                }, callbacks);
        } catch (CameraAccessException | IllegalStateException error) {
            fail("Could not start preview: " + error.getMessage());
        }
    }

    private void capture() {
        if (session == null || !sawFrame) return;
        capture.setEnabled(false);
        try {
            CaptureRequest.Builder request = camera.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
            request.addTarget(jpegReader.getSurface());
            session.capture(request.build(), new CameraCaptureSession.CaptureCallback() {
                @Override
                public void onCaptureFailed(CameraCaptureSession active, CaptureRequest request,
                                            CaptureFailure failure) {
                    if (active == session) {
                        report("Photo request failed: " + failure.getReason());
                        capture.setEnabled(true);
                    }
                }
            }, callbacks);
        } catch (CameraAccessException | IllegalStateException error) {
            fail("Could not capture photo: " + error.getMessage());
        }
    }

    private void readPhoto(ImageReader reader) {
        if (reader != jpegReader) return;
        try (Image image = reader.acquireNextImage()) {
            if (image == null) return;
            ByteBuffer data = image.getPlanes()[0].getBuffer();
            byte[] jpeg = new byte[data.remaining()];
            data.get(jpeg);
            File photo = new File(getFilesDir(), "camera-check.jpg");
            try (FileOutputStream file = new FileOutputStream(photo)) {
                file.write(jpeg);
            }
            Bitmap decoded = BitmapFactory.decodeFile(photo.getPath());
            if (decoded == null) {
                report("Saved camera image failed JPEG readback");
            } else {
                report("Photo saved and read back: " + decoded.getWidth() + "x" + decoded.getHeight());
                decoded.recycle();
            }
            capture.setEnabled(true);
        } catch (IOException | IllegalStateException error) {
            fail("Photo readback failed: " + error.getMessage());
        }
    }

    private void report(String message) {
        status.setText(message);
        Log.i(TAG, message);
    }

    private void fail(String message) {
        closeCamera();
        report(message);
    }

    private void closeCamera() {
        ++generation;
        opening = false;
        sawFrame = false;
        capture.setEnabled(false);
        if (session != null) session.close();
        if (camera != null) camera.close();
        if (jpegReader != null) jpegReader.close();
        if (previewSurface != null) previewSurface.release();
        session = null;
        camera = null;
        jpegReader = null;
        previewSurface = null;
    }

    @Override
    public void onSurfaceTextureAvailable(SurfaceTexture surface, int width, int height) {
        openCamera();
    }

    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture surface, int width, int height) {}

    @Override
    public boolean onSurfaceTextureDestroyed(SurfaceTexture surface) {
        closeCamera();
        return true;
    }

    @Override
    public void onSurfaceTextureUpdated(SurfaceTexture surface) {
        if (session != null && !sawFrame) {
            sawFrame = true;
            report("Rear camera preview received");
            capture.setEnabled(true);
        }
    }
}
