package dev.dartplant.dartplant_fixture

import android.content.Intent
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private var launchProbe: String? = null
    private var launchTest: String? = null

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

    private companion object {
        const val CHANNEL = "dev.dartplant.fixture/launch"
        const val EXTRA_PROBE = "dartplant_probe"
        const val EXTRA_TEST = "dartplant_test"
    }
}
