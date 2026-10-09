/* SPDX-License-Identifier: Apache-2.0 */
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.reflect.Proxy;

public final class NceProxy {
    @Retention(RetentionPolicy.RUNTIME)
    public @interface Info { int number(); Class<?> kind(); }
    @Info(number = 37, kind = String.class)
    public static final class Annotated {}
    public interface Values {
        int number(int first, Object second, long third);
        Object echo(Object value);
        Object many(Object a, Object b, Object c, Object d, Object e, Object f, Object g, Object h);
        void fail() throws Exception;
    }
    private static void check(boolean value) {
        if (!value) throw new AssertionError("proxy reference conversion");
    }
    public static void run() throws Exception {
        Info annotation = Annotated.class.getAnnotation(Info.class);
        check(annotation.number() == 37 && annotation.kind() == String.class);
        Object retained = new Object();
        Values proxy = (Values) Proxy.newProxyInstance(Values.class.getClassLoader(),
                new Class<?>[]{Values.class}, (receiver, method, arguments) -> {
            switch (method.getName()) {
                case "number":
                    check(arguments[1] == retained);
                    return (Integer) arguments[0] + (int) (long) (Long) arguments[2];
                case "echo":
                    System.gc();
                    return arguments[0];
                case "many":
                    System.gc();
                    for (Object argument : arguments) check(argument == retained);
                    return arguments[7];
                case "fail": throw new Exception("proxy-check");
                default: throw new AssertionError(method.getName());
            }
        });
        check(proxy.number(19, retained, 23L) == 42);
        check(proxy.echo(retained) == retained && proxy.echo(null) == null);
        check(proxy.many(retained, retained, retained, retained, retained, retained,
                         retained, retained) == retained);
        boolean caught = false;
        try { proxy.fail(); }
        catch (Exception expected) { caught = "proxy-check".equals(expected.getMessage()); }
        check(caught);
        System.out.println("{\"test\":\"android_art_proxy_references\",\"passed\":true,"
                + "\"annotation\":true,\"gc_requests\":3,\"stack_arguments\":true,\"exception\":true}");
    }
}
