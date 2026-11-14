package org.vita3k.emulator.data

/**
 * Ready-made sets of settings. They are starting points chosen for a phone with a fast GPU (Galaxy S22+ class),
 * not guarantees: every game behaves differently.
 */
enum class PerformanceProfile {
    Balanced,
    Performance,
    Quality,
    Battery;

    fun applyTo(cfg: EmulatorConfig) {
        cfg.shaderCache = true
        when (this) {
            Balanced -> {
                cfg.resolutionMultiplier = 1.0f
                cfg.anisotropicFiltering = 1
                cfg.vSync = true
                cfg.asyncPipelineCompilation = true
                cfg.accurateThreadScheduling = true
                cfg.highAccuracy = true
                cfg.guestCores = 3
            }
            Performance -> {
                cfg.resolutionMultiplier = 1.0f
                cfg.anisotropicFiltering = 1
                cfg.vSync = false
                cfg.asyncPipelineCompilation = true
                cfg.accurateThreadScheduling = false
                cfg.highAccuracy = false
                cfg.guestCores = 3
            }
            Quality -> {
                cfg.resolutionMultiplier = 2.0f
                cfg.anisotropicFiltering = 4
                cfg.vSync = true
                cfg.asyncPipelineCompilation = true
                cfg.accurateThreadScheduling = true
                cfg.highAccuracy = true
                cfg.guestCores = 3
                if (cfg.backendRenderer == "Vulkan") cfg.screenFilter = "FSR"
            }
            Battery -> {
                cfg.resolutionMultiplier = 1.0f
                cfg.anisotropicFiltering = 1
                cfg.vSync = true
                cfg.asyncPipelineCompilation = true
                cfg.accurateThreadScheduling = false
                cfg.highAccuracy = false
                cfg.guestCores = 2
            }
        }
    }

    fun matches(cfg: EmulatorConfig): Boolean {
        val probe = EmulatorConfig()
        probe.backendRenderer = cfg.backendRenderer
        applyTo(probe)
        return cfg.resolutionMultiplier == probe.resolutionMultiplier &&
            cfg.anisotropicFiltering == probe.anisotropicFiltering &&
            cfg.vSync == probe.vSync &&
            cfg.asyncPipelineCompilation == probe.asyncPipelineCompilation &&
            cfg.accurateThreadScheduling == probe.accurateThreadScheduling &&
            cfg.highAccuracy == probe.highAccuracy &&
            cfg.guestCores == probe.guestCores
    }

    companion object {
        /** The profile the settings currently match, or null for custom settings. */
        fun current(cfg: EmulatorConfig): PerformanceProfile? = entries.firstOrNull { it.matches(cfg) }
    }
}
