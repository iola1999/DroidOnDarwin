public final class StockArtExceptions {
    private static volatile int progress;
    private static volatile int recursionDepth;

    private static final class Box {
        final int value;
        Box next;
        Box(int value) { this.value = value; }
    }

    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("stock-art exception fixture");
    }

    private static boolean containsFrame(Throwable error, String method) {
        for (StackTraceElement frame : error.getStackTrace()) {
            if (frame.getClassName().equals("StockArtExceptions") &&
                    frame.getMethodName().equals(method)) return true;
        }
        return false;
    }

    private static long readAfterLoop(Box object, int iterations) {
        long sum = 0;
        for (int i = 0; i < iterations; ++i) {
            sum += i;
            progress = i;
        }
        return sum + object.value;
    }

    public static int implicitNull() {
        Box object = new Box(37);
        require(readAfterLoop(object, 200000) == 19999900037L);
        progress = -1;
        try {
            readAfterLoop(null, 200000);
            throw new AssertionError("missing null exception");
        } catch (NullPointerException expected) {
            require(progress == 199999 && containsFrame(expected, "readAfterLoop"));
        }
        return (int)readAfterLoop(object, 5);
    }

    private static int recurse(int depth, int limit) {
        recursionDepth = depth;
        if (depth == limit) return depth;
        return depth ^ recurse(depth + 1, limit);
    }

    private static int stackOverflowOnCurrentThread() {
        require(recurse(0, 32) == 32);
        try {
            recurse(0, 1000000);
            throw new AssertionError("missing stack overflow");
        } catch (StackOverflowError expected) {
            require(recursionDepth > 32 && recursionDepth < 1000000 &&
                    containsFrame(expected, "recurse"));
        }
        require(recurse(0, 32) == 32);
        return (int)readAfterLoop(new Box(37), 5);
    }

    public static int stackOverflow() throws InterruptedException {
        final int[] result = new int[1];
        final Throwable[] failure = new Throwable[1];
        // Request 256 KiB so the recursive stack trace fits the 64 MiB fixture heap.
        Thread thread = new Thread(null, new Runnable() {
            @Override
            public void run() {
                try {
                    result[0] = stackOverflowOnCurrentThread();
                } catch (Throwable error) {
                    failure[0] = error;
                }
            }
        }, "stock-art-overflow", 256L << 10);
        thread.start();
        thread.join();
        if (failure[0] != null) throw new AssertionError(failure[0]);
        return result[0];
    }

    public static long gcWithLiveReferences() {
        Box first = new Box(0);
        Box last = first;
        for (int i = 1; i < 4096; ++i) {
            last.next = new Box(i);
            last = last.next;
        }
        last.next = first;
        System.gc();
        require(first != last && first.value == 0 && last.value == 4095 && last.next == first);
        long sum = 0;
        Box current = first;
        for (int i = 0; i < 4096; ++i) {
            require(current.value == i);
            sum += current.value;
            current = current.next;
        }
        require(current == first && progress == 4);
        return sum;
    }
}
