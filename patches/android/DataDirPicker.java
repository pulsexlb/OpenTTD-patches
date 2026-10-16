/*
First-run choice of the game data directory, so the player can keep savegames
and downloaded content somewhere visible (external storage) instead of the
app-private directory.

Simplified port of the OpenTTD JGRPP Android port's DataDirPicker, adapted to
pelya's SDL2 template (project/javaSDL2/): the choice is stored in
SharedPreferences and applied to Globals.DataDir before Settings.setEnvVars()
and Settings.nativeChdir() run, so no C or game code is involved.

Differences from the original: the choice is only offered on the first run and
happens before anything is downloaded, so there is no data migration and no
process restart -- the normal start-up continues in place once the player has
answered. Changing the directory later is not offered (there is no settings
menu on the SDL2 path).

ChangeAppSettings.sh rewrites the package line to the app's package.
*/
package net.sourceforge.clonekeenplus;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.util.Log;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;

class DataDirPicker
{
	/* Must not clash with SettingsMenuMisc.StorageAccessConfig.REQUEST_STORAGE_ID (42). */
	private static final int REQ_PICK_DATA_DIR = 43;
	private static final int REQ_ALL_FILES_ACCESS = 44;

	private static final String PREFS_NAME = "openttd_datadir";
	private static final String PREF_USER_DIR = "user_dir";
	private static final String TAG = "SDL";

	/* Continuation of MainActivity.onCreate() once the directory is settled. */
	private static Runnable pendingContinue = null;

	static String getUserDir(final Context p)
	{
		try
		{
			SharedPreferences prefs = p.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
			String s = prefs.getString(PREF_USER_DIR, "");
			return s == null ? "" : s;
		}
		catch (Exception e)
		{
			return "";
		}
	}

	static void setUserDir(final Context p, final String path)
	{
		try
		{
			p.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
					.edit().putString(PREF_USER_DIR, path).commit();
			Log.i(TAG, "DataDirPicker: saved data directory: " + path);
		}
		catch (Exception e)
		{
			Log.i(TAG, "DataDirPicker: cannot save preference: " + e);
		}
	}

	/**
	 * Applies a previously stored choice to Globals.DataDir.
	 * @return true when a usable stored directory was applied.
	 */
	static boolean applyStoredDir(final Context p)
	{
		String dir = getUserDir(p);
		if (dir.length() == 0) return false;
		if (testWritable(dir))
		{
			Globals.DataDir = dir;
			Log.i(TAG, "DataDirPicker: using stored data directory " + dir);
			return true;
		}
		Log.i(TAG, "DataDirPicker: stored data directory is not writable, falling back: " + dir);
		return false;
	}

