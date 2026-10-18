# Milestone 5 Architecture Survey Report: Jetpack Compose Audiophile Dashboard & VU Meter UI

**Agent**: `explorer_m5_ui`  
**Date**: 2026-10-18  
**Scope**: Production-grade Jetpack Compose UI architecture, 60 FPS Stereo VU Meter Canvas, Buffer Occupancy Watermark Gauge, Android 14+ Bit-Perfect Audio Device Selector, Telemetry Statistics, and Dark Audiophile Material 3 Theme.

---

## 1. Executive Summary & Design System Philosophy

The VBAN Receptor on Android requires a user interface tailored to high-end audiophiles, studio engineers, and network audio professionals. Unlike generic consumer media players, this application processes uncompressed PCM and IEEE 754 Float32 audio streams at up to 768 kHz with micro-second clock drift compensation and direct hardware mixer bypass.

The visual and architectural design principles established for Milestone 5 are:

1. **Dark Audiophile Studio Aesthetic**:
   - OLED pitch-black background (`#0A0E17`) with low eye-strain high-contrast elements.
   - Distinctive neon cyan (`#00E5FF`) and warm gold (`#FFD600`) accent highlights reminiscent of high-end DACs and professional studio hardware (Accuphase, McIntosh, Benchmark DAC3).
   - Monospace digital telemetry readouts (`FontFamily.Monospace`) for immediate, jitter-free numerical scanning.

2. **Strict Zero-Allocation Real-Time Canvas Rendering**:
   - The stereo VU meter and buffer occupancy gauge run at display refresh rate (60 Hz to 120 Hz).
   - In Compose, allocating `Brush`, `Paint`, `Path`, or `String` instances inside `onDraw` causes severe GC pressure, micro-stutters, and battery drain.
   - All shaders, gradients, tick coordinates, and layout parameters are cached via `remember` or computed using immutable primitives.

3. **True Broadcast Ballistics (DIN 45406 / EBU R128)**:
   - Peak level meters feature instantaneous attack (0 ms rise time) to catch inter-sample peaks.
   - Smooth decay at standard broadcast release rates ($20\text{ dB/s}$).
   - Peak-hold indicators latch the highest transient for $1.2\text{ seconds}$ before decaying at $15\text{ dB/s}$.
   - True digital overload latch (Crimson Red `#FF1744`) illuminates when signals reach $\ge -0.1\text{ dBFS}$.

4. **Bit-Perfect Hardware Transparency**:
   - On Android 14+ (API 34+), the UI prominently badges USB DACs operating in bit-perfect direct mode (`MIXER_BEHAVIOR_BIT_PERFECT`), notifying the user that the Android system audio mixer, software resampling, and volume quantization have been completely bypassed.

---

## 2. Architecture & Data Flow

```
┌────────────────────────────────────────────────────────────────────────┐
│                   Native VBAN C++20 Core & Oboe Engine                 │
│  - UDP Worker (Port 6980)  - SPSC Ring Buffer  - Oboe Audio Callback   │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Non-blocking Atomic JNI Query
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│                     NativeVbanEngine (Kotlin Singleton)                │
│  - StateFlow<Boolean> isRunning                                        │
│  - StateFlow<NativeTelemetry> telemetry (Polls C++ every ~33ms)        │
└───────────────────▲───────────────────────────────┬────────────────────┘
                    │                               │
        Service / UI Control                        │ Reactive State Flow
                    │                               ▼
┌───────────────────┴──────────────┐   ┌────────────────────────────────┐
│      VbanPlaybackService         │   │       MainActivity             │
│  - Foreground Service (API 34)   │   │  - Edge-to-Edge Layout         │
│  - 3-Lock Power Management       │   │  - POST_NOTIFICATIONS Handler  │
│  - Notification Telemetry Sync   │   │  - Service Intent Dispatcher   │
└───────────────────▲──────────────┘   └────────────┬───────────────────┘
                    │                               │
                    │ Service Action Intents        ▼
                    │                  ┌────────────────────────────────┐
                    │                  │       DashboardScreen          │
                    │                  │  - Dark Audiophile Theme       │
                    │                  │  - Connection Configuration    │
                    │                  │  - DeviceSelector Card         │
                    │                  │  - Stereo VU Meter Card        │
                    │                  │  - Buffer & Telemetry Card     │
                    └──────────────────┴────────────────────────────────┘
```

### Unidirectional Data Flow (UDF)
- **State Ingestion**: `DashboardScreen` observes `NativeVbanEngine.isRunning` and `NativeVbanEngine.telemetry` using lifecycle-aware state collectors (`collectAsStateWithLifecycle()`).
- **Ballistics Decoupling**: While native telemetry polls at $\approx 30\text{ Hz}$ to avoid JNI saturation, the Compose VU meter uses a `withFrameNanos` frame loop to interpolate and decay the meters at the native display refresh rate (60 FPS+), guaranteeing fluid visual animation.
- **Service Interaction**: User interactions (Start, Stop, Device Change) dispatch explicit Intents to `VbanPlaybackService` using standard constants (`ACTION_START`, `ACTION_STOP`), ensuring playback remains persistent even if the activity is minimized or killed by Android low-memory killer.

---

## 3. Component 1: `VuMeter.kt` (60 FPS Stereo VU Meter Canvas)

### 3.1 Mathematical Formulations & Acoustics
The linear amplitude $A \in [0.0, 1.0]$ reported by the C++ engine is converted to decibels relative to Full Scale (dBFS):
$$\text{dBFS} = 20 \cdot \log_{10}(A) \quad (\text{clamped to } [-60.0, 0.0])$$

The normalized horizontal or vertical display position $x \in [0.0, 1.0]$ is mapped linearly across the dBFS domain:
$$x = \frac{\text{dBFS} - (-60.0)}{0.0 - (-60.0)} = \frac{\text{dBFS} + 60.0}{60.0}$$

Key graduation marks:
- **$-60\text{ dBFS}$**: $x = 0.000$ (Silence floor)
- **$-40\text{ dBFS}$**: $x = 0.333$
- **$-20\text{ dBFS}$**: $x = 0.667$ (Studio standard nominal operating level)
- **$-12\text{ dBFS}$**: $x = 0.800$ (Digital broadcast headroom threshold)
- **$-6\text{ dBFS}$**: $x = 0.900$ (Peak warning threshold)
- **$-3\text{ dBFS}$**: $x = 0.950$ (High crest factor region)
- **$0\text{ dBFS}$**: $x = 1.000$ (Full scale / 0 dB digital clipping threshold)

