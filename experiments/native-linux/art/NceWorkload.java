/* SPDX-License-Identifier: Apache-2.0 */
import java.lang.ref.WeakReference;

public final class NceWorkload {
    private static final Object lock = new Object();
    private static long sum;

    private static final class Node {
        final int value;
        Node next;
        Node(int value) { this.value = value; }
    }

    private static void check(boolean condition, String description) {
        if (!condition) throw new AssertionError(description);
    }

    private static WeakReference<byte[]> allocate() {
        byte[] tracked = new byte[64 * 1024];
        tracked[123] = 71;
        return new WeakReference<>(tracked);
    }

    private static void count() {
        for (int i = 1; i <= 2000; i++) {
            synchronized (lock) { sum += i; }
        }
    }

    public static void main(String[] arguments) throws Exception {
        Node[] nodes = new Node[64];
        for (int i = 0; i < nodes.length; i++) nodes[i] = new Node(i);
        for (int i = 0; i < nodes.length; i++) nodes[i].next = nodes[(i + 1) % nodes.length];
        WeakReference<byte[]> reference = allocate();
        long checksum = 0;
        for (int i = 0; i < 512; i++) {
            byte[] memory = new byte[32 * 1024];
            memory[i] = (byte)i;
            checksum += memory[i];
        }
        check(checksum == -256, "allocation checksum");
        for (int i = 0; i < 8 && reference.get() != null; i++) {
            System.gc();
            Thread.sleep(10);
        }
        check(reference.get() == null, "GC clears weak reference");
        Node cursor = nodes[0];
        for (int i = 0; i < nodes.length * 2; i++) {
            check(cursor == nodes[i % nodes.length] && cursor.value == i % nodes.length,
                    "GC preserves reachable object graph");
            cursor = cursor.next;
        }
        Thread first = new Thread(NceWorkload::count, "nce-java-1");
        Thread second = new Thread(NceWorkload::count, "nce-java-2");
        first.start();
        second.start();
        first.join();
        second.join();
        check(sum == 4002000, "thread and monitor result");
        int exceptions = 0;
        try { Object missing = null; missing.toString(); }
        catch (NullPointerException expected) { exceptions++; }
        try { int[] array = new int[2]; sum += array[7]; }
        catch (ArrayIndexOutOfBoundsException expected) { exceptions++; }
        try { throw new IllegalStateException("nce-exception"); }
        catch (IllegalStateException expected) {
            check("nce-exception".equals(expected.getMessage()), "exception message");
            exceptions++;
        }
        check(exceptions == 3, "exception dispatch");
        System.out.println("{\"test\":\"android_art_java_workload\",\"passed\":true,"
                + "\"allocated_bytes\":16777216,\"gc_weak_reference_cleared\":true,"
                + "\"retained_cycle_nodes\":64,\"threads\":2,\"exceptions\":3,\"sum\":4002000}");
    }
}
