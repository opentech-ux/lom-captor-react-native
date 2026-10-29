import { TurboModuleRegistry, type TurboModule } from 'react-native';

export interface Spec extends TurboModule {
  initJSI(): boolean;
}

export default function resolveSessionRecorderReactNative(): Spec | null {
  return TurboModuleRegistry.get<Spec>('SessionRecorderReactNative') ?? null;
}
