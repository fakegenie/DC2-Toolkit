package dc2toolkit.dc2;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import org.libsdl.app.SDLActivity;

public class DC2 extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        Uri picked = data == null ? null : data.getData();
        if (picked != null) {
            try {
                getContentResolver().takePersistableUriPermission(picked, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException refused) {
            }
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    public String displayName(String uri) {
        Uri item = Uri.parse(uri);
        try (Cursor cursor = getContentResolver().query(item, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) return cursor.getString(0);
        } catch (RuntimeException refused) {
        }
        return item.getLastPathSegment();
    }
}
