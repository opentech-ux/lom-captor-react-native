# Optional PagerView 9.x host: identified without linking the SDK to that library.
# No fields or private Compose state are inspected.
-keepnames class com.reactnativepagerview.ComposePagerView
# Optional standard native-stack owner, identified through public Fragment APIs.
-keepnames class com.swmansion.rnscreens.ScreenStackFragment
# Exact RN standard Alert owner, identified through public AndroidX Fragment APIs.
-keepnames class com.facebook.react.modules.dialog.AlertFragment
# Optional datetimepicker 9.2.1 standard owner; no compile-time package dependency.
-keepnames class com.reactcommunity.rndatetimepicker.RNDatePickerDialogFragment
-keepnames class com.reactcommunity.rndatetimepicker.RNTimePickerDialogFragment
