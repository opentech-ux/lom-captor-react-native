import { toMapChunk, type Chunk } from '../models/chunk';
import type { Lom } from '../models/lom';
import { SessionLogger } from '../utils/Logger';
import type { SessionRecorderEngineInternal } from './Engine';

interface QueuedChunk {
  chunk: Chunk;
  retries: number;
}

export class SessionRecorderReporter {
  private static readonly REPORTING_INTERVAL_MS = 10000;
  private static readonly REQUEST_TIMEOUT_MS = 5000;
  private static readonly MAX_PENDING_CHUNKS = 3;
  private static readonly MAX_RESCUED_LOMS = 64;
  private static readonly MAX_RETRIES = 1;

  private timer: ReturnType<typeof setInterval> | null = null;
  private readonly _engine: SessionRecorderEngineInternal;
  private readonly _pendingChunks: QueuedChunk[] = [];
  private readonly _rescuedLoms = new Map<string, Lom>();
  private _isFlushing = false;
  private _isClosed = false;
  private _activeRequest: AbortController | null = null;

  constructor(engine: SessionRecorderEngineInternal) {
    this._engine = engine;
  }

  start(): void {
    if (this._isClosed) return;
    if (!this._engine.config.endpoint) return;
    if (this.timer) return;

    this.timer = setInterval(() => {
      this.flush();
    }, SessionRecorderReporter.REPORTING_INTERVAL_MS);
  }

  stop(): void {
    if (!this.timer) return;

    clearInterval(this.timer);
    this.timer = null;
  }

  close(): void {
    if (this._isClosed) return;

    this._isClosed = true;
    this.stop();
    this._activeRequest?.abort();
    this._activeRequest = null;
    this._pendingChunks.length = 0;
    this._rescuedLoms.clear();
  }

  private async flush(): Promise<void> {
    if (this._isClosed || this._isFlushing) return;
    this._isFlushing = true;

    try {
      const chunk: Chunk | null = this._engine.context.extractChunk();
      if (!this._engine.config.shouldSend) {
        if (this._engine.config.debugLog) {
          SessionLogger.verbose('Not send it cause `shouldSend` is [false]');
        }
        return;
      }

      if (chunk) this.enqueue(chunk);

      await this.drainQueue();
    } catch (error) {
      SessionLogger.error('Reporting flush failed', error);
    } finally {
      this._isFlushing = false;
    }
  }

  private enqueue(chunk: Chunk): void {
    if (
      this._pendingChunks.length >= SessionRecorderReporter.MAX_PENDING_CHUNKS
    ) {
      const dropped = this._pendingChunks.shift();
      if (dropped) this.rescueFullLoms(dropped.chunk);
      SessionLogger.warning('Dropping oldest pending chunk');
    }

    this._pendingChunks.push({ chunk, retries: 0 });
  }

  private async drainQueue(): Promise<void> {
    while (!this._isClosed && this._pendingChunks.length > 0) {
      const queued = this._pendingChunks[0]!;
      this.rehydrateRequiredLoms(queued.chunk);

      const sent = await this.send(queued.chunk);
      if (this._isClosed) return;

      if (sent) {
        this.forgetSentFullLoms(queued.chunk);
        this._pendingChunks.shift();
        continue;
      }

      queued.retries += 1;
      if (queued.retries > SessionRecorderReporter.MAX_RETRIES) {
        this.rescueFullLoms(queued.chunk);
        this._pendingChunks.shift();
        SessionLogger.error('Dropping chunk after retry');
        continue;
      }

      return;
    }
  }

  private async send(chunk: Chunk): Promise<boolean> {
    const uri = this._engine.config.endpoint;
    const controller = new AbortController();
    this._activeRequest = controller;
    let didTimeout = false;
    const timeout = setTimeout(() => {
      didTimeout = true;
      controller.abort();
    }, SessionRecorderReporter.REQUEST_TIMEOUT_MS);

    try {
      const response = await fetch(uri, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json; charset=UTF-8',
        },
        body: JSON.stringify(toMapChunk(chunk)),
        signal: controller.signal,
      });

      if (!response.ok) {
        throw new Error(`Server responded with status ${response.status}`);
      }

