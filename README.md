# LOM Captor React Native

**LOM Captor React Native** is a lightweight SDK for recording user interactions and the structure of the visible UI.

It captures gestures, scrolling, navigation context, and visible element geometry without recording screenshots, displayed text, form values, or media content.

> LOM Captor React Native captures interaction and layout metadata, not screen content.

## Installation

Install the package using npm:

```sh
npm install lom-captor-react-native@1.0.0
```

Or Yarn:

```sh
yarn add lom-captor-react-native@1.0.0
```

LOM Captor React Native uses React Native autolinking. No SDK-specific changes to `MainActivity`, `MainApplication`, or `AppDelegate` are required.

Because the package contains native code, **rebuild the application after installation or upgrades**. Restarting Metro alone is not enough.

### iOS

Install CocoaPods dependencies before rebuilding:

```sh
cd ios
pod install
cd ..
```

### Expo

Expo projects require a **development build**. The package does not work in Expo Go.

```sh
# Android
npx expo run:android

# iOS
npx expo run:ios
```

## Compatibility

| Area         | Requirement or status                              |
| ------------ | -------------------------------------------------- |
| React Native | `>=0.85.3 <0.89.0` (declared compatibility target) |
| React        | `^19.2.0`                                          |
| Architecture | New Architecture / Fabric required                 |
| Android      | Supported                                          |
| iOS          | Supported                                          |
| Web          | Not supported                                      |
| Expo Go      | Not supported                                      |

Android and iOS have been tested with React Native 0.86.3. Compatibility with other React Native versions within the declared range may vary.

The React Native version range is a compatibility target, **not a fully validated support matrix**. Verify native build and runtime behavior with your application's React Native and Expo versions.

## Quick Start

Initialize LOM Captor React Native **once** when your application starts:

```ts
import { SessionRecorder } from 'lom-captor-react-native';

SessionRecorder.init({
  endpoint: 'https://your-subdomain.ux-key.com/endpoint',
});
```

LOM Captor React Native then automatically observes supported interactions and captures the visible UI. No per-component wrappers or manual gesture registration are needed.

**That's all that is required for basic integration.**

> You can optionally provide logical screen names to improve navigation context.

## Navigation

LOM Captor React Native works without additional navigation setup. For more precise screen context, call `SessionRecorder.setScreenName()` when the active logical screen changes.

Use stable names such as `Home`, `Projects`, or `ProjectDetails`, rather than URLs or route parameters containing user data. **Screen names are hashed before being stored or sent.**

### React Navigation

```tsx
import {
  NavigationContainer,
  createNavigationContainerRef,
} from '@react-navigation/native';
import { SessionRecorder } from 'lom-captor-react-native';

const navigationRef = createNavigationContainerRef();

function updateSessionScreen() {
  const name = navigationRef.getCurrentRoute()?.name;
  if (name) SessionRecorder.setScreenName(name);
}

export function Navigation() {
  return (
    <NavigationContainer
      ref={navigationRef}
      onReady={updateSessionScreen}
      onStateChange={updateSessionScreen}
    >
      {/* Your application */}
    </NavigationContainer>
  );
}
```

### Expo Router

You can use Expo Router's route segments to report the active logical screen:

```tsx
import { useEffect } from 'react';
import { useSegments } from 'expo-router';
import { SessionRecorder } from 'lom-captor-react-native';

export function SessionRecorderScreenContext() {
  const segments = useSegments();

  useEffect(() => {
    const screenName = segments
      .filter((segment) => !/^\([^)]+\)$/.test(segment))
      .join('/');

    if (screenName) SessionRecorder.setScreenName(screenName);
  }, [segments]);

  return null;
}
```

Mount this component within your Expo Router layout. For example, `(drawer)/settings` becomes `settings`. Removing route groups is optional and is controlled by your application.

For other navigation libraries, call `SessionRecorder.setScreenName('YourScreenName')` when the current screen changes.

Navigation integration is optional; interaction recording continues without it.

## Configuration

Only `endpoint` is required. Optional settings are available for diagnostics and development:

```ts
SessionRecorder.init({
  endpoint: 'https://your-subdomain.ux-key.com/endpoint',
  debugLog: false,
  debugShowTree: false,
  debugSendSession: false,
  logger: (level, message, error, stackTrace) => {
    // Forward SDK logs to your own monitoring service.
  },
});
```

| Option             | Purpose                                                    |
| ------------------ | ---------------------------------------------------------- |
| `endpoint`         | HTTP(S) endpoint receiving session data. **Required.**     |
| `debugLog`         | Enables SDK diagnostic logs.                               |
| `debugShowTree`    | Enables additional UI-tree diagnostics during development. |
| `debugSendSession` | Allows reporting from development builds.                  |
| `logger`           | Sends SDK logs and errors to a custom callback.            |

Session data is sent normally in release builds. Development builds do not send data by default; enable `debugSendSession` to test reporting during development.

## Data Collection & Privacy

LOM Captor React Native collects **interaction and layout metadata**, not the content displayed on screen.

### Captured

- Taps, double taps, long presses, drag and pinch gestures.
- Interaction coordinates and scrolling activity.
- The structure of the **currently visible** React Native UI.
- Positions and sizes of visible UI elements.
- Structural component/type identifiers.
- Hashed screen names, when provided.
- Session and timing metadata needed to associate interactions with UI captures.

Only materialized, visible elements are represented in the captured UI structure. Elements outside the viewport are excluded, and partially visible elements are clipped to the viewport.

### Not Captured

LOM Captor React Native does **not** capture:

- Screenshots or screen pixels.
- Displayed text or text entered by users.
- Form field values.
- Images, videos, or other media content.
- WebView page content.

Session data is grouped into chunks and sent to the configured endpoint. A separate HTTP request is not sent for each interaction.

> Avoid including personal or sensitive information in logical screen names or custom component names.

## Error Reporting

LOM Captor React Native is designed so that SDK failures do not interrupt your application. If an operation cannot be completed, the affected capture or interaction may be skipped.

To forward errors to your existing monitoring system, configure `logger`:

```ts
SessionRecorder.init({
  endpoint: 'https://your-subdomain.ux-key.com/endpoint',
  logger: (level, message, error, stackTrace) => {
    if (level === 'error') {
      reportRecorderError(message, error, stackTrace);
    }
  },
});
```

`reportRecorderError` is an example function provided by your application. LOM Captor React Native does not depend on a specific monitoring service.

## Native Notes

Standard React Native scrolling components are observed automatically. Custom native components and custom scrolling implementations may have limited support depending on their implementation.

The package requires native integration and Fabric; it cannot run as a JavaScript-only dependency.