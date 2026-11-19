public final class NestvaneArt {
    public static void main(String[] args) {
        int value = 0;
        for (int i = 1; i <= 6; i++) {
            value += i * 2;
        }
        if (value != 42) {
            throw new AssertionError(value);
        }
        System.out.println("nestvane ART computed 42");
    }
}
