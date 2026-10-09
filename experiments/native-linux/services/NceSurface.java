/* SPDX-License-Identifier: GPL-2.0-or-later */
import android.graphics.Canvas;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.IBinder;
import android.hardware.HardwareBuffer;
import android.os.SystemClock;
import android.view.Surface;
import android.view.SurfaceControl;
import java.io.FileOutputStream;

public final class NceSurface {
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
