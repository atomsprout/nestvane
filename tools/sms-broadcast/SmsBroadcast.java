import android.content.Context;
import android.content.Intent;
import android.os.Looper;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.List;
import java.util.TimeZone;

/**
 * Tells Android's apps that a text message arrived: the SMS_RECEIVED broadcast a phone's radio
 * layer sends, carrying the message as the PDUs a network would have delivered. The guest has no
 * phone process, so nothing else sends it. Run as root by fixtures/android17-sms.sh with the
 * sender and the text; only the system may send this broadcast.
 */
public final class SmsBroadcast {
    private static final String TAG = "NestvaneSms";

    public static void main(String[] args) throws Exception {
        if (args.length != 2) throw new IllegalArgumentException("usage: SmsBroadcast <sender> <text>");
        List<byte[]> pdus = pdus(args[0], args[1], System.currentTimeMillis());

        Looper.prepareMainLooper();
        Class<?> thread = Class.forName("android.app.ActivityThread");
        Object main = thread.getMethod("systemMain").invoke(null);
        Context context = (Context) thread.getMethod("getSystemContext").invoke(main);

        Intent intent = new Intent("android.provider.Telephony.SMS_RECEIVED");
        intent.putExtra("pdus", pdus.toArray());
        intent.putExtra("format", "3gpp");
        intent.putExtra("subscription", 1);
        intent.putExtra("phone", 0);
        intent.addFlags(0x01000000);  // FLAG_RECEIVER_INCLUDE_BACKGROUND: receivers in manifests too
        context.sendBroadcast(intent, "android.permission.RECEIVE_SMS");
        Log.i(TAG, "sent SMS_RECEIVED from " + args[0] + " in " + pdus.size() + " part(s)");
        // The broadcast is with the system by now. Leaving main is not enough: the system context has
        // started threads that keep the process alive, sometimes for good.
        System.exit(0);
    }

    /** SMS-DELIVER PDUs in UCS-2: one for up to 70 characters, otherwise parts of 67 joined by a header. */
    static List<byte[]> pdus(String sender, String text, long now) {
        byte[] body = text.getBytes(StandardCharsets.UTF_16BE);
        List<byte[]> out = new ArrayList<>();
        if (body.length <= 140) {
            out.add(pdu(sender, body, null, now));
            return out;
        }
        int parts = Math.min(255, (body.length + 133) / 134);
        int reference = (int) (now & 0xff);
        for (int part = 0; part < parts; ++part) {
            int from = part * 134, length = Math.min(134, body.length - from);
            byte[] piece = new byte[length];
            System.arraycopy(body, from, piece, 0, length);
            out.add(pdu(sender, piece, new byte[] {5, 0, 3, (byte) reference, (byte) parts, (byte) (part + 1)}, now));
        }
        return out;
    }

    private static byte[] pdu(String sender, byte[] body, byte[] header, long now) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(0);                          // no service centre address
        out.write(header == null ? 0x04 : 0x44);  // SMS-DELIVER, no more messages; with a user data header
        address(out, sender);
        out.write(0);                          // protocol identifier
        out.write(0x08);                       // UCS-2
        Calendar time = Calendar.getInstance(TimeZone.getTimeZone("UTC"));
        time.setTimeInMillis(now);
        for (int field : new int[] {time.get(Calendar.YEAR) % 100, time.get(Calendar.MONTH) + 1,
                time.get(Calendar.DAY_OF_MONTH), time.get(Calendar.HOUR_OF_DAY), time.get(Calendar.MINUTE),
                time.get(Calendar.SECOND), 0}) {
            out.write((field % 10) << 4 | field / 10);
        }
        out.write(body.length + (header == null ? 0 : header.length));
        if (header != null) out.write(header, 0, header.length);
        out.write(body, 0, body.length);
        return out.toByteArray();
    }

    /** A number as digits, or anything else as up to eleven letters of the 7-bit alphabet. */
    private static void address(ByteArrayOutputStream out, String sender) {
        String digits = sender.replaceAll("[ ()-]", "");
        boolean international = digits.startsWith("+");
        if (international) digits = digits.substring(1);
        if (digits.matches("[0-9]{1,20}")) {
            out.write(digits.length());
            out.write(international ? 0x91 : 0x81);
            for (int at = 0; at < digits.length(); at += 2) {
                int low = digits.charAt(at) - '0';
                int high = at + 1 < digits.length() ? digits.charAt(at + 1) - '0' : 0xf;
                out.write(high << 4 | low);
            }
            return;
        }
        String name = sender.length() > 11 ? sender.substring(0, 11) : sender;
        int septets = name.length(), octets = (septets * 7 + 7) / 8;
        byte[] packed = new byte[octets];
        for (int index = 0; index < septets; ++index) {
            char letter = name.charAt(index);
            // Letters, digits and the common punctuation sit at the same places in the 7-bit alphabet as in ASCII.
            int septet = letter >= 0x20 && letter < 0x7f && "$@[\\]^_`{|}~".indexOf(letter) < 0 ? letter : '?';
            int bit = index * 7;
            packed[bit / 8] |= (byte) (septet << (bit % 8));
            if (bit % 8 > 1) packed[bit / 8 + 1] |= (byte) (septet >> (8 - bit % 8));
        }
        out.write((septets * 7 + 3) / 4);
        out.write(0xd0);
        out.write(packed, 0, octets);
    }
}
