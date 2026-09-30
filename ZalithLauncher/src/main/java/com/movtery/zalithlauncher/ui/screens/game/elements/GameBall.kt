/*
 * Zalith Launcher 2
 * Copyright (C) 2025 MovTery <movtery228@qq.com> and contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/gpl-3.0.txt>.
 */

package com.movtery.zalithlauncher.ui.screens.game.elements

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.AnimatedVisibilityScope
import androidx.compose.animation.Crossfade
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.expandIn
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkOut
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentSize
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import com.movtery.zalithlauncher.R
import com.movtery.zalithlauncher.ui.components.FloatingBall
import com.movtery.zalithlauncher.ui.screens.content.elements.MemoryPreview
import com.movtery.zalithlauncher.ui.screens.game.GamePerformanceSnapshot
import kotlin.math.abs

private val PerformanceGreen = Color(0xFF4CAF50)
private val PerformanceYellow = Color(0xFFFFC107)
private val PerformanceOrange = Color(0xFFFF9800)
private val PerformanceRed = Color(0xFFF44336)

@Composable
fun DraggableGameBall(
    position: Offset,
    onPositionChanged: (Offset) -> Unit,
    onSavePos: () -> Unit,
    gamePerformance: GamePerformanceSnapshot?,
    showMemory: Boolean,
    opened: Boolean,
    alpha: Float = 1f,
    onClick: () -> Unit = {}
) {
    FloatingBall(
        modifier = Modifier.focusProperties {
            canFocus = false
        },
        position = position,
        onPositionChanged = onPositionChanged,
        onSavePos = onSavePos,
        onClick = onClick,
        alpha = alpha
    ) {
        GameBallContent(
            gamePerformance = gamePerformance,
            showMemory = showMemory,
            opened = opened,
        )
    }
}

@Composable
private fun GameBallContent(
    gamePerformance: GamePerformanceSnapshot?,
    showMemory: Boolean,
    opened: Boolean,
) {
    val showPerformance = gamePerformance != null

    Row(
        modifier = Modifier.padding(all = 2.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier.size(28.dp),
            contentAlignment = Alignment.Center
        ) {
            Crossfade(opened) { state ->
                Icon(
                    modifier = Modifier.size(24.dp),
                    painter = painterResource(
                        if (state) {
                            R.drawable.ic_menu_open
                        } else {
                            R.drawable.ic_menu
                        }
                    ),
                    contentDescription = null
                )
            }
        }

        AnimatedVisibility(
            visible = showPerformance || showMemory
        ) {
            Spacer(Modifier.width(4.dp))
        }

        Column(
            modifier = Modifier
                .wrapContentSize()
                .animateContentSize()
        ) {
            CustomAnimatedVisibility(
                visible = showPerformance || showMemory
            ) {
                Spacer(Modifier.height(4.dp))
            }

            CustomAnimatedVisibility(
                visible = showPerformance
            ) {
                gamePerformance?.let { snapshot ->
                    PerformanceMetrics(snapshot)
                }
            }

            CustomAnimatedVisibility(
                visible = showMemory
            ) {
                MemoryPreview(
                    modifier = Modifier
                        .width(168.dp)
                        .padding(end = 4.dp),
                    mainColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.6f),
                    backgroundColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.6f),
                    textStyle = MaterialTheme.typography.labelSmall,
                    dynamicColor = true,
                    usedText = { usedMemory, totalMemory ->
                        "${usedMemory.toInt()}MB/${totalMemory.toInt()}MB"
                    }
                )
            }

            CustomAnimatedVisibility(
                visible = showPerformance || showMemory
            ) {
                Spacer(Modifier.height(4.dp))
            }
        }
    }
}

