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

    public static Object $compile$unsupportedCast(Object value) {
        return (Class<?>)value;
    }

    public static Node $compile$newNode(int value) { return new Node(value); }
    public static int[] $compile$newArray(int size) { return new int[size]; }

    public interface Reader {
        int read();
        default Object identity(Object value) { return value; }
    }
    public abstract static class Value implements Reader { public abstract int read(); }
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

    // ART hashes abstract interface names with multiplier 31; Aa and BB collide.
    public interface CollisionReader { int Aa(); int BB(); }
    public static final class CollisionValue implements CollisionReader {
        public int Aa() { return 11; }
        public int BB() { return 31; }
    }
    public static int $compile$interfaceCollision(CollisionReader reader) {
        return reader.Aa() * 100 + reader.BB();
    }

    public static final class PatternBox {
        private int number;
        private Object reference;
        private float fraction;
        private long wide;
        private static Object staticReference;
        private static long staticWide;
        public PatternBox(int value) { number = value; }
        public PatternBox(Object value) { reference = value; }
        public int getNumber() { return number; }
        public void setNumber(int value) { number = value; }
        public Object getReference() { return reference; }
        public void setReference(Object value) { reference = value; }
        public float getFraction() { return fraction; }
        public void setFraction(float value) { fraction = value; }
        public long getWide() { return wide; }
        public void setWide(long value) { wide = value; }
        public static Object getStaticReference() { return staticReference; }
        public static long getStaticWide() { return staticWide; }
    }

    public static Object $compile$patternRoundTrip(PatternBox box, Object reference) {
        box.setReference(reference);
        return box.getReference();
    }

    public static void compiledPatterns() {
        Object marker = new Object();
        PatternBox numeric = new PatternBox(71);
        PatternBox object = new PatternBox(marker);
        check(numeric.getNumber() == 71 && object.getReference() == marker, "pattern constructors");
        numeric.setNumber(-93);
        numeric.setFraction(1.25f);
        numeric.setWide(0x123456789abcdefL);
        check(numeric.getNumber() == -93 && numeric.getFraction() == 1.25f
                && numeric.getWide() == 0x123456789abcdefL, "pattern primitive field access");
        check($compile$patternRoundTrip(numeric, marker) == marker, "JIT calls reference patterns");
        check($compile$patternRoundTrip(numeric, null) == null, "pattern null reference");
        object.setReference(numeric);
        PatternBox.staticReference = object;
        PatternBox.staticWide = 0x7123456789abcdeL;
        System.gc();
        check(object.getReference() == numeric && PatternBox.getStaticReference() == object
                && PatternBox.getStaticWide() == 0x7123456789abcdeL, "pattern roots survive GC");
        PatternBox.staticReference = null;
        System.out.println("{\"test\":\"android_art_jit_patterns\",\"passed\":true,\"installed_patterns\":11}");
    }

    private static int successfulInitializations;
    private static int failedInitializations;
    private static int initializeCold() { successfulInitializations++; return 83; }
    private static int initializeBroken() {
        failedInitializations++;
        throw new IllegalStateException("class-init-test");
    }
    private static final class ColdState {
        static int value = initializeCold();
        static Object reference = new Node(value);
    }
    private static final class BrokenState { static int value = initializeBroken(); }
    public static Object $compile$coldReference() { return ColdState.reference; }
    public static int $compile$coldIncrement() { return ++ColdState.value; }
    public static int $compile$brokenClass() { return BrokenState.value; }

    public static int $compile$stringHash(String value) {
        int hash = 0;
        for (int i = 0; i < value.length(); i++) hash = hash * 31 + value.charAt(i);
        return hash;
    }
    public static char $compile$stringAt(String value, int index) { return value.charAt(index); }
    public static char $compile$stringAtOne(String value) { return value.charAt(1); }
    public static String $compile$stringLiteral() { return "native-linux-jit"; }
    public static boolean $compile$stringEquals(String first, Object second) {
        return first.equals(second);
    }
    public static long $compile$combined(String[] keys, Object[] slots, Reader reader, int rounds) {
        long checksum = 0;
        for (int r = 0; r < rounds; r++) {
            int i = r & (keys.length - 1);
            String key = keys[i];
            slots[i] = key;
            checksum += $compile$stringHash(key) + reader.read();
        }
        return checksum;
    }

    public static void compiledStringsAndClasses() throws Exception {
        check(successfulInitializations == 0 && failedInitializations == 0,
                "compilation preserves uninitialized classes");
        Object cold = $compile$coldReference();
        check(cold instanceof Node && ((Node)cold).value == 83, "first initialization object result");
        check($compile$coldIncrement() == 84 && $compile$coldIncrement() == 85
                && successfulInitializations == 1, "static state initializes once");
        System.gc();
        check($compile$coldReference() == cold, "static reference survives GC");
        int initializationErrors = 0;
        try { $compile$brokenClass(); }
        catch (ExceptionInInitializerError expected) { initializationErrors++; }
        try { $compile$brokenClass(); }
        catch (NoClassDefFoundError expected) { initializationErrors++; }
        check(initializationErrors == 2 && failedInitializations == 1, "failed initialization state");
        String ascii = new String(new char[]{'n', 'a', 't', 'i', 'v', 'e'});
        String unicode = new String(new char[]{0x5b89, 0x5353, 0x20ac, 0xd83d, 0xde80});
        String mixed = new String(new char[]{0, 127, 255, 256});
        String[] keys = {ascii, unicode, mixed, "", "native-linux-jit", "linux", "jni", "gc"};
        for (String key : keys) check($compile$stringHash(key) == key.hashCode(), "string hash");
        check($compile$stringAt(ascii, 1) == 'a' && $compile$stringAtOne(ascii) == 'a',
                "compressed character access");
        check($compile$stringAt(unicode, 3) == 0xd83d && $compile$stringAtOne(unicode) == 0x5353,
                "UTF-16 character access");
        check($compile$stringAt(mixed, 0) == 0 && $compile$stringAt(mixed, 2) == 255,
                "zero and non-ASCII character access");
        check($compile$stringLiteral() == "native-linux-jit", "JIT string root identity");
        check($compile$stringEquals(ascii, new String(ascii))
                && !$compile$stringEquals(ascii, unicode) && !$compile$stringEquals(ascii, null),
                "recognized intrinsic executes through its regular method call");
        int exceptions = 0;
        try { $compile$stringAt(null, 0); }
        catch (NullPointerException expected) { exceptions++; }
        try { $compile$stringAt(unicode, unicode.length()); }
        catch (StringIndexOutOfBoundsException expected) { exceptions++; }
        try { $compile$stringAt(ascii, -1); }
        catch (StringIndexOutOfBoundsException expected) { exceptions++; }
        Object[] slots = new Object[keys.length];
        long expected = 0;
        for (int r = 0; r < 100000; r++) expected += keys[r & 7].hashCode() + 17;
        Thread collector = new Thread(() -> {
            for (int i = 0; i < 8; i++) System.gc();
        }, "nce-jit-combined-gc");
        collector.start();
        check($compile$combined(keys, slots, new FirstValue(), 100000) == expected,
                "combined compiled workload checksum");
        collector.join();
        for (int i = 0; i < keys.length; i++) check(slots[i] == keys[i], "combined stored references");
        check(exceptions == 3, "compiled string exceptions");
        System.out.println("{\"test\":\"android_art_jit_strings_classes\",\"passed\":true,"
                + "\"strings\":8,\"combined_iterations\":100000,\"gc_requests\":8,"
                + "\"initialization_errors\":2,\"string_exceptions\":3}");
    }

    public static Object $compile$referenceStore(Object[] array, int index, Object value) {
        array[index] = value;
        return array[index];
    }

    public static void $compile$typedStore(Node[] array, int index, Node value) {
        array[index] = value;
    }

    public static void $compile$clearStore(Object[] array, int index) { array[index] = null; }
    public static int $compile$interfaceRead(Reader value) { return value.read(); }
    public static Object $compile$interfaceIdentity(Reader reader, Object value) {
        return reader.identity(value);
    }

    public static Node $compile$monitor(Node node, Object monitor, int rounds,
                                        RuntimeException failure) {
        synchronized (monitor) {
            synchronized (monitor) {
                for (int i = 0; i < rounds; i++) node.visits++;
                if (failure != null) throw failure;
                return node;
            }
        }
    }

    public static int $compile$catchStore(Object[] array, Object value) {
        try {
            array[0] = value;
            return 7;
        } catch (ArrayStoreException exception) {
            return 13;
        }
    }

    public static void compiledRuntimeFeatures() throws Exception {
        CollisionReader collision = new CollisionValue();
        for (int i = 0; i < 256; i++)
            check($compile$interfaceCollision(collision) == 1131, "interface conflict resolution");
        Object[] references = new Object[256];
        Node[] typed = new Node[256];
        for (int i = 0; i < typed.length; i++) {
            Node value = new Node(i);
            check($compile$referenceStore(references, i, value) == value, "reference store identity");
            $compile$typedStore(typed, i, value);
        }
        System.gc();
        for (int i = 0; i < typed.length; i++) {
            check(references[i] == typed[i] && typed[i].value == i, "reference arrays survive GC");
        }
        $compile$clearStore(references, 0);
        check(references[0] == null, "constant null store");
        check($compile$referenceStore(references, 1, null) == null, "runtime null store");
        Value first = new FirstValue();
        Value second = new SecondValue();
        Object[] covariant = new Value[4];
        check($compile$referenceStore(covariant, 0, first) == first, "covariant subclass store");
        check($compile$referenceStore(covariant, 1, second) == second, "second subclass store");
        check($compile$interfaceRead(first) == 17 && $compile$interfaceRead(second) == 29,
                "interface dispatch targets");
        check($compile$interfaceIdentity(first, typed[7]) == typed[7]
                && $compile$interfaceIdentity(second, null) == null, "default interface method");
        check($compile$catchStore(covariant, first) == 7
                && $compile$catchStore(covariant, typed[0]) == 13,
                "compiled exception handler receives array-store exception");
        int exceptions = 0;
        try { $compile$referenceStore(covariant, 0, typed[0]); }
        catch (ArrayStoreException expected) { exceptions++; }
        try { $compile$referenceStore(null, 0, first); }
        catch (NullPointerException expected) { exceptions++; }
        try { $compile$referenceStore(covariant, covariant.length, first); }
        catch (ArrayIndexOutOfBoundsException expected) { exceptions++; }
        try { $compile$interfaceRead(null); }
        catch (NullPointerException expected) { exceptions++; }
        Object monitor = new Object();
        Node shared = new Node(71);
        RuntimeException failure = new IllegalStateException("monitor-test");
        try { $compile$monitor(shared, monitor, 1, failure); }
        catch (RuntimeException caught) {
            check(caught == failure, "compiled rethrow preserves exception identity");
            exceptions++;
        }
        try { $compile$monitor(shared, null, 1, null); }
        catch (NullPointerException expected) { exceptions++; }
        Thread firstWorker = new Thread(() -> {
            for (int i = 0; i < 2000; i++) $compile$monitor(shared, monitor, 5, null);
        }, "nce-jit-monitor-1");
        Thread secondWorker = new Thread(() -> {
            for (int i = 0; i < 2000; i++) $compile$monitor(shared, monitor, 5, null);
        }, "nce-jit-monitor-2");
        firstWorker.start();
        secondWorker.start();
        for (int i = 0; i < 4; i++) System.gc();
        firstWorker.join();
        secondWorker.join();
        check(shared.visits == 20001, "compiled monitors release on exception and serialize updates");
        check($compile$monitor(shared, monitor, 0, null) == shared, "monitor reference return");
        check(exceptions == 6, "runtime feature exceptions");
        System.out.println("{\"test\":\"android_art_jit_runtime_features\",\"passed\":true,"
                + "\"reference_elements\":256,\"interface_targets\":2,"
                + "\"monitor_calls\":4000,\"monitor_updates\":20001,\"exceptions\":6}");
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