### 3.2 60 FPS Ballistics Engine
- **Attack Phase**:
  If $\text{targetDb} > \text{currentDb}$, set $\text{currentDb} = \text{targetDb}$ immediately ($0\text{ ms}$ attack).
- **Decay Phase**:
  If $\text{targetDb} < \text{currentDb}$, decay smoothly:
  $$\text{currentDb} \leftarrow \max(\text{targetDb}, \text{currentDb} - 20.0 \times \Delta t)$$
- **Peak Hold Phase**:
  If $\text{currentDb} \ge \text{peakHoldDb}$, set $\text{peakHoldDb} = \text{currentDb}$ and reset hold timer to $1200\text{ ms}$.  
  When timer expires ($\le 0\text{ ms}$), decay peak hold marker at $15.0\text{ dB/s}$.
- **Overload Latch**:
  If $\text{peakLevel} \ge 0.999f$ ($\ge -0.1\text{ dBFS}$), latch overload flag to `true` and hold for $1000\text{ ms}$.

### 3.3 Production Code Template: `VuMeter.kt`

```kotlin
package com.vban.receptor.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.isActive
import java.util.Locale

// Audiophile VU Meter Palette Constants
private val MeterBackground = Color(0xFF131B26)
private val MeterGreen = Color(0xFF00E676)
private val MeterAmber = Color(0xFFFFD600)
private val MeterOrange = Color(0xFFFF9100)
private val MeterRed = Color(0xFFFF1744)
private val TickColor = Color(0xFF475569)
private val PeakHoldColor = Color(0xFFFFFFFF)

/**
 * High-performance 60 FPS Stereo VU Meter card with broadcast ballistics.
 *
 * @param leftLinear Linear peak amplitude for Left channel [0.0f, 1.0f].
 * @param rightLinear Linear peak amplitude for Right channel [0.0f, 1.0f].
 * @param isRunning True if engine is streaming; false renders idle standby state.
 */
@Composable
fun StereoVuMeter(
    leftLinear: Float,
    rightLinear: Float,
    isRunning: Boolean,
    modifier: Modifier = Modifier
) {
    // 60 FPS Ballistic state variables
    var currentLeftDb by remember { mutableFloatStateOf(-60.0f) }
    var currentRightDb by remember { mutableFloatStateOf(-60.0f) }
    var peakHoldLeftDb by remember { mutableFloatStateOf(-60.0f) }
    var peakHoldRightDb by remember { mutableFloatStateOf(-60.0f) }
    var peakHoldLeftTimerMs by remember { mutableLongStateOf(0L) }
    var peakHoldRightTimerMs by remember { mutableLongStateOf(0L) }
    var clipLeftTimerMs by remember { mutableLongStateOf(0L) }
    var clipRightTimerMs by remember { mutableLongStateOf(0L) }

    // Targets calculated from inputs
    val targetLeftDb = if (isRunning) linearToDbfs(leftLinear) else -60.0f
    val targetRightDb = if (isRunning) linearToDbfs(rightLinear) else -60.0f

    // 60 FPS Animation Frame Loop
    LaunchedEffect(isRunning) {
        var lastFrameNanos = 0L
        while (isActive) {
            withFrameNanos { frameNanos ->
                if (lastFrameNanos == 0L) {
                    lastFrameNanos = frameNanos
                    return@withFrameNanos
                }
                val dtSec = ((frameNanos - lastFrameNanos) / 1_000_000_000f).coerceIn(0.001f, 0.1f)
                val dtMs = (dtSec * 1000f).toLong()
                lastFrameNanos = frameNanos

                // Ballistic Decay Rates: 20 dB/s release, 15 dB/s peak decay
                val releaseDecay = 20.0f * dtSec
                val peakDecay = 15.0f * dtSec

                // Left Channel Ballistics
                if (targetLeftDb >= currentLeftDb) {
                    currentLeftDb = targetLeftDb // Instant attack
                } else {
                    currentLeftDb = (currentLeftDb - releaseDecay).coerceAtLeast(targetLeftDb)
                }

                if (currentLeftDb >= peakHoldLeftDb) {
                    peakHoldLeftDb = currentLeftDb
                    peakHoldLeftTimerMs = 1200L // Hold 1.2 seconds
                } else if (peakHoldLeftTimerMs > 0L) {
                    peakHoldLeftTimerMs = (peakHoldLeftTimerMs - dtMs).coerceAtLeast(0L)
                } else {
                    peakHoldLeftDb = (peakHoldLeftDb - peakDecay).coerceAtLeast(-60.0f)
                }

                if (leftLinear >= 0.999f && isRunning) {
                    clipLeftTimerMs = 1000L
                } else if (clipLeftTimerMs > 0L) {
                    clipLeftTimerMs = (clipLeftTimerMs - dtMs).coerceAtLeast(0L)
                }

                // Right Channel Ballistics
                if (targetRightDb >= currentRightDb) {
                    currentRightDb = targetRightDb // Instant attack
                } else {
                    currentRightDb = (currentRightDb - releaseDecay).coerceAtLeast(targetRightDb)
                }

                if (currentRightDb >= peakHoldRightDb) {
                    peakHoldRightDb = currentRightDb
                    peakHoldRightTimerMs = 1200L
                } else if (peakHoldRightTimerMs > 0L) {
                    peakHoldRightTimerMs = (peakHoldRightTimerMs - dtMs).coerceAtLeast(0L)
                } else {
                    peakHoldRightDb = (peakHoldRightDb - peakDecay).coerceAtLeast(-60.0f)
                }

                if (rightLinear >= 0.999f && isRunning) {
                    clipRightTimerMs = 1000L
                } else if (clipRightTimerMs > 0L) {
                    clipRightTimerMs = (clipRightTimerMs - dtMs).coerceAtLeast(0L)
                }
            }
        }
    }

    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        shape = RoundedCornerShape(12.dp)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            // Card Title Header
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "STEREO VU METER",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.primary,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 1.2.sp
                )
                Text(
                    text = if (isRunning) "ACTIVE • 60 FPS" else "STANDBY",
                    style = MaterialTheme.typography.labelSmall,
                    color = if (isRunning) MeterGreen else Color.Gray,
                    fontFamily = FontFamily.Monospace
                )
            }

            Spacer(modifier = Modifier.height(12.dp))

            // Calibration scale tick marks
            VuScaleTicks()

            Spacer(modifier = Modifier.height(6.dp))

            // Left Channel Bar
            VuMeterBarRow(
                channelLabel = "L",
                levelDb = currentLeftDb,
                peakHoldDb = peakHoldLeftDb,
                isClipping = clipLeftTimerMs > 0L
            )

            Spacer(modifier = Modifier.height(8.dp))

            // Right Channel Bar
            VuMeterBarRow(
                channelLabel = "R",
                levelDb = currentRightDb,
                peakHoldDb = peakHoldRightDb,
                isClipping = clipRightTimerMs > 0L
            )
        }
    }
}

/**
 * Single audio channel row with label, canvas bar, numeric readout, and clip LED.
 */
@Composable
private fun VuMeterBarRow(
    channelLabel: String,
    levelDb: Float,
    peakHoldDb: Float,
    isClipping: Boolean,
    modifier: Modifier = Modifier
) {
    // Zero-allocation gradient cache
    val meterGradient = remember {
        Brush.horizontalGradient(
            0.00f to MeterGreen,   // -60 dBFS
            0.66f to MeterGreen,   // -20 dBFS
            0.80f to MeterAmber,   // -12 dBFS
            0.90f to MeterOrange,  // -6 dBFS
            1.00f to MeterRed      // 0 dBFS Overload
        )
    }

    val normLevel = ((levelDb + 60.0f) / 60.0f).coerceIn(0.0f, 1.0f)
    val normPeakHold = ((peakHoldDb + 60.0f) / 60.0f).coerceIn(0.0f, 1.0f)

    Row(
        modifier = modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically
    ) {
        // Channel Label
        Text(
            text = channelLabel,
            modifier = Modifier.width(18.dp),
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.onSurface
        )

        Spacer(modifier = Modifier.width(6.dp))

        // High-Performance Zero-Allocation Meter Canvas
        Canvas(
            modifier = Modifier
                .weight(1.0f)
                .height(18.dp)
        ) {
            val width = size.width
            val height = size.height
            val corner = CornerRadius(3.dp.toPx(), 3.dp.toPx())

            // Background trough
            drawRoundRect(
                color = MeterBackground,
                size = Size(width, height),
                cornerRadius = corner
            )

            // Active level bar with gradient
            if (normLevel > 0.001f) {
                drawRoundRect(
                    brush = meterGradient,
                    size = Size(width * normLevel, height),
                    cornerRadius = corner
                )
            }

            // Peak Hold indicator line
            if (normPeakHold > 0.01f) {
                val peakX = width * normPeakHold
                drawLine(
                    color = PeakHoldColor,
                    start = Offset(peakX, 0f),
                    end = Offset(peakX, height),
                    strokeWidth = 2.dp.toPx()
                )
            }
        }

        Spacer(modifier = Modifier.width(8.dp))

        // Numeric Decibel Readout
        Text(
            text = formatDbfs(levelDb),
            modifier = Modifier.width(58.dp),
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            color = if (levelDb >= -1.0f) MeterRed else MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.End
        )

        Spacer(modifier = Modifier.width(6.dp))

        // Hardware Clip LED Indicator
        Box(
            modifier = Modifier
                .size(10.dp)
                .background(
                    color = if (isClipping) MeterRed else MeterBackground,
                    shape = CircleShape
                )
        )
    }
}

/**
 * Scale tick graduation marks (-60, -40, -20, -12, -6, 0 dB).
 */
@Composable
private fun VuScaleTicks(modifier: Modifier = Modifier) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(start = 24.dp, end = 74.dp), // Align exactly with canvas bar
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        val ticks = listOf("-60", "-40", "-20", "-12", "-6", "0")
        ticks.forEach { tick ->
            Text(
                text = tick,
                style = MaterialTheme.typography.labelSmall.copy(fontSize = 9.sp),
                color = TickColor,
                fontFamily = FontFamily.Monospace
            )
        }
    }
}

private fun linearToDbfs(linear: Float): Float {
    if (linear <= 0.00001f) return -60.0f
    val db = 20.0f * kotlin.math.log10(linear)
    return db.coerceIn(-60.0f, 0.0f)
}

private fun formatDbfs(db: Float): String {
    return if (db <= -59.5f) {
        "-∞ dB"
    } else {
        String.format(Locale.US, "%5.1f", db)
    }
}
```

