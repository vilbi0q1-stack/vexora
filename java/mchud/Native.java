package mchud;

/** Мост в DLL: метод регистрируется из C++ через RegisterNatives и печатает в консоль DLL. */
final class Native {
    private Native() {}

    static native void log(String s);
}
