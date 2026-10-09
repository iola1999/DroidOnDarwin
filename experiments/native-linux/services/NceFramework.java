/* SPDX-License-Identifier: GPL-2.0-or-later */
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Parcel;
import android.os.SharedMemory;
import android.os.SystemClock;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import java.io.ByteArrayOutputStream;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class NceFramework {
    private static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }

    private static void graphics() throws Exception {
        System.out.println("{\"phase\":\"android_framework_graphics\"}");
        Bitmap bitmap = Bitmap.createBitmap(128, 96, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(Color.BLACK);
        Paint paint = new Paint();
        paint.setColor(Color.RED);
        canvas.drawRect(8, 8, 72, 56, paint);
        paint.setColor(Color.BLUE);
        canvas.drawCircle(96, 32, 20, paint);
        canvas.save();
        canvas.translate(16, 64);
        canvas.clipRect(0, 0, 96, 24);
        canvas.drawColor(Color.GREEN);
        canvas.restore();
        Path triangle = new Path();
        triangle.moveTo(80, 60); triangle.lineTo(112, 60); triangle.lineTo(96, 84); triangle.close();
        paint.setColor(Color.WHITE);
        canvas.drawPath(triangle, paint);
        check(bitmap.getPixel(1, 1) == Color.BLACK && bitmap.getPixel(10, 10) == Color.RED);
        check(bitmap.getPixel(96, 32) == Color.BLUE && bitmap.getPixel(20, 70) == Color.GREEN);
        check(bitmap.getPixel(96, 65) == Color.WHITE);
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        check(bitmap.compress(Bitmap.CompressFormat.PNG, 100, bytes));
        byte[] png = bytes.toByteArray();
        Bitmap decoded = BitmapFactory.decodeByteArray(png, 0, png.length);
        check(decoded != null && decoded.sameAs(bitmap));
        try (FileOutputStream output = new FileOutputStream("/data/nce/framework.png")) {
            output.write(png);
        }
        decoded.recycle(); bitmap.recycle();
        System.out.println("{\"test\":\"android_framework_graphics\",\"passed\":true,\"canvas\":true,\"png_roundtrip\":true}");
    }

    public static void main(String[] args) throws Exception {
        System.out.println("{\"phase\":\"android_framework_java\"}");
        byte[] digest = MessageDigest.getInstance("SHA-256").digest("abc".getBytes(StandardCharsets.US_ASCII));
        StringBuilder hex = new StringBuilder();
        for (byte value : digest) hex.append(String.format("%02x", value & 255));
        check(hex.toString().equals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        System.out.println("{\"test\":\"android_framework_jca\",\"passed\":true}");
        Parcel parcel = Parcel.obtain();
        parcel.writeInt(1234567);
        parcel.writeString("Android framework 测试");
        parcel.writeLong(0x1020304050607080L);
        byte[] marshalled = parcel.marshall();
        parcel.recycle();
        parcel = Parcel.obtain();
        parcel.unmarshall(marshalled, 0, marshalled.length);
        parcel.setDataPosition(0);
        check(parcel.readInt() == 1234567);
        check("Android framework 测试".equals(parcel.readString()));
        check(parcel.readLong() == 0x1020304050607080L);
        parcel.recycle();
        HandlerThread thread = new HandlerThread("nce-framework-looper");
        thread.start();
        Handler handler = new Handler(thread.getLooper());
        CountDownLatch completed = new CountDownLatch(1);
        long started = SystemClock.uptimeMillis();
        check(handler.postDelayed(completed::countDown, 20));
        check(completed.await(2, TimeUnit.SECONDS));
        check(SystemClock.uptimeMillis() - started >= 20);
        check(thread.quitSafely());
        thread.join();
        try (SharedMemory shared = SharedMemory.create("nce-framework", 16384)) {
            ByteBuffer bytes = shared.mapReadWrite();
            bytes.putInt(0, 0x1234abcd);
            ByteBuffer alias = shared.mapReadOnly();
            check(alias.getInt(0) == 0x1234abcd);
            SharedMemory.unmap(alias);
            SharedMemory.unmap(bytes);
        }
        Class<?> manager = Class.forName("android.os.ServiceManager");
        IBinder service = (IBinder) manager.getDeclaredMethod("checkService", String.class)
                                          .invoke(null, "nce.test");
        check(service != null && service.pingBinder());
        Parcel input = Parcel.obtain(), output = Parcel.obtain();
        input.writeInterfaceToken("org.nce.IBinderTest");
        input.writeInt(73);
        byte[] data = new byte[128];
        int sum = 0;
        for (int i = 0; i < data.length; ++i) { data[i] = (byte)(73 * 7 + i); sum += data[i] & 255; }
        input.writeByteArray(data);
        check(service.transact(1, input, output, 0));
        check(output.readInt() == 73 && output.readInt() == sum);
        input.recycle(); output.recycle();
        java.io.File watched = new java.io.File("/data/nce/observer");
        check(watched.mkdir());
        CountDownLatch changed = new CountDownLatch(1);
        android.os.FileObserver observer = new android.os.FileObserver(watched, android.os.FileObserver.CLOSE_WRITE) {
            @Override public void onEvent(int event, String path) {
                if ((event & CLOSE_WRITE) != 0 && "created".equals(path)) changed.countDown();
            }
        };
        observer.startWatching();
        java.io.File created = new java.io.File(watched, "created");
        try (java.io.FileOutputStream outputFile = new java.io.FileOutputStream(created)) {
            outputFile.write(42);
        }
        check(changed.await(2, TimeUnit.SECONDS));
        observer.stopWatching();
        check(created.delete() && watched.delete());
        System.out.println("{\"test\":\"android_framework_file_observer\",\"passed\":true}");
        graphics();
        System.out.println("{\"test\":\"android_framework_runtime\",\"passed\":true,\"parcel\":true,\"looper\":true,\"shared_memory\":true,\"binder\":true}");
        // Android's shared FileObserver thread outlives individual watches.
        // This standalone process exits after its checks and explicit resource cleanup.
        System.exit(0);
    }
}
