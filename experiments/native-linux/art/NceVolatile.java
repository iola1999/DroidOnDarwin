/* SPDX-License-Identifier: Apache-2.0 */
public final class NceVolatile {
    private volatile float fraction;
    private volatile double wide;
    private static volatile float staticFraction;
    private static volatile double staticWide;

    public static void putFloat(NceVolatile target, float value) { target.fraction = value; }
    public static float getFloat(NceVolatile target) { return target.fraction; }
    public static void putDouble(NceVolatile target, double value) { target.wide = value; }
    public static double getDouble(NceVolatile target) { return target.wide; }
    public static void putStaticFloat(float value) { staticFraction = value; }
    public static float getStaticFloat() { return staticFraction; }
    public static void putStaticDouble(double value) { staticWide = value; }
    public static double getStaticDouble() { return staticWide; }

    private static void check(boolean condition) {
        if (!condition) throw new AssertionError("volatile floating-point field bits");
    }

    public static void checkResults() {
        NceVolatile target = new NceVolatile();
        int[] singles = {0, 0x80000000, 0x3f800000, 0xbf400000, 1,
                0x7f800000, 0xff800000, 0x7fc00123};
        long[] doubles = {0, 0x8000000000000000L, 0x3ff0000000000000L,
                0xbfe8000000000000L, 1, 0x7ff0000000000000L,
                0xfff0000000000000L, 0x7ff8000000000123L};
        for (int bits : singles) {
            float value = Float.intBitsToFloat(bits);
            target.fraction = staticFraction = value;
            check(Float.floatToRawIntBits(getFloat(target)) == bits);
            check(Float.floatToRawIntBits(getStaticFloat()) == bits);
            target.fraction = staticFraction = 17.0f;
            putFloat(target, value);
            putStaticFloat(value);
            check(Float.floatToRawIntBits(target.fraction) == bits);
            check(Float.floatToRawIntBits(staticFraction) == bits);
        }
        for (long bits : doubles) {
            double value = Double.longBitsToDouble(bits);
            target.wide = staticWide = value;
            check(Double.doubleToRawLongBits(getDouble(target)) == bits);
            check(Double.doubleToRawLongBits(getStaticDouble()) == bits);
            target.wide = staticWide = 17.0;
            putDouble(target, value);
            putStaticDouble(value);
            check(Double.doubleToRawLongBits(target.wide) == bits);
            check(Double.doubleToRawLongBits(staticWide) == bits);
        }
        int exceptions = 0;
        try { getFloat(null); } catch (NullPointerException expected) { exceptions++; }
        try { getDouble(null); } catch (NullPointerException expected) { exceptions++; }
        try { putFloat(null, 1); } catch (NullPointerException expected) { exceptions++; }
        try { putDouble(null, 1); } catch (NullPointerException expected) { exceptions++; }
        check(exceptions == 4);
        System.out.println("{\"test\":\"android_art_jit_volatile_floating\",\"passed\":true,"
                + "\"compiled_methods\":8,\"bit_patterns\":16,\"null_exceptions\":4}");
    }
}
