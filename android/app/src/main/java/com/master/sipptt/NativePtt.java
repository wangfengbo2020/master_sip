package com.master.sipptt;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public final class NativePtt {
    static {
        System.loadLibrary("ptt_native");
    }

    private NativePtt() {
    }

    public static native boolean nativeStart(String server,
                                             String deviceId,
                                             String groupId,
                                             boolean selfLoopback);

    public static native boolean nativeRequestTalk();

    public static native boolean nativeReleaseTalk();

    public static native void nativeFlushTxDiagnostics(String reason);

    public static native void nativeSetNetworkType(String networkType);

    public static native boolean nativePushCapturePcm(short[] pcm,
                                                       int samples,
                                                       long captureFrameId,
                                                       long captureTimeMs);

    public static native int nativeReadPlaybackPcm(short[] pcm, int maxSamples);

    private static native String nativeGetStatus();

    private static native String nativeDrainReceivePackets();

    private static native String nativeDrainSendPackets();

    public static native void nativeShutdown();

    public static Status getStatus() {
        return Status.parse(nativeGetStatus());
    }

    public static List<ReceivePacket> drainReceivePackets() {
        List<ReceivePacket> packets = new ArrayList<>();
        String raw = nativeDrainReceivePackets();
        if (raw == null || raw.isEmpty()) {
            return packets;
        }
        String[] lines = raw.split("\n");
        for (String line : lines) {
            String[] values = line.split("\t", -1);
            if (values.length < 14) {
                continue;
            }
            try {
                packets.add(new ReceivePacket(
                        Integer.parseInt(values[0]),
                        Long.parseLong(values[1]),
                        Long.parseLong(values[2]),
                        Long.parseLong(values[3]),
                        Long.parseLong(values[4]),
                        Long.parseLong(values[5]),
                        Long.parseLong(values[6]),
                        Long.parseLong(values[7]),
                        Integer.parseInt(values[8]),
                        "1".equals(values[9]),
                        "1".equals(values[10]),
                        "1".equals(values[11]),
                        "1".equals(values[12]),
                        values[13]));
            } catch (NumberFormatException ignored) {
                // Ignore one malformed diagnostic record without affecting audio.
            }
        }
        return packets;
    }

    public static List<SendPacket> drainSendPackets() {
        List<SendPacket> packets = new ArrayList<>();
        String raw = nativeDrainSendPackets();
        if (raw == null || raw.isEmpty()) {
            return packets;
        }
        String[] lines = raw.split("\n");
        for (String line : lines) {
            String[] values = line.split("\t", -1);
            if (values.length < 9) {
                continue;
            }
            try {
                packets.add(new SendPacket(
                        Integer.parseInt(values[0]),
                        Long.parseLong(values[1]),
                        Long.parseLong(values[2]),
                        Long.parseLong(values[3]),
                        Long.parseLong(values[4]),
                        Integer.parseInt(values[5]),
                        "1".equals(values[6]),
                        "1".equals(values[7]),
                        Integer.parseInt(values[8])));
            } catch (NumberFormatException ignored) {
                // Ignore one malformed diagnostic record without affecting audio.
            }
        }
        return packets;
    }

    public static final class SendPacket {
        public final int sequence;
        public final long rtpTimestamp;
        public final long sourceFrameId;
        public final long captureTimeMs;
        public final long sendTimeMs;
        public final int payloadLength;
        public final boolean marker;
        public final boolean succeeded;
        public final int errorCode;

        private SendPacket(int sequence,
                           long rtpTimestamp,
                           long sourceFrameId,
                           long captureTimeMs,
                           long sendTimeMs,
                           int payloadLength,
                           boolean marker,
                           boolean succeeded,
                           int errorCode) {
            this.sequence = sequence;
            this.rtpTimestamp = rtpTimestamp;
            this.sourceFrameId = sourceFrameId;
            this.captureTimeMs = captureTimeMs;
            this.sendTimeMs = sendTimeMs;
            this.payloadLength = payloadLength;
            this.marker = marker;
            this.succeeded = succeeded;
            this.errorCode = errorCode;
        }
    }

    public static final class ReceivePacket {
        public final int sequence;
        public final long sourceFrameId;
        public final long captureTimeMs;
        public final long sendTimeMs;
        public final long arrivalTimeMs;
        public final long roundTripMs;
        public final long arrivalGapMs;
        public final long captureGapMs;
        public final int payloadLength;
        public final boolean continuous;
        public final boolean sourceContinuous;
        public final boolean hasSourceFrameId;
        public final boolean hasSendTime;
        public final String status;

        private ReceivePacket(int sequence,
                              long sourceFrameId,
                              long captureTimeMs,
                              long sendTimeMs,
                              long arrivalTimeMs,
                              long roundTripMs,
                              long arrivalGapMs,
                              long captureGapMs,
                              int payloadLength,
                              boolean continuous,
                              boolean sourceContinuous,
                              boolean hasSourceFrameId,
                              boolean hasSendTime,
                              String status) {
            this.sequence = sequence;
            this.sourceFrameId = sourceFrameId;
            this.captureTimeMs = captureTimeMs;
            this.sendTimeMs = sendTimeMs;
            this.arrivalTimeMs = arrivalTimeMs;
            this.roundTripMs = roundTripMs;
            this.arrivalGapMs = arrivalGapMs;
            this.captureGapMs = captureGapMs;
            this.payloadLength = payloadLength;
            this.continuous = continuous;
            this.sourceContinuous = sourceContinuous;
            this.hasSourceFrameId = hasSourceFrameId;
            this.hasSendTime = hasSendTime;
            this.status = status;
        }
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
