/* SPDX-License-Identifier: GPL-2.0-or-later */
import android.graphics.Canvas;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.content.ComponentName;
import android.os.IBinder;
import android.hardware.HardwareBuffer;
import android.os.SystemClock;
import android.view.Surface;
import android.view.SurfaceControl;
import java.io.FileOutputStream;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.HashSet;

public final class NceSurface {
    private static boolean activityDrawn(IBinder manager, ComponentName activity) throws Exception {
        String path = "/data/nce/activity-state.txt";
        try (FileOutputStream output = new FileOutputStream(path)) {
            manager.dump(output.getFD(), new String[]{"-a", activity.flattenToString()});
        }
        String state = new String(Files.readAllBytes(Paths.get(path)), java.nio.charset.StandardCharsets.UTF_8);
        return state.contains("reportedDrawn=true") && state.contains("firstWindowDrawn=true")
                && !state.contains("startingDisplayed=true");
    }

    private static String focusedWindow(IBinder window) throws Exception {
        String path = "/data/nce/window-state.txt";
        try (FileOutputStream output = new FileOutputStream(path)) {
            window.dump(output.getFD(), new String[]{"displays"});
        }
        for (String line : Files.readAllLines(Paths.get(path))) {
            if (line.trim().startsWith("mCurrentFocus=")) return line.trim();
        }
        return "no focused window";
    }