---

## 4. Component 2: `BufferOccupancyGauge.kt` (Real-Time Buffer & Watermark Gauge)

### 4.1 Engineering Principles & Watermark Math
The VBAN native core utilizes a $2^{18} = 262,144$ frame lock-free SPSC circular ring buffer.
- At $48\text{ kHz}$, total buffer capacity is $\approx 5461.3\text{ ms}$.
- At $768\text{ kHz}$, total buffer capacity is $\approx 341.3\text{ ms}$.
- The adaptive clock drift servo targets a nominal watermark of $\approx 50\%$ (or configured initial latency window).

### 4.2 Stability Zones & Color Thresholds
1. **Critical Underrun Zone ($0\% \le \text{fill} < 15\%$)**:  
   Buffer is starving due to network congestion or packet loss. *Color: Crimson Red (`#EF4444`)*.
2. **Jitter Warning Low Zone ($15\% \le \text{fill} < 30\%$)**:  
   Drift compensator is ramping up playback speed to refill. *Color: Amber (`#F59E0B`)*.
3. **Optimal Operating Zone ($30\% \le \text{fill} \le 70\%$)**:  
   Clock drift servo is locked; jitter is absorbed. *Color: Audiophile Emerald (`#10B981`)*. Target line at $50\%$.
4. **Jitter Warning High Zone ($70\% < \text{fill} \le 85\%$)**:  
   Buffer accumulating excess frames; drift controller ramping down playback rate. *Color: Amber (`#F59E0B`)*.
