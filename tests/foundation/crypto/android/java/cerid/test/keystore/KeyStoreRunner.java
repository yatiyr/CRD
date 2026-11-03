// crd-crypto tests -- the instrumentation that runs crd-crypto's Android Keystore case in an application process
// (NET.2, decision K15): a plain executable has no Java VM, so the Keystore protector and key device are proven here,
// in the app's own VM, uid and SELinux domain, as every Cerid product on Android reaches them.
//
// The extras `argc` and `arg0`..`arg<argc-1>` are the Catch2 arguments; the native side writes Catch2's output to
// `files/catch.txt`, which scripts/android-device.py reads through run-as. The result bundle carries `exit` (Catch2's
// exit code) or `error` (why the case could not start).
package cerid.test.keystore;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import android.system.Os;

import java.io.File;

public final class KeyStoreRunner extends Instrumentation
{
    private Bundle extras = new Bundle();

    private static native int run(String[] arguments, String output);

    @Override
    public void onCreate(Bundle arguments)
    {
        if (arguments != null)
        {
            extras = arguments;
        }
        start();
    }

    @Override
    public void onStart()
    {
        final Bundle result = new Bundle();
        int code = -1;
        try
        {
            final File files = getTargetContext().getFilesDir();
            Os.setenv("TMPDIR", getTargetContext().getCacheDir().getPath(), true);
            System.loadLibrary("crd-crypto-keystore-app");
            final int count = Integer.parseInt(extras.getString("argc", "0"));
            final String[] arguments = new String[count + 1];
            arguments[0] = "crd-crypto-keystore-app";
            for (int i = 0; i < count; ++i)
            {
                arguments[i + 1] = extras.getString("arg" + i, "");
            }
            code = run(arguments, new File(files, "catch.txt").getPath());
            result.putString("exit", Integer.toString(code));
        }
        catch (Throwable error)
        {
            result.putString("error", error.toString());
        }
        finish(code == 0 ? Activity.RESULT_OK : Activity.RESULT_CANCELED, result);
    }
}
