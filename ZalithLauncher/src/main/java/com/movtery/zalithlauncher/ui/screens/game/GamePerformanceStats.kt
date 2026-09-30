package com.movtery.zalithlauncher.ui.screens.game

data class GamePerformanceStats(
    val fps: Int,
    val minFps: Int,
    val averageFps: Int,
    val maxFps: Int,
    val frameTimeMs: Double
) {
    companion object {
        fun fromNative(values: LongArray): GamePerformanceStats {
            require(values.size >= 5)

            return GamePerformanceStats(
                fps = values[0].toInt(),
                minFps = values[1].toInt(),
                averageFps = values[2].toInt(),
                maxFps = values[3].toInt(),
                frameTimeMs = values[4] / 1000.0
            )
        }
    }
}

data class GamePerformanceSnapshot(
    val current: GamePerformanceStats,
    val previous: GamePerformanceStats?
)
