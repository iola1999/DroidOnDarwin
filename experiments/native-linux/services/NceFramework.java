/* SPDX-License-Identifier: GPL-2.0-or-later */
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Parcel;
import android.os.SharedMemory;
import android.os.SystemClock;
import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class NceFramework {
    private static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }

    public static void main(String[] args) throws Exception {
        System.out.println("{\"phase\":\"android_framework_java\"}");
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
        System.out.println("{\"test\":\"android_framework_runtime\",\"passed\":true,\"parcel\":true,\"looper\":true,\"shared_memory\":true,\"binder\":true}");
    }
}
