package com.master.sipptt;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.util.AttributeSet;
import android.view.View;

public final class WaveView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private boolean active;
    private float phase;
    private long lastFrameNanos;

    public WaveView(Context context) {
        super(context);
        initialize();
    }

    public WaveView(Context context, AttributeSet attributes) {
        super(context, attributes);
        initialize();
    }

    private void initialize() {
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeCap(Paint.Cap.ROUND);
        setLayerType(View.LAYER_TYPE_SOFTWARE, null);
    }

    public void setActive(boolean active) {
        if (this.active == active) {
            return;
        }
        this.active = active;
        lastFrameNanos = 0L;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (!active || getWidth() == 0 || getHeight() == 0) {
            return;
        }

        long now = System.nanoTime();
        if (lastFrameNanos != 0L) {
            float elapsedSeconds = (now - lastFrameNanos) / 1_000_000_000f;
            phase += elapsedSeconds * 5.2f;
        }
        lastFrameNanos = now;

        float centerY = getHeight() / 2f;
        float width = getWidth();
        int[] colors = {Color.rgb(0, 188, 212), Color.rgb(33, 150, 243), Color.rgb(103, 58, 183)};
        float[] amplitudes = {0.28f, 0.20f, 0.12f};

        for (int wave = 0; wave < colors.length; wave++) {
            paint.setColor(colors[wave]);
            paint.setAlpha(225 - wave * 45);
            paint.setStrokeWidth(dp(3f - wave * 0.55f));
            float previousX = 0f;
            float previousY = centerY;
            for (int x = 0; x <= getWidth(); x += 4) {
                float normalized = x / width;
                float envelope = (float) Math.sin(Math.PI * normalized);
                float angle = normalized * (float) Math.PI * 4.0f + phase + wave * 0.9f;
                float y = centerY + (float) Math.sin(angle) * getHeight() * amplitudes[wave] * envelope;
                if (x > 0) {
                    canvas.drawLine(previousX, previousY, x, y, paint);
                }
                previousX = x;
                previousY = y;
            }
        }
        postInvalidateOnAnimation();
    }

    private float dp(float value) {
        return value * getResources().getDisplayMetrics().density;
    }
}
