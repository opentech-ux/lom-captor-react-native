import type { SessionRecorderConfig } from '../models/config';

export enum SessionLogLevel {
  ERROR = 'error',
  WARNING = 'warning',
  INFO = 'info',
  VERBOSE = 'verbose',
}

export type SessionLoggerCallback = (
  level: SessionLogLevel,
  message: string,
  error?: any,
  stackTrace?: string
) => void;

declare const __DEV__: boolean;

export const defaultSessionLogger: SessionLoggerCallback = (
  level,
  message,
  error,
  _stackTrace
) => {
  if (typeof __DEV__ !== 'undefined' && !__DEV__) return;

  const prefixes: Record<SessionLogLevel, string> = {
    [SessionLogLevel.ERROR]: '!!',
    [SessionLogLevel.WARNING]: '!',
    [SessionLogLevel.INFO]: '>',
    [SessionLogLevel.VERBOSE]: '-',
  };

  const prefix = prefixes[level] || '';

  const detail =
    error == null ? '' : String(error instanceof Error ? error.message : error);
  const formattedMsg =
    `[SessionRecorder] ${prefix} ${message}${detail ? `: ${detail}` : ''}`.replace(
      /[\r\n]+/g,
      ' '
    );

  switch (level) {
    case SessionLogLevel.ERROR:
      // Recoverable SDK diagnostics must not enter Expo's exception UI.
      console.info(formattedMsg);
      break;
    case SessionLogLevel.WARNING:
      console.warn(formattedMsg);
      break;
    case SessionLogLevel.INFO:
      console.info(formattedMsg);
      break;
    case SessionLogLevel.VERBOSE:
      console.debug(formattedMsg);
      break;
  }
};

/**
 * Defines the delegation mechanism for the SDK's internal logs.
 * * This prevents console pollution and gives the client application absolute
 * control over how, when, and where logs are stored or displayed.
 */
export class SessionLogger {
  private static _config: SessionRecorderConfig = {
    endpoint: '',
    debugLog: false,
  };
  private static _delegate: SessionLoggerCallback = defaultSessionLogger;

  static configure(configuration: SessionRecorderConfig): void {
    this._config = configuration;
    if (configuration.logger) {
      this._delegate = configuration.logger;
    }
  }

  static isEnabled(level: SessionLogLevel): boolean | undefined {
    if (level === SessionLogLevel.ERROR) return true;
    if (!this._config.debugLog) return false;

    const defaultLoggerUnavailableInRelease =
      this._delegate === defaultSessionLogger &&
      typeof __DEV__ !== 'undefined' &&
      !__DEV__;

    return !defaultLoggerUnavailableInRelease;
  }

  static info(message: string): void {
    if (this._config.debugLog) {
      this._invokeDelegate(SessionLogLevel.INFO, message);
    }
  }

  static verbose(message: string): void {
    if (this._config.debugLog) {
      this._invokeDelegate(SessionLogLevel.VERBOSE, message);
    }
  }

  static warning(message: string): void {
    if (this._config.debugLog) {
      this._invokeDelegate(SessionLogLevel.WARNING, message);
    }
  }

  static error(message: string, e?: any): void {
    try {
      if (e == null) this._invokeDelegate(SessionLogLevel.ERROR, message);
      else
        this._invokeDelegate(
          SessionLogLevel.ERROR,
          message,
          e,
          e instanceof Error ? e.stack : undefined
        );
    } catch {
      // Even an exotic error/stack getter must not affect the host.
    }
  }

  private static _invokeDelegate(
    ...args: Parameters<SessionLoggerCallback>
  ): void {
    try {
      // The public callback returns void, but async adapters can return a value.
      const result: unknown = this._delegate(...args);
      if (
        result !== null &&
        (typeof result === 'object' || typeof result === 'function')
      ) {
        // Assimilation also contains a throwing then getter or thenable.
        Promise.resolve(result).catch(() => {});
      }
    } catch {
      // A diagnostic failure must never interrupt recording or log recursively.
    }
  }
}
