package dev.dartplant.integration;

import android.util.Log;
import android.os.Build;
import io.github.libxposed.api.XposedInterface.HookHandle;
import io.github.libxposed.api.XposedModule;
import io.github.libxposed.api.XposedModuleInterface.HotReloadingParam;
import io.github.libxposed.api.XposedModuleInterface.ModuleLoadedParam;
import io.github.libxposed.api.XposedModuleInterface.PackageReadyParam;
import java.lang.reflect.Method;

/** Test-only module; only the declared Flutter fixture is in scope. */
public final class DartPlantModule extends XposedModule {
    private static final String TAG = "DartPlantModule";
    private static final String TARGET = "dev.dartplant.dartplant_fixture";
    private static final int CALLBACK_ABI_VERSION = 2;
    private HookHandle javaProbeHook;
    private boolean loaded;

    private static native long nativeStatus();
    private static native long nativeBootstrapEntry();
    private static native long nativeCountsEntry();
    private static native long nativeRetireEntry();
    private static native long nativeMappingEntry();
    private static native long nativeExceptionEntry();
    private static native long nativeObjectRootEntry();
    private static native long nativeLoaderDrainEntry();
    private static native long nativeTypeArgsPrepareEntry();
    private static native long nativeTypeArgsProbeEntry();
    private static native long nativeP6InstallEntry();
    private static native long nativeP6ProbeEntry();
    private static native long nativeClosureInstallEntry();
    private static native long nativeClosureProbeEntry();
    private static native long nativeOrdinaryInstallEntry();
    private static native long nativeOrdinaryMarkSharedEntry();
    private static native long nativeOrdinaryProbeEntry();
    private static native long nativeInitTranslatedDobby();
    private static native long nativeInitNativeStrictDobby();

    @Override
    public void onModuleLoaded(ModuleLoadedParam param) {
        if (!TARGET.equals(param.getProcessName())) return;
        try {
            System.loadLibrary("dartplant");
            loaded = nativeStatus() == 1;
            // Android NativeBridge translates ARM64 guest code in an x86_64
            // process. Vector's x86_64 do_dlopen hook cannot see guest dlopen
            // and its x86_64 Dobby backend cannot patch an ARM64 guest entry.
            // Keep API 102 Java lifecycle, but select exact ARM64 Dobby only
            // for this AVD-specific physical Hook backend. Native v2 remains
            // mandatory for the later real ARM64 LSPosed/Vector matrix.
            if (!loaded && Build.SUPPORTED_64_BIT_ABIS.length > 0
                    && "x86_64".equals(Build.SUPPORTED_64_BIT_ABIS[0])) {
                loaded = nativeInitTranslatedDobby() == 1;
            }
            if (!loaded && BuildConfig.NATIVE_STRICT_FALLBACK
                    && Build.SUPPORTED_64_BIT_ABIS.length > 0
                    && "arm64-v8a".equals(Build.SUPPORTED_64_BIT_ABIS[0])) {
                // Test-only explicit fallback. Native v2 unhook preflight
                // failure remains a separate failed host gate, never reported
                // as native-v2 success.
                loaded = nativeInitNativeStrictDobby() == 1;
            }
            Log.i(TAG, "DARTPLANT_HOST {event=module_loaded, native_ready=" + loaded
                    + ", process=" + param.getProcessName() + "}");
        } catch (Throwable t) {
            Log.e(TAG, "DARTPLANT_HOST {event=module_loaded, native_ready=false}", t);
        }
    }

    @Override
    public void onPackageReady(PackageReadyParam param) {
        if (!loaded || !TARGET.equals(param.getPackageName())) return;
        try {
            Class<?> target = param.getClassLoader().loadClass(
                    "dev.dartplant.dartplant_fixture.ExternalModuleProbe");
            int callbackAbi =
                    (Integer) target.getDeclaredMethod("callbackAbiVersion").invoke(null);
            if (callbackAbi != CALLBACK_ABI_VERSION) {
                throw new IllegalStateException(
                        "callback ABI mismatch module=" + CALLBACK_ABI_VERSION
                                + " fixture=" + callbackAbi);
            }
            Method method = target.getDeclaredMethod("value");
            javaProbeHook = hook(method).intercept(chain ->
                    ((Integer) chain.proceed()) + 100);
            target.getDeclaredMethod("registerCallbacks", long.class, long.class, long.class,
                            long.class, long.class, long.class, long.class, long.class, long.class,
                            long.class, long.class, long.class, long.class, long.class, long.class,
                            long.class)
                    .invoke(null, nativeBootstrapEntry(), nativeCountsEntry(),
                            nativeRetireEntry(), nativeMappingEntry(), nativeExceptionEntry(),
                            nativeObjectRootEntry(), nativeLoaderDrainEntry(),
                            nativeTypeArgsPrepareEntry(), nativeTypeArgsProbeEntry(),
                            nativeP6InstallEntry(), nativeP6ProbeEntry(),
                            nativeClosureInstallEntry(), nativeClosureProbeEntry(),
                            nativeOrdinaryInstallEntry(), nativeOrdinaryMarkSharedEntry(),
                            nativeOrdinaryProbeEntry());
            Log.i(TAG, "DARTPLANT_HOST {event=package_ready, hook=installed, "
                    + "bootstrap=registered, callback_abi=" + callbackAbi + "}");
        } catch (Throwable t) {
            Log.e(TAG, "DARTPLANT_HOST {event=package_ready, hook=failed}", t);
        }
    }

    @Override
    public boolean onHotReloading(HotReloadingParam param) {
        // The Native API v2 callback is not unregisterable. Until a complete
        // generation drain protocol is implemented, reject hot reload without
        // altering the current generation's hooks or execution state.
        Log.i(TAG, "DARTPLANT_HOST {event=hot_reload, result=refused, "
                + "reason=native_callback_lifetime}");
        return false;
    }
}
