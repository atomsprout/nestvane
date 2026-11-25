package dev.nestvane.media;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.AudioRecord;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.MediaRecorder;
import android.media.MediaRecorder.AudioSource;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;

import java.io.File;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.ByteBuffer;

/**
 * Records and says in the log how it went, for the screen recorder's and the sound recorder's ways
 * of recording. Started with the extra "what": "surface" records drawn frames through a recorder's
 * surface, as the screen recorder does; "buffers" hands the encoder frames in memory; "sound" records
 * three seconds from the microphone and says at what rate the samples came and how loud they were.
 * "loop" plays a 1 kHz tone through the speaker while it records, and says the pitch and the loudness
 * of each channel it recorded: a pitch other than 1 kHz is a rate that does not match. "store" saves the surface recording into Movies the way the screen recorder does (a pending item of
 * the media library, then a copy); "store-recorder" records straight into such an item, as a camera does.
 */
public final class RecordActivity extends Activity {
    private static final String TAG = "NestvaneMedia";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        final String what = getIntent().getStringExtra("what");
        new Thread(() -> {
            try {
                if ("surface".equals(what)) surface();
                else if ("buffers".equals(what)) buffers();
                else if ("sound".equals(what)) sound();
                else if ("loop".equals(what)) loop();
                else if ("net".equals(what)) net();
                else if ("store".equals(what)) store(false);
                else if ("store-recorder".equals(what)) store(true);
                else Log.i(TAG, "record: no such test " + what);
            } catch (Throwable error) {
                Log.w(TAG, "record " + what + " failed", error);
            }
            runOnUiThread(this::finish);
        }, "record").start();
    }

    private void surface() throws Exception {
        File file = new File(getExternalFilesDir(null), "surface.mp4");
        MediaRecorder recorder = new MediaRecorder(this);
        recorder.setOnErrorListener((r, what, extra) -> Log.w(TAG, "record surface: error " + what + " " + extra));
        recorder.setOnInfoListener((r, what, extra) -> Log.i(TAG, "record surface: info " + what + " " + extra));
        recorder.setVideoSource(MediaRecorder.VideoSource.SURFACE);
        recorder.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
        recorder.setVideoEncoder(MediaRecorder.VideoEncoder.H264);
        recorder.setVideoSize(720, 1280);
        recorder.setVideoFrameRate(30);
        recorder.setVideoEncodingBitRate(6_000_000);
        recorder.setOutputFile(file);
        recorder.prepare();
        Surface input = recorder.getSurface();
        recorder.start();
        Paint paint = new Paint();
        paint.setColor(Color.WHITE);
        paint.setTextSize(120);
        long began = System.nanoTime();
        for (int frame = 0; frame < 90; ++frame) {
            Canvas canvas = getIntent().getBooleanExtra("software", false) ? input.lockCanvas(null) : input.lockHardwareCanvas();
            canvas.drawColor(Color.rgb(frame * 2, 40, 255 - frame * 2));
            canvas.drawText("frame " + frame, 80, 640, paint);
            input.unlockCanvasAndPost(canvas);
            Thread.sleep(33);
        }
        recorder.stop();
        recorder.release();
        Log.i(TAG, "record surface: " + file.length() + " bytes in " + (System.nanoTime() - began) / 1_000_000 + " ms");
    }

    private void store(boolean direct) throws Exception {
        ContentResolver resolver = getContentResolver();
        ContentValues values = new ContentValues();
        values.put(MediaStore.Video.Media.DISPLAY_NAME, "nestvane-" + (direct ? "direct" : "copy") + ".mp4");
        values.put(MediaStore.Video.Media.MIME_TYPE, "video/mp4");
        values.put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/");
        values.put(MediaStore.Video.Media.IS_PENDING, 1);
        Uri item = resolver.insert(MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), values);
        Log.i(TAG, "record store: item " + item);
        File temporary = new File(getExternalFilesDir(null), "surface.mp4");
        if (direct) {
            try (ParcelFileDescriptor file = resolver.openFileDescriptor(item, "rw")) {
                MediaRecorder recorder = new MediaRecorder(this);
                recorder.setVideoSource(MediaRecorder.VideoSource.SURFACE);
                recorder.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
                recorder.setVideoEncoder(MediaRecorder.VideoEncoder.H264);
                recorder.setVideoSize(720, 1280);
                recorder.setVideoFrameRate(30);
                recorder.setOutputFile(file.getFileDescriptor());
                recorder.prepare();
                Surface input = recorder.getSurface();
                recorder.start();
                for (int frame = 0; frame < 60; ++frame) {
                    Canvas canvas = input.lockHardwareCanvas();
                    canvas.drawColor(Color.rgb(frame * 4, 80, 255 - frame * 4));
                    input.unlockCanvasAndPost(canvas);
                    Thread.sleep(33);
                }
                recorder.stop();
                recorder.release();
            }
        } else {
            try (OutputStream out = resolver.openOutputStream(item, "w")) {
                Files.copy(temporary.toPath(), out);
            }
            Log.i(TAG, "record store: copied " + temporary.length() + " bytes");
        }
        values.clear();
        values.put(MediaStore.Video.Media.IS_PENDING, 0);
        resolver.update(item, values, null, null);
        try (Cursor cursor = resolver.query(item, new String[] {MediaStore.Video.Media.SIZE, MediaStore.Video.Media.DATA}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst())
                Log.i(TAG, "record store: the library has " + cursor.getLong(0) + " bytes at " + cursor.getString(1)
                        + ", the file " + new File(cursor.getString(1)).length());
        }
    }

    private void buffers() throws Exception {
        MediaFormat format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, 720, 1280);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        format.setInteger(MediaFormat.KEY_BIT_RATE, 4_000_000);
        format.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
        MediaCodec codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        Log.i(TAG, "record buffers: encoder " + codec.getName());
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        codec.start();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long bytes = 0;
        int sent = 0, got = 0;
        while (got < 30) {
            int in = sent < 30 ? codec.dequeueInputBuffer(10_000) : -1;
            if (in >= 0) {
                ByteBuffer buffer = codec.getInputBuffer(in);
                int size = 720 * 1280 * 3 / 2;
                for (int i = 0; i < size && buffer.hasRemaining(); ++i) buffer.put((byte) (i + sent));
                codec.queueInputBuffer(in, 0, size, sent * 33_333L, sent == 29 ? MediaCodec.BUFFER_FLAG_END_OF_STREAM : 0);
                ++sent;
            }
            int out = codec.dequeueOutputBuffer(info, 10_000);
            if (out >= 0) {
                bytes += info.size;
                ++got;
                codec.releaseOutputBuffer(out, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
            }
        }
        codec.stop();
        codec.release();
        Log.i(TAG, "record buffers: " + got + " outputs, " + bytes + " bytes");
    }

    private void net() throws Exception {
        String name = getIntent().getStringExtra("host");
        if (name == null) name = "www.google.com";
        StringBuilder text = new StringBuilder("record net: " + name + " is");
        InetAddress six = null;
        for (InetAddress address : InetAddress.getAllByName(name)) {
            text.append(' ').append(address.getHostAddress());
            if (six == null && address instanceof Inet6Address) six = address;
        }
        Log.i(TAG, text.toString());
        if (six == null) six = InetAddress.getByName("2606:4700:4700::1111");
        try (Socket socket = new Socket()) {
            long began = System.nanoTime();
            socket.connect(new InetSocketAddress(six, 443), 8000);
            Log.i(TAG, "record net: connected to " + six.getHostAddress() + " in " + (System.nanoTime() - began) / 1_000_000
                    + " ms, from " + socket.getLocalAddress().getHostAddress());
        } catch (Exception error) {
            Log.i(TAG, "record net: " + six.getHostAddress() + ": " + error);
        }
    }

    private void loop() throws Exception {
        int rate = getIntent().getIntExtra("rate", 48000);
        int channels = getIntent().getIntExtra("channels", 1);
        final int playRate = 48000;
        final short[] tone = new short[playRate * 4];
        for (int i = 0; i < tone.length; ++i) tone[i] = (short) (Math.sin(2 * Math.PI * 1000 * i / playRate) * 9000);
        AudioTrack track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(playRate).setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setTransferMode(AudioTrack.MODE_STREAM).build();
        track.play();
        Thread player = new Thread(() -> track.write(tone, 0, tone.length), "tone");
        player.start();
        Thread.sleep(700);
        int mask = channels == 2 ? AudioFormat.CHANNEL_IN_STEREO : AudioFormat.CHANNEL_IN_MONO;
        AudioRecord record = new AudioRecord(AudioSource.MIC, rate, mask, AudioFormat.ENCODING_PCM_16BIT, rate * channels * 2);
        short[] samples = new short[rate * channels * 2];
        record.startRecording();
        int got = 0;
        while (got < samples.length) {
            int read = record.read(samples, got, samples.length - got);
            if (read <= 0) break;
            got += read;
        }
        record.stop();
        record.release();
        player.join();
        track.stop();
        track.release();
        StringBuilder text = new StringBuilder("record loop: " + rate + " Hz x" + channels + ", " + got / channels + " frames");
        for (int channel = 0; channel < channels; ++channel) {
            double sum = 0;
            int crossings = 0, frames = 0;
            short before = 0;
            // The second half: the first is the recording starting.
            for (int i = got / 2 / channels * channels + channel; i < got; i += channels) {
                short sample = samples[i];
                sum += (double) sample * sample;
                if (before < 0 && sample >= 0) ++crossings;
                before = sample;
                ++frames;
            }
            text.append("; channel ").append(channel).append(": rms ").append(Math.round(Math.sqrt(sum / Math.max(1, frames))))
                .append(", pitch ").append(Math.round(crossings * (double) rate / Math.max(1, frames))).append(" Hz");
        }
        Log.i(TAG, text.toString());
    }

    private void sound() throws Exception {
        int rate = getIntent().getIntExtra("rate", 48000);
        int channels = getIntent().getIntExtra("channels", 1);
        int mask = channels == 2 ? AudioFormat.CHANNEL_IN_STEREO : AudioFormat.CHANNEL_IN_MONO;
        int minimum = AudioRecord.getMinBufferSize(rate, mask, AudioFormat.ENCODING_PCM_16BIT);
        AudioRecord record = new AudioRecord(AudioSource.MIC, rate, mask, AudioFormat.ENCODING_PCM_16BIT, Math.max(minimum, rate));
        record.startRecording();
        short[] samples = new short[rate * channels / 10];
        long total = 0, began = System.nanoTime();
        double sum = 0;
        int peak = 0;
        while (System.nanoTime() - began < 3_000_000_000L) {
            int read = record.read(samples, 0, samples.length);
            if (read < 0) {
                Log.w(TAG, "record sound: read " + read);
                break;
            }
            for (int i = 0; i < read; ++i) {
                sum += (double) samples[i] * samples[i];
                peak = Math.max(peak, Math.abs(samples[i]));
            }
            total += read;
        }
        double seconds = (System.nanoTime() - began) / 1e9;
        record.stop();
        Log.i(TAG, "record sound: asked " + rate + " Hz x" + channels + ", got " + Math.round(total / channels / seconds)
                + " frames a second, rms " + Math.round(Math.sqrt(sum / Math.max(1, total))) + ", peak " + peak
                + ", the record says " + record.getSampleRate() + " Hz x" + record.getChannelCount());
        record.release();
    }
}