5. **Critical Overflow Zone ($85\% < \text{fill} \le 100\%$)**:  
   Imminent buffer overflow risking packet drops. *Color: Crimson Red (`#EF4444`)*.

### 4.3 Production Code Template: `BufferOccupancyGauge.kt`

```kotlin
package com.vban.receptor.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.util.Locale

private val ZoneOptimal = Color(0xFF10B981)
private val ZoneWarning = Color(0xFFF59E0B)
private val ZoneDanger = Color(0xFFEF4444)
private val TrackColor = Color(0xFF1E293B)
private val WatermarkTargetColor = Color(0xFF00E5FF)

/**
 * Circular Arc Speedometer-style Buffer Occupancy & Watermark Gauge.
 *
 * @param occupancyPercent Current SPSC buffer fill percentage [0.0f, 100.0f].
 * @param sampleRate Current audio sample rate in Hz.
 * @param jitterMs RFC 3550 inter-arrival jitter in milliseconds.
 */
@Composable
fun BufferOccupancyGauge(
    occupancyPercent: Float,
    sampleRate: Int,
    jitterMs: Float,
    modifier: Modifier = Modifier
) {
    val clampedPercent = occupancyPercent.coerceIn(0.0f, 100.0f)
    
    // Smooth animated gauge sweep
    val animatedPercent by animateFloatAsState(
        targetValue = clampedPercent,
        animationSpec = tween(durationMillis = 150, easing = FastOutSlowInEasing),
        label = "BufferGaugeAnimation"
    )

    // Dynamic zone color
    val statusColor by animateColorAsState(
        targetValue = when {
            clampedPercent < 15.0f || clampedPercent > 85.0f -> ZoneDanger
            clampedPercent < 30.0f || clampedPercent > 70.0f -> ZoneWarning
            else -> ZoneOptimal
        },
        label = "GaugeColorAnimation"
    )

    val statusText = when {
        clampedPercent < 15.0f -> "CRITICAL UNDERRUN"
        clampedPercent < 30.0f -> "DRIFT RE-SYNC LOW"
        clampedPercent <= 70.0f -> "WATERMARK LOCKED"
        clampedPercent <= 85.0f -> "DRIFT RE-SYNC HIGH"
        else -> "OVERFLOW HAZARD"
    }

    // Calculated buffer latency in milliseconds
    val bufferFrames = (clampedPercent / 100.0f) * 262144f
    val bufferLatencyMs = if (sampleRate > 0) (bufferFrames / sampleRate.toFloat()) * 1000f else 0.0f

    Card(
        modifier = modifier,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        shape = RoundedCornerShape(12.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                text = "BUFFER WATERMARK & OCCUPANCY",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.Bold,
                letterSpacing = 1.1.sp
            )

            Spacer(modifier = Modifier.height(12.dp))

            // Speedometer Arc Gauge
            Box(
                modifier = Modifier.size(160.dp),
                contentAlignment = Alignment.Center
            ) {
                Canvas(modifier = Modifier.size(150.dp)) {
                    val strokeWidth = 14.dp.toPx()
                    val arcSize = size.width - strokeWidth
                    val topLeft = Offset(strokeWidth / 2f, strokeWidth / 2f)
                    val startAngle = 140f
                    val sweepRange = 260f

                    // 1. Background Arc Track
                    drawArc(
                        color = TrackColor,
                        startAngle = startAngle,
                        sweepAngle = sweepRange,
                        useCenter = false,
                        topLeft = topLeft,
                        size = Size(arcSize, arcSize),
                        style = Stroke(width = strokeWidth, cap = StrokeCap.Round)
                    )

                    // 2. Safe Watermark Zone Highlight (30% to 70%)
                    val safeStart = startAngle + (sweepRange * 0.30f)
                    val safeSweep = sweepRange * 0.40f
                    drawArc(
                        color = ZoneOptimal.copy(alpha = 0.20f),
                        startAngle = safeStart,
                        sweepAngle = safeSweep,
                        useCenter = false,
                        topLeft = topLeft,
                        size = Size(arcSize, arcSize),
                        style = Stroke(width = strokeWidth + 2.dp.toPx(), cap = StrokeCap.Butt)
                    )

                    // 3. Active Fill Arc
                    val currentSweep = (animatedPercent / 100.0f) * sweepRange
                    if (currentSweep > 1.0f) {
                        drawArc(
                            color = statusColor,
                            startAngle = startAngle,
                            sweepAngle = currentSweep,
                            useCenter = false,
                            topLeft = topLeft,
                            size = Size(arcSize, arcSize),
                            style = Stroke(width = strokeWidth, cap = StrokeCap.Round)
                        )
                    }

                    // 4. Target 50% Watermark Calibration Marker
                    val targetAngleRad = Math.toRadians((startAngle + (sweepRange * 0.50f)).toDouble())
                    val radius = arcSize / 2f
                    val center = Offset(size.width / 2f, size.height / 2f)
                    val innerR = radius - (strokeWidth / 2f) - 3.dp.toPx()
                    val outerR = radius + (strokeWidth / 2f) + 3.dp.toPx()

                    val markStart = Offset(
                        (center.x + innerR * Math.cos(targetAngleRad)).toFloat(),
                        (center.y + innerR * Math.sin(targetAngleRad)).toFloat()
                    )
                    val markEnd = Offset(
                        (center.x + outerR * Math.cos(targetAngleRad)).toFloat(),
                        (center.y + outerR * Math.sin(targetAngleRad)).toFloat()
                    )

                    drawLine(
                        color = WatermarkTargetColor,
                        start = markStart,
                        end = markEnd,
                        strokeWidth = 3.dp.toPx()
                    )
                }

                // Center Numerical Readout
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(
                        text = String.format(Locale.US, "%.1f%%", animatedPercent),
                        style = MaterialTheme.typography.headlineMedium,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace,
                        color = statusColor
                    )
                    Text(
                        text = String.format(Locale.US, "%.1f ms", bufferLatencyMs),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        fontFamily = FontFamily.Monospace
                    )
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            // State & Network Jitter Metric
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = statusText,
                    style = MaterialTheme.typography.labelSmall,
                    color = statusColor,
                    fontWeight = FontWeight.SemiBold
                )

                Text(
                    text = String.format(Locale.US, "Jitter: %.2f ms", jitterMs),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontFamily = FontFamily.Monospace
                )
            }
        }
    }
}
```

