/* SPDX-License-Identifier: Apache-2.0 */
public final class NceInlining {
    public static final class Node { int value; Node(int value) { this.value = value; } }
    public static int leaf(Node node) { return ++node.value; }
    public static int middle(Node[] nodes, int index) { return leaf(nodes[index]); }
    public static long $compile$run(Node[] nodes, int rounds) {
        long sum = 0;
        for (int i = 0; i < rounds; i++) sum += middle(nodes, i & 7);
        return sum;
    }
    public static Node preserve(Node node) { System.gc(); node.value++; return node; }
    public static Node $compile$gc(Node node) {
        for (int i = 0; i < 16; i++) node = preserve(node);
        return node;
    }
    public static Node[] fixture() {
        Node[] nodes = new Node[8];
        for (int i = 0; i < nodes.length; i++) nodes[i] = new Node(i);
        return nodes;
    }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    public static void checkResults() {
        Node[] nodes = fixture();
        Node retained = nodes[3];
        check($compile$gc(retained) == retained && retained.value == 19, "inlined GC references");
        nodes[0] = null;
        int found = 0;
        try { $compile$run(nodes, 1); }
        catch (NullPointerException expected) {
            for (StackTraceElement frame : expected.getStackTrace()) {
                if (frame.getClassName().equals("NceInlining")) {
                    if (frame.getMethodName().equals("leaf")) found |= 1;
                    if (frame.getMethodName().equals("middle")) found |= 2;
                    if (frame.getMethodName().equals("$compile$run")) found |= 4;
                }
            }
        }
        check(found == 7, "inlined exception stack retains all Java frames");
        int bounds = 0;
        try { $compile$run(new Node[0], 1); }
        catch (ArrayIndexOutOfBoundsException expected) { bounds++; }
        check(bounds == 1, "inlined array bounds exception");
    }
}
