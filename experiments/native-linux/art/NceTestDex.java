/* SPDX-License-Identifier: Apache-2.0 */
import com.android.tools.smali.dexlib2.*;
import com.android.tools.smali.dexlib2.iface.*;
import com.android.tools.smali.dexlib2.iface.instruction.Instruction;
import com.android.tools.smali.dexlib2.iface.instruction.formats.Instruction35c;
import com.android.tools.smali.dexlib2.iface.reference.MethodReference;
import com.android.tools.smali.dexlib2.immutable.*;
import com.android.tools.smali.dexlib2.immutable.instruction.ImmutableInstruction35c;
import com.android.tools.smali.dexlib2.immutable.reference.ImmutableMethodReference;
import com.android.tools.smali.dexlib2.writer.pool.DexPool;
import java.io.File;
import java.util.*;

// 仅生成回归 DEX；javac/D8 会将这三个调用写为 invoke-virtual Object。
public final class NceTestDex {
    public static void main(String[] args) throws Exception {
        DexFile input = DexFileFactory.loadDexFile(new File(args[0]), Opcodes.forApi(34));
        Map<String, String> targets = Map.of("interfaceEquals", "equals", "interfaceHash", "hashCode", "interfaceString", "toString");
        Set<String> changed = new HashSet<>();
        List<ClassDef> classes = new ArrayList<>();
        for (ClassDef type : input.getClasses()) {
            if (!type.getType().equals("LNceFastBaseline;")) { classes.add(type); continue; }
            List<Method> methods = new ArrayList<>();
            for (Method method : type.getMethods()) {
                if (!targets.containsKey(method.getName())) { methods.add(method); continue; }
                MethodImplementation original = method.getImplementation();
                List<Instruction> instructions = new ArrayList<>();
                for (Instruction instruction : original.getInstructions()) instructions.add(instruction);
                Instruction35c call = (Instruction35c) instructions.get(0);
                MethodReference reference = (MethodReference) call.getReference();
                if (call.getOpcode() != Opcode.INVOKE_VIRTUAL || !reference.getDefiningClass().equals("Ljava/lang/Object;") ||
                    !reference.getName().equals(targets.get(method.getName()))) {
                    throw new IllegalStateException("Unexpected ART regression DEX: " + method.getName());
                }
                instructions.set(0, new ImmutableInstruction35c(Opcode.INVOKE_INTERFACE, call.getRegisterCount(),
                    call.getRegisterC(), call.getRegisterD(), call.getRegisterE(), call.getRegisterF(), call.getRegisterG(),
                    new ImmutableMethodReference("LNceFastBaseline$Reader;", reference.getName(), reference.getParameterTypes(), reference.getReturnType())));
                MethodImplementation replacement = new ImmutableMethodImplementation(original.getRegisterCount(), instructions,
                    original.getTryBlocks(), original.getDebugItems());
                methods.add(new ImmutableMethod(method.getDefiningClass(), method.getName(), method.getParameters(), method.getReturnType(),
                    method.getAccessFlags(), method.getAnnotations(), method.getHiddenApiRestrictions(), replacement));
                changed.add(method.getName());
            }
            classes.add(new ImmutableClassDef(type.getType(), type.getAccessFlags(), type.getSuperclass(), type.getInterfaces(),
                type.getSourceFile(), type.getAnnotations(), type.getFields(), methods));
        }
        if (!changed.equals(targets.keySet())) throw new IllegalStateException("Missing ART interface regression methods");
        DexPool.writeTo(args[0], new ImmutableDexFile(input.getOpcodes(), classes));
    }
}