      SessionLogger.info('Sended Data - SESSION RECORDER');
      return true;
    } catch (error) {
      SessionLogger.error(
        didTimeout ? 'Request timed out' : 'Unexpected error sending data',
        error
      );
      return false;
    } finally {
      clearTimeout(timeout);
      if (this._activeRequest === controller) {
        this._activeRequest = null;
      }
    }
  }

  private rescueFullLoms(chunk: Chunk): void {
    for (const lom of chunk.loms) {
      if (!this.isFullLom(lom)) continue;
      this.rememberRescuedLom(lom);
    }
  }

  private rehydrateRequiredLoms(chunk: Chunk): void {
    if (this._rescuedLoms.size === 0) return;

    const referencedRefs = new Set<string>();
    for (const lom of chunk.loms) {
      if (this.isLomRef(lom)) this.addValidRef(referencedRefs, lom.ref);
    }
    for (const action of chunk.actionEvents) {
      this.addValidRef(referencedRefs, action.lomRef);
    }
    for (const exploration of chunk.explorationEvents) {
      if ('lomRef' in exploration) {
        this.addValidRef(referencedRefs, exploration.lomRef);
      }
    }

    if (referencedRefs.size === 0) return;

    const fullRefs = new Set<string>();
    for (const lom of chunk.loms) {
      if (this.isFullLom(lom)) this.addLomAliases(fullRefs, lom);
    }

    for (let index = 0; index < chunk.loms.length; index += 1) {
      const lom = chunk.loms[index]!;
      if (!this.isLomRef(lom) || fullRefs.has(lom.ref)) continue;

      const rescued = this.findRescuedLom(lom.ref);
      if (!rescued) continue;

      chunk.loms.splice(index, 0, rescued);
      index += 1;
      this.addLomAliases(fullRefs, rescued);
    }

    for (const ref of referencedRefs) {
      if (fullRefs.has(ref)) continue;

      const rescued = this.findRescuedLom(ref);
      if (!rescued) continue;

      if (
        chunk.loms.some((lom) => this.isFullLom(lom) && lom.id === rescued.id)
      ) {
        this.addLomAliases(fullRefs, rescued);
        continue;
      }

      chunk.loms.push(rescued);
      this.addLomAliases(fullRefs, rescued);
    }
  }

  private forgetSentFullLoms(chunk: Chunk): void {
    for (const lom of chunk.loms) {
      if (this.isFullLom(lom)) this._rescuedLoms.delete(lom.id);
    }
  }

  private rememberRescuedLom(lom: Lom): void {
    const existing = this._rescuedLoms.get(lom.id);
    // A UUID collision with a different structural identity is ambiguous;
    // retain the existing rescue rather than silently replacing it.
    if (existing && existing.ref !== lom.ref) return;

    this._rescuedLoms.delete(lom.id);
    if (this._rescuedLoms.size >= SessionRecorderReporter.MAX_RESCUED_LOMS) {
      const oldestRef = this._rescuedLoms.keys().next().value;
      if (oldestRef !== undefined) this._rescuedLoms.delete(oldestRef);
    }

    this._rescuedLoms.set(lom.id, lom);
  }

  private findRescuedLom(alias: string): Lom | undefined {
    const matches = [...this._rescuedLoms.values()].filter(
      (lom) => lom.id === alias || lom.ref === alias
    );
    if (matches.length === 0) return undefined;

    const structuralRefs = new Set(matches.map((lom) => lom.ref));
    if (structuralRefs.size > 1) return undefined;

    // Multiple materializations with one structural ref represent the same
    // structure; insertion order makes the newest rescue deterministic.
    return matches[matches.length - 1];
  }

  private addLomAliases(aliases: Set<string>, lom: Lom): void {
    this.addValidRef(aliases, lom.id);
    this.addValidRef(aliases, lom.ref);
  }

  private isFullLom(lom: Lom): boolean {
    return lom.fromCache === false && lom.root != null;
  }

  private isLomRef(lom: Lom): boolean {
    return lom.fromCache === true;
  }

  private addValidRef(refs: Set<string>, ref: string | null): void {
    const normalized = ref?.trim();
    if (normalized) refs.add(normalized);
  }
}
