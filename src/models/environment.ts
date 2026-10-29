import { NativeModules, Platform } from 'react-native';

export type SessionEnvironment = {
  fwk: 'react-native';
  os: 'android' | 'ios';
  bmd: 'debug' | 'release';
};

export const resolveSessionEnvironment = (): SessionEnvironment | undefined => {
  try {
    const os = Platform.OS;
    if (os !== 'android' && os !== 'ios') return undefined;

    const bmd: unknown =
      NativeModules.SessionRecorderLifecycle?.getConstants?.()?.buildMode;
    if (bmd !== 'debug' && bmd !== 'release') return undefined;

    return Object.freeze<SessionEnvironment>({ fwk: 'react-native', os, bmd });
  } catch {
    // Environment metadata must never prevent recording or reporting.
    return undefined;
  }
};