---

## 5. Component 3: `DeviceSelector.kt` (Audio Device Routing & Bit-Perfect Badge)

### 5.1 Android 14+ Bit-Perfect Bypass Integration
To achieve bit-perfect output, Android 14 (API 34) introduced `AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT`.
When an external USB DAC is plugged in:
- `AudioDeviceManager` scans the `AudioDeviceInfo` objects.
- `BitPerfectController` checks `audioManager.getSupportedMixerAttributes(device)`.
- If supported, the UI displays a prominent gold badge:  
  `★ BIT-PERFECT DIRECT (Android 14 Mixer Bypass Active)`.
- When non-USB or legacy Android devices are active, the UI indicates:  
  `AAUDIO EXCLUSIVE` or `SYSTEM MIXED`.

### 5.2 Production Code Template: `DeviceSelector.kt`

```kotlin
package com.vban.receptor.ui

import android.media.AudioDeviceInfo
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Headphones
import androidx.compose.material.icons.filled.Speaker
import androidx.compose.material.icons.filled.Usb
import androidx.compose.material.icons.filled.VolumeUp
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * Representation of an audio output device for UI display.
 */
data class AudioOutputDeviceItem(
    val id: Int,
    val name: String,
    val type: Int,
    val isUsb: Boolean,
    val isBitPerfectSupported: Boolean
)

private val GoldBadgeGradient = Brush.horizontalGradient(
    listOf(Color(0xFFFFD700), Color(0xFFFF9100))
)
private val CyanBadgeGradient = Brush.horizontalGradient(
    listOf(Color(0xFF00E5FF), Color(0xFF0091EA))
)

/**
 * Audio Device Selector card with Android 14+ Bit-Perfect verification badge.
 */
@Composable
fun DeviceSelector(
    devices: List<AudioOutputDeviceItem>,
    selectedDeviceId: Int,
    onDeviceSelected: (AudioOutputDeviceItem) -> Unit,
    modifier: Modifier = Modifier
) {
    var expanded by remember { mutableStateOf(false) }
    val currentDevice = devices.find { it.id == selectedDeviceId } 
        ?: devices.firstOrNull() 
        ?: AudioOutputDeviceItem(0, "Default Audio HAL", AudioDeviceInfo.TYPE_BUILTIN_SPEAKER, false, false)

    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        shape = RoundedCornerShape(12.dp)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = "AUDIO OUTPUT ROUTING",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.Bold,
                letterSpacing = 1.1.sp
            )

            Spacer(modifier = Modifier.height(10.dp))

            // Clickable Dropdown Trigger Box
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(8.dp))
                    .background(Color(0xFF0E1624), RoundedCornerShape(8.dp))
                    .clickable { expanded = true }
                    .padding(horizontal = 14.dp, vertical = 12.dp)
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.weight(1f)
                    ) {
                        Icon(
                            imageVector = getDeviceIcon(currentDevice.type),
                            contentDescription = "Device Type",
                            tint = if (currentDevice.isUsb) Color(0xFF00E5FF) else MaterialTheme.colorScheme.onSurface,
                            modifier = Modifier.size(24.dp)
                        )

                        Spacer(modifier = Modifier.width(12.dp))

                        Column {
                            Text(
                                text = currentDevice.name,
                                style = MaterialTheme.typography.bodyLarge,
                                fontWeight = FontWeight.SemiBold,
                                color = MaterialTheme.colorScheme.onSurface
                            )

                            Text(
                                text = getDeviceTypeDescription(currentDevice.type),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }

                    Icon(
                        imageVector = Icons.Default.ArrowDropDown,
                        contentDescription = "Open Device Dropdown",
                        tint = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }

                // Dropdown Options
                DropdownMenu(
                    expanded = expanded,
                    onDismissRequest = { expanded = false },
                    modifier = Modifier.background(Color(0xFF131D2D))
                ) {
                    devices.forEach { device ->
                        DropdownMenuItem(
                            text = {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Icon(
                                        imageVector = getDeviceIcon(device.type),
                                        contentDescription = null,
                                        tint = if (device.isUsb) Color(0xFF00E5FF) else Color.Gray,
                                        modifier = Modifier.size(20.dp)
                                    )
                                    Spacer(modifier = Modifier.width(10.dp))
                                    Column {
                                        Text(device.name, fontWeight = FontWeight.Medium)
                                        if (device.isBitPerfectSupported) {
                                            Text(
                                                "Bit-Perfect Direct Supported",
                                                style = MaterialTheme.typography.labelSmall,
                                                color = Color(0xFFFFD700)
                                            )
                                        }
                                    }
                                }
                            },
                            onClick = {
                                onDeviceSelected(device)
                                expanded = false
                            }
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.height(10.dp))

            // Audiophile Bit-Perfect Status Badge
            if (currentDevice.isBitPerfectSupported) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(GoldBadgeGradient, RoundedCornerShape(6.dp))
                        .padding(horizontal = 10.dp, vertical = 6.dp)
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.Center,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text(
                            text = "★ BIT-PERFECT DIRECT (Android 14 Mixer Bypass Active)",
                            style = MaterialTheme.typography.labelSmall,
                            fontWeight = FontWeight.Bold,
                            color = Color.Black
                        )
                    }
                }
            } else if (currentDevice.isUsb) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(CyanBadgeGradient, RoundedCornerShape(6.dp))
                        .padding(horizontal = 10.dp, vertical = 6.dp)
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.Center,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text(
                            text = "AAUDIO EXCLUSIVE (Low Latency Direct HAL)",
                            style = MaterialTheme.typography.labelSmall,
                            fontWeight = FontWeight.Bold,
                            color = Color.Black
                        )
                    }
                }
            } else {
                Text(
                    text = "Standard Shared Audio Routing",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

private fun getDeviceIcon(type: Int): ImageVector {
    return when (type) {
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_HEADSET -> Icons.Default.Usb
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> Icons.Default.Headphones
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> Icons.Default.Speaker
        else -> Icons.Default.VolumeUp
    }
}

private fun getDeviceTypeDescription(type: Int): String {
    return when (type) {
        AudioDeviceInfo.TYPE_USB_DEVICE -> "External USB Audio DAC"
        AudioDeviceInfo.TYPE_USB_HEADSET -> "USB-C Digital Headset"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "3.5mm Analog Headphones"
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "Internal Device Speaker"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "Bluetooth Audio Device (A2DP)"
        else -> "Android Audio Output Device"
    }
}
```