@Composable
private fun PerformanceMetrics(
    snapshot: GamePerformanceSnapshot
) {
    val current = snapshot.current
    val previous = snapshot.previous

    Row(
        modifier = Modifier.padding(end = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        PerformanceMetric(
            label = "FPS",
            value = current.fps.toString(),
            color = fpsColor(current.fps),
            direction = direction(
                current.fps.toDouble(),
                previous?.fps?.toDouble()
            )
        )

        MetricSeparator()

        PerformanceMetric(
            label = "Min",
            value = current.minFps.toString(),
            color = fpsColor(current.minFps),
            direction = direction(
                current.minFps.toDouble(),
                previous?.minFps?.toDouble()
            )
        )

        MetricSeparator()

        PerformanceMetric(
            label = "Med",
            value = current.averageFps.toString(),
            color = fpsColor(current.averageFps),
            direction = direction(
                current.averageFps.toDouble(),
                previous?.averageFps?.toDouble()
            )
        )

        MetricSeparator()

        PerformanceMetric(
            label = "Max",
            value = current.maxFps.toString(),
            color = fpsColor(current.maxFps),
            direction = direction(
                current.maxFps.toDouble(),
                previous?.maxFps?.toDouble()
            )
        )

        MetricSeparator()

        PerformanceMetric(
            label = "Frame",
            value = formatFrameTime(current.frameTimeMs),
            color = frameTimeColor(current.frameTimeMs),
            direction = direction(
                current.frameTimeMs,
                previous?.frameTimeMs
            ),
            suffix = " ms"
        )
    }
}

@Composable
private fun PerformanceMetric(
    label: String,
    value: String,
    color: Color,
    direction: Int,
    suffix: String = ""
) {
    Box(
        modifier = Modifier
            .background(
                color = color.copy(alpha = 0.16f),
                shape = RoundedCornerShape(7.dp)
            )
            .padding(
                horizontal = 5.dp,
                vertical = 2.dp
            )
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = label,
                color = color,
                style = MaterialTheme.typography.labelSmall
            )

            Spacer(Modifier.width(3.dp))

            Text(
                text = value + suffix,
                color = color,
                style = MaterialTheme.typography.labelSmall
            )

            if (direction != 0) {
                Spacer(Modifier.width(2.dp))

                Text(
                    text = if (direction > 0) "↑" else "↓",
                    color = color,
                    style = MaterialTheme.typography.labelSmall
                )
            }
        }
    }
}

@Composable
private fun MetricSeparator() {
    Text(
        text = "●",
        modifier = Modifier.padding(horizontal = 2.dp),
        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.35f),
        style = MaterialTheme.typography.labelSmall
    )
}

private fun direction(
    current: Double,
    previous: Double?
): Int {
    if (previous == null) return 0

    return when {
        current > previous -> 1
        current < previous -> -1
        else -> 0
    }
}

private fun fpsColor(fps: Int): Color {
    return when {
        fps >= 60 -> PerformanceGreen
        fps >= 45 -> PerformanceYellow
        fps >= 30 -> PerformanceOrange
        else -> PerformanceRed
    }
}

private fun frameTimeColor(frameTimeMs: Double): Color {
    return when {
        frameTimeMs <= 0.0 -> PerformanceRed
        frameTimeMs <= 16.7 -> PerformanceGreen
        frameTimeMs <= 22.2 -> PerformanceYellow
        frameTimeMs <= 33.3 -> PerformanceOrange
        else -> PerformanceRed
    }
}

private fun formatFrameTime(frameTimeMs: Double): String {
    if (frameTimeMs <= 0.0) return "0.0"

    val rounded = kotlin.math.round(frameTimeMs * 10.0) / 10.0
    return rounded.toString()
}

@Composable
private fun ColumnScope.CustomAnimatedVisibility(
    visible: Boolean,
    content: @Composable (AnimatedVisibilityScope.() -> Unit)
) {
    AnimatedVisibility(
        visible = visible,
        enter = expandIn(expandFrom = Alignment.CenterStart) + fadeIn(),
        exit = shrinkOut(shrinkTowards = Alignment.CenterStart) + fadeOut(),
        content = content
    )
}
