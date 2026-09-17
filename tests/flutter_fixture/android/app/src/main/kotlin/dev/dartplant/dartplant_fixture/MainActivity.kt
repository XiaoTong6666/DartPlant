package dev.dartplant.dartplant_fixture

import android.content.Intent
import io.flutter.FlutterInjector
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.dart.DartExecutor
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private var launchProbe: String? = null
    private var launchTest: String? = null
    private var secondaryEngine: FlutterEngine? = null
    private var secondaryChannel: MethodChannel? = null
    private var pendingSecondaryReady: MethodChannel.Result? = null
    private var secondaryIncarnation = 0

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        launchProbe = intent?.getStringExtra(EXTRA_PROBE)
        launchTest = intent?.getStringExtra(EXTRA_TEST)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler {
            call,
            result,
            ->
            when (call.method) {
                "launchProbe" -> result.success(launchProbe)
                "launchTest" -> result.success(launchTest)
                "multiOwnerStart" -> startSecondaryEngine(result)
                "multiOwnerCommand" -> {
                    val command = call.argument<String>("command")
                    if (command.isNullOrEmpty()) {
                        result.error("bad-command", "secondary command is missing", null)
                    } else {
                        invokeSecondary(command, call.arguments, result)
                    }
                }
                "multiOwnerDestroy" -> {
                    destroySecondaryEngine()
                    result.success(true)
                }
                "multiOwnerRecreate" -> {
                    destroySecondaryEngine()
                    startSecondaryEngine(result)
                }
                else -> result.notImplemented()
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        launchProbe = intent.getStringExtra(EXTRA_PROBE)
        launchTest = intent.getStringExtra(EXTRA_TEST)
    }

    override fun onDestroy() {
        destroySecondaryEngine()
        super.onDestroy()
    }

    private fun startSecondaryEngine(result: MethodChannel.Result) {
        if (secondaryEngine != null || pendingSecondaryReady != null) {
            result.error("already-started", "secondary FlutterEngine already exists", null)
            return
        }
        secondaryIncarnation += 1
        pendingSecondaryReady = result
        val engine = FlutterEngine(applicationContext)
        val channel = MethodChannel(engine.dartExecutor.binaryMessenger, SECONDARY_CHANNEL)
        channel.setMethodCallHandler { call, secondaryResult ->
            when (call.method) {
                "ready" -> {
                    secondaryResult.success(true)
                    pendingSecondaryReady?.success(
                        mapOf("incarnation" to secondaryIncarnation),
                    )
                    pendingSecondaryReady = null
                }
                else -> secondaryResult.notImplemented()
            }
        }
        secondaryEngine = engine
        secondaryChannel = channel
        try {
            val appBundlePath =
                FlutterInjector.instance().flutterLoader().findAppBundlePath()
            engine.dartExecutor.executeDartEntrypoint(
                DartExecutor.DartEntrypoint(appBundlePath, SECONDARY_ENTRYPOINT),
            )
        } catch (error: Throwable) {
            pendingSecondaryReady = null
            secondaryChannel = null
            secondaryEngine = null
            engine.destroy()
            result.error(
                "engine-start-failed",
                "secondary FlutterEngine failed to start: ${error.javaClass.name}: ${error.message}",
                null,
            )
        }
    }

    private fun invokeSecondary(
        command: String,
        arguments: Any?,
        result: MethodChannel.Result,
    ) {
        val channel = secondaryChannel
        if (secondaryEngine == null || channel == null || pendingSecondaryReady != null) {
            result.error("not-ready", "secondary FlutterEngine is not ready", null)
            return
        }
        channel.invokeMethod(
            command,
            arguments,
            object : MethodChannel.Result {
                override fun success(value: Any?) = result.success(value)

                override fun error(code: String, message: String?, details: Any?) =
                    result.error(code, message, details)

                override fun notImplemented() = result.notImplemented()
            },
        )
    }

    private fun destroySecondaryEngine() {
        pendingSecondaryReady?.error(
            "engine-destroyed",
            "secondary FlutterEngine was destroyed before becoming ready",
            null,
        )
        pendingSecondaryReady = null
        secondaryChannel?.setMethodCallHandler(null)
        secondaryChannel = null
        secondaryEngine?.destroy()
        secondaryEngine = null
    }

    private companion object {
        const val CHANNEL = "dev.dartplant.fixture/launch"
        const val SECONDARY_CHANNEL = "dev.dartplant.fixture/secondary"
        const val SECONDARY_ENTRYPOINT = "secondaryEngineMain"
        const val EXTRA_PROBE = "dartplant_probe"
        const val EXTRA_TEST = "dartplant_test"
    }
}
