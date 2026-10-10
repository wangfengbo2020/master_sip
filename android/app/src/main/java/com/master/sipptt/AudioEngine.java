package com.master.sipptt;

import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import java.util.Locale;

final class AudioEngine {
    private static final String TAG = "SipPttAudio";
    private static final int CAPTURE_RATE = 48000;
    private static final int PLAYBACK_RATE = 8000;
    private static final int CAPTURE_SAMPLES = 960;
    private static final int PLAYBACK_SAMPLES = 160;
    private static final long PLAYBACK_IDLE_CLOSE_MS = 500L;

    private volatile boolean running;
    private volatile boolean talking;
    private Thread captureThread;
    private Thread playbackThread;

    synchronized void start() {
        if (running) {
            return;
        }
        running = true;
        captureThread = new Thread(this::captureLoop, "ptt-capture");
        playbackThread = new Thread(this::playbackLoop, "ptt-playback");
        captureThread.start();
        playbackThread.start();
    }

    void setTalking(boolean enabled) {
        talking = enabled;
    }

    synchronized void stop() {
        if (!running) {
            return;
        }
        running = false;
        talking = false;
        joinQuietly(captureThread);
        joinQuietly(playbackThread);
        captureThread = null;
        playbackThread = null;
    }

    private void captureLoop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
        short[] frame = new short[CAPTURE_SAMPLES];

