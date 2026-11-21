import java.util.Arrays;
import java.util.Random;
import java.util.zip.CRC32;

// Work that runs inside the boot class path, which is compiled ahead of time on both sides of the
// comparison. The loops here only call into it, so the interpreter that runs this class matters little.
public class Bench {
    static long mark;

    static void phase(String name, long check) {
        final long now = System.nanoTime();
        System.out.println(name + " ms=" + (now - mark) / 1000000 + " check=" + check);
        mark = System.nanoTime();
    }

    public static void main(String[] args) throws Exception {
        final long begin = mark = System.nanoTime();
        int[] numbers = new Random(1).ints(4000000).toArray();
        phase("random_ints", numbers[12345]);
        Arrays.sort(numbers);
        phase("sort_ints", numbers[2000000]);
        String text = "lorem ipsum dolor sit amet consectetur ".repeat(300000);
        phase("repeat", text.length());
        String[] words = text.split(" ");
        phase("split", words.length);
        Arrays.sort(words);
        phase("sort_strings", words[words.length / 2].length());
        String joined = String.join(",", words);
        phase("join", joined.length());
        String replaced = joined.replace("lorem", "LOREM").toUpperCase();
        phase("replace_upper", replaced.hashCode());
        byte[] bytes = replaced.getBytes("UTF-8");
        String back = new String(bytes, "UTF-8");
        phase("utf8", back.length());
        CRC32 crc = new CRC32();
        for (int round = 0; round < 20; ++round) crc.update(bytes);
        phase("crc32", crc.getValue());
        StringBuilder builder = new StringBuilder(back);
        builder.reverse();
        phase("reverse", builder.charAt(7));
        System.out.println("total ms=" + (System.nanoTime() - begin) / 1000000);
    }
}
