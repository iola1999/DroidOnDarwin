/* SPDX-License-Identifier: Apache-2.0 */
public final class NceIntrinsics {
    public static long bitsBaseline(long value) {
        int low = (int)value;
        return Integer.reverse(low) + (long)Integer.reverseBytes(low)
                + Integer.numberOfLeadingZeros(low) + Integer.numberOfTrailingZeros(low)
                + Integer.bitCount(low) + Integer.highestOneBit(low) + Integer.lowestOneBit(low)
                + Long.reverse(value) + Long.reverseBytes(value)
                + Long.numberOfLeadingZeros(value) + Long.numberOfTrailingZeros(value)
                + Long.bitCount(value) + Long.highestOneBit(value) + Long.lowestOneBit(value)
                + Short.reverseBytes((short)value);
    }
    public static long $compile$bits(long value) {
        int low = (int)value;
        return Integer.reverse(low) + (long)Integer.reverseBytes(low)
                + Integer.numberOfLeadingZeros(low) + Integer.numberOfTrailingZeros(low)
                + Integer.bitCount(low) + Integer.highestOneBit(low) + Integer.lowestOneBit(low)
                + Long.reverse(value) + Long.reverseBytes(value)
                + Long.numberOfLeadingZeros(value) + Long.numberOfTrailingZeros(value)
                + Long.bitCount(value) + Long.highestOneBit(value) + Long.lowestOneBit(value)
                + Short.reverseBytes((short)value);
    }
    public static long mathBaseline(double value) {
        return Double.doubleToRawLongBits(Math.sqrt(value))
                ^ Double.doubleToRawLongBits(Math.ceil(value))
                ^ Double.doubleToRawLongBits(Math.floor(value))
                ^ Double.doubleToRawLongBits(Math.rint(value))
                ^ Math.round(value) ^ Math.round((float)value);
    }
    public static long $compile$math(double value) {
        return Double.doubleToRawLongBits(Math.sqrt(value))
                ^ Double.doubleToRawLongBits(Math.ceil(value))
                ^ Double.doubleToRawLongBits(Math.floor(value))
                ^ Double.doubleToRawLongBits(Math.rint(value))
                ^ Math.round(value) ^ Math.round((float)value);
    }
    public static long $compile$doubleBits(long bits) {
        return Double.doubleToRawLongBits(Double.longBitsToDouble(bits));
    }
    public static int $compile$floatBits(int bits) {
        return Float.floatToRawIntBits(Float.intBitsToFloat(bits));
    }
    public static int $compile$constants(String value) {
        int result = 0;
        if ("".equals(value)) result |= 1;
        if ("abc".equals(value)) result |= 2;
        if (value.equals("native-linux-jit")) result |= 4;
        if ("a string longer than thirty two bytes for equality".equals(value)) result |= 8;
        if (value.equals("安卓€🚀")) result |= 16;
        return result;
    }
    public static long mixBaseline(int rounds, long value) {
        for (int i = 0; i < rounds; i++) {
            value = Long.reverseBytes(value) + Long.bitCount(value) + i;
            value ^= Long.highestOneBit(value) + Long.numberOfTrailingZeros(value);
        }
        return value;
    }
    public static long $compile$mix(int rounds, long value) {
        for (int i = 0; i < rounds; i++) {
            value = Long.reverseBytes(value) + Long.bitCount(value) + i;
            value ^= Long.highestOneBit(value) + Long.numberOfTrailingZeros(value);
        }
        return value;
    }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    public static void checkResults() {
        long[] values = {0, 1, -1, Long.MIN_VALUE, Long.MAX_VALUE,
                0x80000000L, 0xffffffffL, 0x123456789abcdef0L};
        for (long value : values) check($compile$bits(value) == bitsBaseline(value), "bit boundary");
        long random = 413;
        for (int i = 0; i < 256; i++) {
            random = random * 6364136223846793005L + 1442695040888963407L;
            check($compile$bits(random) == bitsBaseline(random), "bit randomized result");
            check($compile$doubleBits(random) == random, "double raw bits");
            check($compile$floatBits((int)random) == (int)random, "float raw bits");
        }
        double[] fractions = {0.0, -0.0, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5,
                3.125, -3.125, Double.MIN_VALUE, Double.MAX_VALUE,
                Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY, Double.NaN};
        for (double value : fractions) check($compile$math(value) == mathBaseline(value), "math boundary");
        String[] constants = {"", "abc", "native-linux-jit",
                "a string longer than thirty two bytes for equality", "安卓€🚀"};
        for (int i = 0; i < constants.length; i++) {
            check($compile$constants(new String(constants[i].toCharArray())) == 1 << i,
                    "constant equality");
        }
        int comparisons = 0;
        for (int length : new int[]{0, 1, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33, 65, 257}) {
            for (int encoding = 0; encoding < 2; encoding++) {
                char[] chars = new char[length];
                for (int i = 0; i < length; i++) chars[i] = (char)(encoding == 0 ? 'a' + i % 26 : 0x4e00 + i);
                String left = new String(chars), right = new String(chars);
                System.gc();
                check(NceWorkload.$compile$stringEquals(left, left), "reference equality");
                check(NceWorkload.$compile$stringEquals(left, right), "content equality after GC");
                check(!NceWorkload.$compile$stringEquals(left, null), "null argument");
                check(!NceWorkload.$compile$stringEquals(left, new Object()), "non-string argument");
                if (length > 0) {
                    chars[length - 1] ^= 1;
                    check(!NceWorkload.$compile$stringEquals(left, new String(chars)), "last character mismatch");
                }
                comparisons++;
            }
        }
        int exceptions = 0;
        try { NceWorkload.$compile$stringEquals(null, "abc"); }
        catch (NullPointerException expected) { exceptions++; }
        try { $compile$constants(null); }
        catch (NullPointerException expected) { exceptions++; }
        check(exceptions == 2, "null receiver exceptions");
        System.out.println("{\"test\":\"android_art_jit_intrinsics\",\"passed\":true,"
                + "\"integer_inputs\":264,\"floating_inputs\":15,\"string_cases\":" + comparisons + "}");
    }
}
