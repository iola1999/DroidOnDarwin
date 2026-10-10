/* SPDX-License-Identifier: Apache-2.0 */
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.invoke.VarHandle;

public final class NceInterpreter {
    private volatile Object value;
    private static volatile Object shared;

    public static Object identity(Object value) { return value; }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void run() throws Throwable {
        NceInterpreter holder = new NceInterpreter();
        Object first = new Object();
        Object second = new Object();
        MethodHandles.Lookup lookup = MethodHandles.lookup();
        MethodHandle identity = lookup.findStatic(NceInterpreter.class, "identity",
                MethodType.methodType(Object.class, Object.class));
        check((Object)identity.invokeExact(first) == first, "invokeExact reference");
        check(identity.invoke(second) == second, "invoke reference");
        check((Object)identity.invokeExact((Object)null) == null, "invokeExact null");

        VarHandle field = lookup.findVarHandle(NceInterpreter.class, "value", Object.class);
        field.setVolatile(holder, first);
        check(field.getVolatile(holder) == first, "VarHandle reference load");
        check(field.compareAndSet(holder, first, second), "VarHandle reference CAS");
        check(field.getAndSet(holder, null) == second && holder.value == null, "VarHandle null store");
        holder.value = first;
        shared = second;
        Object[] objects = {first, second, null};
        int[] numbers = {17, -23, 41};
        check(objects.length == 3 && objects[2] == null && numbers[1] == -23, "array initialization");
        objects[2] = first;
        synchronized (holder) {
            System.gc();
            check(holder.value == first && shared == second && objects[2] == first, "live references across GC");
        }
        check(objects instanceof Object[] && !((Object)numbers instanceof Object[]), "array type checks");
        Object strings = new String[] {"kept"};
        check(((Object[])strings)[0].equals("kept"), "array cast");
        int caught = 0;
        try { Object[] incompatible = (Object[])(Object)numbers; shared = incompatible; }
        catch (ClassCastException expected) { caught++; }
        try { ((Object[])strings)[0] = first; }
        catch (ArrayStoreException expected) { caught++; }
        try { synchronized ((Object)null) { shared = first; } }
        catch (NullPointerException expected) { caught++; }
        check(caught == 3, "reference exceptions");
        shared = null;
        System.out.println("{\"test\":\"android_art_interpreter_references\",\"passed\":true,"
                + "\"method_handles\":3,\"var_handles\":4,\"exceptions\":3}");
    }
}
