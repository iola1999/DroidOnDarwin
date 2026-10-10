/* SPDX-License-Identifier: Apache-2.0 */
public final class NceFastBaseline {
    int number;
    Object reference;
    byte small;
    short signed;
    char letter;
    boolean flag;
    volatile int volatileNumber;

    interface Reader { Object read(); }
    static class Base { Object read() { return "base"; } }
    static final class Child extends Base implements Reader {
        @Override public Object read() { return "child"; }
        @Override public boolean equals(Object value) { return value instanceof Child; }
        @Override public int hashCode() { return 719; }
        @Override public String toString() { return "NCE interface Object dispatch"; }
    }
    public static int number(NceFastBaseline holder) { return holder.number; }
    public static Object reference(NceFastBaseline holder) { return holder.reference; }
    public static void storeReference(NceFastBaseline holder, Object value) { holder.reference = value; }
    public static void storeNumber(NceFastBaseline holder, int value) { holder.number = value; }
    public static void storeNull(NceFastBaseline holder) { holder.reference = null; }
    public static byte small(NceFastBaseline holder) { return holder.small; }
    public static short signed(NceFastBaseline holder) { return holder.signed; }
    public static char letter(NceFastBaseline holder) { return holder.letter; }
    public static boolean flag(NceFastBaseline holder) { return holder.flag; }
    public static String literal() { return "NCE中🚀"; }
    public static Object allocate() { return new NceFastBaseline(); }
    public static NceFastBaseline cast(Object value) { return (NceFastBaseline)value; }
    public static Object virtualCall(Base value) { return value.read(); }
    public static Object interfaceCall(Reader value) { return value.read(); }
    public static boolean interfaceEquals(Reader value, Object other) { return value.equals(other); }
    public static int interfaceHash(Reader value) { return value.hashCode(); }
    public static String interfaceString(Reader value) { return value.toString(); }
    public static Object acrossGc(Object value) { System.gc(); return value; }
    public static void throwValue(RuntimeException value) { throw value; }
    // Volatile accesses must keep the optimizing fallback's memory ordering.
    public static int volatileFallback(NceFastBaseline holder) { return holder.volatileNumber; }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    public static void run() {
        NceFastBaseline holder = new NceFastBaseline();
        holder.small = -123;
        holder.signed = -30000;
        holder.letter = '中';
        holder.flag = true;
        holder.volatileNumber = 431;
        Child child = new Child();
        for (int i = 0; i < 32; ++i) {
            Object value = new Object();
            storeReference(holder, value);
            storeNumber(holder, -137);
            System.gc();
            check(reference(holder) == value && number(holder) == -137, "fields across GC");
            check(small(holder) == -123 && signed(holder) == -30000 && letter(holder) == '中' && flag(holder), "narrow fields");
            check(literal().equals("NCE中🚀"), "JIT string root");
            check(allocate() instanceof NceFastBaseline, "allocation runtime result");
            check(cast(holder) == holder && cast(null) == null, "cast identity and null");
            check(virtualCall(child).equals("child") && interfaceCall(child).equals("child"), "virtual and interface dispatch");
            check(interfaceEquals(child, new Child()) && !interfaceEquals(child, holder), "interface Object equals");
            check(interfaceHash(child) == 719 && interfaceString(child).equals("NCE interface Object dispatch"), "interface Object methods");
            check(acrossGc(value) == value, "compiled register root across GC");
            check(volatileFallback(holder) == 431, "volatile optimizing fallback");
            storeNull(holder);
            check(reference(holder) == null, "null field store");
        }
        int caught = 0;
        try { number(null); } catch (NullPointerException expected) { ++caught; }
        try { reference(null); } catch (NullPointerException expected) { ++caught; }
        try { storeReference(null, holder); } catch (NullPointerException expected) { ++caught; }
        try { storeNumber(null, 4); } catch (NullPointerException expected) { ++caught; }
        try { virtualCall(null); } catch (NullPointerException expected) { ++caught; }
        try { interfaceCall(null); } catch (NullPointerException expected) { ++caught; }
        try { interfaceEquals(null, holder); } catch (NullPointerException expected) { ++caught; }
        try { interfaceHash(null); } catch (NullPointerException expected) { ++caught; }
        try { interfaceString(null); } catch (NullPointerException expected) { ++caught; }
        try { cast(new Object()); } catch (ClassCastException expected) { ++caught; }
        RuntimeException failure = new RuntimeException("fast-baseline");
        try { throwValue(failure); } catch (RuntimeException expected) { check(expected == failure, "exception identity"); ++caught; }
        try { throwValue(null); } catch (NullPointerException expected) { ++caught; }
        check(caught == 12, "compiled exceptions");
        System.out.println("{\"test\":\"android_art_fast_baseline_paths\",\"passed\":true,\"gc_requests\":64,\"exceptions\":12}");
    }
}
