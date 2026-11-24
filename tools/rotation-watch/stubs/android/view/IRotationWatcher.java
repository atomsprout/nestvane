package android.view;

/** Only what RotationWatch is compiled against; the guest's own class is the one that runs. */
public interface IRotationWatcher extends android.os.IInterface {
    void onRotationChanged(int rotation) throws android.os.RemoteException;

    abstract class Stub extends android.os.Binder implements IRotationWatcher {
        public Stub() { throw new RuntimeException("stub"); }
        @Override public android.os.IBinder asBinder() { throw new RuntimeException("stub"); }
    }
}
