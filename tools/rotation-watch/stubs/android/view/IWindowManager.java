package android.view;

/** Only what RotationWatch is compiled against; the guest's own class is the one that runs. */
public interface IWindowManager extends android.os.IInterface {
    int watchRotation(IRotationWatcher watcher, int displayId) throws android.os.RemoteException;

    abstract class Stub {
        public static IWindowManager asInterface(android.os.IBinder binder) { throw new RuntimeException("stub"); }
    }
}
