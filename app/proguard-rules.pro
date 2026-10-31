# Proguard / R8 rules for VeuxTorch
-keep class com.example.veuxtorch.IFlashService** { *; }
-keep class com.example.veuxtorch.FlashNative { *; }
-keep class com.example.veuxtorch.FlashStatus { *; }
-keep class com.example.veuxtorch.FlashRootService { *; }
-keep class com.topjohnwu.superuser.** { *; }
-dontwarn com.topjohnwu.superuser.**