    private static Bitmap capture(IBinder display) throws Exception {
        Class<?> capture = Class.forName("android.window.ScreenCapture");
        Class<?> argsType = Class.forName("android.window.ScreenCapture$DisplayCaptureArgs");
        Class<?> builderType = Class.forName("android.window.ScreenCapture$DisplayCaptureArgs$Builder");
        Object builder = builderType.getConstructor(IBinder.class).newInstance(display);
        Object args = builderType.getMethod("build").invoke(builder);
        Object result = capture.getMethod("captureDisplay", argsType).invoke(null, args);
        if (result == null) throw new AssertionError("SurfaceFlinger capture failed");
        HardwareBuffer buffer = (HardwareBuffer) result.getClass().getMethod("getHardwareBuffer").invoke(result);
        Bitmap gpu = (Bitmap) result.getClass().getMethod("asBitmap").invoke(result);
        try {
            return gpu.copy(Bitmap.Config.ARGB_8888, false);
        } finally {
            gpu.recycle();
            buffer.close();
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_servers");
        Class<?> displayControl = Class.forName("com.android.server.display.DisplayControl");
        long[] displays = (long[]) displayControl.getDeclaredMethod("getPhysicalDisplayIds").invoke(null);
        if (displays.length != 1) throw new AssertionError("physical displays=" + displays.length);
        IBinder display = (IBinder) displayControl.getDeclaredMethod("getPhysicalDisplayToken", long.class).invoke(null, displays[0]);
        if (args.length == 2) {
            Class<?> manager = Class.forName("android.app.ActivityTaskManager");
            Object service = manager.getMethod("getService").invoke(null);
            Class<?> contract = Class.forName("android.app.IActivityTaskManager");
            IBinder activityBinder = (IBinder) Class.forName("android.os.ServiceManager")
                    .getMethod("getService", String.class).invoke(null, "activity");
            IBinder windowBinder = (IBinder) Class.forName("android.os.ServiceManager")
                    .getMethod("getService", String.class).invoke(null, "window");
            Object windows = Class.forName("android.view.IWindowManager$Stub")
                    .getMethod("asInterface", IBinder.class).invoke(null, windowBinder);
            java.lang.reflect.Method isKeyguardLocked = Class.forName("android.view.IWindowManager")
                    .getMethod("isKeyguardLocked");
            IBinder userBinder = (IBinder) Class.forName("android.os.ServiceManager")
                    .getMethod("getService", String.class).invoke(null, "user");
            Object users = Class.forName("android.os.IUserManager$Stub")
                    .getMethod("asInterface", IBinder.class).invoke(null, userBinder);
            java.lang.reflect.Method isUnlocked = Class.forName("android.os.IUserManager")
                    .getMethod("isUserUnlocked", int.class);
            String observed = "no focused task";
            long deadline = SystemClock.uptimeMillis() + 180000;
            try (NceClockUi clock = "com.android.deskclock".equals(args[0]) ? new NceClockUi() : null) {
                while (SystemClock.uptimeMillis() < deadline) {
                    SystemClock.sleep(250);
                    if (!(Boolean) isUnlocked.invoke(users, 0)) {
                        observed = "user 0 remains locked";
                        continue;
                    }
                    Object task = contract.getMethod("getFocusedRootTaskInfo").invoke(service);
                    if (task == null) continue;
                    ComponentName top = (ComponentName) task.getClass().getField("topActivity").get(task);
                    boolean visible = task.getClass().getField("isVisible").getBoolean(task);
                    observed = "top=" + top + " visible=" + visible;
                    if (top == null || !visible || !args[0].equals(top.getPackageName())) continue;
                    boolean locked = (Boolean) isKeyguardLocked.invoke(windows);
                    String focus = focusedWindow(windowBinder);
                    observed += " keyguard=" + locked + " " + focus;
                    if (locked || !focus.contains(" " + args[0] + "/")) {
                        SystemClock.sleep(750);
                        continue;
                    }
                    boolean drawn = activityDrawn(activityBinder, top);
                    observed += " drawn=" + drawn;
                    if (!drawn) {
                        SystemClock.sleep(750);
                        continue;
                    }
                    if (clock != null && !clock.ready(null)) {
                        observed += " clock controls pending";
                        SystemClock.sleep(750);
                        continue;
                    }
                    System.err.println("Capturing desktop activity: " + top);
                    Bitmap pixels = capture(display);
                    System.err.println("Desktop capture completed: " + pixels.getWidth() + "x" + pixels.getHeight());
                    try {
                        if (clock != null && !clock.ready(pixels)) {
                            observed += " clock pixels pending";
                            SystemClock.sleep(750);
                            continue;
                        }
                        HashSet<Integer> colors = new HashSet<>();
                        for (int y = 40; y < pixels.getHeight() - 40; y += 8)
                            for (int x = 8; x < pixels.getWidth() - 8; x += 8)
                                colors.add(pixels.getPixel(x, y));
                        observed += " colors=" + colors.size();
                        if (colors.size() < 8) continue;
                        try (FileOutputStream output = new FileOutputStream(args[1])) {
                            if (!pixels.compress(Bitmap.CompressFormat.PNG, 100, output))
                                throw new AssertionError("desktop capture PNG failed");
                        }
                        System.out.println("{\"test\":\"android_desktop_visible_activity\",\"passed\":true,\"package\":\""
                            + args[0] + "\",\"sampled_colors\":" + colors.size() + "}");
                        if (clock != null) System.out.println("{\"test\":\"android_clock_controls_ready\",\"passed\":true}");
                        return;
                    } finally {
                        pixels.recycle();
                    }
                }
                throw new AssertionError("visible desktop activity timed out: " + args[0] + "; " + observed);
            }
        }
        if (args.length != 0) throw new IllegalArgumentException("expected package and PNG path");
        SurfaceControl.class.getDeclaredMethod("setDisplayPowerMode", IBinder.class, int.class).invoke(null, display, 2);
        SurfaceControl control = new SurfaceControl.Builder().setName("NCE SurfaceFlinger check")
            .setBufferSize(360, 640).setOpaque(true).build();
        try (SurfaceControl.Transaction transaction = new SurfaceControl.Transaction()) {
            SurfaceControl.Transaction.class.getDeclaredMethod("setDisplayLayerStack", IBinder.class, int.class).invoke(transaction, display, 0);
            SurfaceControl.Transaction.class.getDeclaredMethod("setDisplayProjection", IBinder.class, int.class, Rect.class, Rect.class)
                .invoke(transaction, display, 0, new Rect(0, 0, 360, 640), new Rect(0, 0, 360, 640));
            SurfaceControl.Transaction.class.getDeclaredMethod("setLayerStack", SurfaceControl.class, int.class).invoke(transaction, control, 0);
            transaction.setLayer(control, 1).setVisibility(control, true).apply();
        }
        Surface surface = new Surface(control);
        Paint paint = new Paint();
        int[] left = {Color.RED, Color.GREEN, Color.WHITE};
        int[] right = {Color.BLUE, Color.MAGENTA, Color.BLACK};
        for (int frame = 0; frame < 3; ++frame) {
            Canvas canvas = surface.lockCanvas(null);
            canvas.drawColor(left[frame]);
            paint.setColor(right[frame]);
            canvas.drawRect(180, 0, 360, 640, paint);
            surface.unlockCanvasAndPost(canvas);
            boolean matched = false;
            for (int attempt = 0; attempt < 40; ++attempt) {
                SystemClock.sleep(50);
                Bitmap pixels = capture(display);
                if (pixels.getWidth() != 360 || pixels.getHeight() != 640)
                    throw new AssertionError("capture size=" + pixels.getWidth() + "x" + pixels.getHeight());
                matched = pixels.getPixel(90, 320) == left[frame] && pixels.getPixel(270, 320) == right[frame];
                if (matched && frame == 2) {
                    try (FileOutputStream output = new FileOutputStream("/data/nce/surface.png")) {
                        if (!pixels.compress(Bitmap.CompressFormat.PNG, 100, output))
                            throw new AssertionError("capture PNG failed");
                    }
                }
                pixels.recycle();
                if (matched) break;
            }
            if (!matched) throw new AssertionError("SurfaceFlinger frame " + frame + " pixels did not update");
        }
        surface.release();
        control.release();
        System.out.println("{\"test\":\"android_surfaceflinger_composed_pixels\",\"passed\":true,\"frames\":3}");
    }
}
