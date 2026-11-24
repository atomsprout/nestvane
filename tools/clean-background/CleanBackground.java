import android.util.Log;

import java.lang.reflect.Field;
import java.util.List;

/**
 * Clears the apps in the background, as Clear all among Android's recent apps does: removes every
 * recent task except the ones on the screen and the home screen's, and prints the packages they were of,
 * one a line. Removing a task only marks its process to be ended later; the script that runs this
 * (fixtures/android17-sms.sh, as root, when the person long-presses Home in Nestvane) ends those
 * packages' background processes at once and then the cached ones that are left.
 */
public final class CleanBackground {
    private static final String TAG = "Nestvane";
    private static final int ACTIVITY_TYPE_HOME = 2;

    public static void main(String[] args) throws Exception {
        Object tasks = Class.forName("android.app.ActivityTaskManager").getMethod("getService").invoke(null);
        Object slice = tasks.getClass().getMethod("getRecentTasks", int.class, int.class, int.class)
                .invoke(tasks, 100, 0, 0);
        List<?> recent = (List<?>) slice.getClass().getMethod("getList").invoke(slice);
        int removed = 0, kept = 0;
        for (int index = 0; index < recent.size(); ++index) {
            Object info = recent.get(index);
            int id = field(info, "taskId");
            boolean home = field(info, "topActivityType") == ACTIVITY_TYPE_HOME;
            // What is on the screen stays. The list leaves the home screen out, so its first task is
            // not always the one in front.
            Boolean visible = flag(info, "isVisible");
            if (home || (visible != null ? visible : index == 0)) {
                ++kept;
                continue;
            }
            String packageName = component(info, "baseActivity");
            if (packageName == null) packageName = component(info, "topActivity");
            if ((Boolean) tasks.getClass().getMethod("removeTask", int.class).invoke(tasks, id)) {
                ++removed;
                if (packageName != null) System.out.println(packageName);
            }
        }
        Log.i(TAG, "clean: removed " + removed + " recent tasks, kept " + kept);
    }

    private static Boolean flag(Object object, String name) {
        for (Class<?> type = object.getClass(); type != null; type = type.getSuperclass()) {
            try {
                Field field = type.getDeclaredField(name);
                field.setAccessible(true);
                return field.getBoolean(object);
            } catch (ReflectiveOperationException e) {
                // in a superclass
            }
        }
        return null;
    }

    private static String component(Object object, String name) {
        for (Class<?> type = object.getClass(); type != null; type = type.getSuperclass()) {
            try {
                Field field = type.getDeclaredField(name);
                field.setAccessible(true);
                Object value = field.get(object);
                return value == null ? null : ((android.content.ComponentName) value).getPackageName();
            } catch (ReflectiveOperationException e) {
                // in a superclass
            }
        }
        return null;
    }

    private static int field(Object object, String name) throws ReflectiveOperationException {
        for (Class<?> type = object.getClass(); type != null; type = type.getSuperclass()) {
            try {
                Field field = type.getDeclaredField(name);
                field.setAccessible(true);
                return field.getInt(object);
            } catch (NoSuchFieldException e) {
                // in a superclass
            }
        }
        throw new NoSuchFieldException(name);
    }
}
