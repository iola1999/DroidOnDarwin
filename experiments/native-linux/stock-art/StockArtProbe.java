import java.lang.ref.ReferenceQueue;
import java.lang.ref.WeakReference;
import java.util.concurrent.atomic.AtomicReference;

public final class StockArtProbe {
    private static volatile int allocationSink;

    public static final class Box {
        public final long value;
        public Box(long value) { this.value = value; }
    }

    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("stock-art fixture");
    }

    public static long primitiveArguments(boolean z, byte b, char c, short s, int i,
            long j, float f, double d, Object object) {
        require(z && b == -101 && c == '\u4e2d' && s == -12345 && i == 0x12345678);
        require(j == 0x1122334455667788L && f == 1.25f && d == -123.5);
        require("jni-token".equals(object));
        return j;
    }

    private static native Object nativeIdentity(Object object);
    private static native long nativeValues(boolean z, byte b, char c, short s, int i,
            long j, float f, double d, Object object);
    private static native void nativeThrow();

    public static long registeredNatives(Object object) {
        require(nativeIdentity(object) == object);
        long result = nativeValues(true, (byte)-101, '\u4e2d', (short)-12345,
                0x12345678, 0x1122334455667788L, 1.25f, -123.5, object);
        require(result == 0x1122334455667788L);
        try {
            nativeThrow();
            throw new AssertionError("missing JNI exception");
        } catch (IllegalStateException expected) {
            require("jni-native".equals(expected.getMessage()));
        }
        return result;
    }

    public static Box makeBox(long value) { return new Box(value); }

    private static WeakReference<Object> collectable(ReferenceQueue<Object> queue) {
        return new WeakReference<Object>(new Object(), queue);
    }

    public static int collectUnreachable() throws InterruptedException {
        ReferenceQueue<Object> queue = new ReferenceQueue<Object>();
        WeakReference<Object> reference = collectable(queue);
        for (int attempt = 1; attempt <= 32; ++attempt) {
            byte[][] pressure = new byte[16][];
            for (int i = 0; i < pressure.length; ++i) {
                pressure[i] = new byte[128 * 1024];
                pressure[i][0] = (byte)i;
                allocationSink ^= pressure[i][0];
            }
            System.gc();
            if (reference.get() == null && queue.remove(100) == reference) return attempt;
        }
        throw new AssertionError("weak reference was not cleared and enqueued");
    }

    public static int worker(int value) { return value * 3 + 7; }

    public static int javaThreads() throws InterruptedException {
        final Object lock = new Object();
        final int[] counter = new int[1];
        final ThreadLocal<Integer> local = new ThreadLocal<Integer>();
        final AtomicReference<Throwable> failure = new AtomicReference<Throwable>();
        Thread[] threads = new Thread[2];
        for (int index = 0; index < threads.length; ++index) {
            final int value = index + 41;
            threads[index] = new Thread(new Runnable() {
                public void run() {
                    try {
                        local.set(value);
                        for (int i = 0; i < 1000; ++i) {
                            synchronized (lock) { ++counter[0]; }
                            require(local.get().intValue() == value);
                        }
                    } catch (Throwable error) {
                        failure.compareAndSet(null, error);
                    } finally {
                        local.remove();
                    }
                }
            }, "stock-art-" + index);
            threads[index].start();
        }
        for (Thread thread : threads) thread.join();
        if (failure.get() != null) throw new AssertionError(failure.get());
        require(counter[0] == 2000);
        return counter[0];
    }

    public static void throwExpected() {
        throw new IllegalArgumentException("java-expected");
    }

    private static int hot(int value) { return ((value * 17) ^ (value >>> 3)) + 9; }

    public static long warmup(int iterations) {
        long result = 0;
        for (int i = 0; i < iterations; ++i) result += hot(i);
        return result;
    }
}