---

## 6. Component 4: `DashboardScreen.kt` (Audiophile Control Center)

### 6.1 Layout & Component Coordination
`DashboardScreen` coordinates the complete reactive control deck:
1. **Top Header**: Brand logo, connection status badge (STANDBY vs ACTIVE STREAMING).
2. **Network Connection Configuration Card**:
   - `Listen IP`: Text input defaulting to `0.0.0.0` with quick selector chips for Unicast and Multicast (`239.255.0.1`).
   - `UDP Port`: Defaulting to standard `6980`.
   - `Stream Name`: Filter matching incoming VBAN packet header.
   - `Start / Stop Toggle Button`: High-contrast gradient button triggering `VbanPlaybackService`.
3. **Audio Output & Bit-Perfect Card**: Integrates `DeviceSelector`.
4. **Stereo VU Meter Card**: Integrates 60 FPS `StereoVuMeter`.
5. **Buffer Watermark & Telemetry Card**: Integrates `BufferOccupancyGauge` and real-time statistics grid.

### 6.2 Production Code Template: `DashboardScreen.kt`

```kotlin
package com.vban.receptor.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.vban.receptor.native.NativeTelemetry
import com.vban.receptor.native.NativeVbanEngine
import java.util.Locale

/**
 * Main Audiophile Dashboard Screen for VBAN Receptor.
 */
@Composable
fun DashboardScreen(
    onStartService: (ip: String, port: Int, stream: String, deviceId: Int) -> Unit,
    onStopService: () -> Unit,
    availableDevices: List<AudioOutputDeviceItem> = emptyList(),
    modifier: Modifier = Modifier
) {
    val isRunning by NativeVbanEngine.isRunning.collectAsStateWithLifecycle()
    val telemetry by NativeVbanEngine.telemetry.collectAsStateWithLifecycle()

    var listenIp by remember { mutableStateOf("0.0.0.0") }
    var portText by remember { mutableStateOf("6980") }
    var streamName by remember { mutableStateOf("Stream1") }
    var selectedDeviceId by remember { mutableIntStateOf(0) }

    val scrollState = rememberScrollState()

    Scaffold(
        modifier = modifier.fillMaxSize(),
        containerColor = MaterialTheme.colorScheme.background
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .verticalScroll(scrollState)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            // Header Bar
            DashboardHeader(isRunning = isRunning)

            // Connection Configuration Card
            ConnectionConfigCard(
                listenIp = listenIp,
                onIpChanged = { listenIp = it },
                portText = portText,
                onPortChanged = { portText = it },
                streamName = streamName,
                onStreamNameChanged = { streamName = it },
                isRunning = isRunning,
                onTogglePlayback = {
                    if (isRunning) {
                        onStopService()
                    } else {
                        val port = portText.toIntOrNull() ?: 6980
                        onStartService(listenIp, port, streamName, selectedDeviceId)
                    }
                }
            )

            // Audio Output Routing & Bit-Perfect Badge
            DeviceSelector(
                devices = availableDevices,
                selectedDeviceId = selectedDeviceId,
                onDeviceSelected = { device ->
                    selectedDeviceId = device.id
                    NativeVbanEngine.setDevice(device.id)
                }
            )

            // 60 FPS Stereo VU Meter
            StereoVuMeter(
                leftLinear = telemetry.peakLevelLeft,
                rightLinear = telemetry.peakLevelRight,
                isRunning = isRunning
            )

            // Buffer Watermark Gauge & Stream Telemetry
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                BufferOccupancyGauge(
                    occupancyPercent = telemetry.bufferOccupancyPercent,
                    sampleRate = telemetry.sampleRate,
                    jitterMs = telemetry.jitterMs,
                    modifier = Modifier.weight(1f)
                )

                StreamTelemetryCard(
                    telemetry = telemetry,
                    isRunning = isRunning,
                    modifier = Modifier.weight(1f)
                )
            }
        }
    }
}

@Composable
private fun DashboardHeader(isRunning: Boolean) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column {
            Text(
                text = "VBAN RECEPTOR",
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.ExtraBold,
                color = MaterialTheme.colorScheme.onBackground,
                letterSpacing = 1.5.sp
            )
            Text(
                text = "High-Res Float32 Low-Latency Audio",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.primary
            )
        }

        Box(
            modifier = Modifier
                .background(
                    color = if (isRunning) Color(0xFF00E676).copy(alpha = 0.2f) else Color(0xFF334155),
                    shape = RoundedCornerShape(16.dp)
                )
                .padding(horizontal = 12.dp, vertical = 6.dp)
        ) {
            Text(
                text = if (isRunning) "STREAMING ACTIVE" else "STANDBY",
                style = MaterialTheme.typography.labelSmall,
                fontWeight = FontWeight.Bold,
                color = if (isRunning) Color(0xFF00E676) else Color(0xFF94A3B8)
            )
        }
    }
}

@Composable
private fun ConnectionConfigCard(
    listenIp: String,
    onIpChanged: (String) -> Unit,
    portText: String,
    onPortChanged: (String) -> Unit,
    streamName: String,
    onStreamNameChanged: (String) -> Unit,
    isRunning: Boolean,
    onTogglePlayback: () -> Unit,
    modifier: Modifier = Modifier
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        shape = RoundedCornerShape(12.dp)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = "NETWORK STREAM CONFIGURATION",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.Bold,
                letterSpacing = 1.1.sp
            )

            Spacer(modifier = Modifier.height(12.dp))

            // IP & Port Inputs
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                OutlinedTextField(
                    value = listenIp,
                    onValueChange = onIpChanged,
                    label = { Text("Listen IP") },
                    enabled = !isRunning,
                    modifier = Modifier.weight(2f),
                    singleLine = true,
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = MaterialTheme.colorScheme.primary,
                        unfocusedBorderColor = MaterialTheme.colorScheme.outline
                    )
                )

                OutlinedTextField(
                    value = portText,
                    onValueChange = onPortChanged,
                    label = { Text("UDP Port") },
                    enabled = !isRunning,
                    modifier = Modifier.weight(1f),
                    singleLine = true,
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = MaterialTheme.colorScheme.primary,
                        unfocusedBorderColor = MaterialTheme.colorScheme.outline
                    )
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            // IP Presets (Unicast / Multicast)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(
                    selected = listenIp == "0.0.0.0",
                    onClick = { onIpChanged("0.0.0.0") },
                    label = { Text("Unicast (0.0.0.0)") },
                    enabled = !isRunning
                )
                FilterChip(
                    selected = listenIp == "239.255.0.1",
                    onClick = { onIpChanged("239.255.0.1") },
                    label = { Text("Multicast (239.255.0.1)") },
                    enabled = !isRunning
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            // Stream Name Input
            OutlinedTextField(
                value = streamName,
                onValueChange = onStreamNameChanged,
                label = { Text("Stream Name (VBAN Header Filter)") },
                enabled = !isRunning,
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = MaterialTheme.colorScheme.primary,
                    unfocusedBorderColor = MaterialTheme.colorScheme.outline
                )
            )

            Spacer(modifier = Modifier.height(14.dp))

            // Primary Playback Toggle Button
            Button(
                onClick = onTogglePlayback,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(50.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (isRunning) Color(0xFFEF4444) else MaterialTheme.colorScheme.primary
                ),
                shape = RoundedCornerShape(8.dp)
            ) {
                Icon(
                    imageVector = if (isRunning) Icons.Default.Stop else Icons.Default.PlayArrow,
                    contentDescription = null,
                    modifier = Modifier.size(22.dp)
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text(
                    text = if (isRunning) "STOP RECEPTOR" else "START RECEPTOR",
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 1.0.sp
                )
            }
        }
    }
}

@Composable
private fun StreamTelemetryCard(
    telemetry: NativeTelemetry,
    isRunning: Boolean,
    modifier: Modifier = Modifier
) {
    Card(
        modifier = modifier,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        shape = RoundedCornerShape(12.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp)
        ) {
            Text(
                text = "STREAM TELEMETRY",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.Bold,
                letterSpacing = 1.1.sp
            )

            Spacer(modifier = Modifier.height(12.dp))

            TelemetryItem(
                label = "Sample Rate",
                value = if (isRunning && telemetry.sampleRate > 0) "${telemetry.sampleRate} Hz" else "---"
            )
            TelemetryItem(
                label = "Channels",
                value = if (isRunning && telemetry.channelCount > 0) "${telemetry.channelCount} ch" else "---"
            )
            TelemetryItem(
                label = "Bit Depth",
                value = if (isRunning && telemetry.bitDepth > 0) "${telemetry.bitDepth}-bit" else "---"
            )
            TelemetryItem(
                label = "Packets",
                value = if (isRunning) "${telemetry.totalPacketsReceived}" else "---"
            )
            TelemetryItem(
                label = "Loss Rate",
                value = if (isRunning) String.format(Locale.US, "%.2f%%", telemetry.packetLossRatePercent) else "---",
                isWarning = telemetry.packetLossRatePercent > 0.1f
            )
        }
    }
}

@Composable
private fun TelemetryItem(
    label: String,
    value: String,
    isWarning: Boolean = false
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.SemiBold,
            color = if (isWarning) Color(0xFFEF4444) else MaterialTheme.colorScheme.onSurface
        )
    }
}
```

