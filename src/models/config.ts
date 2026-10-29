import {
  defaultSessionLogger,
  type SessionLoggerCallback,
} from '../utils/Logger';

export interface SessionRecorderConfig {
  /** The backend endpoint that receives session data. */
  endpoint?: string;
  /** Whether to show the debug logs. Defaults to false. */
  debugLog?: boolean;
  /**
   * Paints captured rectangles and includes the technical component type in
   * diagnostic LOM nodes during development. Defaults to false.
   */
  debugShowTree?: boolean;
  /** Force sending data to the endpoint even in debug mode. Defaults to false. */
  debugSendSession?: boolean;
  /** Custom logger delegation. */
  logger?: SessionLoggerCallback;
}

export class ResolvedSessionRecorderConfig {
  public readonly endpoint: string;
  public readonly debugLog: boolean;
  public readonly debugShowTree: boolean;
  public readonly shouldSend: boolean;
  public readonly logger: SessionLoggerCallback;

  constructor(options: SessionRecorderConfig) {
    this.endpoint = options.endpoint || '';
    this.debugLog = options.debugLog ?? false;
    this.debugShowTree = options.debugShowTree ?? false;

    const isDev = typeof __DEV__ !== 'undefined' ? !__DEV__ : true;

    this.shouldSend = isDev || (options.debugSendSession ?? false);

    this.logger = options.logger ?? defaultSessionLogger;
  }

  public validate(): void {
    const endpoint = this.endpoint.trim();

    try {
      const uri = new URL(endpoint);
      if (
        !/^https?:\/\//i.test(endpoint) ||
        !uri.hostname ||
        (uri.protocol !== 'http:' && uri.protocol !== 'https:')
      ) {
        throw new Error();
      }
    } catch {
      throw new Error(
        'Invalid Endpoint. Expected an absolute HTTP or HTTPS URI with a host.'
      );
    }
  }
}
