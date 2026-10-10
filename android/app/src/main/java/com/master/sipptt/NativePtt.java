package com.master.sipptt;

import java.util.Locale;

public final class NativePtt {
    static {
        System.loadLibrary("ptt_native");
    }

    private NativePtt() {
    }

    public static native boolean nativeStart(String server, String deviceId, String groupId);

    public static native boolean nativeRequestTalk();

    public static native boolean nativeReleaseTalk();

    public static native boolean nativePushCapturePcm(short[] pcm, int samples);

    public static native int nativeReadPlaybackPcm(short[] pcm, int maxSamples);

    private static native String nativeGetStatus();

    public static native void nativeShutdown();

    public static Status getStatus() {
        return Status.parse(nativeGetStatus());
    }

    public static final class Status {
        public String mode = "STOPPED";
        public boolean registered;
        public boolean joined;
        public boolean floorGranted;
        public int localSipPort;
        public int localRtpPort;
        public long transmittedPackets;
        public long receivedPackets;
        public long receivePacketsPerSecond;
        public double latencyAverageMs;
        public long latencyMinMs;
        public long latencyMaxMs;
        public String detail = "尚未加入组队";
        public String speaker = "";

        private static Status parse(String raw) {
            Status status = new Status();
            if (raw == null) {
                status.mode = "ERROR";
                status.detail = "无法读取C核心状态";
                return status;
            }
            String[] values = raw.split("\\t", -1);
            if (values.length < 14) {
                status.mode = "ERROR";
                status.detail = "C核心状态格式错误";
                return status;
            }
            try {
                status.mode = values[0];
                status.registered = "1".equals(values[1]);
                status.joined = "1".equals(values[2]);
                status.floorGranted = "1".equals(values[3]);
                status.localSipPort = Integer.parseInt(values[4]);
                status.localRtpPort = Integer.parseInt(values[5]);
                status.transmittedPackets = Long.parseLong(values[6]);
                status.receivedPackets = Long.parseLong(values[7]);
                status.receivePacketsPerSecond = Long.parseLong(values[8]);
                status.latencyAverageMs = Double.parseDouble(values[9]);
                status.latencyMinMs = Long.parseLong(values[10]);
                status.latencyMaxMs = Long.parseLong(values[11]);
                status.detail = values[12];
                status.speaker = values[13];
            } catch (NumberFormatException exception) {
                status.mode = "ERROR";
                status.detail = "C核心状态数值错误";
            }
            return status;
        }

        public String latencyText() {
            if (receivedPackets == 0 || receivePacketsPerSecond == 0) {
                return "--";
            }
            return String.format(Locale.US,
                    "%.1f ms（%d~%d）",
                    latencyAverageMs,
                    latencyMinMs,
                    latencyMaxMs);
        }
    }
}
