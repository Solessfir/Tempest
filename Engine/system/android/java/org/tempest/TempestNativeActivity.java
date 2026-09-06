package org.tempest;

import android.app.NativeActivity;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;

public class TempestNativeActivity extends NativeActivity {
    private EditText textInput;
    private boolean synchronizingText;

    private native void nativeSetText(String text);
    private native void nativeEditorAction();

    @Override
    protected void onCreate(Bundle state) {
        loadNativeLibrary();
        super.onCreate(state);

        textInput = new EditText(this);
        textInput.setSingleLine(true);
        textInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        textInput.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        textInput.setBackgroundColor(Color.TRANSPARENT);
        textInput.setTextColor(Color.TRANSPARENT);
        textInput.setCursorVisible(false);
        textInput.setAlpha(0.01f);
        textInput.setPadding(0, 0, 0, 0);
        textInput.setFocusable(false);

        FrameLayout.LayoutParams layout = new FrameLayout.LayoutParams(1, 1, Gravity.BOTTOM | Gravity.LEFT);
        addContentView(textInput, layout);

        textInput.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence text, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence text, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable text) {
                if (!synchronizingText)
                    nativeSetText(text.toString());
            }
        });
        textInput.setOnEditorActionListener((view, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_DONE) {
                nativeEditorAction();
                return true;
            }
            return false;
        });
    }

    private void loadNativeLibrary() {
        try {
            ApplicationInfo info = getPackageManager().getApplicationInfo(
                    getPackageName(), PackageManager.GET_META_DATA);
            System.loadLibrary(info.metaData.getString("android.app.lib_name"));
        } catch (PackageManager.NameNotFoundException error) {
            throw new IllegalStateException("Cannot read the NativeActivity library name", error);
        }
    }

    public void showSoftInput(String text) {
        runOnUiThread(() -> {
            synchronizingText = true;
            textInput.setText(text);
            textInput.setSelection(textInput.length());
            synchronizingText = false;
            textInput.setFocusableInTouchMode(true);
            textInput.setFocusable(true);
            textInput.requestFocus();

            InputMethodManager keyboard = getSystemService(InputMethodManager.class);
            keyboard.restartInput(textInput);
            textInput.post(() -> keyboard.showSoftInput(textInput, InputMethodManager.SHOW_IMPLICIT));
        });
    }

    public void hideSoftInput() {
        runOnUiThread(() -> {
            InputMethodManager keyboard = getSystemService(InputMethodManager.class);
            keyboard.hideSoftInputFromWindow(textInput.getWindowToken(), 0);
            textInput.clearFocus();
            textInput.setFocusable(false);
        });
    }
}
