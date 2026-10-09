// Bridge phone app (docs/BRIDGE_PHONE_DESIGN.md decision 13): AGP 8.7.x + Kotlin 2.0.x, JDK 17.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.android) apply false
    alias(libs.plugins.kotlin.serialization) apply false
    alias(libs.plugins.ksp) apply false
    alias(libs.plugins.google.services) apply false
}
