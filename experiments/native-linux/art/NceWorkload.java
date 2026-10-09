/* SPDX-License-Identifier: Apache-2.0 */
import java.lang.ref.WeakReference;

public final class NceWorkload {
    private static final Object lock = new Object();
    private static long sum;

    public static long mixBaseline(long rounds, long seed) {
        for (long i = 0; i < rounds; i++) {
            seed = seed * 6364136223846793005L + 1442695040888963407L;
            seed ^= seed >>> 27;
        }
        return seed;
    }

    public static long $compile$mix(long rounds, long seed) {
        for (long i = 0; i < rounds; i++) {
            seed = seed * 6364136223846793005L + 1442695040888963407L;
            seed ^= seed >>> 27;
        }
        return seed;
    }

    private static final class Node {
        final int value;
        Node next;
        int visits;
        Node(int value) { this.value = value; }
    }

    public static long $compile$mutate(int[] values, int rounds) {
        long checksum = 0;
        for (int r = 0; r < rounds; r++) {
            for (int i = 0; i < values.length; i++) {
                values[i] = values[i] * 1664525 + 1013904223;
                checksum += values[i];
            }
        }
        return checksum;
    }

    public static long $compile$walk(Node node, int rounds) {
        long checksum = 0;
        for (int i = 0; i < rounds; i++) {
            checksum += node.value;
            node = node.next;
        }
        return checksum;
    }

    public static void $compile$link(Node first, Node second) {
        first.next = second;
        first.visits++;
    }

    public static int $compile$at(int[] values, int index) {
        return values[index];
    }

    public static Node $compile$identity(Node node) { return node; }
    public static Node interpretedIdentity(Node node) { return node; }
    public static native Object nativeIdentity(Object value);

    public static Node $compile$callInterpreted(Node node) { return interpretedIdentity(node); }
    public static Node $compile$callCompiled(Node node) { return $compile$identity(node); }
    public static Object $compile$callNative(Object value) { return nativeIdentity(value); }

    public static int automaticMix(int value) { return value * 3 + 17; }

    public static long warmJit() {
        long sum = 0;
        for (int i = 0; i < 200000; i++) sum += automaticMix(i);
        return sum;
    }

    public static Object $compile$unsupportedMonitor(Object value) {
        synchronized (value) { return value; }
    }

    public static Node $compile$newNode(int value) { return new Node(value); }
    public static int[] $compile$newArray(int size) { return new int[size]; }

    public abstract static class Value { public abstract int read(); }
    public static final class FirstValue extends Value { public int read() { return 17; } }
    public static final class SecondValue extends Value { public int read() { return 29; } }
    public static int $compile$virtual(Value value) { return value.read(); }

    public static long $compile$allocateLoop(int rounds) {
        long checksum = 0;
        for (int i = 0; i < rounds; i++) {
            Node node = new Node(i);
            int[] array = new int[8];
            array[3] = node.value;
            checksum += array[3];
        }
        return checksum;
    }

    public static void compiledAllocations() {
        Node node = $compile$newNode(71);
        int[] array = $compile$newArray(1024);
        check(node.value == 71 && array.length == 1024 && array[17] == 0,
                "compiled allocation initializes memory");
        check($compile$allocateLoop(40000) == 799980000L, "compiled allocation loop");
        check($compile$virtual(new FirstValue()) == 17
                && $compile$virtual(new SecondValue()) == 29, "compiled virtual dispatch");
        System.gc();
        check(node.value == 71 && array.length == 1024, "allocated references survive GC");
        int exceptions = 0;
        try { $compile$newArray(-1); }
        catch (NegativeArraySizeException expectedException) { exceptions++; }
        try { $compile$virtual(null); }
        catch (NullPointerException expectedException) { exceptions++; }
        check(exceptions == 2, "allocation and virtual-call exceptions");
        System.out.println("{\"test\":\"android_art_jit_allocations\",\"passed\":true,"
                + "\"object_array_pairs\":40000,\"virtual_targets\":2,\"exceptions\":2}");
    }

    public static Object compiledCalls() {
        Node value = new Node(47);
        check($compile$identity(value) == value, "interpreted to compiled reference return");
        check($compile$callInterpreted(value) == value, "compiled to interpreted reference return");
        check($compile$callCompiled(value) == value, "compiled to compiled reference return");
        check($compile$callNative(value) == value, "compiled to JNI reference return");
        check(nativeIdentity(value) == value, "interpreted to JNI reference return");
        check($compile$identity(null) == null && $compile$callInterpreted(null) == null
                && $compile$callCompiled(null) == null && $compile$callNative(null) == null,
                "null reference returns");
        check($compile$unsupportedMonitor(value) == value, "unsupported method retains interpreter");
        System.out.println("{\"test\":\"android_art_jit_calls\",\"passed\":true,"
                + "\"reference_returns\":5,\"null_returns\":4}");
        return value;
    }

    public static void compiledObjects() throws Exception {
        int[] values = new int[4096];
        long expected = 0;
        for (int i = 0; i < values.length; i++) {
            int value = i;
            for (int r = 0; r < 8; r++) {
                value = value * 1664525 + 1013904223;
                expected += value;
            }
            values[i] = i;
        }
        check($compile$mutate(values, 8) == expected, "compiled array update checksum");
        Node[] nodes = new Node[64];
        for (int i = 0; i < nodes.length; i++) nodes[i] = new Node(i);
        for (int i = 0; i < nodes.length; i++) $compile$link(nodes[i], nodes[(i + 1) % nodes.length]);
        Thread collector = new Thread(() -> {
            for (int i = 0; i < 8; i++) System.gc();
        }, "nce-jit-gc");
        collector.start();
        check($compile$walk(nodes[0], 20000000) == 630000000L, "compiled object traversal");
        collector.join();
        for (int i = 0; i < nodes.length; i++) {
            check(nodes[i].visits == 1 && nodes[i].next == nodes[(i + 1) % nodes.length],
                    "compiled stores survive GC");
        }
        int exceptions = 0;
        try { $compile$at(null, 0); }
        catch (NullPointerException expectedException) { exceptions++; }
        try { $compile$at(values, values.length); }
        catch (ArrayIndexOutOfBoundsException expectedException) { exceptions++; }
        check(exceptions == 2, "compiled exception dispatch");
        System.out.println("{\"test\":\"android_art_jit_objects\",\"passed\":true,"
                + "\"array_updates\":32768,\"field_traversals\":20000000,"
                + "\"gc_requests\":8,\"retained_cycle_nodes\":64,\"exceptions\":2}");
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
        check(new FirstValue().read() == 17 && new SecondValue().read() == 29,
                "initialize virtual targets");
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