---

## 7. Component 5: `MainActivity.kt` (Lifecycle, Service Control & Permissions)

### 7.1 Lifecycle & Permissions Requirements
1. **Android 13+ Notification Permission (`POST_NOTIFICATIONS`)**:
   Mandatory runtime permission prompt before launching `VbanPlaybackService` with persistent ongoing notification.
2. **Foreground Service Communication**:
   Sends `ACTION_START` with extras (`EXTRA_LISTEN_IP`, `EXTRA_PORT`, `EXTRA_STREAM_NAME`, `EXTRA_DEVICE_ID`) and `ACTION_STOP`.
3. **Dynamic Hotplug Audio Device Monitoring**:
   Registers `AudioDeviceCallback` with `AudioManager` to keep available USB DACs up to date in real time.

### 7.2 Production Code Template: `MainActivity.kt`

```kotlin
package com.vban.receptor

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.mutableStateListOf
import androidx.core.content.ContextCompat
import com.vban.receptor.service.VbanPlaybackService
import com.vban.receptor.ui.AudioOutputDeviceItem
import com.vban.receptor.ui.DashboardScreen
import com.vban.receptor.ui.theme.VbanReceptorTheme

class MainActivity : ComponentActivity() {

    private lateinit var audioManager: AudioManager
    private val availableDevices = mutableStateListOf<AudioOutputDeviceItem>()

    private val notificationPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { isGranted ->
        if (!isGranted) {
            Toast.makeText(
                this,
                "Notification permission required for background audio streaming.",
                Toast.LENGTH_LONG
            ).show()
        }
    }

    private val audioDeviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) {
            refreshAudioDevices()
        }

        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) {
            refreshAudioDevices()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        requestNotificationPermission()
        refreshAudioDevices()
        audioManager.registerAudioDeviceCallback(audioDeviceCallback, null)

        setContent {
            VbanReceptorTheme {
                DashboardScreen(
                    onStartService = { ip, port, stream, deviceId ->
                        startVbanService(ip, port, stream, deviceId)
                    },
                    onStopService = {
                        stopVbanService()
                    },
                    availableDevices = availableDevices
                )
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        audioManager.unregisterAudioDeviceCallback(audioDeviceCallback)
    }

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(
                    this,
                    Manifest.permission.POST_NOTIFICATIONS
                ) != PackageManager.PERMISSION_GRANTED
            ) {
                notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
    }

    private fun refreshAudioDevices() {
        val outputs = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        availableDevices.clear()
        outputs.forEach { dev ->
            val isUsb = dev.type == AudioDeviceInfo.TYPE_USB_DEVICE || 
                        dev.type == AudioDeviceInfo.TYPE_USB_HEADSET
            val isBitPerfectSupported = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                try {
                    audioManager.getSupportedMixerAttributes(dev).any { attr ->
                        attr.mixerBehavior == android.media.AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT
                    }
                } catch (_: Exception) {
                    false
                }
            } else {
                false
            }

            availableDevices.add(
                AudioOutputDeviceItem(
                    id = dev.id,
                    name = dev.productName.toString().ifBlank { "Audio Device #${dev.id}" },
                    type = dev.type,
                    isUsb = isUsb,
                    isBitPerfectSupported = isBitPerfectSupported
                )
            )
        }
    }

    private fun startVbanService(ip: String, port: Int, stream: String, deviceId: Int) {
        val intent = Intent(this, VbanPlaybackService::class.java).apply {
            action = VbanPlaybackService.ACTION_START
            putExtra(VbanPlaybackService.EXTRA_LISTEN_IP, ip)
            putExtra(VbanPlaybackService.EXTRA_PORT, port)
            putExtra(VbanPlaybackService.EXTRA_STREAM_NAME, stream)
            putExtra(VbanPlaybackService.EXTRA_DEVICE_ID, deviceId)
        }
        ContextCompat.startForegroundService(this, intent)
    }

    private fun stopVbanService() {
        val intent = Intent(this, VbanPlaybackService::class.java).apply {
            action = VbanPlaybackService.ACTION_STOP
        }
        startService(intent)
    }
}
```

