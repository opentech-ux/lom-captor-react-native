import { NativeModules, Platform } from 'react-native';

export type SessionEnvironment = {
  fwk: 'react-native';
  os: 'android' | 'ios';
  bmd: 'debug' | 'release';
};

export const resolveSessionEnvironment = (): SessionEnvironment => {
  const os = Platform.OS;
  if (os !== 'android' && os !== 'ios') {
    throw new Error('Session Recorder requires Android or iOS');
  }

  const bmd: unknown =
    NativeModules.SessionRecorderLifecycle?.getConstants?.()?.buildMode;
  if (bmd !== 'debug' && bmd !== 'release') {
    throw new Error('Session Recorder native buildMode is unavailable');
  }

  return Object.freeze<SessionEnvironment>({ fwk: 'react-native', os, bmd });
};
