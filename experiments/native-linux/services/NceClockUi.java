/* SPDX-License-Identifier: GPL-2.0-or-later */
import android.accessibilityservice.AccessibilityServiceInfo;
import android.app.UiAutomation;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.os.HandlerThread;
import android.os.Looper;
import android.view.accessibility.AccessibilityNodeInfo;
import java.util.List;
import java.util.HashSet;

// A drawn Activity can still display the application's own splash screen.
// Check controls from the pinned Clock APK before accepting its screenshot.
final class NceClockUi implements AutoCloseable {
    private final HandlerThread callbacks = new HandlerThread("nce-ui-check");
    private UiAutomation automation;

    NceClockUi() throws Exception {
        // app_process does not create the main Looper that accessibility uses.
        if (Looper.getMainLooper() == null) Looper.prepareMainLooper();
        callbacks.start();
        boolean connected = false;
        try {
            Class<?> contract = Class.forName("android.app.IUiAutomationConnection");
            Object connection = Class.forName("android.app.UiAutomationConnection")
                    .getConstructor().newInstance();
            automation = (UiAutomation) UiAutomation.class.getConstructor(Looper.class, contract)
                    .newInstance(callbacks.getLooper(), connection);
            UiAutomation.class.getMethod("connect", int.class).invoke(automation,
                    UiAutomation.FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES);
            connected = true;
            AccessibilityServiceInfo info = automation.getServiceInfo();
            info.flags |= AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS;
            automation.setServiceInfo(info);
        } catch (Exception error) {
            if (connected) {
                try {
                    UiAutomation.class.getMethod("disconnect").invoke(automation);
                } catch (Exception cleanup) {
                    error.addSuppressed(cleanup);
                }
            }
            callbacks.quitSafely();
            callbacks.join();
            throw error;
        }
    }

    // A null bitmap checks the view tree before starting a display capture.
    boolean ready(Bitmap pixels) {
        AccessibilityNodeInfo root = automation.getRootInActiveWindow();
        if (root == null) return false;
        try {
            if (!"com.google.android.deskclock".equals(String.valueOf(root.getPackageName()))) return false;
            return visible(root, "tab_menu_alarm", pixels) && visible(root, "tab_menu_timer", pixels);
        } finally {
            root.recycle();
        }
    }

    private static boolean visible(AccessibilityNodeInfo root, String id, Bitmap pixels) {
        List<AccessibilityNodeInfo> nodes = root.findAccessibilityNodeInfosByViewId(
                "com.google.android.deskclock:id/" + id);
        boolean found = false;
        for (AccessibilityNodeInfo node : nodes) {
            if (node.isVisibleToUser() && node.isEnabled()) {
                if (pixels == null) {
                    found = true;
                } else {
                    Rect bounds = new Rect();
                    node.getBoundsInScreen(bounds);
                    if (bounds.intersect(0, 0, pixels.getWidth(), pixels.getHeight())) {
                        HashSet<Integer> colors = new HashSet<>();
                        for (int y = bounds.top; y < bounds.bottom && colors.size() < 8; ++y)
                            for (int x = bounds.left; x < bounds.right && colors.size() < 8; ++x)
                                colors.add(pixels.getPixel(x, y));
                        found |= colors.size() >= 8;
                    }
                }
            }
            node.recycle();
        }
        return found;
    }

    @Override public void close() throws Exception {
        try {
            UiAutomation.class.getMethod("disconnect").invoke(automation);
        } finally {
            callbacks.quitSafely();
            callbacks.join();
        }
    }
}
