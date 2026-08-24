# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# Keep line number information for readable crash stack traces, but hide the
# original source file name.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile

# ---- Rhino JS engine (plugin system: PluginManager.kt) ----
# Rhino does extensive reflection and dynamic class loading internally
# (LiveConnect, interpreter/bytecode generation) that R8's static analysis
# can't see, so it's kept whole rather than risking a stripped/renamed
# class it loads by name at runtime.
-keep class org.mozilla.javascript.** { *; }
-dontwarn org.mozilla.javascript.**

# ---- BouncyCastle (AirPlay 2 pairing/crypto, RAOP encryption) ----
# JCE providers and algorithms are looked up by string name via
# Security.addProvider()/Cipher.getInstance(...), not by a direct reference
# R8 can trace - keep the provider whole.
-keep class org.bouncycastle.** { *; }
-dontwarn org.bouncycastle.**

# ---- Ktor / OkHttp / Okio (AudioCastService's HTTP/WebSocket client) ----
-dontwarn io.ktor.**
-keepclassmembers class io.ktor.** { volatile <fields>; }
-dontwarn okhttp3.**
-dontwarn okio.**

# ---- kotlinx.serialization (@Serializable classes, e.g. TrackMetadata) ----
-keepattributes *Annotation*, InnerClasses
-keepclasseswithmembers class com.aria.ariacast.** {
    kotlinx.serialization.KSerializer serializer(...);
}
-keep,includedescriptorclasses class com.aria.ariacast.**$$serializer { *; }
-keepclassmembers class com.aria.ariacast.** {
    *** Companion;
}

# ---- JmDNS (RaopDiscovery.kt) ----
-keep class javax.jmdns.** { *; }
-dontwarn javax.jmdns.**
