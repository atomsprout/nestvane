package dev.nestvane.smoke;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        SharedPreferences preferences = getSharedPreferences("state", MODE_PRIVATE);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);

        int savedCount = preferences.getInt("count", 0);
        TextView value = new TextView(this);
        value.setTextSize(24);
        value.setText("Count: " + savedCount);
        layout.addView(value);

        Button increment = new Button(this);
        increment.setText("Increment");
        increment.setOnClickListener(view -> {
            int next = preferences.getInt("count", 0) + 1;
            if (preferences.edit().putInt("count", next).commit()) {
                value.setText("Count: " + next);
                Log.i("NestvaneSmoke", "Saved count " + next);
            } else {
                value.setText("Could not save count");
            }
        });
        layout.addView(increment);
        Button camera = new Button(this);
        camera.setText("Check rear camera");
        camera.setOnClickListener(view -> startActivity(new Intent(this, CameraActivity.class)));
        layout.addView(camera);
        setContentView(layout);
        Log.i("NestvaneSmoke", "Opened activity with count " + savedCount);
    }
}
