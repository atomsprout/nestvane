package dev.nestvane.webview;

import android.app.Activity;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.webkit.ConsoleMessage;
import android.webkit.CookieManager;
import android.webkit.WebChromeClient;
import android.webkit.WebView;
import android.webkit.WebViewClient;

public final class MainActivity extends Activity {
    private static final String TAG = "NestvaneWebView";
    private static final String PAGE = "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
            + "<style>body{font-family:sans-serif;margin:24px;background:#fff8f0}h1{color:#e8590c}"
            + ".box{width:120px;height:60px;background:linear-gradient(90deg,#ffa94d,#e8590c);border-radius:12px}</style>"
            + "</head><body><h1>WebView works</h1><p id=t>Script has not run.</p><div class=box></div>"
            + "<ul><li>one</li><li>two</li><li>three</li></ul>"
            + "<script>var s=0;for(var i=0;i<100000;i++){s+=i%7}"
            + "document.getElementById('t').textContent='Script ran: '+s;"
            + "console.log('script ran '+s);requestAnimationFrame(function(){console.log('frame drawn')});</script>"
            + "</body></html>";
    private long started;

    private void say(String text) {
        Log.i(TAG, (SystemClock.uptimeMillis() - started) + " ms: " + text);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        started = SystemClock.uptimeMillis();
        say("creating the cookie manager");
        CookieManager.getInstance();
        say("cookie manager made");
        WebView view = new WebView(this);
        say("WebView made: " + WebView.getCurrentWebViewPackage());
        view.getSettings().setJavaScriptEnabled(true);
        view.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageStarted(WebView view, String url, Bitmap favicon) {
                say("page started " + url.substring(0, Math.min(url.length(), 60)));
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                say("page finished, title " + view.getTitle());
            }
        });
        view.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onConsoleMessage(ConsoleMessage message) {
                say("console: " + message.message());
                return true;
            }
        });
        view.postVisualStateCallback(1, new WebView.VisualStateCallback() {
            @Override
            public void onComplete(long requestId) {
                say("visual state ready");
            }
        });
        setContentView(view);
        String url = getIntent().getStringExtra("url");
        if (url != null) {
            view.loadUrl(url);
        } else {
            view.loadDataWithBaseURL("https://nestvane.test/", PAGE, "text/html", "utf-8", null);
        }
        say("load asked");
    }
}
