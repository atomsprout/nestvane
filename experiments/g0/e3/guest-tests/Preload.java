import java.io.BufferedReader;
import java.io.FileReader;

// Loads and initializes the classes of a preloaded-classes list, as the zygote does at startup.
// Under dalvikvm the framework's native methods are not registered, so initializers that need them
// fail. The count says how much of the list this covers.
public class Preload {
    public static void main(String[] args) throws Exception {
        final long begin = System.nanoTime();
        int loaded = 0, missing = 0, failed = 0;
        String firstFailure = null;
        try (BufferedReader reader = new BufferedReader(new FileReader(args[0]))) {
            for (String line = reader.readLine(); line != null; line = reader.readLine()) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                try {
                    Class.forName(line, true, null);
                    ++loaded;
                } catch (ClassNotFoundException e) {
                    ++missing;
                } catch (Throwable e) {
                    ++failed;
                    if (firstFailure == null) firstFailure = line + ": " + e;
                }
            }
        }
        System.out.println("loaded=" + loaded + " missing=" + missing + " failed=" + failed + " ms=" + (System.nanoTime() - begin) / 1000000);
        if (firstFailure != null) System.out.println("first failure: " + firstFailure);
    }
}
