package dev.dartplant.dartplant_fixture

import android.app.Application
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import io.flutter.FlutterInjector
import io.flutter.embedding.engine.FlutterJNI
import io.flutter.embedding.engine.deferredcomponents.DeferredComponentManager
import io.flutter.embedding.engine.systemchannels.DeferredComponentChannel
import java.io.File
import java.util.zip.ZipFile

class FixtureApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        Log.i(
            "DartPlantDeferred",
            "application_on_create t=${SystemClock.elapsedRealtimeNanos()} " +
                "thread=${Thread.currentThread().name}/${Thread.currentThread().id}",
        )
        FlutterInjector.setInstance(
            FlutterInjector.Builder()
                .setDeferredComponentManager(LocalDeferredComponentManager(this))
                .build(),
        )
    }
}

private class LocalDeferredComponentManager(private val context: Context) :
    DeferredComponentManager {
    companion object {
        private const val TAG = "DartPlantDeferred"
        private const val LOADING_UNIT_MAPPING =
            "io.flutter.embedding.engine.deferredcomponents.DeferredComponentManager.loadingUnitMapping"
    }

    private var flutterJNI: FlutterJNI? = null
    private var channel: DeferredComponentChannel? = null
    private val mainHandler = Handler(Looper.getMainLooper())

    private fun trace(event: String, details: String = "") {
        val thread = Thread.currentThread()
        Log.i(
            TAG,
            "event=$event t=${SystemClock.elapsedRealtimeNanos()} " +
                "thread=${thread.name}/${thread.id} main=${Looper.myLooper() == Looper.getMainLooper()} " +
                details,
        )
    }

    private fun deferredMappings(): String = try {
        File("/proc/self/maps")
            .useLines { lines ->
                lines.filter { line ->
                    line.contains("libapp.so-", ignoreCase = false) ||
                        line.contains("split_deferred_probe", ignoreCase = false)
                }.take(24).toList()
            }
            .joinToString(" | ")
            .ifEmpty { "<none>" }
    } catch (error: Throwable) {
        "<maps-error:${error.javaClass.simpleName}:${error.message}>"
    }

    private fun scheduleMappingTrace(loadingUnitId: Int, delayMillis: Long) {
        mainHandler.postDelayed(
            {
                trace(
                    "maps_after_flutter_jni",
                    "unit=$loadingUnitId delay_ms=$delayMillis mappings=${deferredMappings()}",
                )
            },
            delayMillis,
        )
    }
    private val componentNames: Map<Int, String> by lazy {
        val applicationInfo = context.packageManager.getApplicationInfo(
            context.packageName,
            PackageManager.GET_META_DATA,
        )
        val rawMapping = applicationInfo.metaData?.getString(LOADING_UNIT_MAPPING).orEmpty()
        val resolved = buildMap {
            for (entry in rawMapping.split(',')) {
                if (entry.isBlank()) continue
                val fields = entry.split(':')
                val loadingUnitId = fields.firstOrNull()?.toIntOrNull() ?: continue
                val component = fields.getOrNull(1) ?: continue
                put(loadingUnitId, component)
            }
        }
        trace(
            "loading_unit_mapping",
            "raw=$rawMapping parsed=$resolved source=${applicationInfo.sourceDir} " +
                "split_names=${applicationInfo.splitNames?.contentToString()} " +
                "split_sources=${applicationInfo.splitSourceDirs?.contentToString()}",
        )
        resolved
    }

    private fun resolveComponentName(loadingUnitId: Int, componentName: String?): String =
        componentName ?: componentNames[loadingUnitId]
        ?: error("component name for loading unit $loadingUnitId is unavailable")

    override fun setJNI(flutterJNI: FlutterJNI) {
        this.flutterJNI = flutterJNI
        trace(
            "set_jni",
            "jni=${System.identityHashCode(flutterJNI)} attached=${flutterJNI.isAttached}",
        )
    }

    override fun setDeferredComponentChannel(channel: DeferredComponentChannel) {
        this.channel = channel
        trace("set_channel", "channel=${System.identityHashCode(channel)}")
    }

    override fun installDeferredComponent(loadingUnitId: Int, componentName: String?) {
        trace(
            "install_enter",
            "unit=$loadingUnitId component_arg=$componentName mappings=${deferredMappings()}",
        )
        val resolvedComponentName = resolveComponentName(loadingUnitId, componentName)
        trace(
            "install_resolved",
            "unit=$loadingUnitId component=$resolvedComponentName",
        )
        loadDartLibrary(loadingUnitId, resolvedComponentName)
        trace(
            "install_after_load_dart_library",
            "unit=$loadingUnitId component=$resolvedComponentName mappings=${deferredMappings()}",
        )
        channel?.completeInstallSuccess(resolvedComponentName)
        trace(
            "install_after_channel_success",
            "unit=$loadingUnitId component=$resolvedComponentName",
        )
    }

    override fun getDeferredComponentInstallState(
        loadingUnitId: Int,
        componentName: String?,
    ): String {
        trace(
            "get_install_state",
            "unit=$loadingUnitId component_arg=$componentName result=installedPendingLoad",
        )
        return "installedPendingLoad"
    }

    override fun loadAssets(loadingUnitId: Int, componentName: String?) {
        trace("load_assets", "unit=$loadingUnitId component_arg=$componentName")
    }

    override fun loadDartLibrary(loadingUnitId: Int, componentName: String?) {
        val resolvedComponentName = resolveComponentName(loadingUnitId, componentName)
        val libraryName = "libapp.so-$loadingUnitId.part.so"
        val abi = Build.SUPPORTED_ABIS.firstOrNull { it == "arm64-v8a" }
            ?: Build.SUPPORTED_ABIS.first()
        val applicationInfo = context.applicationInfo
        trace(
            "load_dart_enter",
            "unit=$loadingUnitId component=$resolvedComponentName abi=$abi " +
                "source=${applicationInfo.sourceDir} split_names=${applicationInfo.splitNames?.contentToString()} " +
                "split_sources=${applicationInfo.splitSourceDirs?.contentToString()} " +
                "jni=${flutterJNI?.let(System::identityHashCode)} attached=${flutterJNI?.isAttached} " +
                "mappings=${deferredMappings()}",
        )
        val featureSplit = context.applicationInfo.splitSourceDirs
            ?.firstOrNull { path -> path.contains(resolvedComponentName) }
            ?: error("installed split for deferred component $resolvedComponentName was not found")
        // The feature split is pre-installed by the ARM64 fixture harness but
        // its AOT image remains unmapped until Dart calls loadLibrary(). The
        // engine explicitly supports uncompressed apk!lib/... dlopen paths.
        val apkPath = "$featureSplit!lib/$abi/$libraryName"
        val splitFile = File(featureSplit)
        ZipFile(splitFile).use { zip ->
            val entryName = "lib/$abi/$libraryName"
            val entry = zip.getEntry(entryName)
                ?: error("installed split has no deferred AOT entry $entryName")
            trace(
                "load_dart_apk_entry",
                "unit=$loadingUnitId split=$featureSplit split_exists=${splitFile.isFile} " +
                    "split_size=${splitFile.length()} entry=$entryName method=${entry.method} " +
                    "size=${entry.size} compressed_size=${entry.compressedSize} crc=${entry.crc} " +
                    "search_path=$apkPath",
            )
        }
        val jni = flutterJNI
            ?: error("FlutterJNI is unavailable for loading unit $loadingUnitId")
        trace(
            "before_flutter_jni_load",
            "unit=$loadingUnitId path=$apkPath mappings=${deferredMappings()}",
        )
        try {
            jni.loadDartDeferredLibrary(
                loadingUnitId,
                // Match Flutter's PlayStoreDeferredComponentManager ordering.
                // Native code pops from the end, so it tries the explicit APK
                // path first and falls back to the bare soname. Android's app
                // linker namespace already includes the installed feature
                // split's native-library directory, and some devices only
                // resolve the deferred image through that namespace path.
                arrayOf(libraryName, apkPath),
            )
        } catch (error: Throwable) {
            trace(
                "flutter_jni_load_throw",
                "unit=$loadingUnitId error=${error.javaClass.name}:${error.message} " +
                    "mappings=${deferredMappings()}",
            )
            throw error
        }
        val mappingsAfterFlutterJni = deferredMappings()
        trace(
            "after_flutter_jni_load",
            "unit=$loadingUnitId mappings=$mappingsAfterFlutterJni",
        )
        for (delay in longArrayOf(50L, 250L, 1000L, 3000L, 8000L)) {
            scheduleMappingTrace(loadingUnitId, delay)
        }
    }

    override fun uninstallDeferredComponent(
        loadingUnitId: Int,
        componentName: String?,
    ): Boolean {
        trace("uninstall", "unit=$loadingUnitId component_arg=$componentName result=false")
        return false
    }

    override fun destroy() {
        trace("destroy")
        channel = null
        flutterJNI = null
    }
}
