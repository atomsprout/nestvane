package android.os;

/** Only what RotationWatch is compiled against; the guest's own class is the one that runs. */
public final class ServiceManager {
    public static IBinder getService(String name) { throw new RuntimeException("stub"); }
}
