package com.master.sipptt;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.Settings;
import android.text.Spannable;
import android.text.SpannableStringBuilder;
import android.text.InputType;
import android.text.style.ForegroundColorSpan;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.Locale;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;

public final class MainActivity extends Activity {
    private static final String TEST_LOG_TAG = "SipPttTest";
    private static final String SERVER_HOST = "123.57.79.87";
    private static final long CONTROL_INTERVAL_MS = 100L;
    private static final long DIAGNOSTIC_INTERVAL_MS = 1000L;
    private static final int REQUEST_RECORD_AUDIO = 1001;
    private static final int MAX_PACKET_LOG_LINES = 200;
    private static final int PACKET_ERROR_COLOR = Color.rgb(211, 47, 47);
    private static final int PACKET_WARNING_COLOR = Color.rgb(230, 126, 34);
    private static final int PACKET_NORMAL_COLOR = Color.rgb(52, 65, 82);
    private static final long PACKET_GAP_WARNING_MS = 60L;
    private static final long LOOPBACK_RTT_WARNING_MS = 500L;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final AudioEngine audioEngine = new AudioEngine();
    private final SpannableStringBuilder packetLogBuffer = new SpannableStringBuilder();
    private final SimpleDateFormat packetTimeFormat =
            new SimpleDateFormat("HH:mm:ss.SSS", Locale.US);
    private final Runnable statusTask = new Runnable() {
        @Override
        public void run() {
            refreshStatus();
            handler.postDelayed(this, CONTROL_INTERVAL_MS);
        }
    };

