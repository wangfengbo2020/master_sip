package com.master.sipptt;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.Locale;

public final class MainActivity extends Activity {
    private static final String SERVER_HOST = "123.57.79.87";
    private static final long STATUS_INTERVAL_MS = 200L;
    private static final int REQUEST_RECORD_AUDIO = 1001;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final AudioEngine audioEngine = new AudioEngine();
    private final Runnable statusTask = new Runnable() {
        @Override
        public void run() {
            refreshStatus();
            handler.postDelayed(this, STATUS_INTERVAL_MS);
        }
    };

    private EditText groupInput;
    private Button joinButton;
    private Button talkButton;
    private TextView statusText;
    private TextView statusBadge;
    private TextView endpointText;
    private TextView statisticsText;
    private WaveView waveView;
    private boolean nativeStarted;
    private boolean joined;
    private String currentMode = "STOPPED";

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

        endpointText = text("服务器 " + SERVER_HOST + " · SIP 5060 · RTP 40000", 13,
                Color.rgb(91, 105, 124));
        LinearLayout.LayoutParams endpointParams = matchWrap();
        endpointParams.topMargin = dp(18);
        root.addView(endpointText, endpointParams);

        statisticsText = text("TX 0 包  ·  RX 0 包  ·  接收速率 0 包/秒  ·  延时 --", 13,
                Color.rgb(91, 105, 124));
        LinearLayout.LayoutParams statisticsParams = matchWrap();
        statisticsParams.topMargin = dp(7);
        root.addView(statisticsText, statisticsParams);

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
        nativeStarted = NativePtt.nativeStart(SERVER_HOST, deviceId, group);
        if (!nativeStarted) {
            Toast.makeText(this, "C核心启动失败，请查看Logcat", Toast.LENGTH_LONG).show();
            return;
        }
        audioEngine.start();
        groupInput.setEnabled(false);
        joinButton.setEnabled(false);
        joinButton.setText("连接中…");
        statusText.setText("正在连接服务器");
        setBadge("连接中", Color.rgb(31, 111, 235));
    }

    private void onTalkClicked() {
        if ("TALKING".equals(currentMode)) {
            if (!NativePtt.nativeReleaseTalk()) {
                Toast.makeText(this, "当前没有讲话权", Toast.LENGTH_SHORT).show();
            }
        } else if (!NativePtt.nativeRequestTalk()) {
            Toast.makeText(this, "暂时不能申请讲话", Toast.LENGTH_SHORT).show();
        }
    }

    private void refreshStatus() {
        if (!nativeStarted) {
            return;
        }
        NativePtt.Status status = NativePtt.getStatus();
        currentMode = status.mode;
        joined = status.joined;

        statusText.setText(status.detail.isEmpty() ? status.mode : status.detail);
        endpointText.setText(String.format(Locale.US,
                "服务器 %s · SIP 5060（本地 %d）· RTP 40000（本地 %d）",
                SERVER_HOST, status.localSipPort, status.localRtpPort));
        statisticsText.setText(String.format(Locale.US,
                "TX %d 包  ·  RX %d 包  ·  接收速率 %d 包/秒  ·  延时 %s",
                status.transmittedPackets,
                status.receivedPackets,
                status.receivePacketsPerSecond,
                status.latencyText()));

        if (status.joined) {
            groupInput.setEnabled(false);
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
        groupInput.setEnabled(true);
        joinButton.setEnabled(true);
        joinButton.setText("加入组队");
        statusText.setText("请输入组名后加入");
        setBadge("未连接", Color.rgb(117, 130, 150));
        waveView.setActive(false);
        waveView.setVisibility(View.GONE);
        setTalkButton(false, "开始说话", Color.rgb(15, 163, 113));
        endpointText.setText("服务器 " + SERVER_HOST + " · SIP 5060 · RTP 40000");
        statisticsText.setText("TX 0 包  ·  RX 0 包  ·  接收速率 0 包/秒  ·  延时 --");
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