	/** True while the player has not answered yet on a device with external storage. */
	static boolean needsFirstRunChoice(final Context p)
	{
		return getUserDir(p).length() == 0
				&& Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState());
	}

	/** Asks once, then always runs onDone on the UI thread. */
	static void startFirstRun(final MainActivity p, final Runnable onDone)
	{
		pendingContinue = onDone;
		Log.i(TAG, "DataDirPicker: first run - asking for the game data directory");
		AlertDialog.Builder builder = new AlertDialog.Builder(p);
		builder.setTitle("OpenTTD data folder / 数据目录");
		builder.setMessage("Where should savegames and downloaded content be stored?\n"
				+ "Choose an external folder to keep them visible outside the app.\n\n"
				+ "存档和下载的内容保存在哪里？选择外部文件夹可让文件在应用之外可见。");
		builder.setPositiveButton("Choose folder / 选择文件夹", new DialogInterface.OnClickListener()
		{
			public void onClick(DialogInterface dialog, int item)
			{
				dialog.dismiss();
				proceed(p);
			}
		});
		builder.setNegativeButton("Internal storage / 内部存储", new DialogInterface.OnClickListener()
		{
			public void onClick(DialogInterface dialog, int item)
			{
				dialog.dismiss();
				/* Remember the decision so the question is not asked again. */
				setUserDir(p, p.getFilesDir().getAbsolutePath());
				finish();
			}
		});
		builder.setOnCancelListener(new DialogInterface.OnCancelListener()
		{
			public void onCancel(DialogInterface dialog)
			{
				setUserDir(p, p.getFilesDir().getAbsolutePath());
				finish();
			}
		});
		AlertDialog alert = builder.create();
		alert.setOwnerActivity(p);
		alert.show();
	}

	private static void proceed(final MainActivity p)
	{
		if (!hasAllFilesAccess())
		{
			requestAllFilesAccess(p);
		}
		else
		{
			launchPicker(p);
		}
	}

	/* Always true before Android 11, where a picked folder needs no extra grant. */
	private static boolean hasAllFilesAccess()
	{
		return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
	}

	private static void requestAllFilesAccess(final MainActivity p)
	{
		try
		{
			p.startActivityForResult(new Intent(
					android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
					Uri.parse("package:" + p.getPackageName())), REQ_ALL_FILES_ACCESS);
		}
		catch (Exception e)
		{
			try
			{
				p.startActivityForResult(new Intent(
						android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION), REQ_ALL_FILES_ACCESS);
			}
			catch (Exception e2)
			{
				Log.i(TAG, "DataDirPicker: cannot open all files access settings: " + e2);
				Toast.makeText(p, "Cannot request folder access / 无法请求文件夹权限", Toast.LENGTH_LONG).show();
				setUserDir(p, p.getFilesDir().getAbsolutePath());
				finish();
			}
		}
	}

	/*
	 * The permission state may not be refreshed the moment the player returns, and
	 * the onResume() driven by the focus change must not race with this check.
	 */
	private static void checkAllFilesAccessDelayed(final MainActivity p, final int attempt)
	{
		new Handler(Looper.getMainLooper()).postDelayed(new Runnable()
		{
			public void run()
			{
				if (hasAllFilesAccess())
				{
					launchPicker(p);
				}
				else if (attempt < 2)
				{
					checkAllFilesAccessDelayed(p, attempt + 1);
				}
				else
				{
					Log.i(TAG, "DataDirPicker: all files access not granted, using the internal directory");
					Toast.makeText(p, "Folder access not granted / 未授予文件夹权限", Toast.LENGTH_LONG).show();
					setUserDir(p, p.getFilesDir().getAbsolutePath());
					finish();
				}
			}
		}, 200);
	}

	private static void launchPicker(final MainActivity p)
	{
		try
		{
			Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
			intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
					| Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
			p.startActivityForResult(intent, REQ_PICK_DATA_DIR);
		}
		catch (Exception e)
		{
			Log.i(TAG, "DataDirPicker: cannot launch the directory picker: " + e);
			setUserDir(p, p.getFilesDir().getAbsolutePath());
			finish();
		}
	}

	/** @return true when the result was consumed here. */
	static boolean onActivityResult(final MainActivity p, final int requestCode, final int resultCode, final Intent data)
	{
		if (requestCode == REQ_ALL_FILES_ACCESS)
		{
			checkAllFilesAccessDelayed(p, 0);
			return true;
		}
		if (requestCode != REQ_PICK_DATA_DIR) return false;

		if (resultCode != Activity.RESULT_OK || data == null || data.getData() == null)
		{
			/* Cancelled: keep the internal directory, no complaint needed. */
			Log.i(TAG, "DataDirPicker: folder selection cancelled, using the internal directory");
			setUserDir(p, p.getFilesDir().getAbsolutePath());
			finish();
			return true;
		}

		Uri treeUri = data.getData();
		try
		{
			p.getContentResolver().takePersistableUriPermission(treeUri,
					Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
		}
		catch (Exception e) {}
		String newDir = resolveTreePath(treeUri);

		if (newDir != null && testWritable(newDir))
		{
			setUserDir(p, newDir);
			Globals.DataDir = newDir;
			Toast.makeText(p, newDir, Toast.LENGTH_LONG).show();
		}
		else
		{
			Log.i(TAG, "DataDirPicker: cannot use the selected directory, using the internal one");
			Toast.makeText(p, "Cannot use that folder / 无法使用该文件夹", Toast.LENGTH_LONG).show();
			setUserDir(p, p.getFilesDir().getAbsolutePath());
		}
		finish();
		return true;
	}

	/* Storage Access Framework tree URI -> real filesystem path, so that the
	 * game (which uses plain POSIX file access) can work with it. */
	private static String resolveTreePath(final Uri treeUri)
	{
		try
		{
			String docId = DocumentsContract.getTreeDocumentId(treeUri);
			String[] parts = docId.split(":");
			if (parts.length < 1) return null;
			String volume = parts[0];
			String rel = parts.length > 1 ? parts[1] : "";
			String base = null;
			if ("primary".equals(volume))
			{
				base = Environment.getExternalStorageDirectory().getAbsolutePath();
			}
			else
			{
				File sd = new File("/storage/" + volume);
				if (sd.isDirectory()) base = sd.getAbsolutePath();
			}
			if (base == null)
			{
				Log.i(TAG, "DataDirPicker: cannot map tree uri to a real path: " + treeUri);
				return null;
			}
			String path = rel.length() > 0 ? base + "/" + rel : base;
			Log.i(TAG, "DataDirPicker: resolved " + treeUri + " -> " + path);
			return path;
		}
		catch (Exception e)
		{
			Log.i(TAG, "DataDirPicker: cannot resolve tree uri " + treeUri + ": " + e);
			return null;
		}
	}

	/*
	 * A picked folder is only useful if the game can really write into it, which
	 * on Android 11+ needs "All files access"; verify by creating a file.
	 */
	private static boolean testWritable(final String path)
	{
		try
		{
			File dir = new File(path);
			if (!dir.isDirectory() && !dir.mkdirs()) return false;
			File probe = new File(dir, ".openttd-write-test");
			FileOutputStream out = new FileOutputStream(probe);
			out.write(0);
			out.close();
			probe.delete();
			return true;
		}
		catch (Exception e)
		{
			Log.i(TAG, "DataDirPicker: not writable: " + path + " (" + e + ")");
			return false;
		}
	}

	private static void finish()
	{
		Runnable r = pendingContinue;
		pendingContinue = null;
		if (r != null) r.run();
	}
}