    private EditText groupInput;
    private CheckBox loopbackCheckBox;
    private Button joinButton;
    private Button talkButton;
    private TextView statusText;
    private TextView statusBadge;
    private TextView endpointText;
    private TextView statisticsText;
    private TextView receivePacketLogText;
    private ScrollView receivePacketLogScroll;
    private WaveView waveView;
    private int packetLogLineCount;
    private boolean nativeStarted;
    private boolean joined;
    private boolean loopbackEnabled;
    private String currentMode = "STOPPED";
    private long lastDiagnosticRefreshAt;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(Color.rgb(9, 18, 36));
        getWindow().setNavigationBarColor(Color.rgb(9, 18, 36));
        setContentView(createContentView());
        handler.post(statusTask);
    }

    private View createContentView() {
        ScrollView scrollView = new ScrollView(this);
        scrollView.setFillViewport(true);
        scrollView.setBackgroundColor(Color.rgb(245, 248, 252));

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(24), dp(30), dp(24), dp(30));
        scrollView.addView(root, matchWrap());

        TextView title = text("4G 对讲 Demo", 28, Color.rgb(17, 32, 55));
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title, matchWrap());

        TextView subtitle = text("SIP 抢麦 · RTP/Opus真实语音 · Android/PC 共用C核心", 14,
                Color.rgb(88, 103, 125));
        LinearLayout.LayoutParams subtitleParams = matchWrap();
        subtitleParams.topMargin = dp(6);
        root.addView(subtitle, subtitleParams);

        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(20), dp(20), dp(20), dp(20));
        card.setBackground(rounded(Color.WHITE, 18, Color.rgb(224, 231, 240), 1));
        LinearLayout.LayoutParams cardParams = matchWrap();
        cardParams.topMargin = dp(26);
        root.addView(card, cardParams);

        TextView groupLabel = text("组队名称", 13, Color.rgb(80, 94, 114));
        groupLabel.setTypeface(Typeface.DEFAULT_BOLD);
        card.addView(groupLabel, matchWrap());

        groupInput = new EditText(this);
        groupInput.setSingleLine(true);
        groupInput.setText("group_01");
        groupInput.setHint("例如 group_01");
        groupInput.setTextSize(17);
        groupInput.setTextColor(Color.rgb(21, 38, 62));
        groupInput.setHintTextColor(Color.rgb(151, 164, 181));
        groupInput.setInputType(InputType.TYPE_CLASS_TEXT);
        groupInput.setPadding(dp(14), 0, dp(14), 0);
        groupInput.setBackground(rounded(Color.rgb(247, 249, 252), 10,
                Color.rgb(210, 219, 231), 1));
        LinearLayout.LayoutParams inputParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(52));
        inputParams.topMargin = dp(8);
        card.addView(groupInput, inputParams);

        loopbackCheckBox = new CheckBox(this);
        loopbackCheckBox.setText("ECS回环延时测试（自发自收）");
        loopbackCheckBox.setTextSize(14);
        loopbackCheckBox.setTextColor(Color.rgb(52, 65, 82));
        loopbackCheckBox.setButtonTintList(android.content.res.ColorStateList.valueOf(
                Color.rgb(31, 111, 235)));
        LinearLayout.LayoutParams loopbackParams = matchWrap();
        loopbackParams.topMargin = dp(10);
        card.addView(loopbackCheckBox, loopbackParams);

        TextView loopbackHint = text(
                "开启后，ECS会把本人发送的原始RTP包回送，用手机本地时钟统计真实往返延时。",
                12,
                Color.rgb(112, 126, 145));
        LinearLayout.LayoutParams loopbackHintParams = matchWrap();
        loopbackHintParams.topMargin = dp(2);
        card.addView(loopbackHint, loopbackHintParams);

        joinButton = new Button(this);
        joinButton.setText("加入组队");
        joinButton.setTextSize(16);
        joinButton.setAllCaps(false);
        joinButton.setTextColor(Color.WHITE);
        joinButton.setTypeface(Typeface.DEFAULT_BOLD);
        joinButton.setBackground(rounded(Color.rgb(31, 111, 235), 12, Color.TRANSPARENT, 0));
        joinButton.setOnClickListener(view -> onJoinClicked());
        LinearLayout.LayoutParams joinParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(52));
        joinParams.topMargin = dp(14);
        card.addView(joinButton, joinParams);

        LinearLayout statusRow = new LinearLayout(this);
        statusRow.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams statusRowParams = matchWrap();
        statusRowParams.topMargin = dp(22);
        card.addView(statusRow, statusRowParams);

        statusBadge = text("未连接", 12, Color.WHITE);
        statusBadge.setTypeface(Typeface.DEFAULT_BOLD);
        statusBadge.setGravity(Gravity.CENTER);
        statusBadge.setPadding(dp(10), dp(5), dp(10), dp(5));
        statusBadge.setBackground(rounded(Color.rgb(117, 130, 150), 20,
                Color.TRANSPARENT, 0));
        statusRow.addView(statusBadge, wrapWrap());

        statusText = text("请输入组名后加入", 16, Color.rgb(25, 44, 70));
        LinearLayout.LayoutParams statusTextParams = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        statusTextParams.leftMargin = dp(12);
        statusRow.addView(statusText, statusTextParams);

        waveView = new WaveView(this);
        waveView.setVisibility(View.GONE);
        LinearLayout.LayoutParams waveParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(90));
        waveParams.topMargin = dp(12);
        card.addView(waveView, waveParams);

        talkButton = new Button(this);
        talkButton.setText("开始说话");
        talkButton.setTextSize(21);
        talkButton.setAllCaps(false);
        talkButton.setTypeface(Typeface.DEFAULT_BOLD);
        talkButton.setTextColor(Color.WHITE);
        talkButton.setEnabled(false);
        talkButton.setAlpha(0.45f);
        talkButton.setBackground(rounded(Color.rgb(15, 163, 113), 34,
                Color.TRANSPARENT, 0));
        talkButton.setOnClickListener(view -> onTalkClicked());
        LinearLayout.LayoutParams talkParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(68));
        talkParams.topMargin = dp(18);
        card.addView(talkButton, talkParams);

        endpointText = text("Android v" + BuildConfig.VERSION_NAME + " · 服务器 " +
                        SERVER_HOST + " · SIP 5060 · RTP 40000", 13,
                Color.rgb(91, 105, 124));
        LinearLayout.LayoutParams endpointParams = matchWrap();
        endpointParams.topMargin = dp(18);
        root.addView(endpointText, endpointParams);

        statisticsText = text("TX 0 包  ·  RX 0 包  ·  接收速率 0 包/秒  ·  延时 --", 13,
                Color.rgb(91, 105, 124));
        LinearLayout.LayoutParams statisticsParams = matchWrap();
        statisticsParams.topMargin = dp(7);
        root.addView(statisticsText, statisticsParams);

        LinearLayout packetLogCard = new LinearLayout(this);
        packetLogCard.setOrientation(LinearLayout.VERTICAL);
        packetLogCard.setPadding(dp(14), dp(12), dp(14), dp(12));
        packetLogCard.setBackground(rounded(Color.WHITE, 12,
                Color.rgb(210, 219, 231), 1));
        LinearLayout.LayoutParams packetLogCardParams = matchWrap();
        packetLogCardParams.topMargin = dp(18);
        root.addView(packetLogCard, packetLogCardParams);

        LinearLayout packetLogHeader = new LinearLayout(this);
        packetLogHeader.setGravity(Gravity.CENTER_VERTICAL);
        packetLogCard.addView(packetLogHeader, matchWrap());

        TextView packetLogTitle = text("收发包日志（断号红色，间隔>60ms橙色）", 13,
                Color.rgb(25, 44, 70));
        packetLogTitle.setTypeface(Typeface.DEFAULT_BOLD);
        packetLogHeader.addView(packetLogTitle,
                new LinearLayout.LayoutParams(0,
                        LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        Button clearPacketLogButton = new Button(this);
        clearPacketLogButton.setText("清空");
        clearPacketLogButton.setTextSize(12);
        clearPacketLogButton.setAllCaps(false);
        clearPacketLogButton.setOnClickListener(view -> clearReceivePacketLog());
        packetLogHeader.addView(clearPacketLogButton,
                new LinearLayout.LayoutParams(dp(72), dp(40)));

        receivePacketLogText = text("等待发送或接收音频包…", 12,
                Color.rgb(52, 65, 82));
        receivePacketLogText.setTypeface(Typeface.MONOSPACE);
        receivePacketLogText.setPadding(dp(8), dp(8), dp(8), dp(8));
        receivePacketLogText.setTextIsSelectable(true);

        receivePacketLogScroll = new ScrollView(this);
        receivePacketLogScroll.setFillViewport(true);
        receivePacketLogScroll.setNestedScrollingEnabled(true);
        receivePacketLogScroll.setBackgroundColor(Color.rgb(247, 249, 252));
        receivePacketLogScroll.addView(receivePacketLogText, matchWrap());
        LinearLayout.LayoutParams packetScrollParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(220));
        packetScrollParams.topMargin = dp(8);
        packetLogCard.addView(receivePacketLogScroll, packetScrollParams);

        TextView notice = text("麦克风以48 kHz PCM16单声道采集，共享C核心降采样到8 kHz并进行Opus编解码；空闲时关闭麦克风以降低功耗。",
                12, Color.rgb(48, 103, 82));
        notice.setPadding(dp(14), dp(12), dp(14), dp(12));
        notice.setBackground(rounded(Color.rgb(232, 249, 242), 10,
                Color.rgb(167, 222, 200), 1));
        LinearLayout.LayoutParams noticeParams = matchWrap();
        noticeParams.topMargin = dp(22);
        root.addView(notice, noticeParams);

        return scrollView;
    }

    private void onJoinClicked() {
        if (nativeStarted && joined) {
            audioEngine.stop();
            NativePtt.nativeShutdown();
            nativeStarted = false;
            joined = false;
            applyDisconnectedUi();
            return;
        }

        String group = groupInput.getText().toString().trim();
        if (!group.matches("[A-Za-z0-9_-]{1,48}")) {
            Toast.makeText(this, "组名只能包含字母、数字、下划线和短横线，最多48个字符",
                    Toast.LENGTH_LONG).show();
            return;
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) !=
                PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO},
                    REQUEST_RECORD_AUDIO);
            return;
        }
        String androidId = Settings.Secure.getString(
                getContentResolver(), Settings.Secure.ANDROID_ID);
        if (androidId == null || androidId.isEmpty()) {
            androidId = Long.toHexString(System.currentTimeMillis());
        }
        String deviceId = "android_" + androidId;
        clearReceivePacketLog();
        loopbackEnabled = loopbackCheckBox.isChecked();
        NativePtt.nativeSetNetworkType(currentNetworkType());
        nativeStarted = NativePtt.nativeStart(
                SERVER_HOST, deviceId, group, loopbackEnabled);
        if (!nativeStarted) {
            Toast.makeText(this, "C核心启动失败，请查看Logcat", Toast.LENGTH_LONG).show();
            return;
        }
        audioEngine.start();
        groupInput.setEnabled(false);
        loopbackCheckBox.setEnabled(false);
        joinButton.setEnabled(false);
        joinButton.setText("连接中…");
        statusText.setText("正在连接服务器");
        setBadge("连接中", Color.rgb(31, 111, 235));
    }

    private void onTalkClicked() {
        String networkType = currentNetworkType();
        NativePtt.nativeSetNetworkType(networkType);
        if ("TALKING".equals(currentMode)) {
            audioEngine.setTalking(false);
            Log.i(TEST_LOG_TAG, "[test-network] event=talk-release network=" +
                    networkType);
            if (!NativePtt.nativeReleaseTalk()) {
                audioEngine.setTalking(true);
                Toast.makeText(this, "当前没有讲话权", Toast.LENGTH_SHORT).show();
            } else {
                waveView.setActive(false);
                waveView.setVisibility(View.GONE);
                setBadge("结束中", Color.rgb(31, 111, 235));
                setTalkButton(false, "正在结束…", Color.rgb(31, 111, 235));
            }
        } else {
            Log.i(TEST_LOG_TAG, "[test-network] event=talk-request network=" +
                    networkType + " loopback=" + loopbackEnabled);
            if (!NativePtt.nativeRequestTalk()) {
                Toast.makeText(this, "暂时不能申请讲话", Toast.LENGTH_SHORT).show();
            } else {
                currentMode = "REQUESTING";
                statusText.setText("正在申请讲话权");
                setBadge("申请中", Color.rgb(31, 111, 235));
                setTalkButton(false, "正在申请…", Color.rgb(31, 111, 235));
            }
        }
    }

    private void refreshStatus() {
        long now;
        boolean refreshDiagnostics;

        if (!nativeStarted) {
            return;
        }
        NativePtt.Status status = NativePtt.getStatus();
        now = SystemClock.elapsedRealtime();
        refreshDiagnostics = lastDiagnosticRefreshAt == 0L ||
                now - lastDiagnosticRefreshAt >= DIAGNOSTIC_INTERVAL_MS;
        currentMode = status.mode;
        joined = status.joined;

        statusText.setText(status.detail.isEmpty() ? status.mode : status.detail);
        if (refreshDiagnostics) {
            NativePtt.nativeSetNetworkType(currentNetworkType());
            List<NativePtt.SendPacket> sendPackets = NativePtt.drainSendPackets();
            List<NativePtt.ReceivePacket> receivePackets =
                    NativePtt.drainReceivePackets();
            appendSendPacketLogs(sendPackets);
            appendReceivePacketLogs(receivePackets);
            if (!sendPackets.isEmpty() || !receivePackets.isEmpty()) {
                refreshPacketLogView();
            }
            endpointText.setText(String.format(Locale.US,
                    "服务器 %s · SIP 5060（本地 %d）· RTP 40000（本地 %d）",
                    SERVER_HOST, status.localSipPort, status.localRtpPort));
            statisticsText.setText(String.format(Locale.US,
                    "TX %d 包  ·  RX %d 包  ·  接收速率 %d 包/秒  ·  %s %s",
                    status.transmittedPackets,
                    status.receivedPackets,
                    status.receivePacketsPerSecond,
                    loopbackEnabled ? "回环RTT" : "延时",
                    status.latencyText()));
            lastDiagnosticRefreshAt = now;
        }

        if (status.joined) {
            groupInput.setEnabled(false);
            loopbackCheckBox.setEnabled(false);
            joinButton.setEnabled(true);
            joinButton.setText("退出组队");
        }

        boolean talking = "TALKING".equals(status.mode);
        audioEngine.setTalking(talking);
        waveView.setActive(talking);
        waveView.setVisibility(talking ? View.VISIBLE : View.GONE);

        switch (status.mode) {
            case "IDLE":
                setBadge("空闲", Color.rgb(15, 163, 113));
                setTalkButton(true, "开始说话", Color.rgb(15, 163, 113));
                break;
            case "REQUESTING":
                setBadge("申请中", Color.rgb(31, 111, 235));
                setTalkButton(false, "正在申请…", Color.rgb(31, 111, 235));
                break;
            case "TALKING":
                setBadge("讲话中", Color.rgb(224, 75, 75));
                setTalkButton(true, "结束说话", Color.rgb(224, 75, 75));
                break;
            case "LISTENING":
            case "BUSY":
                setBadge("频道占用", Color.rgb(230, 147, 35));
                setTalkButton(false, "频道占用", Color.rgb(230, 147, 35));
                break;
            case "ERROR":
                setBadge("错误", Color.rgb(205, 51, 67));
                setTalkButton(false, "开始说话", Color.rgb(117, 130, 150));
                groupInput.setEnabled(true);
                loopbackCheckBox.setEnabled(true);
                joinButton.setEnabled(true);
                joinButton.setText("重新加入");
                break;
            default:
                setBadge("连接中", Color.rgb(31, 111, 235));
                setTalkButton(false, "开始说话", Color.rgb(117, 130, 150));
                break;
        }
    }

    private void applyDisconnectedUi() {
        currentMode = "STOPPED";
        lastDiagnosticRefreshAt = 0L;
        groupInput.setEnabled(true);
        loopbackCheckBox.setEnabled(true);
        joinButton.setEnabled(true);
        joinButton.setText("加入组队");
        statusText.setText("请输入组名后加入");
        setBadge("未连接", Color.rgb(117, 130, 150));
        waveView.setActive(false);
        waveView.setVisibility(View.GONE);
        setTalkButton(false, "开始说话", Color.rgb(15, 163, 113));
        endpointText.setText("服务器 " + SERVER_HOST + " · SIP 5060 · RTP 40000");
        statisticsText.setText("TX 0 包  ·  RX 0 包  ·  接收速率 0 包/秒  ·  延时 --");
        clearReceivePacketLog();
    }

    private void appendReceivePacketLogs(List<NativePtt.ReceivePacket> packets) {
        if (packets.isEmpty()) {
            return;
        }
        for (NativePtt.ReceivePacket packet : packets) {
            String time = packetTimeFormat.format(new Date(packet.arrivalTimeMs));
            String sendTime = packet.hasSendTime
                    ? packetTimeFormat.format(new Date(packet.sendTimeMs))
                    : "--:--:--.---";
            String captureFrame = packet.hasSourceFrameId
                    ? String.format(Locale.US, "%d", packet.sourceFrameId)
                    : "-";
            String line;
            if (loopbackEnabled) {
                line = String.format(Locale.US,
                        "RX-LOOP cap=%-6s seq=%5d sent=%s recv=%s " +
                                "RTT=%4dms len=%3dB %s\n",
                        captureFrame,
                        packet.sequence,
                        sendTime,
                        time,
                        packet.hasSendTime ? packet.roundTripMs : -1L,
                        packet.payloadLength,
                        packetStatusLabel(packet.status));
            } else {
                line = String.format(Locale.US,
                        "RX cap=%-6s seq=%5d time=%s len=%3dB " +
                                "dArr=%3dms dCap=%3dms %s\n",
                        captureFrame,
                        packet.sequence,
                        time,
                        packet.payloadLength,
                        packet.arrivalGapMs,
                        packet.captureGapMs,
                        packetStatusLabel(packet.status));
            }
            int color = PACKET_NORMAL_COLOR;
            if (!packet.continuous ||
                    (packet.hasSourceFrameId && !packet.sourceContinuous)) {
                color = PACKET_ERROR_COLOR;
            } else if (packet.arrivalGapMs > PACKET_GAP_WARNING_MS ||
                    packet.captureGapMs > PACKET_GAP_WARNING_MS ||
                    (loopbackEnabled && packet.hasSendTime &&
                            packet.roundTripMs > LOOPBACK_RTT_WARNING_MS)) {
                color = PACKET_WARNING_COLOR;
            }
            appendPacketLogLine(line, color);
        }
    }

    private void appendSendPacketLogs(List<NativePtt.SendPacket> packets) {
        if (packets.isEmpty()) {
            return;
        }
        for (NativePtt.SendPacket packet : packets) {
            String time = packetTimeFormat.format(new Date(packet.sendTimeMs));
            String line = String.format(Locale.US,
                    "%s cap=%-6d seq=%5d time=%s len=%3dB c2s=%3dms %s\n",
                    loopbackEnabled ? "TX-LOOP" : "TX",
                    packet.sourceFrameId,
                    packet.sequence,
                    time,
                    packet.payloadLength,
                    Math.max(0L, packet.sendTimeMs - packet.captureTimeMs),
                    packet.succeeded
                            ? (packet.marker ? "开始/已发送" : "已发送")
                            : "发送失败 errno=" + packet.errorCode);
            appendPacketLogLine(line,
                    packet.succeeded ? PACKET_NORMAL_COLOR : PACKET_ERROR_COLOR);
        }
    }

    private String currentNetworkType() {
        ConnectivityManager manager =
                (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        if (manager == null) {
            return "unknown";
        }
        Network network = manager.getActiveNetwork();
        NetworkCapabilities capabilities = network != null
                ? manager.getNetworkCapabilities(network)
                : null;
        if (capabilities == null) {
            return "offline";
        }
        if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) {
            return "lte";
        }
        if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
            return "wifi";
        }
        if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) {
            return "ethernet";
        }
        if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) {
            return "vpn";
        }
        return "other";
    }

    private void appendPacketLogLine(String line, int color) {
        int start = packetLogBuffer.length();
        packetLogBuffer.append(line);
        if (color != PACKET_NORMAL_COLOR) {
            packetLogBuffer.setSpan(
                    new ForegroundColorSpan(color),
                    start,
                    packetLogBuffer.length(),
                    Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        ++packetLogLineCount;
    }

    private void refreshPacketLogView() {
        while (packetLogLineCount > MAX_PACKET_LOG_LINES) {
            int newline = packetLogBuffer.toString().indexOf('\n');
            if (newline < 0) {
                packetLogBuffer.clear();
                packetLogLineCount = 0;
                break;
            }
            packetLogBuffer.delete(0, newline + 1);
            --packetLogLineCount;
        }
        receivePacketLogText.setText(packetLogBuffer, TextView.BufferType.SPANNABLE);
        receivePacketLogScroll.post(() ->
                receivePacketLogScroll.fullScroll(View.FOCUS_DOWN));
    }

    private void clearReceivePacketLog() {
        packetLogBuffer.clear();
        packetLogLineCount = 0;
        if (receivePacketLogText != null) {
            receivePacketLogText.setText("等待发送或接收音频包…");
        }
    }

    private String packetStatusLabel(String status) {
        switch (status) {
            case "start":
                return "开始";
            case "first":
                return "首包";
            case "continuous":
                return "连续";
            case "gap":
                return "不连续/缺包";
            case "duplicate":
                return "重复包";
            case "out_of_order":
                return "乱序包";
            default:
                return status;
        }
    }

    private void setTalkButton(boolean enabled, String label, int color) {
        talkButton.setEnabled(enabled);
        talkButton.setAlpha(enabled ? 1f : 0.45f);
        talkButton.setText(label);
        talkButton.setBackground(rounded(color, 34, Color.TRANSPARENT, 0));
    }

    private void setBadge(String label, int color) {
        statusBadge.setText(label);
        statusBadge.setBackground(rounded(color, 20, Color.TRANSPARENT, 0));
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        audioEngine.stop();
        if (nativeStarted) {
            NativePtt.nativeShutdown();
            nativeStarted = false;
        }
        super.onDestroy();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode,
                                           String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_RECORD_AUDIO) {
            if (grantResults.length > 0 &&
                    grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                Toast.makeText(this, "麦克风权限已允许，请再次点击加入组队",
                        Toast.LENGTH_SHORT).show();
            } else {
                Toast.makeText(this, "需要麦克风权限才能发送语音",
                        Toast.LENGTH_LONG).show();
            }
        }
    }

    private TextView text(String value, int sizeSp, int color) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(sizeSp);
        view.setTextColor(color);
        view.setLineSpacing(0f, 1.15f);
        return view;
    }

    private GradientDrawable rounded(int fillColor, int radiusDp, int strokeColor, int strokeDp) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(fillColor);
        drawable.setCornerRadius(dp(radiusDp));
        if (strokeDp > 0) {
            drawable.setStroke(dp(strokeDp), strokeColor);
        }
        return drawable;
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams wrapWrap() {
        return new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
