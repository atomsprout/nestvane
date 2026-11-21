public class Hello {
    public static void main(String[] args) {
        StringBuilder text = new StringBuilder("hello from ART:");
        for (String argument : args) text.append(' ').append(argument);
        long sum = 0;
        for (int index = 0; index < 1000000; ++index) sum += index % 7;
        System.out.println(text + " sum=" + sum);
    }
}
