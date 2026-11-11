package app.droidondarwin.fixture;

import android.app.Activity;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;
import java.util.UUID;
import org.json.JSONObject;

public final class MainActivity extends Activity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String result;
        try {
            result = check().toString();
            Log.i("DroidApkFixture", result);
        } catch (Throwable error) {
            result = "{\"status\":\"failed\",\"error\":" + JSONObject.quote(error.toString()) + "}";
            Log.e("DroidApkFixture", result, error);
        }
        TextView output = new TextView(this);
        output.setText(result);
        output.setTextIsSelectable(true);
        output.setPadding(24, 24, 24, 24);
        setContentView(output);
    }

    private JSONObject check() throws Exception {
        int value = BuildConfig.USE_JNI ? NativeBridge.value() : 100 + BuildConfig.VERSION;
        if (value != 100 + BuildConfig.VERSION) {
            throw new IllegalStateException("Unexpected implementation value: " + value);
        }
        SharedPreferences preferences = getSharedPreferences("fixture", MODE_PRIVATE);
        String marker = preferences.getString("marker", null);
        boolean created = marker == null;
        int createdVersion = preferences.getInt("created_version", 0);
        if (created) {
            marker = UUID.randomUUID().toString();
            createdVersion = BuildConfig.VERSION;
        } else if (createdVersion < 1) {
            throw new IllegalStateException("Private data has no creation version");
        }
        int launches = preferences.getInt("launch_count", 0) + 1;
        // 更新验证比较两次输出的随机标识；同步提交使输出发生在数据写入之后。
        if (!preferences.edit().putString("marker", marker).putInt("created_version", createdVersion)
                .putInt("launch_count", launches).commit()) {
            throw new IllegalStateException("Private data commit failed");
        }
        return new JSONObject().put("status", "passed").put("package", getPackageName())
                .put("mode", BuildConfig.USE_JNI ? "jni" : "java").put("version", BuildConfig.VERSION)
                .put("value", value).put("marker", marker).put("created_now", created)
                .put("created_version", createdVersion).put("launch_count", launches);
    }
}