        while (running) {
            if (!talking) {
                sleepQuietly(20L);
                continue;
            }
            AudioRecord recorder = createRecorder();
            if (recorder == null) {
                sleepQuietly(250L);
                continue;
            }
            try {
                recorder.startRecording();
                Log.i(TAG, String.format(Locale.US,
                        "capture started rate=%dHz frame_samples=%d frame_ms=%.1f session=%d buffer_frames=%d",
                        recorder.getSampleRate(),
                        CAPTURE_SAMPLES,
                        CAPTURE_SAMPLES * 1000.0 / CAPTURE_RATE,
                        recorder.getAudioSessionId(),
                        recorder.getBufferSizeInFrames()));
                long statsStartedAt = SystemClock.elapsedRealtime();
                long statsFrames = 0L;
                long statsSamples = 0L;
                long statsReadCalls = 0L;
                long statsShortReads = 0L;
                long statsReadErrors = 0L;
                long statsNativeAccepted = 0L;
                long statsNativeRejected = 0L;
                long statsAbsoluteSum = 0L;
                int statsPeak = 0;
                while (running && talking) {
                    int filled = 0;
                    while (running && talking && filled < frame.length) {
                        int requested = frame.length - filled;
                        int read = recorder.read(frame,
                                filled,
                                requested,
                                AudioRecord.READ_BLOCKING);
                        statsReadCalls++;
                        if (read <= 0) {
                            statsReadErrors++;
                            Log.w(TAG, "AudioRecord.read failed code=" + read +
                                    " recording_state=" + recorder.getRecordingState());
                            break;
                        }
                        if (read < requested) {
                            statsShortReads++;
                        }
                        filled += read;
                    }
                    if (filled == frame.length) {
                        for (short sample : frame) {
                            int value = sample;
                            int absolute = value < 0 ? -value : value;
                            statsAbsoluteSum += absolute;
                            if (absolute > statsPeak) {
                                statsPeak = absolute;
                            }
                        }
                        statsFrames++;
                        statsSamples += frame.length;
                        if (NativePtt.nativePushCapturePcm(frame, frame.length)) {
                            statsNativeAccepted++;
                        } else {
                            statsNativeRejected++;
                        }
                    }

                    long now = SystemClock.elapsedRealtime();
                    if (now - statsStartedAt >= 1000L) {
                        double averageAbsolute = statsSamples > 0
                                ? (double) statsAbsoluteSum / statsSamples
                                : 0.0;
                        Log.i(TAG, String.format(Locale.US,
                                "[capture-tx/1s] frames=%d samples=%d read_calls=%d " +
                                        "short_reads=%d read_errors=%d pcm_level avg_abs=%.1f peak=%d " +
                                        "native_accepted=%d native_rejected=%d",
                                statsFrames,
                                statsSamples,
                                statsReadCalls,
                                statsShortReads,
                                statsReadErrors,
                                averageAbsolute,
                                statsPeak,
                                statsNativeAccepted,
                                statsNativeRejected));
                        statsStartedAt = now;
                        statsFrames = 0L;
                        statsSamples = 0L;
                        statsReadCalls = 0L;
                        statsShortReads = 0L;
                        statsReadErrors = 0L;
                        statsNativeAccepted = 0L;
                        statsNativeRejected = 0L;
                        statsAbsoluteSum = 0L;
                        statsPeak = 0;
                    }
                }
            } catch (IllegalStateException exception) {
                Log.e(TAG, "capture state failure", exception);
            } finally {
                try {
                    recorder.stop();
                } catch (IllegalStateException ignored) {
                }
                recorder.release();
                Log.i(TAG, "capture stopped");
            }
        }
    }

    private void playbackLoop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
        short[] pcm = new short[PLAYBACK_SAMPLES];
        AudioTrack track = null;
        long lastAudioAt = 0L;

        while (running) {
            int samples = NativePtt.nativeReadPlaybackPcm(pcm, pcm.length);
            if (samples > 0) {
                if (track == null) {
                    track = createTrack();
                    if (track == null) {
                        continue;
                    }
                    track.play();
                }
                track.write(pcm, 0, samples, AudioTrack.WRITE_BLOCKING);
                lastAudioAt = android.os.SystemClock.elapsedRealtime();
            } else {
                if (track != null &&
                        android.os.SystemClock.elapsedRealtime() - lastAudioAt >=
                                PLAYBACK_IDLE_CLOSE_MS) {
                    releaseTrack(track);
                    track = null;
                }
                sleepQuietly(5L);
            }
        }
        if (track != null) {
            releaseTrack(track);
        }
    }

    private AudioRecord createRecorder() {
        int minimum = AudioRecord.getMinBufferSize(CAPTURE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        int bufferBytes = Math.max(minimum, CAPTURE_SAMPLES * 2 * 4);
        try {
            AudioRecord recorder = new AudioRecord(MediaRecorder.AudioSource.MIC,
                    CAPTURE_RATE,
                    AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT,
                    bufferBytes);
            if (recorder.getState() != AudioRecord.STATE_INITIALIZED) {
                Log.e(TAG, "AudioRecord is not initialized, min_buffer=" + minimum +
                        " requested_buffer=" + bufferBytes);
                recorder.release();
                return null;
            }
            return recorder;
        } catch (IllegalArgumentException | SecurityException exception) {
            Log.e(TAG, "cannot create AudioRecord", exception);
            return null;
        }
    }

    private AudioTrack createTrack() {
        int minimum = AudioTrack.getMinBufferSize(PLAYBACK_RATE,
                AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        int bufferBytes = Math.max(minimum, PLAYBACK_SAMPLES * 2 * 4);
        try {
            AudioTrack track = new AudioTrack(AudioManager.STREAM_MUSIC,
                    PLAYBACK_RATE,
                    AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_16BIT,
                    bufferBytes,
                    AudioTrack.MODE_STREAM);
            if (track.getState() != AudioTrack.STATE_INITIALIZED) {
                track.release();
                return null;
            }
            return track;
        } catch (IllegalArgumentException exception) {
            return null;
        }
    }

    private void releaseTrack(AudioTrack track) {
        try {
            track.stop();
        } catch (IllegalStateException ignored) {
        }
        track.flush();
        track.release();
    }

    private void joinQuietly(Thread thread) {
        if (thread == null) {
            return;
        }
        try {
            thread.join(1500L);
        } catch (InterruptedException exception) {
            Thread.currentThread().interrupt();
        }
    }

    private void sleepQuietly(long milliseconds) {
        try {
            Thread.sleep(milliseconds);
        } catch (InterruptedException exception) {
            Thread.currentThread().interrupt();
        }
    }
}
