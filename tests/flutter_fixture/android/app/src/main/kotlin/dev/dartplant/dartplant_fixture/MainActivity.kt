package dev.dartplant.dartplant_fixture

import android.content.Intent
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private var launchProbe: String? = null

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        launchProbe = intent?.getStringExtra(EXTRA_PROBE)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler {
            call,
            result,
            ->
            when (call.method) {
                "launchProbe" -> result.success(launchProbe)
                else -> result.notImplemented()
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        launchProbe = intent.getStringExtra(EXTRA_PROBE)
    }

    private companion object {
        const val CHANNEL = "dev.dartplant.fixture/launch"
        const val EXTRA_PROBE = "dartplant_probe"
    }
}
