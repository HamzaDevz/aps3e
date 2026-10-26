// SPDX-License-Identifier: WTFPL
package aenu.aps3e;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

//The emulator boots a single game per process: this starts the next game
//once the process of the previous one has exited
public class RelaunchActivity extends Activity {

	static final String EXTRA_TARGET = "relaunch_target";

	static final long POLL_MS = 100;
	static final int MAX_POLLS = 50;

	static void relaunch(Activity from, Intent target) {
		Intent intent = new Intent(from, RelaunchActivity.class)
				.putExtra(EXTRA_TARGET, new Intent(target))
				.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_NO_ANIMATION);
		from.startActivity(intent);
	}

	final Handler handler = new Handler(Looper.getMainLooper());
	int polls = 0;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		handler.post(this::start_when_gone);
	}

	void start_when_gone() {
		if (emulator_running() && ++polls < MAX_POLLS) {
			handler.postDelayed(this::start_when_gone, POLL_MS);
			return;
		}

		Intent target = getIntent().getParcelableExtra(EXTRA_TARGET);
		if (target != null) {
			target.setComponent(new ComponentName(this, EmulatorActivity.class));
			target.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
			startActivity(target);
		}
		finish();
		overridePendingTransition(0, 0);
	}

	boolean emulator_running() {
		ActivityManager am = getSystemService(ActivityManager.class);
		String emu_process = getPackageName() + ":emu";
		for (ActivityManager.RunningAppProcessInfo info : am.getRunningAppProcesses()) {
			if (emu_process.equals(info.processName))
				return true;
		}
		return false;
	}
}
