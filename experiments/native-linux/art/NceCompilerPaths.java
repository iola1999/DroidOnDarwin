/* SPDX-License-Identifier: Apache-2.0 */
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.invoke.VarHandle;
import java.nio.charset.StandardCharsets;

public final class NceCompilerPaths {
    private volatile Object field;

    public static String fromChars(char[] chars, int offset, int count) {
        return new String(chars, offset, count);
    }
    public static String fromBytes(byte[] bytes) { return new String(bytes, StandardCharsets.UTF_8); }
    public static String concat(String left, String right, int number, long wide, float fraction,
                                double decimal, boolean flag, char letter) {
        return left + ":" + right + ":" + number + ":" + wide + ":" + fraction + ":" + decimal
                + ":" + flag + ":" + letter;
    }
    public static int minInt(int a, int b) { return Math.min(a, b); }
    public static long maxLong(long a, long b) { return Math.max(a, b); }
    public static float maxFloat(float a, float b) { return Math.max(a, b); }
    public static double minDouble(double a, double b) { return Math.min(a, b); }
    public static Object invoke(MethodHandle handle, Object value) throws Throwable {
        return (Object)handle.invokeExact(value);
    }
    public static int invokeInt(MethodHandle handle, int value) throws Throwable {
        return (int)handle.invokeExact(value);
    }
    public static boolean cas(VarHandle handle, NceCompilerPaths holder, Object before, Object after) {
        return handle.compareAndSet(holder, before, after);
    }
    public static Object identity(Object value) { System.gc(); return value; }
    public static int increment(int value) { return value + 1; }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    public static void run() throws Throwable {
        char[] chars = {'x', 'A', '中', '\ud83d', '\ude80', 'z'};
        for (int i = 0; i < 64; i++) {
            check(fromChars(chars, 1, 4).equals("A中🚀"), "String constructor UTF-16 slice");
            check(fromBytes(new byte[] {65, (byte)0xe4, (byte)0xb8, (byte)0xad}).equals("A中"), "String constructor UTF-8");
            check(concat("a中", null, -7, Long.MAX_VALUE, -0.0f, Double.NaN, true, '€')
                    .equals("a中:null:-7:9223372036854775807:-0.0:NaN:true:€"), "StringBuilder mixed arguments");
            if ((i & 7) == 0) System.gc();
        }
        check(minInt(Integer.MIN_VALUE, Integer.MAX_VALUE) == Integer.MIN_VALUE, "min int bounds");
        check(minInt(17, -19) == -19, "min int order");
        check(maxLong(Long.MIN_VALUE, Long.MAX_VALUE) == Long.MAX_VALUE, "max long bounds");
        check(maxLong(17, -19) == 17, "max long order");
        check(Float.floatToRawIntBits(maxFloat(-0.0f, 0.0f)) == 0, "max float signed zero");
        check(Float.floatToRawIntBits(maxFloat(0.0f, -0.0f)) == 0, "max float reversed zero");
        check(Float.isNaN(maxFloat(1.0f, Float.NaN)), "max float NaN");
        check(maxFloat(Float.NEGATIVE_INFINITY, Float.POSITIVE_INFINITY) == Float.POSITIVE_INFINITY, "max float infinity");
        check(Double.doubleToRawLongBits(minDouble(0.0, -0.0)) == Long.MIN_VALUE, "min double signed zero");
        check(Double.doubleToRawLongBits(minDouble(-0.0, 0.0)) == Long.MIN_VALUE, "min double reversed zero");
        check(Double.isNaN(minDouble(Double.NaN, 1.0)), "min double NaN");
        check(minDouble(Double.NEGATIVE_INFINITY, Double.POSITIVE_INFINITY) == Double.NEGATIVE_INFINITY, "min double infinity");
        MethodHandles.Lookup lookup = MethodHandles.lookup();
        MethodHandle identity = lookup.findStatic(NceCompilerPaths.class, "identity", MethodType.methodType(Object.class, Object.class));
        MethodHandle increment = lookup.findStatic(NceCompilerPaths.class, "increment", MethodType.methodType(int.class, int.class));
        Object first = new Object();
        Object second = new Object();
        check(invoke(identity, first) == first && invoke(identity, null) == null, "polymorphic reference across GC");
        check(invokeInt(increment, 41) == 42, "polymorphic int result");
        VarHandle field = lookup.findVarHandle(NceCompilerPaths.class, "field", Object.class);
        NceCompilerPaths holder = new NceCompilerPaths();
        check(cas(field, holder, null, first) && holder.field == first, "VarHandle initial CAS");
        check(!cas(field, holder, second, null) && holder.field == first, "VarHandle failed CAS");
        check(cas(field, holder, first, null) && holder.field == null, "VarHandle clear CAS");
        int caught = 0;
        try { fromChars(null, 0, 1); } catch (NullPointerException expected) { caught++; }
        try { fromChars(chars, -1, 2); } catch (IndexOutOfBoundsException expected) { caught++; }
        try { invoke(null, first); } catch (NullPointerException expected) { caught++; }
        check(caught == 3, "compiled runtime exceptions");
        System.out.println("{\"test\":\"android_art_compiler_paths\",\"passed\":true,\"gc_requests\":10,\"exceptions\":3}");
    }
}