---

## 8. Component 6: Theme & Design System (`Theme.kt`, `Color.kt`, `Type.kt`)

### 8.1 Color Palette: `Color.kt`
```kotlin
package com.vban.receptor.ui.theme

import androidx.compose.ui.graphics.Color

// OLED Audiophile Palette
val DarkBackground = Color(0xFF0A0E17)
val SurfaceDark = Color(0xFF111827)
val SurfaceVariantDark = Color(0xFF1B2436)
val OutlineDark = Color(0xFF2E3D56)

// Neon & High-Res Accents
val CyanAccent = Color(0xFF00E5FF)
val GoldAccent = Color(0xFFFFD600)
val EmeraldGreen = Color(0xFF00E676)
val CrimsonRed = Color(0xFFFF1744)
val WarningAmber = Color(0xFFF59E0B)

val OnSurfaceLight = Color(0xFFF8FAFC)
val OnSurfaceMuted = Color(0xFF94A3B8)
```

### 8.2 Typography: `Type.kt`
```kotlin
package com.vban.receptor.ui.theme

import androidx.compose.material3.Typography
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

val VbanTypography = Typography(
    headlineMedium = TextStyle(
        fontFamily = FontFamily.Monospace,
        fontWeight = FontWeight.Bold,
        fontSize = 24.sp
    ),
    titleLarge = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.ExtraBold,
        fontSize = 20.sp
    ),
    bodyLarge = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.Normal,
        fontSize = 15.sp
    ),
    bodyMedium = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.Normal,
        fontSize = 14.sp
    ),
    labelMedium = TextStyle(
        fontFamily = FontFamily.Monospace,
        fontWeight = FontWeight.Bold,
        fontSize = 12.sp
    ),
    labelSmall = TextStyle(
        fontFamily = FontFamily.Monospace,
        fontWeight = FontWeight.Normal,
        fontSize = 11.sp
    )
)
```

### 8.3 Theme: `Theme.kt`
```kotlin
package com.vban.receptor.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val AudiophileColorScheme = darkColorScheme(
    primary = CyanAccent,
    secondary = GoldAccent,
    background = DarkBackground,
    surface = SurfaceDark,
    surfaceVariant = SurfaceVariantDark,
    outline = OutlineDark,
    onBackground = OnSurfaceLight,
    onSurface = OnSurfaceLight,
    onSurfaceVariant = OnSurfaceMuted
)

@Composable
fun VbanReceptorTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = AudiophileColorScheme,
        typography = VbanTypography,
        content = content
    )
}
```

---

## 9. Implementation Checklist & File Placement Plan for Workers

The target implementation files strictly follow the layout specified in `PROJECT.md`:

| # | Target File Path | Purpose | Dependencies |
|---|------------------|---------|--------------|
| 1 | `app/src/main/java/com/vban/receptor/ui/theme/Color.kt` | OLED Dark Audiophile palette | Compose UI Graphics |
| 2 | `app/src/main/java/com/vban/receptor/ui/theme/Type.kt` | Monospace & telemetry typography | Compose Material3 |
| 3 | `app/src/main/java/com/vban/receptor/ui/theme/Theme.kt` | Material 3 dark color scheme | `Color.kt`, `Type.kt` |
| 4 | `app/src/main/java/com/vban/receptor/ui/VuMeter.kt` | 60 FPS Stereo VU Meter Canvas | `Theme.kt`, Compose Canvas |
| 5 | `app/src/main/java/com/vban/receptor/ui/BufferOccupancyGauge.kt` | Real-time buffer fill & watermark gauge | `Theme.kt`, Compose Canvas |
| 6 | `app/src/main/java/com/vban/receptor/ui/DeviceSelector.kt` | Audio device picker & bit-perfect badge | `Theme.kt`, Material3 Dropdown |
| 7 | `app/src/main/java/com/vban/receptor/ui/DashboardScreen.kt` | Main reactive audiophile control deck | All UI components, `NativeVbanEngine` |
| 8 | `app/src/main/java/com/vban/receptor/MainActivity.kt` | Edge-to-edge Activity, permissions, service dispatch | `DashboardScreen`, `VbanPlaybackService` |

---

## 10. Verification & Quality Assurance Strategy

1. **Canvas Frame Rate & Recomposition Hygiene**:
   - Ensure `drawWithCache` or cached `remember` is used for all `Brush` gradients.
   - Verify that `StereoVuMeter` runs at 60 FPS without GC allocation spikes via Android Studio Memory & Allocation Profiler.
2. **Unit Test Coverage**:
   - Test `linearToDbfs` conversion across edge cases ($0.0f$, $1.0f$, $10^{-6}f$, $>1.0f$).
   - Test buffer percentage clamping and latency estimation math across different sample rates ($44.1\text{ kHz}$ to $768\text{ kHz}$).
3. **Hardware / Device Verification**:
   - Verify hotplug device detection triggers `refreshAudioDevices()` when USB DACs are plugged or unplugged.
   - Verify that bit-perfect badges activate only when `AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT` is confirmed on Android 14+.
