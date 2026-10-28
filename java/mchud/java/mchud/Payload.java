package mchud;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.Optional;

/**
 * Java-часть, зашитая в DLL. Выполняется внутри процесса Minecraft (Fabric).
 * Этап 1: проверяет, что код реально работает в игре, и собирает отчёт об именах классов
 * и методов (intermediary) для 1.21.11. Отчёт печатается в консоль DLL и копируется в буфер обмена.
 */
public final class Payload implements Runnable {
    private static final StringBuilder REPORT = new StringBuilder();

    public static void start() {
        Thread t = new Thread(new Payload(), "mchud-payload");
        t.setDaemon(true);
        t.start();
    }

    @Override
    public void run() {
        try {
            report();
        } catch (Throwable t) {
            Native.log("[java] payload crashed: " + t);
        }
    }

    private static void out(String s) {
        REPORT.append(s).append('\n');
        Native.log(s);
    }

    private static Class<?> load(String name, ClassLoader cl) {
        try {
            return Class.forName(name, false, cl);
        } catch (Throwable t) {
            return null;
        }
    }

    private static String typeName(Class<?> c) {
        String n = c.getName();
        return n.startsWith("net.minecraft.") ? n.substring("net.minecraft.".length()) : n;
    }

    private static String sig(Method m) {
        StringBuilder sb = new StringBuilder();
        if (Modifier.isStatic(m.getModifiers())) sb.append("static ");
        sb.append(typeName(m.getReturnType())).append(' ').append(m.getName()).append('(');
        Class<?>[] p = m.getParameterTypes();
        for (int i = 0; i < p.length; i++) {
            if (i > 0) sb.append(", ");
            sb.append(typeName(p[i]));
        }
        return sb.append(')').toString();
    }

    private static boolean hasParam(Method m, Class<?> type) {
        if (type == null) return false;
        for (Class<?> p : m.getParameterTypes()) if (p == type) return true;
        return false;
    }

    private static String minecraftVersion(ClassLoader cl) {
        try {
            Class<?> fl = Class.forName("net.fabricmc.loader.api.FabricLoader", false, cl);
            Object inst = fl.getMethod("getInstance").invoke(null);
            Object opt = fl.getMethod("getModContainer", String.class).invoke(inst, "minecraft");
            Object container = ((Optional<?>) opt).get();
            Class<?> mc = Class.forName("net.fabricmc.loader.api.ModContainer", false, cl);
            Object meta = mc.getMethod("getMetadata").invoke(container);
            Class<?> mm = Class.forName("net.fabricmc.loader.api.metadata.ModMetadata", false, cl);
            Object ver = mm.getMethod("getVersion").invoke(meta);
            Class<?> vc = Class.forName("net.fabricmc.loader.api.Version", false, cl);
            return String.valueOf(vc.getMethod("getFriendlyString").invoke(ver));
        } catch (Throwable t) {
            return "unknown (" + t.getClass().getSimpleName() + ")";
        }
    }

    private static void report() {
        ClassLoader cl = Payload.class.getClassLoader();
        out("[java] payload is running inside the game");
        out("[java] Java " + System.getProperty("java.version") + ", loader " + cl.getClass().getName());
        out("[java] Minecraft version (Fabric): " + minecraftVersion(cl));

        String[] names = {
            "net.minecraft.class_310",  // MinecraftClient
            "net.minecraft.class_329",  // InGameHud
            "net.minecraft.class_332",  // DrawContext
            "net.minecraft.class_327",  // TextRenderer
            "net.minecraft.class_757",  // GameRenderer
            "net.minecraft.class_9779", // RenderTickCounter
            "net.fabricmc.fabric.api.client.rendering.v1.HudRenderCallback",
            "net.fabricmc.fabric.api.client.rendering.v1.hud.HudElementRegistry",
            "org.objectweb.asm.ClassReader",
        };
        for (String n : names) out("[java] class " + n + " : " + (load(n, cl) != null ? "OK" : "MISSING"));

        Class<?> mc = load("net.minecraft.class_310", cl);
        Class<?> hud = load("net.minecraft.class_329", cl);
        Class<?> ctx = load("net.minecraft.class_332", cl);
        Class<?> text = load("net.minecraft.class_327", cl);
        Class<?> gr = load("net.minecraft.class_757", cl);
        Class<?> tick = load("net.minecraft.class_9779", cl);

        if (mc != null) {
            out("--- class_310 (MinecraftClient): static getters and key fields ---");
            for (Method m : mc.getDeclaredMethods())
                if (Modifier.isStatic(m.getModifiers()) && m.getReturnType() == mc && m.getParameterCount() == 0)
                    out("  " + sig(m));
            for (Field f : mc.getDeclaredFields()) {
                Class<?> t = f.getType();
                if (t == hud || t == text || t == gr || t == mc)
                    out("  field " + f.getName() + " : " + typeName(t) + (Modifier.isStatic(f.getModifiers()) ? " (static)" : ""));
            }
        }
        if (hud != null) {
            out("--- class_329 (InGameHud): methods taking class_332 ---");
            for (Method m : hud.getDeclaredMethods()) if (hasParam(m, ctx)) out("  " + sig(m));
        }
        if (ctx != null) {
            out("--- class_332 (DrawContext): methods taking class_327 ---");
            int n = 0;
            for (Method m : ctx.getDeclaredMethods()) {
                if (hasParam(m, text) && n++ < 40) out("  " + sig(m));
            }
        }
        if (tick != null) {
            out("--- class_9779 (RenderTickCounter): methods ---");
            for (Method m : tick.getDeclaredMethods()) out("  " + sig(m));
        }

        out("[java] done");
        try {
            java.awt.Toolkit.getDefaultToolkit().getSystemClipboard()
                .setContents(new java.awt.datatransfer.StringSelection(REPORT.toString()), null);
            Native.log("[java] report copied to clipboard - paste it into the chat (Ctrl+V)");
        } catch (Throwable t) {
            Native.log("[java] clipboard unavailable: " + t + " - copy the text from this window instead");
        }
    }
}
