// Bridge phone app module (docs/BRIDGE_PHONE_DESIGN.md decision 13): applicationId
// app.kidpager.bridge, minSdk 26, targetSdk 35; Firebase Messaging only when
// google-services.json is present (BuildConfig.FCM), else the outbox is poll-only.
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.serialization)
    alias(libs.plugins.ksp)
}

val hasGoogleServices = file("google-services.json").exists()
// Default relay URL (Prefs.relayUrl): -PRELAY_URL=https://... or gradle.properties; else the prod host.
val relayUrl = (project.findProperty("RELAY_URL") as String?)?.trim()?.ifEmpty { null } ?: "https://kidpager.sps-by-the-numbers.com"
if (hasGoogleServices) {
    apply(plugin = "com.google.gms.google-services")
}

android {
    namespace = "app.kidpager.bridge"
    compileSdk = 35

    defaultConfig {
        applicationId = "app.kidpager.bridge"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
        buildConfigField("boolean", "FCM", hasGoogleServices.toString())
        buildConfigField("String", "DEFAULT_RELAY_URL", "\"$relayUrl\"")
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        viewBinding = true
        buildConfig = true
    }
    sourceSets {
        // A5: the real FirebaseMessagingService only when the Firebase config is present.
        getByName("main").java.srcDir(if (hasGoogleServices) "src/fcm/java" else "src/nofcm/java")
    }
    testOptions {
        unitTests.isReturnDefaultValues = true
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.service)
    implementation(libs.androidx.work.runtime.ktx)
    implementation(libs.androidx.room.runtime)
    implementation(libs.androidx.room.ktx)
    ksp(libs.androidx.room.compiler)
    implementation(libs.androidx.security.crypto)
    implementation(libs.okhttp)
    implementation(libs.kotlinx.serialization.json)
    if (hasGoogleServices) {
        implementation(platform(libs.firebase.bom))
        implementation(libs.firebase.messaging)
    }
    testImplementation(libs.junit)
}
